#include "acquisitionengine.h"

AcquisitionEngine::AcquisitionEngine(QObject *parent)
    : QObject(parent)
{
    // 方便调试，看一下 USB 定时读取
    m_usbTimer.setTimerType(Qt::PreciseTimer);
    m_usbTimer.setInterval(10); // tighter polling to reduce FIFO buildup
    connect(&m_usbTimer, &QTimer::timeout,
            this, &AcquisitionEngine::onUsbTimer);
}

bool AcquisitionEngine::waitForStop(int timeoutMs)
{
    if (!m_rhxController) return true;

    QElapsedTimer timer;
    timer.start();

    while (m_rhxController->isRunning()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(2);

        if (timer.elapsed() >= timeoutMs) {
            return !m_rhxController->isRunning();
        }
    }

    return true;
}

void AcquisitionEngine::setStimStepSize(StimStepSize stepSize)
{
    if (stepSize == StimStepSizeUnrecognized) {
        return;
    }

    m_stimStepSize = stepSize;
    if (m_stimController) {
        m_stimController->setStimStepSize(stepSize);
    }

    if (!m_deviceOpened || !m_rhxController) {
        return;
    }

    if (!m_rhxController->isRunning()) {
        applyStimStepSizeToHardware();
    }
}

void AcquisitionEngine::setClosedLoopStimPhaseUs(int phaseUs)
{
    m_closedLoopStimPhaseUs = qMax(1, phaseUs);
}

bool AcquisitionEngine::applyStimStepSizeToHardware()
{
    if (!m_deviceOpened || !m_rhxController) {
        return false;
    }
    if (m_rhxController->isRunning()) {
        return false;
    }

    RHXRegisters chipRegisters(m_rhxController->getType(),
                               m_rhxController->getSampleRate(),
                               m_stimStepSize);
    std::vector<unsigned int> commandList;

    int commandSequenceLength =
        chipRegisters.createCommandListRHSRegisterConfig(commandList, true);
    if (commandSequenceLength <= 0) {
        return false;
    }

    m_rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0);
    m_rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1, 0, commandSequenceLength - 1);

    chipRegisters.createCommandListDummy(commandList, 8192,
                                         chipRegisters.createRHXCommand(RHXRegisters::RHXCommandRegRead, 255));
    m_rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd2, 0);

    chipRegisters.createCommandListDummy(commandList, 8192,
                                         chipRegisters.createRHXCommand(RHXRegisters::RHXCommandRegRead, 254));
    m_rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd3, 0);

    chipRegisters.createCommandListDummy(commandList, 8192,
                                         chipRegisters.createRHXCommand(RHXRegisters::RHXCommandRegRead, 253));
    m_rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd4, 0);

    m_rhxController->enableAuxCommandsOnAllStreams();
    m_rhxController->setMaxTimeStep(RHXDataBlock::samplesPerDataBlock(m_rhxController->getType()));
    m_rhxController->setContinuousRunMode(false);
    m_rhxController->setStimCmdMode(false);
    m_rhxController->run();

    if (!waitForStop(1500)) {
        emit logMessage(QStringLiteral("刺激量程/步进写入硬件超时"));
        return false;
    }

    m_rhxController->flush();
    m_rhxController->setMaxTimeStep(0);
    m_appliedStimStepSize = m_stimStepSize;
    emit logMessage(QStringLiteral("刺激量程/步进已应用到硬件：%1")
                        .arg(StimStepSizeString[int(m_stimStepSize)]));
    return true;
}

void AcquisitionEngine::resetTimestampDiagnostics()
{
    m_tsDiagStream0 = TimestampDiagState();
    m_tsDiagStream1 = TimestampDiagState();

    if (m_tsDiagClock.isValid()) {
        m_tsDiagClock.restart();
    } else {
        m_tsDiagClock.start();
    }
}

void AcquisitionEngine::inspectTimestampBatch(const char *streamTag,
                                             const QVector<uint32_t> &timeStamps,
                                             TimestampDiagState &state,
                                             unsigned int fifoWords)
{
    if (timeStamps.isEmpty()) return;
    if (!m_tsDiagClock.isValid()) {
        m_tsDiagClock.start();
    }

    ++state.totalBatches;
    state.totalSamples += timeStamps.size();

    qint64 batchDuplicates = 0;
    qint64 batchBackwards = 0;
    qint64 batchGapSamples = 0;
    bool anomaly = false;
    QString anomalyText;

    const uint32_t first = timeStamps.front();
    const uint32_t last = timeStamps.back();

    auto appendIssue = [&](const QString &issue) {
        if (!anomalyText.isEmpty()) {
            anomalyText += QStringLiteral(" | ");
        }
        anomalyText += issue;
    };

    if (state.hasLastTimestamp) {
        const uint32_t expectedFirst = state.lastTimestamp + 1u;
        if (first != expectedFirst) {
            anomaly = true;
            if (first > expectedFirst) {
                const qint64 gap = qint64(first) - qint64(expectedFirst);
                batchGapSamples += gap;
                state.gapSamples += gap;
                appendIssue(QStringLiteral("prev-gap=%1").arg(gap));
            } else if (first == state.lastTimestamp) {
                ++batchDuplicates;
                ++state.duplicateEvents;
                appendIssue(QStringLiteral("prev-dup=1"));
            } else {
                ++batchBackwards;
                ++state.backwardEvents;
                appendIssue(QStringLiteral("prev-back expected=%1 got=%2")
                                .arg(expectedFirst)
                                .arg(first));
            }
        }
    }

    for (int i = 1; i < timeStamps.size(); ++i) {
        const uint32_t prev = timeStamps[i - 1];
        const uint32_t curr = timeStamps[i];
        if (curr == prev) {
            anomaly = true;
            ++batchDuplicates;
            ++state.duplicateEvents;
            continue;
        }
        if (curr > prev + 1u) {
            anomaly = true;
            const qint64 gap = qint64(curr) - qint64(prev) - 1;
            batchGapSamples += gap;
            state.gapSamples += gap;
            continue;
        }
        if (curr < prev) {
            anomaly = true;
            ++batchBackwards;
            ++state.backwardEvents;
        }
    }

    state.lastTimestamp = last;
    state.hasLastTimestamp = true;

    const qint64 nowMs = m_tsDiagClock.elapsed();
    const bool shouldReport = anomaly || state.lastReportMs < 0 || (nowMs - state.lastReportMs >= 1000);
    if (!shouldReport) {
        return;
    }
    state.lastReportMs = nowMs;

    QString message = QStringLiteral("[TS] %1 batches=%2 samples=%3 range=%4..%5 next=%6 fifoWords=%7 dup=%8 gap=%9 back=%10")
                          .arg(QString::fromLatin1(streamTag))
                          .arg(state.totalBatches)
                          .arg(state.totalSamples)
                          .arg(first)
                          .arg(last)
                          .arg(last + 1u)
                          .arg(fifoWords)
                          .arg(state.duplicateEvents)
                          .arg(state.gapSamples)
                          .arg(state.backwardEvents);

    if (anomaly) {
        message += QStringLiteral(" batchDup=%1 batchGap=%2 batchBack=%3 anomaly=%4")
                       .arg(batchDuplicates)
                       .arg(batchGapSamples)
                       .arg(batchBackwards)
                       .arg(anomalyText.isEmpty() ? QStringLiteral("internal") : anomalyText);
    }

    emit logMessage(message);
}
void AcquisitionEngine::enqueueBlockForRecording(RHXDataBlock *block)
{
    if (!block) return;
    size_t queueSize = 0;
    {
        std::lock_guard<std::mutex> lock(m_recordQueueMutex);
        m_recordQueue.push_back(block);
        queueSize = m_recordQueue.size();
    }
    if (queueSize > m_recordQueueHighWatermark) {
        m_recordQueueHighWatermark = queueSize;
    }
    const size_t warnStepBlocks = 256;
    if (queueSize >= warnStepBlocks) {
        const size_t warnLevel = queueSize / warnStepBlocks;
        if (warnLevel > m_recordQueueLastWarnLevel) {
            m_recordQueueLastWarnLevel = warnLevel;
            emit logMessage(QStringLiteral("录制写盘积压：待写数据块=%1，长时间持续增大可能导致内存占用过高")
                                .arg(queueSize));
        }
    }
    m_recordQueueCv.notify_one();
}

void AcquisitionEngine::recordingWorkerLoop()
{
    while (true) {
        RHXDataBlock *block = nullptr;
        {
            std::unique_lock<std::mutex> lock(m_recordQueueMutex);
            m_recordQueueCv.wait(lock, [&] {
                return m_recordWorkerStopRequested || !m_recordQueue.empty();
            });

            if (m_recordQueue.empty()) {
                if (m_recordWorkerStopRequested) {
                    break;
                }
                continue;
            }

            block = m_recordQueue.front();
            m_recordQueue.pop_front();
        }

        writeBlockToRecording(block);
        ++m_recordBlocksSinceFlush;
        if (m_recordStream.is_open() && (m_recordBlocksSinceFlush == 1 || (m_recordBlocksSinceFlush % 64) == 0)) {
            m_recordStream.flush();
        }
        delete block;
    }
}

void AcquisitionEngine::stopRecordingWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_recordQueueMutex);
        m_recordWorkerStopRequested = true;
    }
    m_recordQueueCv.notify_all();

    if (m_recordWorker.joinable()) {
        m_recordWorker.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_recordQueueMutex);
        while (!m_recordQueue.empty()) {
            delete m_recordQueue.front();
            m_recordQueue.pop_front();
        }
        m_recordWorkerStopRequested = false;
    }
}
void AcquisitionEngine::cleanup()
{
    m_usbTimer.stop();
    m_continuousRunning = false;
    m_isRecording = false;
    m_appliedStimStepSize = StimStepSizeUnrecognized;
    m_recordQueueHighWatermark = 0;
    m_recordQueueLastWarnLevel = 0;
    m_recordBlocksSinceFlush = 0;
    stopRecordingWorker();
    if (m_recordStream.is_open()) {
        m_recordStream.flush();
        m_recordStream.close();
    }
    m_recordNumStreams = 0;
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
    resetTimestampDiagnostics();
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
    m_stimController->setStimStepSize(m_stimStepSize);

    // 记录流和通道数
    m_numEnabledStreams =
        m_rhxController->getNumEnabledDataStreams();
    m_channelsPerStream =
        RHXDataBlock::channelsPerStream(m_rhxController->getType());

    m_deviceOpened = true;
    if (!applyStimStepSizeToHardware()) {
        emit logMessage(QStringLiteral("警告：设备打开后未能立即应用刺激量程/步进"));
    }
    resetTimestampDiagnostics();
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

    if (m_stimController) {
        m_stimController->setStimStepSize(m_stimStepSize);
    }
    if (hasPendingStimStepSizeApply() && !applyStimStepSizeToHardware()) {
        emit errorOccurred(QStringLiteral("无法应用刺激量程/步进，请重新打开设备后再试"));
        return;
    }

    // 采集模式：连续 + 刺激命令模式
    resetTimestampDiagnostics();
    m_rhxController->setContinuousRunMode(true);
    m_rhxController->setStimCmdMode(true);

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
    m_usbTimer.stop();
    m_continuousRunning = false;

    if (!m_deviceOpened || !m_rhxController) return;

    bool forcedReset = false;

    if (m_rhxController->isRunning()) {
        m_rhxController->setContinuousRunMode(false);
        if (!waitForStop(1500)) {
            forcedReset = true;
            emit logMessage(QStringLiteral("采集停止超时，正在强制复位硬件"));
        }
    }

    m_rhxController->setStimCmdMode(false);
    m_rhxController->setMaxTimeStep(0);
    m_rhxController->resetSequencers();

    if (forcedReset) {
        m_rhxController->resetBoard();
        m_rhxController->resetFpga();
        emit logMessage(QStringLiteral("采集已强制停止并复位硬件"));
    } else {
        m_rhxController->flush();
        emit logMessage(QStringLiteral("采集已停止"));
    }
}

void AcquisitionEngine::shutdownDevice()
{
    m_usbTimer.stop();
    stopBinaryRecording();

    if (m_rhxController && m_deviceOpened) {
        stopAcquisition();
        m_rhxController->setStimCmdMode(false);
        m_rhxController->setContinuousRunMode(false);
        m_rhxController->setMaxTimeStep(0);
        m_rhxController->resetSequencers();
        m_rhxController->resetBoard();
        m_rhxController->resetFpga();
    }

    cleanup();
}

void AcquisitionEngine::onUsbTimer()
{
    if (!m_deviceOpened || !m_rhxController) return;

    const int enabledStreams = m_rhxController->getNumEnabledDataStreams();
    const int wordsPerBlock = RHXDataBlock::dataBlockSizeInWords(m_rhxController->getType(), enabledStreams);
    if (wordsPerBlock <= 0) return;

    const unsigned int availableWords = m_rhxController->getNumWordsInFifo();
    const int availableBlocks = int(availableWords / unsigned(wordsPerBlock));
    if (availableBlocks <= 0) {
        if (!m_rhxController->isRunning()) {
            return;
        }
        return;
    }

    const int blocksToRead = qBound(1, availableBlocks, MaxNumBlocksToRead);
    const bool usbDataRead = m_rhxController->readDataBlocks(blocksToRead, m_dataQueue);

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

    const int blocksCount = (int)m_dataQueue.size();
    RHXDataBlock *first = m_dataQueue.front();
    const int samplesPerBlock = first->samplesPerDataBlock();
    const int totalSamples = blocksCount * samplesPerBlock;

    // ===== 聚合缓存：stream0 =====
    QVector<uint32_t> ts0; ts0.reserve(totalSamples);
    QVector<QVector<int>> ch0(m_channelsPerStream);
    for (int ch=0; ch<m_channelsPerStream; ++ch) ch0[ch].reserve(totalSamples);

    // ===== 聚合缓存：stream2（如果有） =====
    QVector<uint32_t> ts1;
    QVector<QVector<int>> ch1;
    if (streamIdx1 >= 0) {
        ts1.reserve(totalSamples);
        ch1 = QVector<QVector<int>>(m_channelsPerStream);
        for (int ch=0; ch<m_channelsPerStream; ++ch) ch1[ch].reserve(totalSamples);
    }

    while (!m_dataQueue.empty()) {
        RHXDataBlock *block = m_dataQueue.front();
        m_dataQueue.pop_front();

        for (int t=0; t<samplesPerBlock; ++t) {
            uint32_t ts = block->timeStamp(t);
            ts0.append(ts);
            for (int ch=0; ch<m_channelsPerStream; ++ch)
                ch0[ch].append(block->amplifierData(streamIdx0, ch, t));

            if (streamIdx1 >= 0) {
                ts1.append(ts);
                for (int ch=0; ch<m_channelsPerStream; ++ch)
                    ch1[ch].append(block->amplifierData(streamIdx1, ch, t));
            }
        }

        // 录制：建议这里用 writeBlockToRecording(block)（你现在 writeBlockStream() 逻辑其实不太对）
        if (m_isRecording) {
            enqueueBlockForRecording(block);
            block = nullptr;
        }

        if (block) {
            delete block;
        }
    }

    const unsigned int fifoWords = m_rhxController ? m_rhxController->getNumWordsInFifo() : 0;
    inspectTimestampBatch("S0", ts0, m_tsDiagStream0, fifoWords);
    if (streamIdx1 >= 0) {
        inspectTimestampBatch("S1", ts1, m_tsDiagStream1, fifoWords);
    }

    emit newSamples(ts0, ch0);
    if (streamIdx1 >= 0) emit newSamplesStream2(ts1, ch1);
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
    if (!m_recordStream.is_open()) return;
    if (!block) return;

    int numStreams = m_recordNumStreams;
    if (numStreams <= 0 && m_rhxController) {
        numStreams = m_rhxController->getNumEnabledDataStreams();
    }

    block->write(m_recordStream, numStreams);
}

void AcquisitionEngine::writeBlockStream()
{
    m_rhxController->queueToFile(m_dataQueue,m_recordStream);
}

// ====== 刺激相关接口 ======

void AcquisitionEngine::configureStim(const QString &electrodeName,
                                      int firstPhaseAmplitude_nA,
                                      int secondPhaseAmplitude_nA,
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

    const int refractoryPeriod_us = qMax(1000,
                                         firstPhaseDuration_us + secondPhaseDuration_us + interPhaseDelay_us);

    ele->SetStimulationTiming(0,
                              firstPhaseDuration_us,
                              secondPhaseDuration_us,
                              refractoryPeriod_us);
    ele->interphaseDelay = interPhaseDelay_us;
    ele->refractoryPeriod = refractoryPeriod_us;
    ele->SetStimulationAmplitude(firstPhaseAmplitude_nA,
                                 secondPhaseAmplitude_nA);
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
    m_stimController->stimTrigger(triggerSource, on);
}

void AcquisitionEngine::applyAdaptiveStim(const QString &electrodeName,
                                          int amplitude_uA,
                                          int numPulses,
                                          int triggerSource)
{
    applyReplayStim(electrodeName,
                    amplitude_uA,
                    numPulses,
                    triggerSource,
                    m_closedLoopStimPhaseUs);
    return;

    if (!m_deviceOpened || !m_stimController) return;
    if (amplitude_uA <= 0 || numPulses <= 0) return;

    // ===== 1）如果当前正在连续采集，先暂停 USB 读取 =====
    bool resumeAfter = m_continuousRunning;
    if (resumeAfter) {
        emit logMessage("自适应刺激：更新刺激参数…");
        pauseContinuousForStim();
    }

    // ===== 2）正式配置刺激并触发 =====
    const int phaseUs = qMax(1, m_closedLoopStimPhaseUs);
    const int firstDur_us   = phaseUs;
    const int secondDur_us  = phaseUs;
    const int interphase_us = 0;
    // Algorithm output is in uA. Low-level stimulation parameters are stored
    // in nA, and the closed-loop path keeps its experiment-specific /5 scaling.
    const int closedLoopAmplitude_nA = qMax(1, qRound(amplitude_uA * 1000.0 / 5.0));

    configureStim(electrodeName,
                  closedLoopAmplitude_nA,
                  closedLoopAmplitude_nA,
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

void AcquisitionEngine::applyReplayStim(const QString &electrodeName,
                                        int amplitude_uA,
                                        int numPulses,
                                        int triggerSource,
                                        int phaseUs)
{
    if (!m_deviceOpened || !m_stimController) return;
    if (amplitude_uA <= 0 || numPulses <= 0) return;

    const bool resumeAfter = m_continuousRunning;
    if (resumeAfter) {
        emit logMessage("回放/闭环刺激：更新刺激参数...");
        pauseContinuousForStim();
    }

    phaseUs = qMax(1, phaseUs);
    const int firstDur_us = phaseUs;
    const int secondDur_us = phaseUs;
    const int interphase_us = 0;
    const int replayAmplitude_nA = qMax(1, qRound(amplitude_uA * 1000.0 / 5.0));

    configureStim(electrodeName,
                  replayAmplitude_nA,
                  replayAmplitude_nA,
                  firstDur_us,
                  secondDur_us,
                  interphase_us,
                  numPulses,
                  triggerSource);

    qDebug() << "当前触发:" << triggerSource;
    m_stimController->stimTrigger(triggerSource, true);
    m_stimController->stimTrigger(triggerSource, false);

    emit logMessage(QStringLiteral("回放/闭环刺激：%1, 幅度=%2 uA, 脉冲数=%3, trigger=%4, phase=%5 us")
                        .arg(electrodeName)
                        .arg(amplitude_uA)
                        .arg(numPulses)
                        .arg(triggerSource)
                        .arg(phaseUs));

    if (resumeAfter) {
        resumeContinuousAfterStim();
    }
}

void AcquisitionEngine::applyFixedReplayStim(const QString &electrodeName,
                                             double amplitude_uA,
                                             int phase_us,
                                             int triggerSource)
{
    if (!m_deviceOpened || !m_stimController) return;
    if (amplitude_uA <= 0.0 || phase_us <= 0) return;

    const bool resumeAfter = m_continuousRunning;
    if (resumeAfter) {
        emit logMessage("固定刺激回放：更新刺激参数…");
        pauseContinuousForStim();
    }

    // Fixed replay UI uses uA, while configureStim() expects nA.
    const int amplitude_nA = qMax(1, qRound(amplitude_uA * 1000.0));

    configureStim(electrodeName,
                  amplitude_nA,
                  amplitude_nA,
                  phase_us,
                  phase_us,
                  0,
                  1,
                  triggerSource);

    m_stimController->stimTrigger(triggerSource, true);
    m_stimController->stimTrigger(triggerSource, false);

    emit logMessage(QStringLiteral("固定刺激回放：%1, 幅度=%2 uA, 相宽=%3 us, trigger=%4")
                        .arg(electrodeName)
                        .arg(amplitude_uA, 0, 'f', 3)
                        .arg(phase_us)
                        .arg(triggerSource));

    if (resumeAfter) {
        resumeContinuousAfterStim();
    }
}

// ====== 录制控制 ======

void AcquisitionEngine::applyFixedTrainStim(const QString &electrodeName,
                                            double amplitude_uA,
                                            int phase_us,
                                            double frequency_hz,
                                            int duration_ms,
                                            int triggerSource)
{
    if (!m_deviceOpened || !m_stimController) return;
    if (amplitude_uA <= 0.0 || phase_us <= 0 || frequency_hz <= 0.0 || duration_ms <= 0) return;

    const int period_us = qMax(1, qRound(1000000.0 / frequency_hz));
    const int stimActive_us = phase_us * 2;
    if (period_us <= stimActive_us) {
        emit errorOccurred(QStringLiteral("Fixed-stim frequency too high for the selected phase width: freq=%1 Hz, phase=%2 us")
                               .arg(frequency_hz, 0, 'f', 3)
                               .arg(phase_us));
        return;
    }

    const int pulses = qMax(1, qRound((duration_ms / 1000.0) * frequency_hz));
    const int refractoryPeriod_us = qMax(0, period_us - stimActive_us);
    // Fixed-train UI uses uA, while the low-level electrode parameters use nA.
    const int amplitude_nA = qMax(1, qRound(amplitude_uA * 1000.0));

    const bool resumeAfter = m_continuousRunning;
    if (resumeAfter) {
        emit logMessage("Fixed-stim train: updating stimulation parameters...");
        pauseContinuousForStim();
    }

    ElectrodeParameters *ele =
        new ElectrodeParameters(electrodeName.toStdString());

    ele->SetStimulationTiming(0,
                              phase_us,
                              phase_us,
                              refractoryPeriod_us);
    ele->interphaseDelay = 0;
    ele->refractoryPeriod = refractoryPeriod_us;
    ele->pulseOrTrain = (pulses > 1) ? 1 : 0;
    ele->pulseTrainPeriod = period_us;
    ele->numOfPulses = pulses;
    ele->SetStimulationAmplitude(amplitude_nA, amplitude_nA);
    ele->SetStimulationSource(triggerSource);

    m_stimController->setStimSequenceParameters(ele);
    m_rhxController->setStimCmdMode(true);

    m_stimController->stimTrigger(triggerSource, true);
    m_stimController->stimTrigger(triggerSource, false);

    emit logMessage(QStringLiteral("Fixed-stim train started: %1, amplitude=%2 uA, phase=%3 us, freq=%4 Hz, pulses=%5, trigger=%6")
                        .arg(electrodeName)
                        .arg(amplitude_uA, 0, 'f', 3)
                        .arg(phase_us)
                        .arg(frequency_hz, 0, 'f', 3)
                        .arg(pulses)
                        .arg(triggerSource));

    if (resumeAfter) {
        resumeContinuousAfterStim();
    }
}

void AcquisitionEngine::applyDualFixedTrainStim(const QString &electrodeNameA,
                                                double amplitudeA_uA,
                                                int phaseA_us,
                                                double frequencyA_hz,
                                                const QString &electrodeNameB,
                                                double amplitudeB_uA,
                                                int phaseB_us,
                                                double frequencyB_hz,
                                                int duration_ms,
                                                int triggerSource)
{
    if (!m_deviceOpened || !m_stimController) return;
    if (duration_ms <= 0 || triggerSource < 0) return;
    if (electrodeNameA.isEmpty() || electrodeNameB.isEmpty()) return;
    if (amplitudeA_uA <= 0.0 || phaseA_us <= 0 || frequencyA_hz <= 0.0) return;
    if (amplitudeB_uA <= 0.0 || phaseB_us <= 0 || frequencyB_hz <= 0.0) return;

    struct DualTrainSpec {
        QString electrodeName;
        double amplitude_uA = 0.0;
        int phase_us = 0;
        double frequency_hz = 0.0;
        int period_us = 0;
        int pulses = 0;
        int refractory_us = 0;
        int amplitude_nA = 0;
    };

    auto buildSpec = [duration_ms](const QString &electrodeName,
                                   double amplitude_uA,
                                   int phase_us,
                                   double frequency_hz,
                                   DualTrainSpec *out,
                                   QString *err) -> bool {
        if (!out) return false;
        const int period_us = qMax(1, qRound(1000000.0 / frequency_hz));
        const int stimActive_us = phase_us * 2;
        if (period_us <= stimActive_us) {
            if (err) {
                *err = QStringLiteral("%1: freq=%2 Hz, phase=%3 us")
                           .arg(electrodeName)
                           .arg(frequency_hz, 0, 'f', 3)
                           .arg(phase_us);
            }
            return false;
        }
        out->electrodeName = electrodeName;
        out->amplitude_uA = amplitude_uA;
        out->phase_us = phase_us;
        out->frequency_hz = frequency_hz;
        out->period_us = period_us;
        out->pulses = qMax(1, qRound((duration_ms / 1000.0) * frequency_hz));
        out->refractory_us = qMax(0, period_us - stimActive_us);
        out->amplitude_nA = qMax(1, qRound(amplitude_uA * 1000.0));
        return true;
    };

    DualTrainSpec specA;
    DualTrainSpec specB;
    QString invalidSpec;
    if (!buildSpec(electrodeNameA, amplitudeA_uA, phaseA_us, frequencyA_hz, &specA, &invalidSpec) ||
        !buildSpec(electrodeNameB, amplitudeB_uA, phaseB_us, frequencyB_hz, &specB, &invalidSpec)) {
        emit errorOccurred(QStringLiteral("Dual fixed-stim frequency too high for the selected phase width: %1")
                               .arg(invalidSpec));
        return;
    }

    const bool resumeAfter = m_continuousRunning;
    if (resumeAfter) {
        emit logMessage("Dual fixed-stim train: updating stimulation parameters...");
        pauseContinuousForStim();
    }

    auto configureOne = [this, triggerSource](const DualTrainSpec &spec) {
        ElectrodeParameters *ele =
            new ElectrodeParameters(spec.electrodeName.toStdString());

        ele->SetStimulationTiming(0,
                                  spec.phase_us,
                                  spec.phase_us,
                                  spec.refractory_us);
        ele->interphaseDelay = 0;
        ele->refractoryPeriod = spec.refractory_us;
        ele->pulseOrTrain = (spec.pulses > 1) ? 1 : 0;
        ele->pulseTrainPeriod = spec.period_us;
        ele->numOfPulses = spec.pulses;
        ele->SetStimulationAmplitude(spec.amplitude_nA, spec.amplitude_nA);
        ele->SetStimulationSource(triggerSource);
        m_stimController->setStimSequenceParameters(ele);
    };

    configureOne(specA);
    configureOne(specB);
    m_rhxController->setStimCmdMode(true);

    m_stimController->stimTrigger(triggerSource, true);
    m_stimController->stimTrigger(triggerSource, false);

    emit logMessage(QStringLiteral("Dual fixed-stim train started: A=%1 (%2 uA, %3 us, %4 Hz, pulses=%5), B=%6 (%7 uA, %8 us, %9 Hz, pulses=%10), trigger=%11")
                        .arg(specA.electrodeName)
                        .arg(specA.amplitude_uA, 0, 'f', 3)
                        .arg(specA.phase_us)
                        .arg(specA.frequency_hz, 0, 'f', 3)
                        .arg(specA.pulses)
                        .arg(specB.electrodeName)
                        .arg(specB.amplitude_uA, 0, 'f', 3)
                        .arg(specB.phase_us)
                        .arg(specB.frequency_hz, 0, 'f', 3)
                        .arg(specB.pulses)
                        .arg(triggerSource));

    if (resumeAfter) {
        resumeContinuousAfterStim();
    }
}

bool AcquisitionEngine::startBinaryRecording(const QString &filePath)
{
    if (!m_deviceOpened) {
        emit errorOccurred("未打开设备，无法开始录制");
        return false;
    }

    if (m_isRecording) {
        stopBinaryRecording();
    }

    stopRecordingWorker();

    m_recordStream.open(filePath.toStdString(),
                        std::ios::binary | std::ios::out | std::ios::trunc);

    if (!m_recordStream.is_open()) {
        emit errorOccurred(QStringLiteral("无法打开录制文件：") + filePath);
        return false;
    }

    m_recordNumStreams = m_rhxController->getNumEnabledDataStreams();
    m_recordQueueHighWatermark = 0;
    m_recordQueueLastWarnLevel = 0;
    m_recordBlocksSinceFlush = 0;
    m_recordWorker = std::thread(&AcquisitionEngine::recordingWorkerLoop, this);
    m_isRecording = true;
    emit logMessage(QStringLiteral("开始录制到文件：") + filePath);
    return true;
}

void AcquisitionEngine::stopBinaryRecording()
{
    if (!m_isRecording && !m_recordWorker.joinable())
        return;

    m_isRecording = false;
    stopRecordingWorker();
    m_recordNumStreams = 0;

    if (m_recordStream.is_open()) {
        m_recordStream.flush();
        m_recordStream.close();
    }

    emit logMessage(QStringLiteral("录制队列峰值=%1 blocks").arg(m_recordQueueHighWatermark));
    emit logMessage("录制已停止");
}
