#include "mainwindow.h"

#include <QFileDialog>
#include <QDateTime>
#include <QDir>

static StackedWaveWidget::FilterSettings buildFilterSettingsFromUi(
    QCheckBox *chkFilter, QComboBox *cmbType,
    QDoubleSpinBox *bp1, QDoubleSpinBox *bp2,
    QDoubleSpinBox *lp, QDoubleSpinBox *hp,
    QCheckBox *chkNotch, QComboBox *cmbNotchHz, QDoubleSpinBox *notchQ)
{
    StackedWaveWidget::FilterSettings s;

    s.enabled = chkFilter->isChecked();

    const QString t = cmbType->currentText();
    if (t == "Off") s.type = StackedWaveWidget::FilterSettings::Type::Off;
    else if (t == "LowPass") s.type = StackedWaveWidget::FilterSettings::Type::LowPass;
    else if (t == "HighPass") s.type = StackedWaveWidget::FilterSettings::Type::HighPass;
    else s.type = StackedWaveWidget::FilterSettings::Type::BandPass;

    s.bp_low_hz  = bp1->value();
    s.bp_high_hz = bp2->value();
    s.lp_hz      = lp->value();
    s.hp_hz      = hp->value();

    s.notchEnabled = chkNotch->isChecked();
    s.notch_hz = (cmbNotchHz->currentText() == "60") ? 60.0 : 50.0;
    s.notchQ   = notchQ->value();
    return s;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();

    // ===== core =====
    m_engine     = new AcquisitionEngine(this);
    m_experiment = new ExperimentControllerAB(m_engine, this);
    m_abAlgo     = new ABAlgorithm(this);

    if (m_abAlgo) {
        m_abAlgo->setSampleRateHz(m_sampleRate);
    }

    // ===== wave feed =====
    connect(m_engine, &AcquisitionEngine::newSamples,
            m_viewA,   &StackedWaveWidget::pushBlock);
    connect(m_engine, &AcquisitionEngine::newSamplesStream2,
            m_viewB,   &StackedWaveWidget::pushBlock);

    connect(m_engine, &AcquisitionEngine::errorOccurred,
            this,     &MainWindow::handleError);
    connect(m_engine, &AcquisitionEngine::logMessage,
            this,     &MainWindow::handleLog);

    connect(m_experiment, &ExperimentControllerAB::logMessage,
            this,         &MainWindow::handleLog);

    connect(m_experiment, &ExperimentControllerAB::epochReady,
            this,         &MainWindow::onABEpochReady);

    // ===== timeline + stim csv =====
    m_timeline = new StimTimelineOverlay();
    m_timeline->show();

    m_stimLog = new StimLogWriter(this);
    m_stimLog->start(QDir::currentPath() + "/stim_log.csv");

    // ===== FFT windows =====
    m_fftA = new FftWindow(m_viewA);
    m_fftB = new FftWindow(m_viewB);
    m_fftA->setFftSize(2048);
    m_fftB->setFftSize(2048);
    m_fftA->setMaxFreq(5000);
    m_fftB->setMaxFreq(5000);
    m_fftA->hide();
    m_fftB->hide();

    ensureTimelineVisible();
    applyDspSettings();
}

MainWindow::~MainWindow()
{
    if (m_experiment) m_experiment->stop();
    if (m_engine)     m_engine->stopAcquisition();

    if (m_stimLog) m_stimLog->stop();

    if (m_fftA) m_fftA->close();
    if (m_fftB) m_fftB->close();
    if (m_timeline) m_timeline->close();
}

void MainWindow::setupUi()
{
    m_central = new QWidget(this);
    m_layout  = new QVBoxLayout(m_central);

    // ===== 顶部按钮行 =====
    {
        QHBoxLayout *row = new QHBoxLayout();

        m_btnOpen     = new QPushButton(tr("打开设备"), this);
        m_btnStart    = new QPushButton(tr("开始采集"), this);
        m_btnStop     = new QPushButton(tr("停止采集"), this);
        m_btnStimOnce = new QPushButton(tr("发一次刺激 (trigger=0)"), this);
        m_btnRecStart = new QPushButton(tr("开始录制(bin)"), this);
        m_btnRecStop  = new QPushButton(tr("停止录制"), this);

        row->addWidget(m_btnOpen);
        row->addWidget(m_btnStart);
        row->addWidget(m_btnStop);
        row->addWidget(m_btnStimOnce);
        row->addWidget(m_btnRecStart);
        row->addWidget(m_btnRecStop);

        row->addSpacing(14);
        row->addWidget(new QLabel(tr("Epoch:"), this));

        m_spinEpochSec = new QDoubleSpinBox(this);
        m_spinEpochSec->setRange(0.1, 3600.0);
        m_spinEpochSec->setDecimals(2);
        m_spinEpochSec->setSingleStep(0.5);
        m_spinEpochSec->setSuffix(" s");
        m_spinEpochSec->setValue(colletion_time);
        row->addWidget(m_spinEpochSec);

        row->addStretch(1);
        m_layout->addLayout(row);
    }

    // ===== 显示范围（±uV）行 =====
    {
        QHBoxLayout *row = new QHBoxLayout();

        row->addWidget(new QLabel(tr("Stream0 显示范围(±uV):"), this));
        m_spinGainA = new QDoubleSpinBox(this);
        m_spinGainA->setRange(10.0, 200000.0);
        m_spinGainA->setDecimals(0);
        m_spinGainA->setSingleStep(100.0);
        m_spinGainA->setValue(500.0);
        m_spinGainA->setSuffix(" µV");
        row->addWidget(m_spinGainA);

        row->addSpacing(20);

        row->addWidget(new QLabel(tr("Stream2 显示范围(±uV):"), this));
        m_spinGainB = new QDoubleSpinBox(this);
        m_spinGainB->setRange(10.0, 200000.0);
        m_spinGainB->setDecimals(0);
        m_spinGainB->setSingleStep(100.0);
        m_spinGainB->setValue(500.0);
        m_spinGainB->setSuffix(" µV");
        row->addWidget(m_spinGainB);

        row->addStretch(1);
        m_layout->addLayout(row);
    }

    // ===== DSP/FFT 控件行 =====
    {
        QHBoxLayout *row = new QHBoxLayout();

        m_chkFilter = new QCheckBox(tr("Enable Filter"), this);
        m_chkFilter->setChecked(false);

        m_cmbFilterType = new QComboBox(this);
        m_cmbFilterType->addItems({"Off","BandPass","LowPass","HighPass"});
        m_cmbFilterType->setCurrentText("BandPass");

        m_spBP1 = new QDoubleSpinBox(this);
        m_spBP1->setRange(0.1, 20000.0);
        m_spBP1->setDecimals(1);
        m_spBP1->setValue(300.0);
        m_spBP1->setSuffix(" Hz");

        m_spBP2 = new QDoubleSpinBox(this);
        m_spBP2->setRange(0.1, 20000.0);
        m_spBP2->setDecimals(1);
        m_spBP2->setValue(3000.0);
        m_spBP2->setSuffix(" Hz");

        m_spLP = new QDoubleSpinBox(this);
        m_spLP->setRange(0.1, 20000.0);
        m_spLP->setDecimals(1);
        m_spLP->setValue(3000.0);
        m_spLP->setSuffix(" Hz");

        m_spHP = new QDoubleSpinBox(this);
        m_spHP->setRange(0.1, 20000.0);
        m_spHP->setDecimals(1);
        m_spHP->setValue(300.0);
        m_spHP->setSuffix(" Hz");

        m_chkNotch = new QCheckBox(tr("Notch"), this);
        m_chkNotch->setChecked(false);

        m_cmbNotchHz = new QComboBox(this);
        m_cmbNotchHz->addItems({"50Hz","60Hz"});
        m_cmbNotchHz->setCurrentText("50Hz");

        m_spNotchQ = new QDoubleSpinBox(this);
        m_spNotchQ->setRange(5.0, 200.0);
        m_spNotchQ->setDecimals(0);
        m_spNotchQ->setValue(30.0);

        m_btnFFT = new QPushButton(tr("FFT (A/B)"), this);

        row->addWidget(m_chkFilter);
        row->addWidget(m_cmbFilterType);

        row->addWidget(new QLabel(tr("BP:"), this));
        row->addWidget(m_spBP1);
        row->addWidget(m_spBP2);

        row->addSpacing(10);
        row->addWidget(new QLabel(tr("LP:"), this));
        row->addWidget(m_spLP);

        row->addSpacing(10);
        row->addWidget(new QLabel(tr("HP:"), this));
        row->addWidget(m_spHP);

        row->addSpacing(10);
        row->addWidget(m_chkNotch);
        row->addWidget(m_cmbNotchHz);
        row->addWidget(new QLabel(tr("Q"), this));
        row->addWidget(m_spNotchQ);

        row->addStretch(1);
        row->addWidget(m_btnFFT);

        m_layout->addLayout(row);
    }

    // ===== 左右分屏：两个 stream =====
    m_split = new QSplitter(Qt::Horizontal, this);

    m_viewA = new StackedWaveWidget(this);
    m_viewB = new StackedWaveWidget(this);

    // configure(ch, fs, initialWinSec, maxWinSec)
    m_viewA->configure(m_channelsPerStream, m_sampleRate, m_visibleWindowSec, m_maxWindowSec);
    m_viewB->configure(m_channelsPerStream, m_sampleRate, m_visibleWindowSec, m_maxWindowSec);

    m_viewA->setTitle("Stream0 (A) - 16ch  (dblclick=single, wheel=gain, Ctrl+wheel=time, Shift+drag=pan)");
    m_viewB->setTitle("Stream2 (B) - 16ch  (dblclick=single, wheel=gain, Ctrl+wheel=time, Shift+drag=pan)");

    m_viewA->setGainUv(m_spinGainA->value());
    m_viewB->setGainUv(m_spinGainB->value());

    m_split->addWidget(m_viewA);
    m_split->addWidget(m_viewB);
    m_split->setStretchFactor(0, 1);
    m_split->setStretchFactor(1, 1);

    m_layout->addWidget(m_split, 3);

    // ===== 日志 =====
    m_logView  = new QPlainTextEdit(this);
    m_logView->setReadOnly(true);
    m_layout->addWidget(m_logView, 1);

    setCentralWidget(m_central);

    // ===== connections =====
    connect(m_btnOpen,     &QPushButton::clicked, this, &MainWindow::onOpenDevice);
    connect(m_btnStart,    &QPushButton::clicked, this, &MainWindow::onStart);
    connect(m_btnStop,     &QPushButton::clicked, this, &MainWindow::onStop);
    connect(m_btnStimOnce, &QPushButton::clicked, this, &MainWindow::onStimOnce);
    connect(m_btnRecStart, &QPushButton::clicked, this, &MainWindow::onRecStart);
    connect(m_btnRecStop,  &QPushButton::clicked, this, &MainWindow::onRecStop);

    connect(m_spinEpochSec, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::onEpochDurationChanged);

    connect(m_spinGainA, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::onGainAChanged);
    connect(m_spinGainB, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::onGainBChanged);

    // DSP apply on any change
    connect(m_chkFilter, &QCheckBox::toggled, this, &MainWindow::applyDspSettings);
    connect(m_chkNotch,  &QCheckBox::toggled, this, &MainWindow::applyDspSettings);
    connect(m_cmbFilterType, &QComboBox::currentTextChanged, this, &MainWindow::applyDspSettings);
    connect(m_cmbNotchHz, &QComboBox::currentTextChanged, this, &MainWindow::applyDspSettings);

    connect(m_spBP1, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &MainWindow::applyDspSettings);
    connect(m_spBP2, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &MainWindow::applyDspSettings);
    connect(m_spLP,  QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &MainWindow::applyDspSettings);
    connect(m_spHP,  QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &MainWindow::applyDspSettings);
    connect(m_spNotchQ, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &MainWindow::applyDspSettings);

    connect(m_btnFFT, &QPushButton::clicked, this, &MainWindow::onToggleFftWindows);
}

void MainWindow::appendLog(const QString &msg)
{
    const QString line = QDateTime::currentDateTime().toString("hh:mm:ss.zzz  ") + msg;
    m_logView->appendPlainText(line);
}

void MainWindow::ensureTimelineVisible()
{
    if (!m_timeline) return;
    const QRect g = this->geometry();
    m_timeline->move(g.topRight() + QPoint(20, 40));
    m_timeline->raise();
}

void MainWindow::ensureFftVisible()
{
    const QRect g = this->geometry();
    if (m_fftA) m_fftA->move(g.topLeft() + QPoint(20, 80));
    if (m_fftB) m_fftB->move(g.topLeft() + QPoint(20, 380));
    if (m_fftA) m_fftA->raise();
    if (m_fftB) m_fftB->raise();
}

QVector<int> MainWindow::meanSelectedChannels(const QVector<QVector<int> > &channelData, const QVector<int> &sel)
{
    if (channelData.isEmpty() || sel.isEmpty()) return {};

    // 以第0通道长度作为基准（你这套采集一般各通道等长）
    const int N = channelData[0].size();
    if (N <= 0) return {};

    QVector<int> out;
    out.resize(N);

    for (int i = 0; i < N; ++i) {
        long long sum = 0;
        int used = 0;
        for (int ch : sel) {
            if (ch < 0 || ch >= channelData.size()) continue;
            const auto &v = channelData[ch];
            if (i < 0 || i >= v.size()) continue;
            sum += (long long)v[i];
            used++;
        }
        if (used == 0) return {}; // sel 全无效就直接失败
        out[i] = (int)std::llround((double)sum / (double)used);
    }
    return out;
}

void MainWindow::applyDspSettings()
{
    if (!m_viewA || !m_viewB) return;

    auto s = buildFilterSettingsFromUi(
        m_chkFilter, m_cmbFilterType,
        m_spBP1, m_spBP2, m_spLP, m_spHP,
        m_chkNotch, m_cmbNotchHz, m_spNotchQ
        );

    m_viewA->setFilterSettings(s);
    m_viewB->setFilterSettings(s);

    // enable/disable fields for clarity
    const QString t = m_cmbFilterType->currentText();
    const bool en = m_chkFilter->isChecked() && (t != "Off");
    m_spBP1->setEnabled(en && t == "BandPass");
    m_spBP2->setEnabled(en && t == "BandPass");
    m_spLP->setEnabled(en && t == "LowPass");
    m_spHP->setEnabled(en && t == "HighPass");

    const bool enNotch = m_chkNotch->isChecked();
    m_cmbNotchHz->setEnabled(enNotch);
    m_spNotchQ->setEnabled(enNotch);
}

void MainWindow::onToggleFftWindows()
{
    if (!m_fftA || !m_fftB || !m_viewA || !m_viewB) return;

    // 跟随当前选中通道
    m_fftA->setChannel(m_viewA->selectedChannel());
    m_fftB->setChannel(m_viewB->selectedChannel());

    const bool show = !m_fftA->isVisible();
    if (show) {
        m_fftA->show();
        m_fftB->show();
        ensureFftVisible();
    } else {
        m_fftA->hide();
        m_fftB->hide();
    }
}

void MainWindow::onOpenDevice()
{
    QString path = QFileDialog::getOpenFileName(
        this,
        tr("选择 ConfigRHSController_7310.bit"),
        QString(),
        tr("Bitfile (*.bit);;All Files (*.*)")
        );
    if (path.isEmpty()) return;

    if (!m_engine->openDevice(path)) appendLog("打开设备失败");
    else appendLog("打开设备成功");
}

void MainWindow::onStart()
{
    if (!m_engine) return;

    m_engine->startContinuousAcquisition();

    if (m_experiment) {
        m_experiment->setEpochDuration(colletion_time);
        m_experiment->start();
    }

    ensureTimelineVisible();
    appendLog(QString("开始采集 + AB epoch=%1 s").arg(colletion_time, 0, 'f', 2));
}

void MainWindow::onStop()
{
    if (m_experiment) m_experiment->stop();
    if (m_engine)     m_engine->stopAcquisition();
    appendLog("已停止采集");
}

void MainWindow::onRecStart()
{
    if (!m_engine) return;

    QString file = QFileDialog::getSaveFileName(
        this,
        tr("选择录制文件保存路径"),
        QDir::currentPath() + "/recording.bar",
        tr("Binary Recording (*.bar);;All Files (*.*)")
        );
    if (file.isEmpty()) return;

    if (m_engine->startBinaryRecording(file)) {
        appendLog("开始录制到文件：" + file);
        if (m_stimLog) {
            m_stimLog->start(file + ".stim.csv");
            appendLog("Stim log 写入：" + file + ".stim.csv");
        }
    } else {
        appendLog("开始录制失败");
    }
}

void MainWindow::onRecStop()
{
    if (!m_engine) return;
    m_engine->stopBinaryRecording();
    appendLog("已停止录制");

    if (m_stimLog) {
        m_stimLog->stop();
        m_stimLog->start(QDir::currentPath() + "/stim_log.csv");
        appendLog("Stim log 切回默认：stim_log.csv");
    }
}

void MainWindow::onStimOnce()
{
    if (!m_engine) return;
    int triggerSource = 0;
    m_engine->triggerStim(triggerSource, true);
    appendLog("已触发一次刺激：trigger=0");
}

void MainWindow::onEpochDurationChanged(double sec)
{
    colletion_time = sec;
    if (m_experiment) m_experiment->setEpochDuration(colletion_time);
    appendLog(QString("Epoch 时长设置为 %1 s").arg(colletion_time, 0, 'f', 2));
}

void MainWindow::onGainAChanged(double halfRangeUv)
{
    if (m_viewA) m_viewA->setGainUv(halfRangeUv);
}

void MainWindow::onGainBChanged(double halfRangeUv)
{
    if (m_viewB) m_viewB->setGainUv(halfRangeUv);
}

void MainWindow::onABEpochReady(int phaseIndex,
                                const QVector<uint32_t> &timeStamps,
                                const QVector<QVector<int>> &channelData)
{
    const QString phaseName = (phaseIndex == 0) ? "PhaseA (A端 stream0)" : "PhaseB (B端 stream2)";
    if (!m_abAlgo || !m_engine) return;
    if (timeStamps.isEmpty() || channelData.isEmpty()) return;

    // ===== 1) 先按通道列表把 16ch -> 2ch（a平均/b平均）=====
    const QVector<int> &sel_a = (phaseIndex == 0) ? kSense_A_a : kSense_B_a;
    const QVector<int> &sel_b = (phaseIndex == 0) ? kSense_A_b : kSense_B_b;

    QVector<int> avg_a = meanSelectedChannels(channelData, sel_a);
    QVector<int> avg_b = meanSelectedChannels(channelData, sel_b);

    if (avg_a.isEmpty() || avg_b.isEmpty() ||
        avg_a.size() != timeStamps.size() || avg_b.size() != timeStamps.size()) {
        appendLog(phaseName + ": 平均信号生成失败（通道列表无效或长度不匹配）");
        return;
    }

    QVector<QVector<int>> avgChannelData;
    avgChannelData.reserve(2);
    avgChannelData.push_back(avg_a); // channelIndex=0 -> a区
    avgChannelData.push_back(avg_b); // channelIndex=1 -> b区

    // ===== 2) 关键：把 a/b 两路分别送进算法 =====
    const QVector<ABAlgorithm::Result> allResults =
        m_abAlgo->analyzeEpoch(phaseIndex, timeStamps, avgChannelData);

    if (allResults.isEmpty()) {
        appendLog(phaseName + ": 本 epoch 未检测到事件");
        return;
    }

    // ===== 3) Phase 决定刺激对象；保持你现有“配置24/25，触发0/1”的体系不改 =====
    // PhaseA：刺激老鼠B；PhaseB：刺激老鼠A
    const int triggerSource = (phaseIndex == 0) ? 1 : 0;

    auto electrodeForRegion = [&](int regionIdx /*0=a, 1=b*/) -> QString {
        if (phaseIndex == 0) { // A -> B
            return (regionIdx == 0) ? kStim_B_a : kStim_B_b;
        } else {               // B -> A
            return (regionIdx == 0) ? kStim_A_a : kStim_A_b;
        }
    };

    // ===== 4) 每路各取最多10个（你要的重点）=====
    QVector<ABAlgorithm::Result> candA; // channelIndex==0 (a平均)
    QVector<ABAlgorithm::Result> candB; // channelIndex==1 (b平均)

    for (const auto &r : allResults) {
        if (!r.needStim) continue;
        if (r.suggestedAmplitude_uA <= 0) continue;
        if (r.channelIndex == 0) candA.push_back(r);
        else if (r.channelIndex == 1) candB.push_back(r);
    }

    if (candA.isEmpty() && candB.isEmpty()) {
        appendLog(phaseName + ": 无需刺激");
        return;
    }

    auto topKByAmp = [](QVector<ABAlgorithm::Result> &v, int k) {
        std::sort(v.begin(), v.end(),
                  [](const ABAlgorithm::Result &a, const ABAlgorithm::Result &b) {
                      return a.spikeAmplitude_uV > b.spikeAmplitude_uV;
                  });
        if (v.size() > k) v.resize(k);
    };

    const int maxPerRegion = 10;
    topKByAmp(candA, maxPerRegion);
    topKByAmp(candB, maxPerRegion);

    QVector<ABAlgorithm::Result> candidates = candA;
    candidates += candB;

    // 合并后按时间排序：按你说的“时间顺序依次触发”
    std::sort(candidates.begin(), candidates.end(),
              [](const ABAlgorithm::Result &a, const ABAlgorithm::Result &b) {
                  return a.triggerTime < b.triggerTime;
              });

    // ===== 5) 生成计划 + singleShot 触发 =====
    const uint32_t epochStartTs = timeStamps.first();
    const double fs = (m_sampleRate > 0.0 ? m_sampleRate : 30000.0);

    const int epochId = ++m_epochCounter;

    QVector<StimTimelineOverlay::Item> items;
    items.reserve(candidates.size());

    for (int i = 0; i < candidates.size(); ++i) {
        const auto &r = candidates[i];

        const int regionIdx = (r.channelIndex == 0) ? 0 : 1; // 0=a, 1=b
        const QString targetElectrode = electrodeForRegion(regionIdx);

        int pulses = (r.suggestedNumPulses > 0) ? r.suggestedNumPulses : 1;

        double offsetSec = 0.0;
        if (r.triggerTime >= epochStartTs)
            offsetSec = double(r.triggerTime - epochStartTs) / fs;

        const double offsetMs = offsetSec * 1000.0;
        int delayMs = int(offsetMs);
        if (delayMs < 0) delayMs = 0;

        StimTimelineOverlay::Item it;
        it.itemIndex = i;
        it.offsetMs = offsetMs;
        it.amp_uA = r.suggestedAmplitude_uA;
        it.pulses = pulses;
        it.ch = r.channelIndex;          // 0/1 => a平均/b平均
        it.spike_uV = r.spikeAmplitude_uV;
        it.electrode = targetElectrode;  // ✅ 按 region 分流到对应刺激电极
        it.fired = false;
        items.push_back(it);

        if (m_stimLog) {
            m_stimLog->logPlanned(epochId, phaseIndex, i, offsetMs,
                                  targetElectrode, it.amp_uA, it.pulses, it.ch, it.spike_uV);
        }

        QTimer::singleShot(delayMs, this,
                           [this, epochId, phaseIndex, itemIdx=i,
                            targetElectrode, triggerSource,
                            amp=it.amp_uA, pulses=it.pulses]() {

                               if (m_timeline) m_timeline->markFired(epochId, itemIdx);
                               if (m_stimLog)  m_stimLog->logFired(epochId, phaseIndex, itemIdx, targetElectrode, amp, pulses);

                               if (!m_engine) return;
                               m_engine->applyAdaptiveStim(targetElectrode, amp, pulses, triggerSource);
                           });
    }

    if (m_timeline) {
        m_timeline->setEpochPlan(epochId, phaseIndex, colletion_time, items);
        ensureTimelineVisible();
    }

    appendLog(QString("%1: epochId=%2 计划刺激=%3（a<=%4, b<=%4）")
                  .arg(phaseName).arg(epochId).arg(items.size()).arg(maxPerRegion));
}



void MainWindow::handleError(const QString &msg)
{
    appendLog(QStringLiteral("错误：") + msg);
}

void MainWindow::handleLog(const QString &msg)
{
    appendLog(msg);
}
