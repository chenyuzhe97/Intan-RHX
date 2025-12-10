#include "acquisitionengine.h"

AcquisitionEngine::AcquisitionEngine(QObject *parent)
    : QObject(parent)
{
    // 方便调试，看一下 USB 定时读取
    m_usbTimer.setInterval(30); // ~33ms ≈ 30 Hz
    connect(&m_usbTimer, &QTimer::timeout,
            this, &AcquisitionEngine::onUsbTimer);
}

AcquisitionEngine::~AcquisitionEngine()
{
    stopAcquisition();
    cleanup();
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
    m_rhxController->setStimCmdMode(true);   // ⭐ 开启刺激命令模式

    // 开始 SPI 采集（同时可以收数 + 刺激）
    m_rhxController->run();

    // 启动 USB 轮询定时器
    m_usbTimer.start();

    // 连续采集模式开
    m_continuousRunning = true;


    emit logMessage("连续采集已启动（允许发送刺激）");
}


#include <QCoreApplication>  // 头文件顶部记得加

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

    const int streamIdx0 = 0;          // 第一个启用的 data stream（A端）
    const int streamIdx1 = (numStreams > 1) ? 1 : -1; // 第二个（B端），如果有的话

    while (!m_dataQueue.empty()) {
        RHXDataBlock *block = m_dataQueue.front();
        m_dataQueue.pop_front();

        int samplesPerBlock = block->samplesPerDataBlock(); // 一般 128

        // ========= stream0: 仍然按你原来的方式输出 =========
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

        emit newSamples(timeStamps0, channelData0);

        // ========= stream2(B端): 如果有第二个 data stream，就再构造一份 =========
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

            emit newSamplesStream2(timeStamps2, channelData2);
        }

        delete block;
    }
}

void AcquisitionEngine::pauseContinuousForStim()
{
    if (!m_deviceOpened) return;
    if (!m_continuousRunning) return;  // 本来就没在连续采集，啥也不做

    // 1. 停掉 USB 定时读取
    m_usbTimer.stop();

    // 2. 告诉 FPGA 不要再 continuous run 了
    m_rhxController->setContinuousRunMode(false);

    // 3. 等当前这一次 run 结束
    while (m_rhxController->isRunning()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }

    m_continuousRunning = false;

    emit logMessage("自适应刺激：已暂停连续采集，准备更新刺激参数");
}

void AcquisitionEngine::resumeContinuousAfterStim()
{
    if (!m_deviceOpened) return;
    if (m_continuousRunning) return;   // 已经在跑就不用重复启

    // 刺激期间可能产生了一点点 FIFO 数据，可以先清掉
    m_rhxController->flush();

    // 再次进入 continuous 采集模式
    m_rhxController->setContinuousRunMode(true);
    m_rhxController->setStimCmdMode(false);
    m_rhxController->run();

    // 重新开启 USB 定时轮询
    m_usbTimer.start();
    m_continuousRunning = true;

    emit logMessage("自适应刺激：刺激参数更新完成，已恢复连续采集");
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
        emit logMessage("自适应刺激：检测到处于连续采集中，先暂停采集以更新刺激参数…");
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


