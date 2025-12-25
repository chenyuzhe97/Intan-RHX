#include "acquisitionengine.h"

AcquisitionEngine::AcquisitionEngine(QObject *parent)
    : QObject(parent)
{
    // 方便调试，看一下 USB 定时读取
    m_usbTimer.setInterval(30); // ~33ms ≈ 30 Hz
    connect(&m_usbTimer, &QTimer::timeout,
            this, &AcquisitionEngine::onUsbTimer);
}

void AcquisitionEngine::cleanup()
{
    // 清空队列里的 RHXDataBlock
    while (!m_dataQueue.empty()) {
        delete m_dataQueue.front();
        m_dataQueue.pop_front();
    }

    if (m_stimController) {
        delete m_stimController;
        m_stimController = nullptr;
    }

    if (m_rhxController) {
        int ledArray[8] = {0,0,0,0,0,0,0,0};
        m_rhxController->setLedDisplay(ledArray);
        delete m_rhxController;
        m_rhxController = nullptr;
    }

    m_deviceOpened = false;
}

bool AcquisitionEngine::openDevice(const QString &bitfilePath)
{
    cleanup(); // 确保干净

    // 1. 创建 RHXController（你 main 里的第一句）
    m_rhxController = new RHXController(ControllerStimRecord,
                                        SampleRate30000Hz);

    // 2. 打开第一个设备
    std::vector<std::string> availableDevices =
        m_rhxController->listAvailableDeviceSerials();
    if (availableDevices.empty()) {
        emit errorOccurred("未找到任何 Opal Kelly XEM7310 设备");
        cleanup();
        return false;
    }

    m_rhxController->open(availableDevices[0]);

    // 3. 加载 bitfile 并初始化
    m_rhxController->uploadFPGABitfile(bitfilePath.toStdString());
    m_rhxController->initialize();

    // 默认先开 stream 0，你之前也打开了 2，这里可以保留
    m_rhxController->enableDataStream(0, true);
    m_rhxController->enableDataStream(2, true);

    // 设置 MISO 采样延迟：假设 3 英尺线缆
    m_rhxController->setCableLengthFeet(PortA, 3.0);
    m_rhxController->setCableLengthFeet(PortB, 3.0);

    // 亮一个 LED 表示程序在跑
    int ledArray[8] = {1,0,0,0,0,0,0,0};
    m_rhxController->setLedDisplay(ledArray);

    // 创建刺激控制器（完全照 main）
    m_stimController = new Controller(m_rhxController);

    // 记录流和通道数
    m_numEnabledStreams =
        m_rhxController->getNumEnabledDataStreams();
    m_channelsPerStream =
        RHXDataBlock::channelsPerStream(m_rhxController->getType());

    m_deviceOpened = true;
    emit logMessage("设备打开并初始化成功");
    return true;
}

void AcquisitionEngine::enableStream(int stream, bool enabled)
{
    if (!m_deviceOpened) return;

    m_rhxController->enableDataStream(stream, enabled);
    m_numEnabledStreams =
        m_rhxController->getNumEnabledDataStreams();
}

void AcquisitionEngine::startContinuousAcquisition()
{
    if (!m_deviceOpened) {
        emit errorOccurred("请先打开设备");
        return;
    }

    // 采集模式：连续
    m_rhxController->setContinuousRunMode(true);
    m_rhxController->setStimCmdMode(false);   // ⭐ 开启刺激命令模式

    // 开始 SPI 采集（同时可以收数 + 刺激）
    m_rhxController->run();

    // 启动 USB 轮询定时器
    m_usbTimer.start();

    // 连续采集模式开
    m_continuousRunning = true;

    emit logMessage("连续采集已启动（允许发送刺激）");
}

void AcquisitionEngine::stopAcquisition()
{
    if (!m_deviceOpened) return;

    // 停止 USB 定时器
    m_usbTimer.stop();
    m_continuousRunning = false;

    // 停止 SPI 连续采集
    if (m_rhxController->isRunning()) {
        m_rhxController->setContinuousRunMode(false);
        // 等一次 run 自己收尾结束
        while (m_rhxController->isRunning()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
    }

    // 清 FIFO
    m_rhxController->flush();
    m_continuousRunning = false;   // ⭐ 关键

    emit logMessage("采集已停止");
}

void AcquisitionEngine::onUsbTimer()
{
    if (!m_deviceOpened) return;

    // 官方文档有一个 blocksFor30Hz(sampleRate) 的工具函数
    int blocksToRead = RHXDataBlock::blocksFor30Hz(
        SampleRate30000Hz);

    // 这里你原来写死 16，也可以换成 blocksToRead
    bool usbDataRead =
        m_rhxController->readDataBlocks(16, m_dataQueue);

    if (!usbDataRead && !m_rhxController->isRunning()) {
        // 没有更多数据，可能被停止了
        return;
    }

    // 处理队列中所有 block
    processDataQueue();
}

void AcquisitionEngine::processDataQueue()
{
    // 如果设备还没打开，或者暂时没有数据块，直接返回
    if (!m_deviceOpened) return;
    if (m_dataQueue.empty()) return;

    // getNumEnabledDataStreams() 是实际启用的 stream 数量。
    int numStreams = m_rhxController->getNumEnabledDataStreams();
    if (numStreams <= 0) return;

    // streamIdx0 永远指向第一个启用的流（A 端）
    const int streamIdx0 = 0;

    // 如果启用了第二个流，则它是 streamIdx1（第二个启用的流），否则设为 -1
    const int streamIdx1 = (numStreams > 1) ? 1 : -1;

    // ============================================================
    // 主循环：每次处理队列中的一个 RHXDataBlock
    // ============================================================
    while (!m_dataQueue.empty()) {
        RHXDataBlock *block = m_dataQueue.front();
        m_dataQueue.pop_front();

        // 通常 Intan 的一个 block 内包含固定数量的样本（如 128）
        int samplesPerBlock = block->samplesPerDataBlock();

        // ------------------------------------------------------------
        // 处理 stream 0（A 端）
        // ------------------------------------------------------------

        // 时间戳缓存
        QVector<uint32_t> timeStamps0(samplesPerBlock);

        // 数据缓存：通道数 × samplesPerBlock
        QVector<QVector<int>> channelData0(
            m_channelsPerStream,
            QVector<int>(samplesPerBlock));

        // 遍历 block 中的每个样本点
        for (int t = 0; t < samplesPerBlock; ++t) {
            // timestamp 单位 = sample index
            timeStamps0[t] = block->timeStamp(t);

            // 读取每个通道的放大器数据（原始 16-bit ADC）
            for (int ch = 0; ch < m_channelsPerStream; ++ch) {
                int value = block->amplifierData(streamIdx0, ch, t);
                // 不转 uV，在 GUI 或算法层再转：
                //   uV = (value - 32768) * 0.195
                channelData0[ch][t] = value;
            }
        }

        // 1) 发射信号给 GUI（实时波形）与闭环算法
        emit newSamples(timeStamps0, channelData0);

        // ------------------------------------------------------------
        // 处理第二个数据流（例如 B 端 / stream2）——如果存在
        // ------------------------------------------------------------
        if (streamIdx1 >= 0) {
            QVector<uint32_t> timeStamps2(samplesPerBlock);
            QVector<QVector<int>> channelData2(
                m_channelsPerStream,
                QVector<int>(samplesPerBlock));

            for (int t = 0; t < samplesPerBlock; ++t) {
                // 所有流共享同一个时间戳
                timeStamps2[t] = block->timeStamp(t);

                for (int ch = 0; ch < m_channelsPerStream; ++ch) {
                    int value = block->amplifierData(streamIdx1, ch, t);
                    channelData2[ch][t] = value;
                }
            }

            // GUI 用的第二路波形
            emit newSamplesStream2(timeStamps2, channelData2);
        }

        // ------------------------------------------------------------
        // 录制：直接按 Intan 官方格式把整个 block 写入文件
        // ------------------------------------------------------------
        if (m_isRecording) {
            writeBlockStream();
            // writeBlockToRecording(block);

        }

        // 释放 block
        delete block;
    }
}

void AcquisitionEngine::pauseContinuousForStim()
{
    if (!m_deviceOpened) return;
    // 只停 USB 定时器，不停板子
    m_usbTimer.stop();
    emit logMessage("自适应刺激：仅暂停 USB 读取，板上采集不停");
}

void AcquisitionEngine::resumeContinuousAfterStim()
{
    if (!m_deviceOpened) return;
    m_usbTimer.start();
    emit logMessage("自适应刺激：恢复 USB 读取");
}

// ⭐ 关键：使用 RHXDataBlock::write() 写原生二进制格式
void AcquisitionEngine::writeBlockToRecording(RHXDataBlock *block)
{
    if (!m_isRecording) return;
    if (!m_recordStream.is_open()) return;
    if (!block) return;

    int numStreams = m_rhxController->getNumEnabledDataStreams();

    // 这和示例里的 queueToFile 在底层是一致的：按 Intan 定义格式写一个 USB data block
    block->write(m_recordStream, numStreams);
}

void AcquisitionEngine::writeBlockStream()
{
    m_rhxController->queueToFile(m_dataQueue,m_recordStream);
}

// ====== 刺激相关接口 ======

void AcquisitionEngine::configureStim(const QString &electrodeName,
                                      int firstPhaseAmplitude,
                                      int secondPhaseAmplitude,
                                      int firstPhaseDuration_us,
                                      int secondPhaseDuration_us,
                                      int interPhaseDelay_us,
                                      int numPulses,
                                      int triggerSource)
{
    if (!m_deviceOpened || !m_stimController) return;

    // 如果板子此刻正在 continuous run，这里只是提示，不强制停
    if (m_rhxController->isRunning()) {
        emit logMessage("当前在连续采集中。");
    }

    ElectrodeParameters *ele =
        new ElectrodeParameters(electrodeName.toStdString());

    ele->SetStimulationTiming(0,
                              firstPhaseDuration_us,
                              secondPhaseDuration_us,
                              interPhaseDelay_us);
    ele->SetStimulationAmplitude(firstPhaseAmplitude,
                                 secondPhaseAmplitude);
    ele->SetStimulationSource(triggerSource);
    ele->numOfPulses = numPulses;

    m_stimController->setStimSequenceParameters(ele);

    emit logMessage(QStringLiteral("已配置刺激电极 %1").arg(electrodeName));
    m_rhxController->setStimCmdMode(true);
}

void AcquisitionEngine::triggerStim(int triggerSource, bool on)
{
    if (!m_deviceOpened || !m_stimController) return;

    // 直接用你已有的接口（注意这里你原来是 triggerSource-24，看你整体工程怎么定义）
    m_stimController->stimTrigger(triggerSource-24, on);
}

void AcquisitionEngine::applyAdaptiveStim(const QString &electrodeName,
                                          int amplitude_uA,
                                          int numPulses,
                                          int triggerSource)
{
    if (!m_deviceOpened || !m_stimController) return;
    if (amplitude_uA <= 0 || numPulses <= 0) return;

    // ===== 1）如果当前正在连续采集，先暂停 USB 读取 =====
    bool resumeAfter = m_continuousRunning;
    if (resumeAfter) {
        emit logMessage("自适应刺激：更新刺激参数…");
        pauseContinuousForStim();
    }

    // ===== 2）正式配置刺激并触发 =====
    int firstDur_us   = 500;
    int secondDur_us  = 500;
    int interphase_us = 500;

    configureStim(electrodeName,
                  amplitude_uA,   // firstPhaseAmplitude
                  amplitude_uA,   // secondPhaseAmplitude
                  firstDur_us,
                  secondDur_us,
                  interphase_us,
                  numPulses,
                  triggerSource);

    // 触发一次刺激
    qDebug() << "当前触发:" << triggerSource;
    m_stimController->stimTrigger(triggerSource, true);
    m_stimController->stimTrigger(triggerSource, false);

    emit logMessage(QStringLiteral("自适应刺激：%1, 幅度=%2 uA, 脉冲数=%3, trigger=%4")
                        .arg(electrodeName)
                        .arg(amplitude_uA)
                        .arg(numPulses)
                        .arg(triggerSource));

    // ===== 3）如果刚才是连续采集，就自动恢复 =====
    if (resumeAfter) {
        resumeContinuousAfterStim();
    }
}

// ====== 录制控制 ======

bool AcquisitionEngine::startBinaryRecording(const QString &filePath)
{
    if (!m_deviceOpened) {
        emit errorOccurred("请先打开设备再开始录制");
        return false;
    }

    // 如果之前已经在录，先关掉
    if (m_isRecording) {
        stopBinaryRecording();
    }

    // ⭐ 按示例 main.cpp 的方式打开二进制文件
    m_recordStream.open(filePath.toStdString(),
                        std::ios::binary | std::ios::out);

    if (!m_recordStream.is_open()) {
        emit errorOccurred("无法打开录制文件：" + filePath);
        return false;
    }

    // 不写任何自定义文件头，直接写 Intan 原生数据块
    m_isRecording = true;
    emit logMessage("开始二进制录制：" + filePath);
    return true;
}

void AcquisitionEngine::stopBinaryRecording()
{
    if (!m_isRecording)
        return;

    m_isRecording = false;

    if (m_recordStream.is_open()) {
        m_recordStream.close();
    }

    emit logMessage("二进制录制已停止");
}

