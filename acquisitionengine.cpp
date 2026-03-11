#include "acquisitionengine.h"

AcquisitionEngine::AcquisitionEngine(QObject *parent)
    : QObject(parent)
{
    m_usbTimer.setInterval(30);
    connect(&m_usbTimer, &QTimer::timeout,
            this, &AcquisitionEngine::onUsbTimer);
}

void AcquisitionEngine::cleanup()
{
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
    cleanup();

    m_rhxController = new RHXController(ControllerStimRecord,
                                        SampleRate30000Hz);

    std::vector<std::string> availableDevices =
        m_rhxController->listAvailableDeviceSerials();
    if (availableDevices.empty()) {
        emit errorOccurred("未找到任何 Opal Kelly XEM7310 设备");
        cleanup();
        return false;
    }

    m_rhxController->open(availableDevices[0]);
    m_rhxController->uploadFPGABitfile(bitfilePath.toStdString());
    m_rhxController->initialize();

    m_rhxController->enableDataStream(0, true);
    m_rhxController->enableDataStream(2, true);

    m_rhxController->setCableLengthFeet(PortA, 3.0);
    m_rhxController->setCableLengthFeet(PortB, 3.0);

    int ledArray[8] = {1,0,0,0,0,0,0,0};
    m_rhxController->setLedDisplay(ledArray);

    m_stimController = new Controller(m_rhxController);

    m_numEnabledStreams = m_rhxController->getNumEnabledDataStreams();
    m_channelsPerStream = RHXDataBlock::channelsPerStream(m_rhxController->getType());

    m_deviceOpened = true;
    emit logMessage("设备打开并初始化成功");
    return true;
}

void AcquisitionEngine::enableStream(int stream, bool enabled)
{
    if (!m_deviceOpened) return;

    m_rhxController->enableDataStream(stream, enabled);
    m_numEnabledStreams = m_rhxController->getNumEnabledDataStreams();
}

void AcquisitionEngine::startContinuousAcquisition()
{
    if (!m_deviceOpened) {
        emit errorOccurred("请先打开设备");
        return;
    }

    m_rhxController->setContinuousRunMode(true);
    m_rhxController->setStimCmdMode(true);
    m_rhxController->run();

    m_usbTimer.start();
    m_continuousRunning = true;

    emit logMessage("连续采集已启动（允许发送刺激）");
}

void AcquisitionEngine::stopAcquisition()
{
    if (!m_deviceOpened) return;

    m_usbTimer.stop();
    m_continuousRunning = false;

    if (m_rhxController->isRunning()) {
        m_rhxController->setContinuousRunMode(false);
        while (m_rhxController->isRunning()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
    }

    m_rhxController->flush();
    m_continuousRunning = false;

    emit logMessage("采集已停止");
}

void AcquisitionEngine::onUsbTimer()
{
    if (!m_deviceOpened) return;

    int blocksToRead = RHXDataBlock::blocksFor30Hz(SampleRate30000Hz);
    bool usbDataRead = m_rhxController->readDataBlocks(blocksToRead, m_dataQueue);

    if (!usbDataRead && !m_rhxController->isRunning()) {
        return;
    }

    processDataQueue();
}

void AcquisitionEngine::processDataQueue()
{
    if (!m_deviceOpened) return;
    if (m_dataQueue.empty()) return;

    int numStreams = m_rhxController->getNumEnabledDataStreams();
    if (numStreams <= 0) return;

    const int streamIdx0 = 0;
    const int streamIdx1 = (numStreams > 1) ? 1 : -1;

    const int blocksCount = static_cast<int>(m_dataQueue.size());
    RHXDataBlock *first = m_dataQueue.front();
    const int samplesPerBlock = first->samplesPerDataBlock();
    const int totalSamples = blocksCount * samplesPerBlock;

    QVector<uint32_t> ts0;
    ts0.reserve(totalSamples);
    QVector<QVector<int>> ch0(m_channelsPerStream);
    for (int ch = 0; ch < m_channelsPerStream; ++ch) ch0[ch].reserve(totalSamples);

    QVector<uint32_t> ts1;
    QVector<QVector<int>> ch1;
    if (streamIdx1 >= 0) {
        ts1.reserve(totalSamples);
        ch1 = QVector<QVector<int>>(m_channelsPerStream);
        for (int ch = 0; ch < m_channelsPerStream; ++ch) ch1[ch].reserve(totalSamples);
    }

    while (!m_dataQueue.empty()) {
        RHXDataBlock *block = m_dataQueue.front();
        m_dataQueue.pop_front();

        for (int t = 0; t < samplesPerBlock; ++t) {
            uint32_t ts = block->timeStamp(t);
            ts0.append(ts);
            for (int ch = 0; ch < m_channelsPerStream; ++ch)
                ch0[ch].append(block->amplifierData(streamIdx0, ch, t));

            if (streamIdx1 >= 0) {
                ts1.append(ts);
                for (int ch = 0; ch < m_channelsPerStream; ++ch)
                    ch1[ch].append(block->amplifierData(streamIdx1, ch, t));
            }
        }

        if (m_isRecording) {
            writeBlockToRecording(block);
        }

        delete block;
    }

    emit newSamples(ts0, ch0);
    if (streamIdx1 >= 0) emit newSamplesStream2(ts1, ch1);
}

void AcquisitionEngine::pauseContinuousForStim()
{
    if (!m_deviceOpened) return;
    m_usbTimer.stop();
    emit logMessage("自适应刺激：仅暂停 USB 读取，板上采集不中断");
}

void AcquisitionEngine::resumeContinuousAfterStim()
{
    if (!m_deviceOpened) return;
    m_usbTimer.start();
    emit logMessage("自适应刺激：恢复 USB 读取");
}

void AcquisitionEngine::writeBlockToRecording(RHXDataBlock *block)
{
    if (!m_isRecording) return;
    if (!m_recordStream.is_open()) return;
    if (!block) return;

    int numStreams = m_rhxController->getNumEnabledDataStreams();
    block->write(m_recordStream, numStreams);
}

void AcquisitionEngine::writeBlockStream()
{
    m_rhxController->queueToFile(m_dataQueue, m_recordStream);
}

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

    if (m_rhxController->isRunning()) {
        emit logMessage("当前处于连续采集中");
    }

    ElectrodeParameters ele(electrodeName.toStdString());
    ele.SetStimulationTiming(0,
                             firstPhaseDuration_us,
                             secondPhaseDuration_us,
                             interPhaseDelay_us);
    ele.SetStimulationAmplitude(firstPhaseAmplitude,
                                secondPhaseAmplitude);
    ele.SetStimulationSource(triggerSource);
    ele.numOfPulses = numPulses;
    m_stimController->setStimSequenceParameters(&ele);

    emit logMessage(QStringLiteral("已配置刺激电极 %1").arg(electrodeName));
    m_rhxController->setStimCmdMode(true);
}

void AcquisitionEngine::triggerStim(int triggerSource, bool on)
{
    if (!m_deviceOpened || !m_stimController) return;

    m_rhxController->setStimCmdMode(true);
    m_stimController->stimTrigger(triggerSource, on);
}

void AcquisitionEngine::pulseStim(int triggerSource)
{
    if (!m_deviceOpened || !m_stimController) return;
    triggerStim(triggerSource, true);
    triggerStim(triggerSource, false);
}

void AcquisitionEngine::applyAdaptiveStim(const QString &electrodeName,
                                          int amplitude_uA,
                                          int numPulses,
                                          int triggerSource)
{
    if (!m_deviceOpened || !m_stimController) return;
    if (amplitude_uA <= 0 || numPulses <= 0) return;

    bool resumeAfter = m_continuousRunning;
    if (resumeAfter) {
        emit logMessage("自适应刺激：更新刺激参数");
        pauseContinuousForStim();
    }

    int firstDur_us   = 500;
    int secondDur_us  = 500;
    int interphase_us = 500;

    configureStim(electrodeName,
                  amplitude_uA,
                  amplitude_uA,
                  firstDur_us,
                  secondDur_us,
                  interphase_us,
                  numPulses,
                  triggerSource);

    qDebug() << "当前触发:" << triggerSource;
    pulseStim(triggerSource);

    emit logMessage(QStringLiteral("自适应刺激：%1, 幅度=%2 uA, 脉冲数=%3, trigger=%4")
                        .arg(electrodeName)
                        .arg(amplitude_uA)
                        .arg(numPulses)
                        .arg(triggerSource));

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

    if (m_isRecording) {
        stopBinaryRecording();
    }

    m_recordStream.open(filePath.toStdString(),
                        std::ios::binary | std::ios::out);

    if (!m_recordStream.is_open()) {
        emit errorOccurred("无法打开录制文件：" + filePath);
        return false;
    }

    m_isRecording = true;
    emit logMessage("开始二进制录制：" + filePath);
    return true;
}

void AcquisitionEngine::stopBinaryRecording()
{
    if (!m_isRecording) return;

    m_isRecording = false;

    if (m_recordStream.is_open()) {
        m_recordStream.close();
    }

    emit logMessage("二进制录制已停止");
}
