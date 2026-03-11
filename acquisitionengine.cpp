#include "acquisitionengine.h"

AcquisitionEngine::AcquisitionEngine(QObject *parent)
    : QObject(parent)
{
    // 鏂逛究璋冭瘯锛岀湅涓€涓?USB 瀹氭椂璇诲彇
    m_usbTimer.setInterval(30); // ~33ms 鈮?30 Hz
    connect(&m_usbTimer, &QTimer::timeout,
            this, &AcquisitionEngine::onUsbTimer);
}

void AcquisitionEngine::cleanup()
{
    // 娓呯┖闃熷垪閲岀殑 RHXDataBlock
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
    cleanup(); // 纭繚骞插噣

    // 1. 鍒涘缓 RHXController锛堜綘 main 閲岀殑绗竴鍙ワ級
    m_rhxController = new RHXController(ControllerStimRecord,
                                        SampleRate30000Hz);

    // 2. 鎵撳紑绗竴涓澶?
    std::vector<std::string> availableDevices =
        m_rhxController->listAvailableDeviceSerials();
    if (availableDevices.empty()) {
        emit errorOccurred("鏈壘鍒颁换浣?Opal Kelly XEM7310 璁惧");
        cleanup();
        return false;
    }

    m_rhxController->open(availableDevices[0]);

    // 3. 鍔犺浇 bitfile 骞跺垵濮嬪寲
    m_rhxController->uploadFPGABitfile(bitfilePath.toStdString());
    m_rhxController->initialize();

    // 榛樿鍏堝紑 stream 0锛屼綘涔嬪墠涔熸墦寮€浜?2锛岃繖閲屽彲浠ヤ繚鐣?
    m_rhxController->enableDataStream(0, true);
    m_rhxController->enableDataStream(2, true);

    // 璁剧疆 MISO 閲囨牱寤惰繜锛氬亣璁?3 鑻卞昂绾跨紗
    m_rhxController->setCableLengthFeet(PortA, 3.0);
    m_rhxController->setCableLengthFeet(PortB, 3.0);

    // 浜竴涓?LED 琛ㄧず绋嬪簭鍦ㄨ窇
    int ledArray[8] = {1,0,0,0,0,0,0,0};
    m_rhxController->setLedDisplay(ledArray);

    // 鍒涘缓鍒烘縺鎺у埗鍣紙瀹屽叏鐓?main锛?
    m_stimController = new Controller(m_rhxController);

    // 璁板綍娴佸拰閫氶亾鏁?
    m_numEnabledStreams =
        m_rhxController->getNumEnabledDataStreams();
    m_channelsPerStream =
        RHXDataBlock::channelsPerStream(m_rhxController->getType());

    m_deviceOpened = true;
    emit logMessage("璁惧鎵撳紑骞跺垵濮嬪寲鎴愬姛");
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
        emit errorOccurred("璇峰厛鎵撳紑璁惧");
        return;
    }

    // 閲囬泦妯″紡锛氳繛缁?
    m_rhxController->setContinuousRunMode(true);
    m_rhxController->setStimCmdMode(true);

    // 寮€濮?SPI 閲囬泦锛堝悓鏃跺彲浠ユ敹鏁?+ 鍒烘縺锛?

    m_rhxController->run();

    // 鍚姩 USB 杞瀹氭椂鍣?
    m_usbTimer.start();

    // 杩炵画閲囬泦妯″紡寮€
    m_continuousRunning = true;

    emit logMessage("杩炵画閲囬泦宸插惎鍔紙鍏佽鍙戦€佸埡婵€锛?);
}

void AcquisitionEngine::stopAcquisition()
{
    if (!m_deviceOpened) return;

    // 鍋滄 USB 瀹氭椂鍣?
    m_usbTimer.stop();
    m_continuousRunning = false;

    // 鍋滄 SPI 杩炵画閲囬泦
    if (m_rhxController->isRunning()) {
        m_rhxController->setContinuousRunMode(false);
        // 绛変竴娆?run 鑷繁鏀跺熬缁撴潫
        while (m_rhxController->isRunning()) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
    }

    // 娓?FIFO
    m_rhxController->flush();
    m_continuousRunning = false;   // 猸?鍏抽敭

    emit logMessage("閲囬泦宸插仠姝?);
}

void AcquisitionEngine::onUsbTimer()
{
    if (!m_deviceOpened) return;

    // 瀹樻柟鏂囨。鏈変竴涓?blocksFor30Hz(sampleRate) 鐨勫伐鍏峰嚱鏁?
    int blocksToRead = RHXDataBlock::blocksFor30Hz(
        SampleRate30000Hz);

    // 杩欓噷浣犲師鏉ュ啓姝?16锛屼篃鍙互鎹㈡垚 blocksToRead
    bool usbDataRead =
        m_rhxController->readDataBlocks(blocksToRead, m_dataQueue);

    if (!usbDataRead && !m_rhxController->isRunning()) {
        // 娌℃湁鏇村鏁版嵁锛屽彲鑳借鍋滄浜?
        return;
    }

    // 澶勭悊闃熷垪涓墍鏈?block
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

    // ===== 鑱氬悎缂撳瓨锛歴tream0 =====
    QVector<uint32_t> ts0; ts0.reserve(totalSamples);
    QVector<QVector<int>> ch0(m_channelsPerStream);
    for (int ch=0; ch<m_channelsPerStream; ++ch) ch0[ch].reserve(totalSamples);

    // ===== 鑱氬悎缂撳瓨锛歴tream2锛堝鏋滄湁锛?=====
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

        // 褰曞埗锛氬缓璁繖閲岀敤 writeBlockToRecording(block)锛堜綘鐜板湪 writeBlockStream() 閫昏緫鍏跺疄涓嶅お瀵癸級
        if (m_isRecording) {
            //writeBlockStream();
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
    // 鍙仠 USB 瀹氭椂鍣紝涓嶅仠鏉垮瓙
    m_usbTimer.stop();
    emit logMessage("鑷€傚簲鍒烘縺锛氫粎鏆傚仠 USB 璇诲彇锛屾澘涓婇噰闆嗕笉鍋?);
}

void AcquisitionEngine::resumeContinuousAfterStim()
{
    if (!m_deviceOpened) return;
    m_usbTimer.start();
    emit logMessage("鑷€傚簲鍒烘縺锛氭仮澶?USB 璇诲彇");
}

// 猸?鍏抽敭锛氫娇鐢?RHXDataBlock::write() 鍐欏師鐢熶簩杩涘埗鏍煎紡
void AcquisitionEngine::writeBlockToRecording(RHXDataBlock *block)
{
    if (!m_isRecording) return;
    if (!m_recordStream.is_open()) return;
    if (!block) return;

    int numStreams = m_rhxController->getNumEnabledDataStreams();

    // 杩欏拰绀轰緥閲岀殑 queueToFile 鍦ㄥ簳灞傛槸涓€鑷寸殑锛氭寜 Intan 瀹氫箟鏍煎紡鍐欎竴涓?USB data block
    block->write(m_recordStream, numStreams);
}

void AcquisitionEngine::writeBlockStream()
{
    m_rhxController->queueToFile(m_dataQueue,m_recordStream);
}

// ====== 鍒烘縺鐩稿叧鎺ュ彛 ======

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

    // 濡傛灉鏉垮瓙姝ゅ埢姝ｅ湪 continuous run锛岃繖閲屽彧鏄彁绀猴紝涓嶅己鍒跺仠
    if (m_rhxController->isRunning()) {
        emit logMessage("褰撳墠鍦ㄨ繛缁噰闆嗕腑銆?);
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

    emit logMessage(QStringLiteral("宸查厤缃埡婵€鐢垫瀬 %1").arg(electrodeName));
    m_rhxController->setStimCmdMode(true);
}

void AcquisitionEngine::triggerStim(int triggerSource, bool on)
{
    if (!m_deviceOpened || !m_stimController) return;

    // 鐩存帴鐢ㄤ綘宸叉湁鐨勬帴鍙ｏ紙娉ㄦ剰杩欓噷浣犲師鏉ユ槸 triggerSource-24锛岀湅浣犳暣浣撳伐绋嬫€庝箞瀹氫箟锛?
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

    // ===== 1锛夊鏋滃綋鍓嶆鍦ㄨ繛缁噰闆嗭紝鍏堟殏鍋?USB 璇诲彇 =====
    bool resumeAfter = m_continuousRunning;
    if (resumeAfter) {
        emit logMessage("鑷€傚簲鍒烘縺锛氭洿鏂板埡婵€鍙傛暟鈥?);
        pauseContinuousForStim();
    }

    // ===== 2锛夋寮忛厤缃埡婵€骞惰Е鍙?=====
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

    // 瑙﹀彂涓€娆″埡婵€
    qDebug() << "褰撳墠瑙﹀彂:" << triggerSource;
    pulseStim(triggerSource);

    emit logMessage(QStringLiteral("鑷€傚簲鍒烘縺锛?1, 骞呭害=%2 uA, 鑴夊啿鏁?%3, trigger=%4")
                        .arg(electrodeName)
                        .arg(amplitude_uA)
                        .arg(numPulses)
                        .arg(triggerSource));

    // ===== 3锛夊鏋滃垰鎵嶆槸杩炵画閲囬泦锛屽氨鑷姩鎭㈠ =====
    if (resumeAfter) {
        resumeContinuousAfterStim();
    }
}

// ====== 褰曞埗鎺у埗 ======

bool AcquisitionEngine::startBinaryRecording(const QString &filePath)
{
    if (!m_deviceOpened) {
        emit errorOccurred("璇峰厛鎵撳紑璁惧鍐嶅紑濮嬪綍鍒?);
        return false;
    }

    // 濡傛灉涔嬪墠宸茬粡鍦ㄥ綍锛屽厛鍏虫帀
    if (m_isRecording) {
        stopBinaryRecording();
    }

    // 猸?鎸夌ず渚?main.cpp 鐨勬柟寮忔墦寮€浜岃繘鍒舵枃浠?
    m_recordStream.open(filePath.toStdString(),
                        std::ios::binary | std::ios::out);

    if (!m_recordStream.is_open()) {
        emit errorOccurred("鏃犳硶鎵撳紑褰曞埗鏂囦欢锛? + filePath);
        return false;
    }

    // 涓嶅啓浠讳綍鑷畾涔夋枃浠跺ご锛岀洿鎺ュ啓 Intan 鍘熺敓鏁版嵁鍧?
    m_isRecording = true;
    emit logMessage("寮€濮嬩簩杩涘埗褰曞埗锛? + filePath);
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

    emit logMessage("浜岃繘鍒跺綍鍒跺凡鍋滄");
}

