#include "mainwindow.h"

#include <QFileDialog>
#include <QDateTime>
#include <QDir>
#include <QSettings>
#include <QDockWidget>
#include <QGroupBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QMessageBox>
#include <QRegularExpression>
#include <QTextDocument>
#include <QSignalBlocker>

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

    setupManualStimDock();
    loadManualStimConfig();

    setupElectrodeConfigDock();
    loadElectrodeConfig();

    // ===== core =====
    m_engine      = new AcquisitionEngine(this);
    m_experiment  = new ExperimentControllerAB(m_engine, this);
    m_abAlgo      = new ABAlgorithm(this);
    m_coordinator = new ABExperimentCoordinator(m_engine, m_abAlgo, this);

    if (m_abAlgo) {
        m_abAlgo->setSampleRateHz(m_sampleRate);
    }
    if (m_coordinator) {
        m_coordinator->setSampleRateHz(m_sampleRate);
        m_coordinator->setEpochDurationSec(colletion_time);
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
    connect(m_coordinator, &ABExperimentCoordinator::logMessage,
            this,          &MainWindow::handleLog);

    connect(m_experiment, &ExperimentControllerAB::epochReady,
            this,         &MainWindow::onABEpochReady);

    // ===== timeline + stim csv =====
    m_timeline = new StimTimelineOverlay();
    m_timeline->setEpochSec(colletion_time);
    m_timeline->hide();

    m_stimLog = new StimLogWriter(this);
    m_stimLog->start(QDir::currentPath() + "/stim_log.csv");

    if (m_coordinator) {
        m_coordinator->setTimelineOverlay(m_timeline);
        m_coordinator->setStimLogWriter(m_stimLog);
        syncExperimentRoutingConfig();
    }

    // ===== FFT windows =====
    m_fftA = new FftWindow(m_viewA);
    m_fftB = new FftWindow(m_viewB);
    m_fftA->setFftSize(2048);
    m_fftB->setFftSize(2048);
        m_fftA->setMaxFreq(5000);
    m_fftB->setMaxFreq(5000);
    connect(m_viewA, &StackedWaveWidget::selectedChannelChanged, m_fftA, &FftWindow::setChannel);
    connect(m_viewB, &StackedWaveWidget::selectedChannelChanged, m_fftB, &FftWindow::setChannel);
    m_fftA->hide();
    m_fftB->hide();

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
    setWindowTitle(tr("RHX Closed-Loop Workbench"));
    resize(1680, 980);
    setMinimumSize(1320, 860);
    setStyleSheet(R"(
        QMainWindow {
            background: #0b1116;
        }
        QWidget {
            color: #dce8ef;
            font-size: 12px;
        }
        QLabel {
            color: #c8d6de;
        }
        QPushButton {
            background: #16232d;
            border: 1px solid #29404f;
            border-radius: 8px;
            padding: 8px 14px;
            min-height: 18px;
        }
        QPushButton:hover {
            background: #1c2d39;
            border-color: #44c8b2;
        }
        QPushButton:pressed {
            background: #102028;
        }
        QPlainTextEdit {
            background: #10171d;
            border: 1px solid #223542;
            border-radius: 12px;
            padding: 6px;
            selection-background-color: #1a7f71;
        }
        QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
            background: #101920;
            border: 1px solid #29404f;
            border-radius: 7px;
            padding: 4px 8px;
            min-height: 20px;
        }
        QGroupBox {
            border: 1px solid #233541;
            border-radius: 12px;
            margin-top: 10px;
            padding-top: 12px;
            background: #0f161d;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 6px;
            color: #8fb8c3;
        }
        QDockWidget {
            color: #dce8ef;
        }
        QDockWidget::title {
            text-align: left;
            background: #111b24;
            padding: 8px 12px;
            border-bottom: 1px solid #223542;
        }
        QSplitter::handle {
            background: #11202a;
            width: 8px;
        }
        QScrollBar:vertical {
            background: #0c141a;
            border: 1px solid #203340;
            border-radius: 7px;
            width: 14px;
            margin: 0px;
        }
        QScrollBar::handle:vertical {
            background: #33515f;
            border-radius: 6px;
            min-height: 28px;
        }
        QScrollBar::handle:vertical:hover {
            background: #44c8b2;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0px;
        }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {
            background: transparent;
        }
        QMenu {
            background: #0d151b;
            color: #d9e5eb;
            border: 1px solid #233947;
            padding: 6px;
        }
        QMenu::item {
            padding: 7px 18px 7px 12px;
            border-radius: 6px;
            background: transparent;
        }
        QMenu::item:selected {
            background: #17303a;
            color: #f3f7f9;
        }
        QMenu::separator {
            height: 1px;
            background: #243845;
            margin: 6px 8px;
        }
    )");

    m_central = new QWidget(this);
    m_layout  = new QVBoxLayout(m_central);
    m_layout->setContentsMargins(16, 16, 16, 16);
    m_layout->setSpacing(12);

    // ===== 顶部按钮行 =====
    {
        QHBoxLayout *row = new QHBoxLayout();

        m_btnOpen            = new QPushButton(tr("打开设备"), this);
        m_btnStart           = new QPushButton(tr("普通采集"), this);
        m_btnStartClosedLoop = new QPushButton(tr("闭环实验采集"), this);
        m_btnStop            = new QPushButton(tr("停止采集"), this);
        m_btnRecStart        = new QPushButton(tr("开始录制(bin)"), this);
        m_btnRecStop         = new QPushButton(tr("停止录制"), this);

        row->addWidget(m_btnOpen);
        row->addWidget(m_btnStart);
        row->addWidget(m_btnStartClosedLoop);
        row->addWidget(m_btnStop);
        row->addWidget(m_btnRecStart);
        row->addWidget(m_btnRecStop);

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

        row->addSpacing(20);
        row->addWidget(new QLabel(tr("总览框高:"), this));
        m_spinOverviewLane = new QSpinBox(this);
        m_spinOverviewLane->setRange(28, 180);
        m_spinOverviewLane->setSingleStep(8);
        m_spinOverviewLane->setValue(58);
        m_spinOverviewLane->setSuffix(" px");
        m_spinOverviewLane->setToolTip(tr("普通滚轮直接调整总览右下角 +/-uV，Shift+滚轮调总览框高，Alt+滚轮滚动总览。"));
        row->addWidget(m_spinOverviewLane);

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

    m_viewA->setTitle("Stream A stacked view  |  wheel footer +/-uV / Alt+wheel scroll / Shift+wheel lane / Ctrl+wheel window");
    m_viewB->setTitle("Stream B stacked view  |  wheel footer +/-uV / Alt+wheel scroll / Shift+wheel lane / Ctrl+wheel window");

    m_viewA->setGainUv(m_spinGainA->value());
    m_viewB->setGainUv(m_spinGainB->value());
    m_viewA->setOverviewLaneHeight(m_spinOverviewLane->value());
    m_viewB->setOverviewLaneHeight(m_spinOverviewLane->value());

        m_split->addWidget(m_viewA);
    m_split->addWidget(m_viewB);
    m_split->setChildrenCollapsible(false);
    m_split->setHandleWidth(10);
    m_split->setOpaqueResize(false);
    m_split->setStretchFactor(0, 1);
    m_split->setStretchFactor(1, 1);

    m_layout->addWidget(m_split, 5);

    // ===== 日志 =====
        m_logView  = new QPlainTextEdit(this);
    m_logView->setObjectName("logView");
    m_logView->setReadOnly(true);
    m_logView->setMinimumHeight(120);
    m_logView->document()->setMaximumBlockCount(3000);
    m_layout->addWidget(m_logView, 1);

    setCentralWidget(m_central);

    // ===== connections =====
    connect(m_btnOpen,     &QPushButton::clicked, this, &MainWindow::onOpenDevice);
    connect(m_btnStart,           &QPushButton::clicked, this, &MainWindow::onStart);
    connect(m_btnStartClosedLoop, &QPushButton::clicked, this, &MainWindow::onStartClosedLoop);
    connect(m_btnStop,     &QPushButton::clicked, this, &MainWindow::onStop);
    connect(m_btnRecStart, &QPushButton::clicked, this, &MainWindow::onRecStart);
    connect(m_btnRecStop,  &QPushButton::clicked, this, &MainWindow::onRecStop);

    connect(m_spinGainA, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::onGainAChanged);
    connect(m_spinGainB, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::onGainBChanged);
    connect(m_spinOverviewLane, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &MainWindow::onOverviewLaneHeightChanged);

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

    connect(m_viewA, &StackedWaveWidget::overviewLaneHeightChanged,
            this, &MainWindow::onOverviewLaneHeightChanged);
    connect(m_viewB, &StackedWaveWidget::overviewLaneHeightChanged,
            this, &MainWindow::onOverviewLaneHeightChanged);
}

void MainWindow::appendLog(const QString &msg)
{
    const QString line = QDateTime::currentDateTime().toString("hh:mm:ss.zzz  ") + msg;
    m_logView->appendPlainText(line);
}

void MainWindow::onOverviewLaneHeightChanged(int px)
{
    const int clamped = qBound(28, px, 180);

    if (m_spinOverviewLane && m_spinOverviewLane->value() != clamped) {
        QSignalBlocker blocker(m_spinOverviewLane);
        m_spinOverviewLane->setValue(clamped);
    }

    if (m_viewA && m_viewA->overviewLaneHeight() != clamped) {
        m_viewA->setOverviewLaneHeight(clamped);
    }
    if (m_viewB && m_viewB->overviewLaneHeight() != clamped) {
        m_viewB->setOverviewLaneHeight(clamped);
    }
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

QString MainWindow::formatChannels1Based(const QVector<int> &zeroBased)
{
    QStringList parts;
    parts.reserve(zeroBased.size());
    for (int ch0 : zeroBased) {
        parts << QString::number(ch0 + 1);
    }
    return parts.join(",");
}

bool MainWindow::parseChannels1Based(const QString &text, QVector<int> &outZeroBased, QString *err)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) {
        if (err) *err = QStringLiteral("不能为空。示例：1,5,7");
        return false;
    }

    // split by comma / space / semicolon
    const QRegularExpression re(QStringLiteral(R"([,\s;]+)"));
    const QStringList tokens = t.split(re, Qt::SkipEmptyParts);

    QVector<int> v;
    v.reserve(tokens.size());

    for (const QString &tok : tokens) {
        bool ok = false;
        const int val1 = tok.toInt(&ok);
        if (!ok) {
            if (err) *err = QStringLiteral("包含非数字：%1").arg(tok);
            return false;
        }
        if (val1 <= 0) {
            if (err) *err = QStringLiteral("通道必须为正整数(1-based)：%1").arg(val1);
            return false;
        }
        v.push_back(val1 - 1); // to 0-based
    }

    if (v.isEmpty()) {
        if (err) *err = QStringLiteral("未解析到任何通道。示例：1,5,7");
        return false;
    }

    outZeroBased = v;
    return true;
}

void MainWindow::setupManualStimDock()
{
    m_dockManualStim = new QDockWidget(tr("普通采集刺激"), this);
    m_dockManualStim->setObjectName("dockManualStimConfig");
    m_dockManualStim->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    QWidget *panel = new QWidget(m_dockManualStim);
    QVBoxLayout *root = new QVBoxLayout(panel);

    QGroupBox *gbManual = new QGroupBox(tr("普通采集 / 手动刺激配置"), panel);
    QVBoxLayout *manualLayout = new QVBoxLayout(gbManual);

    QLabel *tip = new QLabel(tr("这一栏只服务于普通采集：先下发刺激参数，再手动触发。"), gbManual);
    tip->setWordWrap(true);
    manualLayout->addWidget(tip);

    QFormLayout *form = new QFormLayout();

    QWidget *electrodeEditor = new QWidget(gbManual);
    QHBoxLayout *electrodeRow = new QHBoxLayout(electrodeEditor);
    electrodeRow->setContentsMargins(0, 0, 0, 0);
    m_cmbManualStimPrefix = new QComboBox(electrodeEditor);
    m_cmbManualStimPrefix->addItems({"A", "B"});
    m_spinManualStimElectrode = new QSpinBox(electrodeEditor);
    m_spinManualStimElectrode->setRange(1, 64);
    m_spinManualStimElectrode->setSingleStep(1);
    electrodeRow->addWidget(m_cmbManualStimPrefix);
    electrodeRow->addWidget(m_spinManualStimElectrode, 1);

    m_spinManualTriggerSource = new QSpinBox(gbManual);
    m_spinManualTriggerSource->setRange(0, 15);
    m_spinManualTriggerSource->setSingleStep(1);

    m_spinManualFirstAmp = new QSpinBox(gbManual);
    m_spinManualFirstAmp->setRange(1, 5000);
    m_spinManualFirstAmp->setSingleStep(5);
    m_spinManualFirstAmp->setSuffix(" uA");

    m_spinManualSecondAmp = new QSpinBox(gbManual);
    m_spinManualSecondAmp->setRange(1, 5000);
    m_spinManualSecondAmp->setSingleStep(5);
    m_spinManualSecondAmp->setSuffix(" uA");

    m_spinManualPulseCount = new QSpinBox(gbManual);
    m_spinManualPulseCount->setRange(1, 256);
    m_spinManualPulseCount->setSingleStep(1);

    m_spinManualFirstPhaseUs = new QSpinBox(gbManual);
    m_spinManualFirstPhaseUs->setRange(1, 20000);
    m_spinManualFirstPhaseUs->setSingleStep(10);
    m_spinManualFirstPhaseUs->setSuffix(" us");

    m_spinManualSecondPhaseUs = new QSpinBox(gbManual);
    m_spinManualSecondPhaseUs->setRange(1, 20000);
    m_spinManualSecondPhaseUs->setSingleStep(10);
    m_spinManualSecondPhaseUs->setSuffix(" us");

    m_spinManualInterphaseUs = new QSpinBox(gbManual);
    m_spinManualInterphaseUs->setRange(0, 20000);
    m_spinManualInterphaseUs->setSingleStep(10);
    m_spinManualInterphaseUs->setSuffix(" us");

    form->addRow(tr("刺激电极"), electrodeEditor);
    form->addRow(tr("触发源"), m_spinManualTriggerSource);
    form->addRow(tr("一相幅度"), m_spinManualFirstAmp);
    form->addRow(tr("二相幅度"), m_spinManualSecondAmp);
    form->addRow(tr("脉冲数"), m_spinManualPulseCount);
    form->addRow(tr("一相时宽"), m_spinManualFirstPhaseUs);
    form->addRow(tr("二相时宽"), m_spinManualSecondPhaseUs);
    form->addRow(tr("相间间隔"), m_spinManualInterphaseUs);
    manualLayout->addLayout(form);

    QWidget *buttonRow = new QWidget(gbManual);
    QHBoxLayout *buttonLayout = new QHBoxLayout(buttonRow);
    buttonLayout->setContentsMargins(0, 0, 0, 0);

    m_btnApplyManualStim = new QPushButton(tr("应用刺激配置"), buttonRow);
    m_btnStimOnce = new QPushButton(tr("立即触发一次"), buttonRow);
    m_btnApplyManualStim->setToolTip(tr("把当前普通采集刺激参数下发到设备。"));
    m_btnStimOnce->setToolTip(tr("使用当前触发源手动触发一次刺激。"));

    buttonLayout->addWidget(m_btnApplyManualStim);
    buttonLayout->addWidget(m_btnStimOnce);
    manualLayout->addWidget(buttonRow);

    root->addWidget(gbManual);
    root->addStretch(1);

    panel->setLayout(root);
    m_dockManualStim->setWidget(panel);
    addDockWidget(Qt::LeftDockWidgetArea, m_dockManualStim);

    connect(m_btnApplyManualStim, &QPushButton::clicked,
            this, &MainWindow::applyManualStimConfigFromUi);
    connect(m_btnStimOnce, &QPushButton::clicked,
            this, &MainWindow::onStimOnce);
}

void MainWindow::loadManualStimConfig()
{
    if (!m_dockManualStim) return;

    QSettings s;

    const QString prefix = s.value("manual_stim/prefix", QStringLiteral("A")).toString();
    const int prefixIndex = qMax(0, m_cmbManualStimPrefix->findText(prefix));
    m_cmbManualStimPrefix->setCurrentIndex(prefixIndex);

    m_spinManualStimElectrode->setValue(s.value("manual_stim/electrode_num", 2).toInt());
    m_spinManualTriggerSource->setValue(s.value("manual_stim/trigger_source", 0).toInt());
    m_spinManualFirstAmp->setValue(s.value("manual_stim/first_amp_uA", 20).toInt());
    m_spinManualSecondAmp->setValue(s.value("manual_stim/second_amp_uA", 20).toInt());
    m_spinManualPulseCount->setValue(s.value("manual_stim/pulses", 1).toInt());
    m_spinManualFirstPhaseUs->setValue(s.value("manual_stim/first_phase_us", 500).toInt());
    m_spinManualSecondPhaseUs->setValue(s.value("manual_stim/second_phase_us", 500).toInt());
    m_spinManualInterphaseUs->setValue(s.value("manual_stim/interphase_us", 500).toInt());
}

void MainWindow::saveManualStimConfig() const
{
    if (!m_dockManualStim) return;

    QSettings s;
    s.setValue("manual_stim/prefix", m_cmbManualStimPrefix->currentText());
    s.setValue("manual_stim/electrode_num", m_spinManualStimElectrode->value());
    s.setValue("manual_stim/trigger_source", m_spinManualTriggerSource->value());
    s.setValue("manual_stim/first_amp_uA", m_spinManualFirstAmp->value());
    s.setValue("manual_stim/second_amp_uA", m_spinManualSecondAmp->value());
    s.setValue("manual_stim/pulses", m_spinManualPulseCount->value());
    s.setValue("manual_stim/first_phase_us", m_spinManualFirstPhaseUs->value());
    s.setValue("manual_stim/second_phase_us", m_spinManualSecondPhaseUs->value());
    s.setValue("manual_stim/interphase_us", m_spinManualInterphaseUs->value());
}

QString MainWindow::manualStimElectrodeName() const
{
    if (!m_cmbManualStimPrefix || !m_spinManualStimElectrode) {
        return QStringLiteral("A1");
    }

    return QStringLiteral("%1%2")
        .arg(m_cmbManualStimPrefix->currentText())
        .arg(m_spinManualStimElectrode->value());
}

bool MainWindow::configureManualStimHardware(QString *summary)
{
    const QString electrodeName = manualStimElectrodeName();
    const int triggerSource = m_spinManualTriggerSource ? m_spinManualTriggerSource->value() : 0;
    const int firstAmp = m_spinManualFirstAmp ? m_spinManualFirstAmp->value() : 20;
    const int secondAmp = m_spinManualSecondAmp ? m_spinManualSecondAmp->value() : 20;
    const int pulses = m_spinManualPulseCount ? m_spinManualPulseCount->value() : 1;
    const int firstPhaseUs = m_spinManualFirstPhaseUs ? m_spinManualFirstPhaseUs->value() : 500;
    const int secondPhaseUs = m_spinManualSecondPhaseUs ? m_spinManualSecondPhaseUs->value() : 500;
    const int interphaseUs = m_spinManualInterphaseUs ? m_spinManualInterphaseUs->value() : 500;

    const QString detail = QStringLiteral("电极=%1 trigger=%2 一相=%3 uA 二相=%4 uA 脉冲=%5 时宽=%6/%7 us 间隔=%8 us")
                               .arg(electrodeName)
                               .arg(triggerSource)
                               .arg(firstAmp)
                               .arg(secondAmp)
                               .arg(pulses)
                               .arg(firstPhaseUs)
                               .arg(secondPhaseUs)
                               .arg(interphaseUs);
    if (summary) {
        *summary = detail;
    }

    if (!m_engine || !m_engine->rhx() || !m_engine->stimController()) {
        return false;
    }

    m_engine->configureStim(electrodeName,
                            firstAmp,
                            secondAmp,
                            firstPhaseUs,
                            secondPhaseUs,
                            interphaseUs,
                            pulses,
                            triggerSource);
    return true;
}

void MainWindow::applyManualStimConfigFromUi()
{
    saveManualStimConfig();

    QString summary;
    if (!configureManualStimHardware(&summary)) {
        appendLog(QStringLiteral("普通采集刺激参数已保存：%1（打开设备后可应用）").arg(summary));
        return;
    }

    appendLog(QStringLiteral("普通采集刺激参数已应用：%1").arg(summary));
}

void MainWindow::setupElectrodeConfigDock()
{
    m_dockElectrode = new QDockWidget(tr("闭环实验配置"), this);
    m_dockElectrode->setObjectName("dockClosedLoopConfig");
    m_dockElectrode->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);

    QWidget *panel = new QWidget(m_dockElectrode);
    QVBoxLayout *root = new QVBoxLayout(panel);

    QGroupBox *gbExperiment = new QGroupBox(tr("闭环实验运行参数"), panel);
    QVBoxLayout *experimentLayout = new QVBoxLayout(gbExperiment);
    QLabel *experimentTip = new QLabel(tr("这一栏只影响闭环实验采集：AB epoch 切换、感受通道和目标刺激电极。"), gbExperiment);
    experimentTip->setWordWrap(true);
    experimentLayout->addWidget(experimentTip);

    QFormLayout *experimentForm = new QFormLayout();
    m_spinEpochSec = new QDoubleSpinBox(gbExperiment);
    m_spinEpochSec->setRange(0.1, 3600.0);
    m_spinEpochSec->setDecimals(2);
    m_spinEpochSec->setSingleStep(0.5);
    m_spinEpochSec->setSuffix(" s");
    m_spinEpochSec->setValue(colletion_time);
    experimentForm->addRow(tr("AB Epoch 时长"), m_spinEpochSec);
    experimentLayout->addLayout(experimentForm);

    QGroupBox *gbSense = new QGroupBox(tr("闭环感受通道 (UI 用 1-based)"), panel);
    QFormLayout *senseLayout = new QFormLayout(gbSense);

    m_editSense_A_a = new QLineEdit(gbSense);
    m_editSense_A_b = new QLineEdit(gbSense);
    m_editSense_B_a = new QLineEdit(gbSense);
    m_editSense_B_b = new QLineEdit(gbSense);

    m_editSense_A_a->setPlaceholderText("例如 1,5,7");
    m_editSense_A_b->setPlaceholderText("例如 9,11,15");
    m_editSense_B_a->setPlaceholderText("例如 1,5,7");
    m_editSense_B_b->setPlaceholderText("例如 9,11,15");

    senseLayout->addRow(tr("A 鼠 a 区 (stream0)"), m_editSense_A_a);
    senseLayout->addRow(tr("A 鼠 b 区 (stream0)"), m_editSense_A_b);
    senseLayout->addRow(tr("B 鼠 a' 区 (stream2)"), m_editSense_B_a);
    senseLayout->addRow(tr("B 鼠 b' 区 (stream2)"), m_editSense_B_b);

    QGroupBox *gbStim = new QGroupBox(tr("闭环目标刺激电极 (A/B 前缀固定)"), panel);
    QFormLayout *stimLayout = new QFormLayout(gbStim);

    auto makeStimEditor = [&](const QString &prefix, QSpinBox *&spinOut) -> QWidget* {
        QWidget *w = new QWidget(gbStim);
        QHBoxLayout *hl = new QHBoxLayout(w);
        hl->setContentsMargins(0, 0, 0, 0);
        QLabel *lab = new QLabel(prefix, w);
        spinOut = new QSpinBox(w);
        spinOut->setRange(1, 64);
        spinOut->setSingleStep(1);
        hl->addWidget(lab);
        hl->addWidget(spinOut, 1);
        w->setLayout(hl);
        return w;
    };

    stimLayout->addRow(tr("闭环刺激 A a"),  makeStimEditor("A", m_spinStim_A_a));
    stimLayout->addRow(tr("闭环刺激 A b"),  makeStimEditor("A", m_spinStim_A_b));
    stimLayout->addRow(tr("闭环刺激 B a'"), makeStimEditor("B", m_spinStim_B_a));
    stimLayout->addRow(tr("闭环刺激 B b'"), makeStimEditor("B", m_spinStim_B_b));

    m_btnApplyElectrode = new QPushButton(tr("应用闭环配置"), panel);

    root->addWidget(gbExperiment);
    root->addWidget(gbSense);
    root->addWidget(gbStim);
    root->addWidget(m_btnApplyElectrode);
    root->addStretch(1);

    panel->setLayout(root);
    m_dockElectrode->setWidget(panel);

    addDockWidget(Qt::RightDockWidgetArea, m_dockElectrode);

    connect(m_btnApplyElectrode, &QPushButton::clicked,
            this, &MainWindow::applyElectrodeConfigFromUi);
    connect(m_spinEpochSec, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::onEpochDurationChanged);

    m_editSense_A_a->setText(formatChannels1Based(kSense_A_a));
    m_editSense_A_b->setText(formatChannels1Based(kSense_A_b));
    m_editSense_B_a->setText(formatChannels1Based(kSense_B_a));
    m_editSense_B_b->setText(formatChannels1Based(kSense_B_b));

    auto parseSuffix = [](const QString &name, const QChar expectedPrefix, int fallback) -> int {
        if (name.size() >= 2 && name[0] == expectedPrefix) {
            bool ok = false;
            const int n = name.mid(1).toInt(&ok);
            if (ok && n > 0) return n;
        }
        return fallback;
    };

    m_spinStim_A_a->setValue(parseSuffix(kStim_A_a, QChar('A'), 2));
    m_spinStim_A_b->setValue(parseSuffix(kStim_A_b, QChar('A'), 7));
    m_spinStim_B_a->setValue(parseSuffix(kStim_B_a, QChar('B'), 2));
    m_spinStim_B_b->setValue(parseSuffix(kStim_B_b, QChar('B'), 7));
}

void MainWindow::loadElectrodeConfig()
{
    if (!m_dockElectrode) return;

    QSettings s;

    const double savedEpochSec = s.value("closed_loop/epoch_sec", colletion_time).toDouble();
    if (savedEpochSec > 0.0) {
        colletion_time = savedEpochSec;
    }
    if (m_spinEpochSec) {
        QSignalBlocker blocker(m_spinEpochSec);
        m_spinEpochSec->setValue(colletion_time);
    }

    const QString tA_a = s.value("electrode/sense_A_a", m_editSense_A_a->text()).toString();
    const QString tA_b = s.value("electrode/sense_A_b", m_editSense_A_b->text()).toString();
    const QString tB_a = s.value("electrode/sense_B_a", m_editSense_B_a->text()).toString();
    const QString tB_b = s.value("electrode/sense_B_b", m_editSense_B_b->text()).toString();

    m_editSense_A_a->setText(tA_a);
    m_editSense_A_b->setText(tA_b);
    m_editSense_B_a->setText(tB_a);
    m_editSense_B_b->setText(tB_b);

    m_spinStim_A_a->setValue(s.value("electrode/stim_A_a_num", m_spinStim_A_a->value()).toInt());
    m_spinStim_A_b->setValue(s.value("electrode/stim_A_b_num", m_spinStim_A_b->value()).toInt());
    m_spinStim_B_a->setValue(s.value("electrode/stim_B_a_num", m_spinStim_B_a->value()).toInt());
    m_spinStim_B_b->setValue(s.value("electrode/stim_B_b_num", m_spinStim_B_b->value()).toInt());

    QString err;
    QVector<int> tmp;

    if (parseChannels1Based(m_editSense_A_a->text(), tmp, &err)) kSense_A_a = tmp;
    if (parseChannels1Based(m_editSense_A_b->text(), tmp, &err)) kSense_A_b = tmp;
    if (parseChannels1Based(m_editSense_B_a->text(), tmp, &err)) kSense_B_a = tmp;
    if (parseChannels1Based(m_editSense_B_b->text(), tmp, &err)) kSense_B_b = tmp;

    kStim_A_a = QString("A%1").arg(m_spinStim_A_a->value());
    kStim_A_b = QString("A%1").arg(m_spinStim_A_b->value());
    kStim_B_a = QString("B%1").arg(m_spinStim_B_a->value());
    kStim_B_b = QString("B%1").arg(m_spinStim_B_b->value());
}

void MainWindow::syncExperimentRoutingConfig()
{
    if (!m_coordinator) return;

    ABExperimentCoordinator::RoutingConfig config;
    config.senseA_a = kSense_A_a;
    config.senseA_b = kSense_A_b;
    config.senseB_a = kSense_B_a;
    config.senseB_b = kSense_B_b;
    config.stimA_a = kStim_A_a;
    config.stimA_b = kStim_A_b;
    config.stimB_a = kStim_B_a;
    config.stimB_b = kStim_B_b;

    m_coordinator->setRoutingConfig(config);
}

void MainWindow::saveElectrodeConfig() const
{
    if (!m_dockElectrode) return;

    QSettings s;
    s.setValue("closed_loop/epoch_sec", m_spinEpochSec ? m_spinEpochSec->value() : colletion_time);
    s.setValue("electrode/sense_A_a", m_editSense_A_a->text().trimmed());
    s.setValue("electrode/sense_A_b", m_editSense_A_b->text().trimmed());
    s.setValue("electrode/sense_B_a", m_editSense_B_a->text().trimmed());
    s.setValue("electrode/sense_B_b", m_editSense_B_b->text().trimmed());

    s.setValue("electrode/stim_A_a_num", m_spinStim_A_a->value());
    s.setValue("electrode/stim_A_b_num", m_spinStim_A_b->value());
    s.setValue("electrode/stim_B_a_num", m_spinStim_B_a->value());
    s.setValue("electrode/stim_B_b_num", m_spinStim_B_b->value());
}

void MainWindow::applyElectrodeConfigFromUi()
{
    QString err;
    QVector<int> tmp;

    if (!parseChannels1Based(m_editSense_A_a->text(), tmp, &err)) {
        QMessageBox::warning(this, tr("Invalid A a sense list"), err);
        return;
    }
    kSense_A_a = tmp;

    if (!parseChannels1Based(m_editSense_A_b->text(), tmp, &err)) {
        QMessageBox::warning(this, tr("Invalid A b sense list"), err);
        return;
    }
    kSense_A_b = tmp;

    if (!parseChannels1Based(m_editSense_B_a->text(), tmp, &err)) {
        QMessageBox::warning(this, tr("Invalid B a' sense list"), err);
        return;
    }
    kSense_B_a = tmp;

    if (!parseChannels1Based(m_editSense_B_b->text(), tmp, &err)) {
        QMessageBox::warning(this, tr("Invalid B b' sense list"), err);
        return;
    }
    kSense_B_b = tmp;

    kStim_A_a = QString("A%1").arg(m_spinStim_A_a->value());
    kStim_A_b = QString("A%1").arg(m_spinStim_A_b->value());
    kStim_B_a = QString("B%1").arg(m_spinStim_B_a->value());
    kStim_B_b = QString("B%1").arg(m_spinStim_B_b->value());

    saveElectrodeConfig();
    syncExperimentRoutingConfig();

    appendLog(QStringLiteral("闭环实验配置已应用：Epoch=%1 s | SenseA(a)=[%2] SenseA(b)=[%3] SenseB(a')=[%4] SenseB(b')=[%5] | StimA(a)=%6 StimA(b)=%7 StimB(a')=%8 StimB(b')=%9")
                  .arg(colletion_time, 0, 'f', 2)
                  .arg(formatChannels1Based(kSense_A_a))
                  .arg(formatChannels1Based(kSense_A_b))
                  .arg(formatChannels1Based(kSense_B_a))
                  .arg(formatChannels1Based(kSense_B_b))
                  .arg(kStim_A_a)
                  .arg(kStim_A_b)
                  .arg(kStim_B_a)
                  .arg(kStim_B_b));
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

    const bool wasAcquiring = m_engine->isContinuousRunning();
    if (!wasAcquiring) {
        m_engine->startContinuousAcquisition();
    }
    if (!m_engine->isContinuousRunning()) return;

    const bool wasClosedLoop = m_closedLoopExperimentActive;
    if (m_experiment) m_experiment->stop();
    m_closedLoopExperimentActive = false;

    if (m_timeline) m_timeline->hide();

    if (!wasAcquiring) {
        appendLog("已开始普通采集");
    } else if (wasClosedLoop) {
        appendLog("已切换到普通采集：闭环实验已停止");
    } else {
        appendLog("普通采集已在运行");
    }
}

void MainWindow::onStartClosedLoop()
{
    if (!m_engine) return;

    const bool wasAcquiring = m_engine->isContinuousRunning();
    if (!wasAcquiring) {
        m_engine->startContinuousAcquisition();
    }
    if (!m_engine->isContinuousRunning()) return;

    if (m_closedLoopExperimentActive) {
        if (m_timeline) {
            m_timeline->setEpochSec(colletion_time);
            m_timeline->show();
            ensureTimelineVisible();
        }
        appendLog("闭环实验采集已在运行");
        return;
    }

    if (m_experiment) {
        m_experiment->setEpochDuration(colletion_time);
        m_experiment->start();
    }
    m_closedLoopExperimentActive = true;

    if (m_timeline) {
        m_timeline->setEpochSec(colletion_time);
        m_timeline->show();
        ensureTimelineVisible();
    }

    if (!wasAcquiring) {
        appendLog(QString("已开始闭环实验采集：AB epoch=%1 s").arg(colletion_time, 0, 'f', 2));
    } else {
        appendLog(QString("已在当前采集上启动闭环实验：AB epoch=%1 s").arg(colletion_time, 0, 'f', 2));
    }
}

void MainWindow::onStop()
{
    const bool wasClosedLoop = m_closedLoopExperimentActive;
    const bool wasAcquiring = m_engine && m_engine->isContinuousRunning();

    if (m_experiment) m_experiment->stop();
    m_closedLoopExperimentActive = false;

    if (m_engine && wasAcquiring) {
        m_engine->stopAcquisition();
    }
    if (m_timeline) m_timeline->hide();

    if (wasClosedLoop && wasAcquiring) {
        appendLog("已停止闭环实验采集");
    } else if (wasAcquiring) {
        appendLog("已停止普通采集");
    } else if (wasClosedLoop) {
        appendLog("闭环实验已停止");
    } else {
        appendLog("当前没有正在运行的采集");
    }
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
    saveManualStimConfig();

    QString summary;
    if (!configureManualStimHardware(&summary)) {
        appendLog(QStringLiteral("未触发普通采集刺激：%1（请先打开设备）").arg(summary));
        return;
    }

    const int triggerSource = m_spinManualTriggerSource ? m_spinManualTriggerSource->value() : 0;
    m_engine->triggerStim(triggerSource, true);
    m_engine->triggerStim(triggerSource, false);
    appendLog(QStringLiteral("已手动触发一次普通采集刺激：%1").arg(summary));
}

void MainWindow::onEpochDurationChanged(double sec)
{
    colletion_time = sec;
    if (m_experiment) m_experiment->setEpochDuration(colletion_time);
    if (m_coordinator) m_coordinator->setEpochDurationSec(colletion_time);
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
    if (!m_coordinator) return;
    m_coordinator->handleEpochReady(phaseIndex, timeStamps, channelData);
    ensureTimelineVisible();
}

void MainWindow::handleError(const QString &msg)
{
    appendLog(QStringLiteral("错误：") + msg);
}

void MainWindow::handleLog(const QString &msg)
{
    appendLog(msg);
}
