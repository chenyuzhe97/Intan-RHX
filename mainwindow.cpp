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
        m_cmbNotchHz->addItems({"50","60"});
        m_cmbNotchHz->setCurrentText("50");

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

    const QVector<ABAlgorithm::Result> allResults =
        m_abAlgo->analyzeEpoch(phaseIndex, timeStamps, channelData);

    if (allResults.isEmpty()) {
        appendLog(phaseName + ": 本 epoch 未检测到事件");
        return;
    }

    QString targetElectrode;
    int triggerSource = 0;
    if (phaseIndex == 0) { targetElectrode = "B1"; triggerSource = 1; }
    else { targetElectrode = "A1"; triggerSource = 0; }

    const uint32_t epochStartTs = timeStamps.first();
    const double fs = (m_sampleRate > 0.0 ? m_sampleRate : 30000.0);

    QVector<ABAlgorithm::Result> candidates;
    candidates.reserve(allResults.size());
    for (const auto &r : allResults) {
        if (!r.needStim) continue;
        if (r.suggestedAmplitude_uA <= 0) continue;
        candidates.push_back(r);
    }
    if (candidates.isEmpty()) {
        appendLog(phaseName + ": 无需刺激");
        return;
    }

    // 强度最大 10 个
    std::sort(candidates.begin(), candidates.end(),
              [](const ABAlgorithm::Result &a, const ABAlgorithm::Result &b) {
                  return a.spikeAmplitude_uV > b.spikeAmplitude_uV;
              });
    const int maxStimPerEpoch = 10;
    if (candidates.size() > maxStimPerEpoch) candidates.resize(maxStimPerEpoch);

    // 按时间排序
    std::sort(candidates.begin(), candidates.end(),
              [](const ABAlgorithm::Result &a, const ABAlgorithm::Result &b) {
                  return a.triggerTime < b.triggerTime;
              });

    const int epochId = ++m_epochCounter;

    QVector<StimTimelineOverlay::Item> items;
    items.reserve(candidates.size());

    for (int i=0; i<candidates.size(); ++i) {
        const auto r = candidates[i];

        int pulses = (r.suggestedNumPulses > 0) ? r.suggestedNumPulses : 1;
        double offsetSec = 0.0;
        if (r.triggerTime >= epochStartTs) offsetSec = double(r.triggerTime - epochStartTs) / fs;
        const double offsetMs = offsetSec * 1000.0;
        int delayMs = int(offsetMs);
        if (delayMs < 0) delayMs = 0;

        StimTimelineOverlay::Item it;
        it.itemIndex = i;
        it.offsetMs = offsetMs;
        it.amp_uA = r.suggestedAmplitude_uA;
        it.pulses = pulses;
        it.ch = r.channelIndex;
        it.spike_uV = r.spikeAmplitude_uV;
        it.electrode = targetElectrode;
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

    appendLog(QString("%1: epochId=%2 计划刺激=%3")
                  .arg(phaseName).arg(epochId).arg(items.size()));
}

void MainWindow::handleError(const QString &msg)
{
    appendLog(QStringLiteral("错误：") + msg);
}

void MainWindow::handleLog(const QString &msg)
{
    appendLog(msg);
}
