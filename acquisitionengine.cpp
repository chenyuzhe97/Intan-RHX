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
    // 注意：官方例程里函数名是 uploadFpgaBitfile，
    // 你的工程里是 uploadFPGABitfile，就按你工程的来
    m_rhxController->uploadFPGABitfile(bitfilePath.toStdString());
    m_rhxController->initialize();

    // 默认先开 stream 0，后面你可以在 GUI 勾选其它 stream
    m_rhxController->enableDataStream(0, true);
    m_rhxController->enableDataStream(2, true);

    // 设置 MISO 采样延迟：假设 3 英尺线缆
    m_rhxController->setCableLengthFeet(PortA, 3.0);
    m_rhxController->setCableLengthFeet(PortB, 3.0);

    // 亮一个 LED 表示程序在跑
    int ledArray[8] = {1,0,0,0,0,0,0,0};
    m_rhxController->setLedDisplay(ledArray);

    // 创建刺激控制器（完全照你 main）
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

    // 不再 flush，前面我们已经删掉了
    // m_rhxController->flush();

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

    bool usbDataRead =
        m_rhxController->readDataBlocks(blocksToRead, m_dataQueue);

    if (!usbDataRead && !m_rhxController->isRunning()) {
        // 没有更多数据，可能被停止了
        return;
    }

    // 处理队列中所有 block
    processDataQueue();
}

void AcquisitionEngine::processDataQueue()
{
    if (!m_deviceOpened) return;
    if (m_dataQueue.empty()) return;

    // dataBlock 里的 stream 索引：0..(NumEnabledDataStreams-1)
    int numStreams = m_rhxController->getNumEnabledDataStreams();
    if (numStreams <= 0) return;

    const int streamIdx0 = 0;                 // 第一个启用的 data stream（A 端）
    const int streamIdx1 = (numStreams > 1)   // 第二个（B 端），如果有的话
                               ? 1 : -1;

    while (!m_dataQueue.empty()) {
        RHXDataBlock *block = m_dataQueue.front();
        m_dataQueue.pop_front();

        int samplesPerBlock = block->samplesPerDataBlock(); // 一般 128

        // ========= stream0(A端) =========
        QVector<uint32_t> timeStamps0(samplesPerBlock);
        QVector<QVector<int>> channelData0(
            m_channelsPerStream,
            QVector<int>(samplesPerBlock));

        for (int t = 0; t < samplesPerBlock; ++t) {
            timeStamps0[t] = block->timeStamp(t);

            for (int ch = 0; ch < m_channelsPerStream; ++ch) {
                int value = block->amplifierData(streamIdx0, ch, t);
                channelData0[ch][t] = value;
            }
        }

        // 1) 通知 GUI / 闭环算法（单通道图、AB 算法等用这个）
        emit newSamples(timeStamps0, channelData0);

        // 2) ⭐ 写入录制文件：把“第一个 data stream”标记为逻辑 streamIndex = 0（A）
        if (m_isRecording) {
            writeBlockToRecording(/*streamIndexInFile=*/0,
                                  timeStamps0,
                                  channelData0);
        }

        // ========= stream2(B端) =========
        if (streamIdx1 >= 0) {
            QVector<uint32_t> timeStamps2(samplesPerBlock);
            QVector<QVector<int>> channelData2(
                m_channelsPerStream,
                QVector<int>(samplesPerBlock));

            for (int t = 0; t < samplesPerBlock; ++t) {
                timeStamps2[t] = block->timeStamp(t); // 时间一样

                for (int ch = 0; ch < m_channelsPerStream; ++ch) {
                    int value = block->amplifierData(streamIdx1, ch, t);
                    channelData2[ch][t] = value;
                }
            }

            // 1) 通知 GUI / stream2 波形窗口
            emit newSamplesStream2(timeStamps2, channelData2);

            // 2) ⭐ 写入录制文件：把“第二个 data stream”标记为逻辑 streamIndex = 2（B）
            if (m_isRecording) {
                writeBlockToRecording(/*streamIndexInFile=*/2,
                                      timeStamps2,
                                      channelData2);
            }
        }

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

void AcquisitionEngine::writeBlockToRecording(
    int streamIndex,
    const QVector<uint32_t> &timeStamps,
    const QVector<QVector<int>> &channelData)
{
    if (!m_isRecording) return;
    if (!m_recordFile.isOpen()) return;
    if (timeStamps.isEmpty() || channelData.isEmpty()) return;

    int numSamples  = timeStamps.size();
    int numChannels = channelData.size();

    if (numSamples <= 0 || numChannels <= 0) return;

    // ===== 写 Block 头 =====
    quint8 streamIdx = quint8(streamIndex);
    quint8 reserved[3] = {0,0,0};

    m_recordStream << streamIdx;
    m_recordStream.writeRawData(reinterpret_cast<const char*>(reserved), 3);

    m_recordStream << quint32(numChannels);
    m_recordStream << quint32(numSamples);

    // ===== 写数据：逐 sample 写 =====
    for (int i = 0; i < numSamples; ++i) {
        // timestamp
        m_recordStream << quint32(timeStamps[i]);

        // 各通道原始值（int16）
        for (int ch = 0; ch < numChannels; ++ch) {
            qint16 raw = qint16(channelData[ch][i]);
            m_recordStream << raw;
        }
    }

    // 这里不强制 flush，性能会好一点；如果你怕掉电丢数据，可以偶尔 flush 一次：
    // m_recordStream.device()->flush();
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

    // ✅ 如果板子此刻正在 continuous run，就拒绝配置，避免把模式搞乱
    if (m_rhxController->isRunning()) {
        emit logMessage("警告：当前在连续采集中，不能修改刺激参数，请先停止采集再配置刺激。");
        return;
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
}


void AcquisitionEngine::triggerStim(int triggerSource, bool on)
{
    if (!m_deviceOpened || !m_stimController) return;

    // 直接用你已有的接口
    m_stimController->stimTrigger(triggerSource, on);
}


void AcquisitionEngine::applyAdaptiveStim(const QString &electrodeName,
                                          int amplitude_uA,
                                          int numPulses,
                                          int triggerSource)
{
    if (!m_deviceOpened || !m_stimController) return;
    if (amplitude_uA <= 0 || numPulses <= 0) return;

    // ===== 1）如果当前正在连续采集，先暂停 =====
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
    m_stimController->stimTrigger(triggerSource, true);

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

    m_recordFile.setFileName(filePath);
    if (!m_recordFile.open(QIODevice::WriteOnly)) {
        emit errorOccurred("无法打开录制文件：" + filePath);
        return false;
    }

    m_recordStream.setDevice(&m_recordFile);
    m_recordStream.setByteOrder(QDataStream::LittleEndian);

    // ===== 写文件头 =====
    char magic[8] = {'B','A','R','E','C','0','1','\0'};
    m_recordStream.writeRawData(magic, 8);

    quint32 version           = 1;
    float   sampleRateHz      = float(m_rhxController->getSampleRate());
    quint32 channelsPerStream = quint32(m_channelsPerStream);

    // 假设你只用 stream0 和 stream2：mask = bit0 + bit2
    quint32 streamMask        = 0;
    streamMask |= (1u << 0);  // stream0
    streamMask |= (1u << 2);  // stream2

    m_recordStream << version;
    m_recordStream << sampleRateHz;
    m_recordStream << channelsPerStream;
    m_recordStream << streamMask;

    // 4 个 reserved，占位
    for (int i = 0; i < 4; ++i) {
        m_recordStream << quint32(0);
    }

    m_isRecording = true;
    emit logMessage("开始二进制录制：" + filePath);
    return true;
}

void AcquisitionEngine::stopBinaryRecording()
{
    if (!m_isRecording) return;

    m_isRecording = false;
    if (m_recordFile.isOpen()) {
        m_recordFile.close();
    }

    emit logMessage("二进制录制已停止");
}



