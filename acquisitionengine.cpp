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

    // 设置 MISO 采样延迟：假设 3 英尺线缆
    m_rhxController->setCableLengthFeet(PortA, 3.0);

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

    emit logMessage("连续采集已启动（允许发送刺激）");
}


void AcquisitionEngine::stopAcquisition()
{
    if (!m_deviceOpened) return;

    // 1. 停止 USB 定时器，GUI 不再收到新数据
    m_usbTimer.stop();

    // 2. 清空本地队列，防止残留数据被慢慢处理
    while (!m_dataQueue.empty()) {
        delete m_dataQueue.front();
        m_dataQueue.pop_front();
    }

    // 3. **不要调用 flush()**，因为我们没有办法把板子真实停下来
    //    保持它 free-run，等程序退出时在 cleanup() 里统一处理即可

    emit logMessage("采集已停止（停止 USB 读取，但板子仍在运行）");
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

    // 目前先只处理 stream 0
    int streamIndex = 0;

    while (!m_dataQueue.empty()) {
        RHXDataBlock *block = m_dataQueue.front();
        m_dataQueue.pop_front();

        int samplesPerBlock = block->samplesPerDataBlock(); // 固定 128
        QVector<uint32_t> timeStamps(samplesPerBlock);
        QVector<QVector<int>> channelData(
            m_channelsPerStream,
            QVector<int>(samplesPerBlock));

        for (int t = 0; t < samplesPerBlock; ++t) {
            timeStamps[t] = block->timeStamp(t);

            for (int ch = 0; ch < m_channelsPerStream; ++ch) {
                int value = block->amplifierData(streamIndex, ch, t);
                channelData[ch][t] = value;
            }
        }

        // 把这一块数据发出去（给 GUI 或实验流程模块）
        emit newSamples(timeStamps, channelData);

        delete block;
    }
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
