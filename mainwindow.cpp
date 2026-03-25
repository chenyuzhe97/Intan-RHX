#include "mainwindow.h"

#include <QFileDialog>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QDockWidget>
#include <QGroupBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QMessageBox>
#include <QMenuBar>
#include <QRegularExpression>
#include <QTextDocument>
#include <QSignalBlocker>
#include <QCloseEvent>

#include <algorithm>
#include <limits>

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

static QString stimStepSizeDisplayText(StimStepSize step)
{
    const int idx = int(step);
    if (idx >= 0 && idx <= int(StimStepSizeMax)) {
        return StimStepSizeString[idx];
    }
    return QStringLiteral("500 nA step size");
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();

    setupManualStimDock();
    loadManualStimConfig();

    setupElectrodeConfigDock();
    setupFixedStimDock();
    loadElectrodeConfig();
    loadSessionConfig();

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
    connect(m_coordinator, &ABExperimentCoordinator::stimPhaseFinished,
            m_experiment, &ExperimentControllerAB::onStimPhaseFinished);
    connect(m_experiment, &ExperimentControllerAB::roundCompleted,
            this,         &MainWindow::onClosedLoopRoundCompleted);
    connect(m_experiment, &ExperimentControllerAB::experimentCompleted,
            this,         &MainWindow::onClosedLoopExperimentCompleted);
    connect(m_coordinator, &ABExperimentCoordinator::stimPlanned,
            this,         &MainWindow::onStimPlanned);
    connect(m_coordinator, &ABExperimentCoordinator::stimFired,
            this,         &MainWindow::onStimFired);

    // ===== timeline + stim csv =====
    m_timeline = new StimTimelineOverlay(this);
    m_timeline->setEpochSec(colletion_time);

    m_dockTimeline = new QDockWidget(tr("闭环刺激时序图"), this);
    m_dockTimeline->setObjectName("dockTimelineOverlay");
    m_dockTimeline->setAllowedAreas(Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);
    m_dockTimeline->setWidget(m_timeline);
    addDockWidget(Qt::RightDockWidgetArea, m_dockTimeline);
    if (m_dockElectrode) {
        splitDockWidget(m_dockElectrode, m_dockTimeline, Qt::Vertical);
    }
    m_dockTimeline->hide();

    menuBar()->setNativeMenuBar(false);
    QMenu *viewMenu = menuBar()->addMenu(QString::fromUtf8(u8"视图"));
    QMenu *panelMenu = menuBar()->addMenu(QString::fromUtf8(u8"面板"));
    auto addDockToggle = [](QMenu *menu, QDockWidget *dock, const QString &title) {
        if (!menu || !dock) return;
        QAction *action = dock->toggleViewAction();
        action->setText(title);
        menu->addAction(action);
    };
    addDockToggle(panelMenu, m_dockManualStim, QString::fromUtf8(u8"普通采集刺激配置"));
    addDockToggle(panelMenu, m_dockElectrode, QString::fromUtf8(u8"闭环实验配置"));
    addDockToggle(panelMenu, m_dockFixedStim, QString::fromUtf8(u8"固定刺激实验配置"));
    addDockToggle(viewMenu, m_dockTimeline, QString::fromUtf8(u8"刺激时序图"));
    QAction *fftAction = viewMenu->addAction(QString::fromUtf8(u8"FFT 窗口"));
    connect(fftAction, &QAction::triggered, this, &MainWindow::onToggleFftWindows);

    if (m_dockManualStim) m_dockManualStim->hide();
    if (m_dockElectrode) m_dockElectrode->show();
    if (m_dockFixedStim) m_dockFixedStim->show();

    m_stimLog = new StimLogWriter(this);
    m_stimLog->start(QDir::currentPath() + "/stim_log.csv");

    if (m_coordinator) {
        m_coordinator->setTimelineOverlay(m_timeline);
        m_coordinator->setStimLogWriter(m_stimLog);
        syncExperimentRoutingConfig();
    }

    // ===== FFT windows =====
    m_fftA = new FftWindow(m_viewA, this);
    m_fftB = new FftWindow(m_viewB, this);
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
    stopVoidStimReplay(false);
    if (m_coordinator) m_coordinator->endRun();
    finalizeManagedSession();
    if (m_coordinator) m_coordinator->cancelPendingStimPhase();
    if (m_experiment) m_experiment->stop();
    if (m_engine)     m_engine->shutdownDevice();

    if (m_stimLog) m_stimLog->stop();

    if (m_fftA) m_fftA->close();
    if (m_fftB) m_fftB->close();
    if (m_dockTimeline) m_dockTimeline->close();
    else if (m_timeline) m_timeline->close();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    stopVoidStimReplay(false);
    if (m_coordinator) m_coordinator->endRun();
    finalizeManagedSession();
    if (m_coordinator) m_coordinator->cancelPendingStimPhase();
    if (m_experiment) m_experiment->stop();
    m_closedLoopExperimentActive = false;

    if (m_stimLog) m_stimLog->stop();

    if (m_fftA) {
        m_fftA->hide();
        m_fftA->close();
    }
    if (m_fftB) {
        m_fftB->hide();
        m_fftB->close();
    }
    if (m_dockTimeline) {
        m_dockTimeline->hide();
    } else if (m_timeline) {
        m_timeline->hide();
    }

    if (m_engine) {
        m_engine->shutdownDevice();
    }

    QMainWindow::closeEvent(event);
    if (event->isAccepted()) {
        QCoreApplication::quit();
    }
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
        QMenuBar {
            background: #0d151b;
            border: 1px solid #223542;
            border-radius: 10px;
            padding: 4px 6px;
            spacing: 6px;
        }
        QMenuBar::item {
            padding: 6px 12px;
            border-radius: 7px;
            background: transparent;
        }
        QMenuBar::item:selected {
            background: #17303a;
            color: #f3f7f9;
        }
        QPushButton {
            background: #16232d;
            border: 1px solid #29404f;
            border-radius: 8px;
            padding: 8px 14px;
            min-height: 18px;
        }
        QPushButton[variant="experiment"] {
            background: #15313a;
            border-color: #2a6c73;
            color: #eff9fb;
            font-weight: 600;
            min-height: 26px;
        }
        QPushButton[variant="experiment"]:hover {
            background: #1b414b;
            border-color: #54d4c0;
        }
        QPushButton[variant="danger"] {
            background: #2a1820;
            border-color: #744753;
        }
        QPushButton[variant="danger"]:hover {
            background: #38202a;
            border-color: #d48b9f;
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
        QWidget#controlDeck {
            background: #0d151c;
            border: 1px solid #213440;
            border-radius: 16px;
        }
        QLabel#deckTitle {
            color: #f0f7fa;
            font-size: 18px;
            font-weight: 700;
        }
        QLabel#deckSubtitle {
            color: #88a6b1;
            font-size: 12px;
        }
        QGroupBox#controlCard {
            background: #101921;
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

    // ===== 顶部控制台 =====
    {
        QWidget *controlDeck = new QWidget(this);
        controlDeck->setObjectName("controlDeck");
        QVBoxLayout *deckLayout = new QVBoxLayout(controlDeck);
        deckLayout->setContentsMargins(14, 14, 14, 14);
        deckLayout->setSpacing(12);

        QLabel *deckTitle = new QLabel(QString::fromUtf8(u8"实验控制台"), controlDeck);
        deckTitle->setObjectName("deckTitle");
        deckLayout->addWidget(deckTitle);

        QGridLayout *cards = new QGridLayout();
        cards->setHorizontalSpacing(12);
        cards->setVerticalSpacing(12);

        m_btnSelectSaveDir   = new QPushButton(QString::fromUtf8(u8"选择录制位置"), this);
        m_btnVoidStim        = new QPushButton(QString::fromUtf8(u8"实验三：虚空刺激"), this);
        m_btnFixedStim       = new QPushButton(QString::fromUtf8(u8"实验二：固定刺激实验"), this);

        m_btnOpen            = new QPushButton(tr("打开设备"), this);
        m_btnStart           = new QPushButton(tr("普通采集"), this);
        m_btnStartClosedLoop = new QPushButton(QString::fromUtf8(u8"实验一：闭环实验采集"), this);
        m_btnStop            = new QPushButton(tr("停止采集"), this);
        m_btnRecStart        = new QPushButton(tr("开始录制(bin)"), this);
        m_btnRecStop         = new QPushButton(tr("停止录制"), this);
        m_btnShowAllChannels = new QPushButton(tr("显示全部通道"), this);
        m_btnShowAllChannels->setToolTip(tr("恢复 A/B 两边所有被隐藏的通道。"));

        m_btnStartClosedLoop->setProperty("variant", "experiment");
        m_btnFixedStim->setProperty("variant", "experiment");
        m_btnVoidStim->setProperty("variant", "experiment");
        m_btnStop->setProperty("variant", "danger");

        QGroupBox *experimentCard = new QGroupBox(QString::fromUtf8(u8"实验流程"), controlDeck);
        experimentCard->setObjectName("controlCard");
        QGridLayout *experimentLayout = new QGridLayout(experimentCard);
        experimentLayout->setHorizontalSpacing(8);
        experimentLayout->setVerticalSpacing(8);
        experimentLayout->addWidget(m_btnSelectSaveDir, 0, 0, 1, 3);
        experimentLayout->addWidget(m_btnStartClosedLoop, 1, 0);
        experimentLayout->addWidget(m_btnFixedStim, 1, 1);
        experimentLayout->addWidget(m_btnVoidStim, 1, 2);

        QGroupBox *acquisitionCard = new QGroupBox(QString::fromUtf8(u8"采集控制"), controlDeck);
        acquisitionCard->setObjectName("controlCard");
        QHBoxLayout *acquisitionLayout = new QHBoxLayout(acquisitionCard);
        acquisitionLayout->setSpacing(8);
        acquisitionLayout->addWidget(m_btnOpen);
        acquisitionLayout->addWidget(m_btnStart);
        acquisitionLayout->addWidget(m_btnStop);

        QGroupBox *recordingCard = new QGroupBox(QString::fromUtf8(u8"录制与显示"), controlDeck);
        recordingCard->setObjectName("controlCard");
        QHBoxLayout *recordingLayout = new QHBoxLayout(recordingCard);
        recordingLayout->setSpacing(8);
        recordingLayout->addWidget(m_btnRecStart);
        recordingLayout->addWidget(m_btnRecStop);
        recordingLayout->addWidget(m_btnShowAllChannels);

        QGroupBox *hardwareCard = new QGroupBox(QString::fromUtf8(u8"刺激硬件"), controlDeck);
        hardwareCard->setObjectName("controlCard");
        QHBoxLayout *hardwareLayout = new QHBoxLayout(hardwareCard);
        hardwareLayout->setSpacing(8);
        QLabel *label = new QLabel(QString::fromUtf8(u8"全局刺激量程 / 步进"), hardwareCard);
        m_cmbStimStepSize = new QComboBox(hardwareCard);
        for (int step = int(StimStepSize10nA); step <= int(StimStepSize10uA); ++step) {
            const StimStepSize stimStep = static_cast<StimStepSize>(step);
            m_cmbStimStepSize->addItem(stimStepSizeDisplayText(stimStep), step);
        }
        m_cmbStimStepSize->setToolTip(QString::fromUtf8(u8"这是全局硬件刺激档位，普通采集、闭环、虚空刺激和固定刺激共用这一项。"));

        hardwareLayout->addWidget(label);
        hardwareLayout->addWidget(m_cmbStimStepSize, 1);

        connect(m_cmbStimStepSize, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) {
                    m_manualStimConfigApplied = false;
                    m_manualStimConfigDirty = true;
                    m_manualStimLoadedForCurrentRun = false;
                    m_manualStimAppliedSummary.clear();
                    m_manualStimAppliedSignature.clear();
                    updateFixedStimAmplitudeControl();
                    saveSessionConfig();
                });

        cards->addWidget(experimentCard, 0, 0, 2, 2);
        cards->addWidget(acquisitionCard, 0, 2);
        cards->addWidget(recordingCard, 1, 2);
        cards->addWidget(hardwareCard, 0, 3, 2, 1);
        cards->setColumnStretch(0, 2);
        cards->setColumnStretch(1, 2);
        cards->setColumnStretch(2, 2);
        cards->setColumnStretch(3, 2);

        deckLayout->addLayout(cards);
        m_layout->addWidget(controlDeck);
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
    connect(m_btnSelectSaveDir,   &QPushButton::clicked, this, &MainWindow::onSelectRecordingLocation);
    connect(m_btnStart,           &QPushButton::clicked, this, &MainWindow::onStart);
    connect(m_btnStartClosedLoop, &QPushButton::clicked, this, &MainWindow::onStartClosedLoop);
    connect(m_btnVoidStim,        &QPushButton::clicked, this, &MainWindow::onVoidStim);
    connect(m_btnFixedStim,       &QPushButton::clicked, this, &MainWindow::onFixedStimExperiment);
    connect(m_btnStop,     &QPushButton::clicked, this, &MainWindow::onStop);
    connect(m_btnRecStart, &QPushButton::clicked, this, &MainWindow::onRecStart);
    connect(m_btnRecStop,  &QPushButton::clicked, this, &MainWindow::onRecStop);
    connect(m_btnShowAllChannels, &QPushButton::clicked, this, [this]() {
        const bool hadHiddenA = m_viewA && m_viewA->hasHiddenChannels();
        const bool hadHiddenB = m_viewB && m_viewB->hasHiddenChannels();
        if (m_viewA) m_viewA->showAllChannels();
        if (m_viewB) m_viewB->showAllChannels();
        if (hadHiddenA || hadHiddenB) {
            appendLog(QStringLiteral("已恢复所有隐藏通道"));
        } else {
            appendLog(QStringLiteral("当前没有被隐藏的通道"));
        }
    });

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
    if (m_timeline) {
        m_timeline->show();
    }
    if (m_dockTimeline) {
        m_dockTimeline->show();
        m_dockTimeline->raise();
        return;
    }
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

    auto markManualStimDirty = [this]() {
        m_manualStimConfigDirty = true;
    };

    connect(m_cmbManualStimPrefix, &QComboBox::currentTextChanged, this,
            [markManualStimDirty](const QString &) { markManualStimDirty(); });
    connect(m_spinManualStimElectrode, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [markManualStimDirty](int) { markManualStimDirty(); });
    connect(m_spinManualTriggerSource, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [markManualStimDirty](int) { markManualStimDirty(); });
    connect(m_spinManualFirstAmp, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [markManualStimDirty](int) { markManualStimDirty(); });
    connect(m_spinManualSecondAmp, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [markManualStimDirty](int) { markManualStimDirty(); });
    connect(m_spinManualPulseCount, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [markManualStimDirty](int) { markManualStimDirty(); });
    connect(m_spinManualFirstPhaseUs, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [markManualStimDirty](int) { markManualStimDirty(); });
    connect(m_spinManualSecondPhaseUs, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [markManualStimDirty](int) { markManualStimDirty(); });
    connect(m_spinManualInterphaseUs, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [markManualStimDirty](int) { markManualStimDirty(); });
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

void MainWindow::commitManualStimEdits()
{
    if (m_spinManualStimElectrode) m_spinManualStimElectrode->interpretText();
    if (m_spinManualTriggerSource) m_spinManualTriggerSource->interpretText();
    if (m_spinManualFirstAmp) m_spinManualFirstAmp->interpretText();
    if (m_spinManualSecondAmp) m_spinManualSecondAmp->interpretText();
    if (m_spinManualPulseCount) m_spinManualPulseCount->interpretText();
    if (m_spinManualFirstPhaseUs) m_spinManualFirstPhaseUs->interpretText();
    if (m_spinManualSecondPhaseUs) m_spinManualSecondPhaseUs->interpretText();
    if (m_spinManualInterphaseUs) m_spinManualInterphaseUs->interpretText();
}

QString MainWindow::buildManualStimSummary() const
{
    const QString electrodeName = manualStimElectrodeName();
    const int triggerSource = m_spinManualTriggerSource ? m_spinManualTriggerSource->value() : 0;
    const int firstAmp = m_spinManualFirstAmp ? m_spinManualFirstAmp->value() : 20;
    const int secondAmp = m_spinManualSecondAmp ? m_spinManualSecondAmp->value() : 20;
    const int pulses = m_spinManualPulseCount ? m_spinManualPulseCount->value() : 1;
    const int firstPhaseUs = m_spinManualFirstPhaseUs ? m_spinManualFirstPhaseUs->value() : 500;
    const int secondPhaseUs = m_spinManualSecondPhaseUs ? m_spinManualSecondPhaseUs->value() : 500;
    const int interphaseUs = m_spinManualInterphaseUs ? m_spinManualInterphaseUs->value() : 500;

    return QStringLiteral("电极=%1 trigger=%2 一相=%3 uA 二相=%4 uA 脉冲=%5 时宽=%6/%7 us 间隔=%8 us")
        .arg(electrodeName)
        .arg(triggerSource)
        .arg(firstAmp)
        .arg(secondAmp)
        .arg(pulses)
        .arg(firstPhaseUs)
        .arg(secondPhaseUs)
        .arg(interphaseUs);
}

QString MainWindow::buildManualStimSignature() const
{
    const QString electrodeName = manualStimElectrodeName();
    const int triggerSource = m_spinManualTriggerSource ? m_spinManualTriggerSource->value() : 0;
    const int firstAmp = m_spinManualFirstAmp ? m_spinManualFirstAmp->value() : 20;
    const int secondAmp = m_spinManualSecondAmp ? m_spinManualSecondAmp->value() : 20;
    const int pulses = m_spinManualPulseCount ? m_spinManualPulseCount->value() : 1;
    const int firstPhaseUs = m_spinManualFirstPhaseUs ? m_spinManualFirstPhaseUs->value() : 500;
    const int secondPhaseUs = m_spinManualSecondPhaseUs ? m_spinManualSecondPhaseUs->value() : 500;
    const int interphaseUs = m_spinManualInterphaseUs ? m_spinManualInterphaseUs->value() : 500;

    return QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8")
        .arg(electrodeName)
        .arg(triggerSource)
        .arg(firstAmp)
        .arg(secondAmp)
        .arg(pulses)
        .arg(firstPhaseUs)
        .arg(secondPhaseUs)
        .arg(interphaseUs);
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

StimStepSize MainWindow::selectedStimStepSize() const
{
    if (!m_cmbStimStepSize) {
        return StimStepSize500nA;
    }

    bool ok = false;
    const int rawValue = m_cmbStimStepSize->currentData().toInt(&ok);
    if (!ok || rawValue < int(StimStepSize10nA) || rawValue > int(StimStepSize10uA)) {
        return StimStepSize500nA;
    }

    return static_cast<StimStepSize>(rawValue);
}

bool MainWindow::ensureStimStepSizeAppliedForMode(const QString &modeLabel)
{
    if (!m_engine) {
        return false;
    }
    if (!m_engine->rhx() || !m_engine->stimController()) {
        return true;
    }

    const StimStepSize stepSize = selectedStimStepSize();
    const bool wasRunning = m_engine->isContinuousRunning();

    m_engine->setStimStepSize(stepSize);
    if (!m_engine->hasPendingStimStepSizeApply()) {
        return true;
    }

    if (wasRunning) {
        appendLog(QStringLiteral("%1：刺激量程/步进已变更，正在重启采集以应用新档位").arg(modeLabel));
        m_engine->stopAcquisition();
    }

    m_engine->setStimStepSize(stepSize);
    if (m_engine->hasPendingStimStepSizeApply()) {
        appendLog(QStringLiteral("%1：刺激量程/步进应用失败，请重新打开设备后再试").arg(modeLabel));
        if (wasRunning) {
            m_engine->startContinuousAcquisition();
            if (m_engine->isContinuousRunning()) {
                appendLog(QStringLiteral("%1：已恢复采集，但新的刺激步进尚未生效").arg(modeLabel));
            }
        }
        return false;
    }

    if (wasRunning) {
        m_engine->startContinuousAcquisition();
        if (!m_engine->isContinuousRunning()) {
            appendLog(QStringLiteral("%1：刺激量程/步进已更新，但采集恢复失败").arg(modeLabel));
            return false;
        }
    }

    return true;
}

void MainWindow::updateFixedStimAmplitudeControl()
{
    if (!m_spinFixedStimAmp) {
        return;
    }

    const double step_uA = RHXRegisters::stimStepSizeToDouble(selectedStimStepSize()) * 1.0e6;
    const double safeStep_uA = (step_uA > 0.0) ? step_uA : 0.5;
    const double maxAmp_uA = 255.0 * safeStep_uA;

    int decimals = 0;
    double scaledStep = safeStep_uA;
    while (decimals < 3 && qAbs(scaledStep - qRound(scaledStep)) > 1.0e-9) {
        scaledStep *= 10.0;
        ++decimals;
    }

    const double currentValue = m_spinFixedStimAmp->value();
    double snappedValue = qRound(currentValue / safeStep_uA) * safeStep_uA;
    if (snappedValue < safeStep_uA) {
        snappedValue = safeStep_uA;
    }
    if (snappedValue > maxAmp_uA) {
        snappedValue = maxAmp_uA;
    }

    QSignalBlocker blocker(m_spinFixedStimAmp);
    m_spinFixedStimAmp->setDecimals(decimals);
    m_spinFixedStimAmp->setRange(safeStep_uA, maxAmp_uA);
    m_spinFixedStimAmp->setSingleStep(safeStep_uA);
    m_spinFixedStimAmp->setValue(snappedValue);
    m_spinFixedStimAmp->setToolTip(QStringLiteral("当前全局刺激档位：%1，可设置范围 0 ~ %2 uA")
                                       .arg(stimStepSizeDisplayText(selectedStimStepSize()))
                                       .arg(maxAmp_uA, 0, 'f', decimals));
}

bool MainWindow::configureManualStimHardware(QString *summary)
{
    const QString detail = buildManualStimSummary();
    if (summary) {
        *summary = detail;
    }

    if (!m_engine || !m_engine->rhx() || !m_engine->stimController()) {
        return false;
    }

    if (m_closedLoopExperimentActive) {
        appendLog(QStringLiteral("闭环实验运行中，不能应用普通采集刺激配置"));
        return false;
    }

    const QString electrodeName = manualStimElectrodeName();
    const int triggerSource = m_spinManualTriggerSource ? m_spinManualTriggerSource->value() : 0;
    const int firstAmp = m_spinManualFirstAmp ? m_spinManualFirstAmp->value() : 20;
    const int secondAmp = m_spinManualSecondAmp ? m_spinManualSecondAmp->value() : 20;
    const int pulses = m_spinManualPulseCount ? m_spinManualPulseCount->value() : 1;
    const int firstPhaseUs = m_spinManualFirstPhaseUs ? m_spinManualFirstPhaseUs->value() : 500;
    const int secondPhaseUs = m_spinManualSecondPhaseUs ? m_spinManualSecondPhaseUs->value() : 500;
    const int interphaseUs = m_spinManualInterphaseUs ? m_spinManualInterphaseUs->value() : 500;
    // Manual GUI is entered in uA, while configureStim() expects nA.
    const int firstAmp_nA = firstAmp * 1000;
    const int secondAmp_nA = secondAmp * 1000;

    const bool resumeContinuous = m_engine->isContinuousRunning();
    if (resumeContinuous) {
        appendLog(QStringLiteral("普通采集刺激配置：暂停普通采集，按官方流程下发刺激参数"));
        m_engine->stopAcquisition();
    } else {
        appendLog(QStringLiteral("普通采集刺激配置：按官方流程下发刺激参数"));
    }

    m_engine->setStimStepSize(selectedStimStepSize());
    if (m_engine->hasPendingStimStepSizeApply()) {
        appendLog(QStringLiteral("普通采集刺激配置：未能应用刺激量程/步进，请重新打开设备后再试"));
        if (resumeContinuous) {
            m_engine->startContinuousAcquisition();
        }
        return false;
    }

    m_engine->configureStim(electrodeName,
                            firstAmp_nA,
                            secondAmp_nA,
                            firstPhaseUs,
                            secondPhaseUs,
                            interphaseUs,
                            pulses,
                            triggerSource);

    if (resumeContinuous) {
        m_engine->startContinuousAcquisition();
        if (!m_engine->isContinuousRunning()) {
            appendLog(QStringLiteral("普通采集刺激配置：刺激参数已写入，但普通采集恢复失败"));
        }
    }

    m_manualStimLoadedForCurrentRun = true;
    return true;
}

void MainWindow::applyManualStimConfigFromUi()
{
    commitManualStimEdits();
    saveManualStimConfig();

    const QString summary = buildManualStimSummary();
    const QString signature = buildManualStimSignature();
    if (!m_engine || !m_engine->rhx() || !m_engine->stimController()) {
        m_manualStimConfigApplied = false;
        m_manualStimConfigDirty = true;
        m_manualStimLoadedForCurrentRun = false;
        m_manualStimAppliedSummary.clear();
        m_manualStimAppliedSignature.clear();
        appendLog(QStringLiteral("普通采集刺激参数已保存：%1（打开设备并开始普通采集后再点击应用）").arg(summary));
        return;
    }

    QString appliedSummary;
    if (!configureManualStimHardware(&appliedSummary)) {
        m_manualStimConfigApplied = false;
        m_manualStimConfigDirty = true;
        m_manualStimLoadedForCurrentRun = false;
        m_manualStimAppliedSummary.clear();
        m_manualStimAppliedSignature.clear();
        return;
    }

    m_manualStimConfigApplied = true;
    m_manualStimConfigDirty = false;
    m_manualStimLoadedForCurrentRun = true;
    m_manualStimAppliedSummary = appliedSummary;
    m_manualStimAppliedSignature = signature;
    appendLog(QStringLiteral("普通采集刺激参数已应用：%1").arg(appliedSummary));
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
    m_spinClosedLoopRounds = new QSpinBox(gbExperiment);
    m_spinClosedLoopRounds->setRange(1, 100000);
    m_spinClosedLoopRounds->setSingleStep(1);
    m_spinClosedLoopRounds->setValue(1);
    m_spinClosedLoopMaxStimPerEpoch = new QSpinBox(gbExperiment);
    m_spinClosedLoopMaxStimPerEpoch->setRange(1, 1000);
    m_spinClosedLoopMaxStimPerEpoch->setSingleStep(1);
    m_spinClosedLoopMaxStimPerEpoch->setValue(m_closedLoopMaxStimPerEpoch);
    experimentForm->addRow(tr("AB Epoch 时长"), m_spinEpochSec);
    experimentForm->addRow(tr("Rounds"), m_spinClosedLoopRounds);
    experimentForm->addRow(QString::fromUtf8(u8"每个 Epoch 最多刺激数"), m_spinClosedLoopMaxStimPerEpoch);
    experimentLayout->addLayout(experimentForm);

    QGroupBox *gbFixedReplay = new QGroupBox(tr("固定刺激实验参数"), panel);
    QFormLayout *fixedReplayLayout = new QFormLayout(gbFixedReplay);
    m_spinFixedStimAmp = new QDoubleSpinBox(gbFixedReplay);
    m_spinFixedStimAmp->setDecimals(1);
    m_spinFixedStimAmp->setRange(0.5, 127.5);
    m_spinFixedStimAmp->setSingleStep(0.5);
    m_spinFixedStimAmp->setValue(20.0);
    m_spinFixedStimAmp->setSuffix(" uA");
    m_spinFixedStimPhaseUs = new QSpinBox(gbFixedReplay);
    m_spinFixedStimPhaseUs->setRange(1, 20000);
    m_spinFixedStimPhaseUs->setSingleStep(10);
    m_spinFixedStimPhaseUs->setValue(500);
    m_spinFixedStimPhaseUs->setSuffix(" us");
    m_spinFixedStimFreqHz = new QDoubleSpinBox(gbFixedReplay);
    m_spinFixedStimFreqHz->setRange(0.1, 1000.0);
    m_spinFixedStimFreqHz->setDecimals(2);
    m_spinFixedStimFreqHz->setSingleStep(0.5);
    m_spinFixedStimFreqHz->setValue(10.0);
    m_spinFixedStimFreqHz->setSuffix(" Hz");
    fixedReplayLayout->addRow(QString::fromUtf8(u8"固定频率"), m_spinFixedStimFreqHz);
    fixedReplayLayout->addRow(tr("固定振幅"), m_spinFixedStimAmp);
    fixedReplayLayout->addRow(tr("固定相宽"), m_spinFixedStimPhaseUs);
    updateFixedStimAmplitudeControl();

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
    root->addWidget(gbFixedReplay);
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

void MainWindow::setupFixedStimDock()
{
    if (m_spinFixedStimAmp) {
        QWidget *oldGroup = m_spinFixedStimAmp->parentWidget();
        while (oldGroup && !qobject_cast<QGroupBox *>(oldGroup)) {
            oldGroup = oldGroup->parentWidget();
        }
        if (oldGroup) {
            oldGroup->hide();
        }
    }

    m_dockFixedStim = new QDockWidget(tr("固定刺激实验配置"), this);
    m_dockFixedStim->setObjectName("dockFixedStimConfig");
    m_dockFixedStim->setAllowedAreas(Qt::RightDockWidgetArea | Qt::LeftDockWidgetArea);

    QWidget *panel = new QWidget(m_dockFixedStim);
    QVBoxLayout *root = new QVBoxLayout(panel);

    QGroupBox *gbFixedStim = new QGroupBox(tr("固定刺激实验参数"), panel);
    QVBoxLayout *groupLayout = new QVBoxLayout(gbFixedStim);

    QLabel *tip = new QLabel(tr("这一栏只影响固定刺激实验：在固定时间窗内按固定频率、固定振幅和固定相宽连续刺激。"), gbFixedStim);
    tip->setWordWrap(true);
    groupLayout->addWidget(tip);

    QFormLayout *form = new QFormLayout();

    m_spinFixedStimFreqHz = new QDoubleSpinBox(gbFixedStim);
    m_spinFixedStimFreqHz->setRange(0.1, 1000.0);
    m_spinFixedStimFreqHz->setDecimals(2);
    m_spinFixedStimFreqHz->setSingleStep(0.5);
    m_spinFixedStimFreqHz->setValue(10.0);
    m_spinFixedStimFreqHz->setSuffix(" Hz");

    m_spinFixedStimRounds = new QSpinBox(gbFixedStim);
    m_spinFixedStimRounds->setRange(1, 100000);
    m_spinFixedStimRounds->setSingleStep(1);
    m_spinFixedStimRounds->setValue(1);

    m_spinFixedStimAmp = new QDoubleSpinBox(gbFixedStim);
    m_spinFixedStimAmp->setDecimals(1);
    m_spinFixedStimAmp->setRange(0.5, 127.5);
    m_spinFixedStimAmp->setSingleStep(0.5);
    m_spinFixedStimAmp->setValue(20.0);
    m_spinFixedStimAmp->setSuffix(" uA");

    m_spinFixedStimPhaseUs = new QSpinBox(gbFixedStim);
    m_spinFixedStimPhaseUs->setRange(1, 20000);
    m_spinFixedStimPhaseUs->setSingleStep(10);
    m_spinFixedStimPhaseUs->setValue(500);
    m_spinFixedStimPhaseUs->setSuffix(" us");

    m_spinFixedStimCollectPreSec = new QDoubleSpinBox(gbFixedStim);
    m_spinFixedStimCollectPreSec->setRange(0.1, 36000.0);
    m_spinFixedStimCollectPreSec->setDecimals(2);
    m_spinFixedStimCollectPreSec->setSingleStep(1.0);
    m_spinFixedStimCollectPreSec->setValue(60.0);
    m_spinFixedStimCollectPreSec->setSuffix(" s");

    m_spinFixedStimWindowSec = new QDoubleSpinBox(gbFixedStim);
    m_spinFixedStimWindowSec->setRange(0.1, 36000.0);
    m_spinFixedStimWindowSec->setDecimals(2);
    m_spinFixedStimWindowSec->setSingleStep(1.0);
    m_spinFixedStimWindowSec->setValue(60.0);
    m_spinFixedStimWindowSec->setSuffix(" s");

    m_spinFixedStimCollectPostSec = new QDoubleSpinBox(gbFixedStim);
    m_spinFixedStimCollectPostSec->setRange(0.1, 36000.0);
    m_spinFixedStimCollectPostSec->setDecimals(2);
    m_spinFixedStimCollectPostSec->setSingleStep(1.0);
    m_spinFixedStimCollectPostSec->setValue(60.0);
    m_spinFixedStimCollectPostSec->setSuffix(" s");

    m_spinFixedStimIdleSec = new QDoubleSpinBox(gbFixedStim);
    m_spinFixedStimIdleSec->setRange(0.0, 36000.0);
    m_spinFixedStimIdleSec->setDecimals(2);
    m_spinFixedStimIdleSec->setSingleStep(1.0);
    m_spinFixedStimIdleSec->setValue(60.0);
    m_spinFixedStimIdleSec->setSuffix(" s");

    form->addRow(QString::fromUtf8(u8"轮次"), m_spinFixedStimRounds);
    form->addRow(QString::fromUtf8(u8"固定频率"), m_spinFixedStimFreqHz);
    form->addRow(tr("固定振幅"), m_spinFixedStimAmp);
    form->addRow(tr("固定相宽"), m_spinFixedStimPhaseUs);
    form->addRow(QString::fromUtf8(u8"前采集时长"), m_spinFixedStimCollectPreSec);
    form->addRow(QString::fromUtf8(u8"刺激窗口时长"), m_spinFixedStimWindowSec);
    form->addRow(QString::fromUtf8(u8"后采集时长"), m_spinFixedStimCollectPostSec);
    form->addRow(QString::fromUtf8(u8"空窗时长"), m_spinFixedStimIdleSec);
    groupLayout->addLayout(form);

    root->addWidget(gbFixedStim);
    root->addStretch(1);

    panel->setLayout(root);
    m_dockFixedStim->setWidget(panel);
    addDockWidget(Qt::RightDockWidgetArea, m_dockFixedStim);
    if (m_dockElectrode) {
        splitDockWidget(m_dockElectrode, m_dockFixedStim, Qt::Vertical);
    }

    updateFixedStimAmplitudeControl();
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
    if (m_spinClosedLoopRounds) {
        const int savedRounds = qMax(1, s.value("closed_loop/rounds", m_spinClosedLoopRounds->value()).toInt());
        QSignalBlocker blocker(m_spinClosedLoopRounds);
        m_spinClosedLoopRounds->setValue(savedRounds);
    }
    if (m_spinClosedLoopMaxStimPerEpoch) {
        m_closedLoopMaxStimPerEpoch =
            qMax(1, s.value("closed_loop/max_stim_per_epoch", m_closedLoopMaxStimPerEpoch).toInt());
        QSignalBlocker blocker(m_spinClosedLoopMaxStimPerEpoch);
        m_spinClosedLoopMaxStimPerEpoch->setValue(m_closedLoopMaxStimPerEpoch);
    }
    if (m_spinFixedStimAmp) {
        const double savedAmp = qMax(0.0, s.value("fixed_stim/amp_uA", m_spinFixedStimAmp->value()).toDouble());
        QSignalBlocker blocker(m_spinFixedStimAmp);
        m_spinFixedStimAmp->setValue(savedAmp);
    }
    if (m_spinFixedStimPhaseUs) {
        const int savedPhaseUs = qMax(1, s.value("fixed_stim/phase_us", m_spinFixedStimPhaseUs->value()).toInt());
        QSignalBlocker blocker(m_spinFixedStimPhaseUs);
        m_spinFixedStimPhaseUs->setValue(savedPhaseUs);
    }
    if (m_spinFixedStimFreqHz) {
        const double savedFreqHz = qMax(0.1, s.value("fixed_stim/freq_hz", m_spinFixedStimFreqHz->value()).toDouble());
        QSignalBlocker blocker(m_spinFixedStimFreqHz);
        m_spinFixedStimFreqHz->setValue(savedFreqHz);
    }
    if (m_spinFixedStimRounds) {
        const int savedRounds = qMax(1, s.value("fixed_stim/rounds", m_spinFixedStimRounds->value()).toInt());
        QSignalBlocker blocker(m_spinFixedStimRounds);
        m_spinFixedStimRounds->setValue(savedRounds);
    }
    if (m_spinFixedStimCollectPreSec) {
        const double savedSec = qMax(0.1, s.value("fixed_stim/collect_pre_sec", m_spinFixedStimCollectPreSec->value()).toDouble());
        QSignalBlocker blocker(m_spinFixedStimCollectPreSec);
        m_spinFixedStimCollectPreSec->setValue(savedSec);
    }
    if (m_spinFixedStimWindowSec) {
        const double savedSec = qMax(0.1, s.value("fixed_stim/stim_window_sec", m_spinFixedStimWindowSec->value()).toDouble());
        QSignalBlocker blocker(m_spinFixedStimWindowSec);
        m_spinFixedStimWindowSec->setValue(savedSec);
    }
    if (m_spinFixedStimCollectPostSec) {
        const double savedSec = qMax(0.1, s.value("fixed_stim/collect_post_sec", m_spinFixedStimCollectPostSec->value()).toDouble());
        QSignalBlocker blocker(m_spinFixedStimCollectPostSec);
        m_spinFixedStimCollectPostSec->setValue(savedSec);
    }
    if (m_spinFixedStimIdleSec) {
        const double savedSec = qMax(0.0, s.value("fixed_stim/idle_sec", m_spinFixedStimIdleSec->value()).toDouble());
        QSignalBlocker blocker(m_spinFixedStimIdleSec);
        m_spinFixedStimIdleSec->setValue(savedSec);
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
    m_coordinator->setMaxStimPerEpoch(m_spinClosedLoopMaxStimPerEpoch
                                          ? m_spinClosedLoopMaxStimPerEpoch->value()
                                          : m_closedLoopMaxStimPerEpoch);
}

void MainWindow::saveElectrodeConfig() const
{
    if (!m_dockElectrode) return;

    QSettings s;
    s.setValue("closed_loop/epoch_sec", m_spinEpochSec ? m_spinEpochSec->value() : colletion_time);
    s.setValue("closed_loop/rounds", m_spinClosedLoopRounds ? m_spinClosedLoopRounds->value() : 1);
    s.setValue("closed_loop/max_stim_per_epoch",
               m_spinClosedLoopMaxStimPerEpoch ? m_spinClosedLoopMaxStimPerEpoch->value() : m_closedLoopMaxStimPerEpoch);
    s.setValue("fixed_stim/amp_uA", m_spinFixedStimAmp ? m_spinFixedStimAmp->value() : 20);
    s.setValue("fixed_stim/phase_us", m_spinFixedStimPhaseUs ? m_spinFixedStimPhaseUs->value() : 500);
    s.setValue("fixed_stim/freq_hz", m_spinFixedStimFreqHz ? m_spinFixedStimFreqHz->value() : 10.0);
    s.setValue("fixed_stim/rounds", m_spinFixedStimRounds ? m_spinFixedStimRounds->value() : 1);
    s.setValue("fixed_stim/collect_pre_sec", m_spinFixedStimCollectPreSec ? m_spinFixedStimCollectPreSec->value() : 60.0);
    s.setValue("fixed_stim/stim_window_sec", m_spinFixedStimWindowSec ? m_spinFixedStimWindowSec->value() : 60.0);
    s.setValue("fixed_stim/collect_post_sec", m_spinFixedStimCollectPostSec ? m_spinFixedStimCollectPostSec->value() : 60.0);
    s.setValue("fixed_stim/idle_sec", m_spinFixedStimIdleSec ? m_spinFixedStimIdleSec->value() : 60.0);
    s.setValue("electrode/sense_A_a", m_editSense_A_a->text().trimmed());
    s.setValue("electrode/sense_A_b", m_editSense_A_b->text().trimmed());
    s.setValue("electrode/sense_B_a", m_editSense_B_a->text().trimmed());
    s.setValue("electrode/sense_B_b", m_editSense_B_b->text().trimmed());

    s.setValue("electrode/stim_A_a_num", m_spinStim_A_a->value());
    s.setValue("electrode/stim_A_b_num", m_spinStim_A_b->value());
    s.setValue("electrode/stim_B_a_num", m_spinStim_B_a->value());
    s.setValue("electrode/stim_B_b_num", m_spinStim_B_b->value());
}

void MainWindow::loadSessionConfig()
{
    QSettings s;
    m_sessionRootDir = s.value("session/root_dir", QString()).toString().trimmed();
    if (m_cmbStimStepSize) {
        const int savedStimStep =
            s.value("session/stim_step_size",
                    s.value("manual_stim/stim_step_size", int(StimStepSize500nA))).toInt();
        const int comboIndex = qMax(0, m_cmbStimStepSize->findData(savedStimStep));
        m_cmbStimStepSize->setCurrentIndex(comboIndex);
    }
    updateFixedStimAmplitudeControl();
    if (m_btnSelectSaveDir && !m_sessionRootDir.isEmpty()) {
        m_btnSelectSaveDir->setToolTip(m_sessionRootDir);
    }
}

void MainWindow::saveSessionConfig() const
{
    QSettings s;
    s.setValue("session/root_dir", m_sessionRootDir);
    s.setValue("session/stim_step_size",
               m_cmbStimStepSize ? m_cmbStimStepSize->currentData().toInt() : int(StimStepSize500nA));
}

bool MainWindow::ensureSessionRootSelected()
{
    if (!m_sessionRootDir.isEmpty() && QDir(m_sessionRootDir).exists()) {
        return true;
    }

    onSelectRecordingLocation();
    return !m_sessionRootDir.isEmpty() && QDir(m_sessionRootDir).exists();
}

bool MainWindow::prepareManagedSession(const QString &sessionPrefix)
{
    resetManagedSessionState();

    if (!ensureSessionRootSelected()) {
        return false;
    }

    QDir root(m_sessionRootDir);
    if (!root.exists() && !root.mkpath(".")) {
        return false;
    }

    const QString dirName = QString("%1_%2")
                                .arg(sessionPrefix)
                                .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz"));
    if (!root.mkpath(dirName)) {
        return false;
    }

    m_activeSessionDir = root.filePath(dirName);
    m_activeRecordingPath = QDir(m_activeSessionDir).filePath("recording.bar");
    m_activeStimPlanPath = QDir(m_activeSessionDir).filePath("stim_plan.json");
    m_managedStimEvents.clear();
    return true;
}

bool MainWindow::startManagedRecordingInActiveSession()
{
    if (!m_engine || m_activeRecordingPath.isEmpty()) {
        return false;
    }

    if (!m_engine->startBinaryRecording(m_activeRecordingPath)) {
        return false;
    }

    if (m_stimLog) {
        m_stimLog->start(QDir(m_activeSessionDir).filePath("stim_log.csv"));
    }
    return true;
}

void MainWindow::finalizeManagedSession()
{
    if (m_managedSessionMode == ManagedSessionMode::None || m_activeSessionDir.isEmpty()) {
        resetManagedSessionState();
        return;
    }

    const qint64 durationMs = m_managedSessionClockActive ? m_managedSessionClock.elapsed() : 0;

    if (m_managedSessionMode == ManagedSessionMode::ClosedLoop) {
        if (writeStimPlanJson(durationMs)) {
            appendLog(QStringLiteral("Stim plan JSON saved: %1").arg(m_activeStimPlanPath));
        } else {
            appendLog(QStringLiteral("Failed to save stim plan JSON: %1").arg(m_activeStimPlanPath));
        }
    } else if (m_managedSessionMode == ManagedSessionMode::VoidStim && !m_selectedReplayPlanPath.isEmpty()) {
        if (QFile::exists(m_activeStimPlanPath)) {
            QFile::remove(m_activeStimPlanPath);
        }
        if (QFile::copy(m_selectedReplayPlanPath, m_activeStimPlanPath)) {
            appendLog(QStringLiteral("Replay stim plan copied to session: %1").arg(m_activeStimPlanPath));
        } else {
            appendLog(QStringLiteral("Failed to copy replay stim plan into session folder."));
        }
    } else if (m_managedSessionMode == ManagedSessionMode::FixedStim) {
        if (writeFixedStimSessionJson(durationMs)) {
            appendLog(QStringLiteral("Fixed-stim session JSON saved: %1").arg(m_activeStimPlanPath));
        } else {
            appendLog(QStringLiteral("Failed to save fixed-stim session JSON: %1").arg(m_activeStimPlanPath));
        }
    }

    resetManagedSessionState();
}

void MainWindow::resetManagedSessionState()
{
    m_activeSessionDir.clear();
    m_activeRecordingPath.clear();
    m_activeStimPlanPath.clear();
    m_selectedReplayPlanPath.clear();
    m_managedSessionMode = ManagedSessionMode::None;
    m_managedSessionClock.invalidate();
    m_managedSessionClockActive = false;
    m_managedStimEvents.clear();
    m_closedLoopCompletedRounds = 0;
    m_voidStimActive = false;
    m_activeFixedStimElectrode.clear();
    m_activeFixedStimAmp_uA = 0;
    m_activeFixedStimPhaseUs = 0;
    m_activeFixedStimFreqHz = 0.0;
    m_activeFixedStimTriggerSource = 0;
    m_activeFixedStimRounds = 0;
    m_activeFixedStimPulsesPerTrain = 0;
    m_activeFixedStimCollectPreMs = 60000;
    m_activeFixedStimWindowMs = 60000;
    m_activeFixedStimCollectPostMs = 60000;
    m_activeFixedStimIdleMs = 60000;
}

bool MainWindow::writeStimPlanJson(qint64 durationMs) const
{
    if (m_activeStimPlanPath.isEmpty()) {
        return false;
    }

    QJsonArray events;
    for (const StimEventRecord &ev : m_managedStimEvents) {
        QJsonObject obj;
        obj["epoch_id"] = ev.epochId;
        obj["source_phase_index"] = ev.phaseIndex;
        obj["source_phase"] = (ev.phaseIndex == 0) ? "A" : "B";
        obj["item_index"] = ev.itemIndex;
        obj["planned_time_ms"] = QString::number(ev.plannedTimeMs);
        obj["fired_time_ms"] = QString::number(ev.firedTimeMs);
        obj["time_ms"] = QString::number(ev.firedTimeMs >= 0 ? ev.firedTimeMs : ev.plannedTimeMs);
        obj["target_electrode"] = ev.electrode;
        obj["amp_uA"] = ev.amp_uA;
        obj["pulses"] = ev.pulses;
        obj["channel_index"] = ev.ch;
        obj["spike_uV"] = ev.spike_uV;
        obj["trigger_source"] = ev.triggerSource;
        events.append(obj);
    }

    QJsonObject root;
    root["schema_version"] = 1;
    root["session_type"] = "closed_loop";
    root["created_at_iso"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    root["experiment_duration_ms"] = QString::number(qMax<qint64>(0, durationMs));
    root["epoch_sec"] = colletion_time;
    root["rounds_target"] = m_spinClosedLoopRounds ? m_spinClosedLoopRounds->value() : 1;
    root["rounds_completed"] = m_closedLoopCompletedRounds;
    root["max_stim_per_epoch"] = m_closedLoopMaxStimPerEpoch;
    root["stim_step_size_enum"] = int(selectedStimStepSize());
    root["stim_step_size"] = stimStepSizeDisplayText(selectedStimStepSize());
    root["recording_file"] = QFileInfo(m_activeRecordingPath).fileName();
    root["stim_log_file"] = QStringLiteral("stim_log.csv");
    root["events"] = events;

    QFile file(m_activeStimPlanPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    const QJsonDocument doc(root);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

bool MainWindow::writeFixedStimSessionJson(qint64 durationMs) const
{
    if (m_activeStimPlanPath.isEmpty()) {
        return false;
    }

    QJsonArray events;
    for (const StimEventRecord &ev : m_managedStimEvents) {
        QJsonObject obj;
        obj["item_index"] = ev.itemIndex;
        obj["planned_time_ms"] = QString::number(ev.plannedTimeMs);
        obj["fired_time_ms"] = QString::number(ev.firedTimeMs);
        obj["time_ms"] = QString::number(ev.firedTimeMs >= 0 ? ev.firedTimeMs : ev.plannedTimeMs);
        obj["target_electrode"] = ev.electrode;
        obj["amp_uA"] = m_activeFixedStimAmp_uA;
        obj["phase_us"] = m_activeFixedStimPhaseUs;
        obj["frequency_hz"] = m_activeFixedStimFreqHz;
        obj["pulses"] = ev.pulses;
        obj["trigger_source"] = ev.triggerSource;
        events.append(obj);
    }

    const qint64 collectPreMs = qMax<qint64>(0, m_activeFixedStimCollectPreMs);
    const qint64 stimWindowMs = qMax<qint64>(1, m_activeFixedStimWindowMs);
    const qint64 collectPostMs = qMax<qint64>(0, m_activeFixedStimCollectPostMs);
    const qint64 idleMs = qMax<qint64>(0, m_activeFixedStimIdleMs);
    const qint64 roundDurationMs = collectPreMs + stimWindowMs + collectPostMs + idleMs;

    QJsonObject root;
    root["schema_version"] = 1;
    root["session_type"] = "fixed_stim";
    root["created_at_iso"] = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    root["experiment_duration_ms"] = QString::number(qMax<qint64>(0, durationMs));
    root["rounds_target"] = m_activeFixedStimRounds > 0 ? m_activeFixedStimRounds : 1;
    root["round_duration_ms"] = QString::number(roundDurationMs);
    root["collect_pre_ms"] = QString::number(collectPreMs);
    root["stim_window_start_ms"] = QString::number(collectPreMs);
    root["stim_window_duration_ms"] = QString::number(stimWindowMs);
    root["collect_post_ms"] = QString::number(collectPostMs);
    root["idle_ms"] = QString::number(idleMs);
    root["recording_file"] = QFileInfo(m_activeRecordingPath).fileName();
    root["stim_log_file"] = QStringLiteral("stim_log.csv");
    root["target_electrode"] = m_activeFixedStimElectrode;
    root["trigger_source"] = m_activeFixedStimTriggerSource;
    root["stim_step_size_enum"] = int(selectedStimStepSize());
    root["stim_step_size"] = stimStepSizeDisplayText(selectedStimStepSize());
    root["fixed_waveform"] = QStringLiteral("symmetric_biphasic");
    root["fixed_amplitude_uA"] = m_activeFixedStimAmp_uA;
    root["fixed_phase_us"] = m_activeFixedStimPhaseUs;
    root["fixed_frequency_hz"] = m_activeFixedStimFreqHz;
    root["fixed_num_pulses_per_train"] = m_activeFixedStimPulsesPerTrain;
    root["fixed_interphase_us"] = 0;
    root["events"] = events;

    QFile file(m_activeStimPlanPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    file.close();
    return true;
}

bool MainWindow::loadStimPlanJson(const QString &filePath, QByteArray *jsonBytes) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    const QByteArray bytes = file.readAll();
    file.close();

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }

    if (jsonBytes) {
        *jsonBytes = bytes;
    }
    return true;
}

void MainWindow::stopVoidStimReplay(bool logMessage)
{
    ++m_voidStimReplayToken;
    const bool wasActive = m_voidStimActive;
    m_voidStimActive = false;

    if (wasActive && logMessage) {
        appendLog(QStringLiteral("Virtual stimulation stopped."));
    }
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
    m_closedLoopMaxStimPerEpoch = m_spinClosedLoopMaxStimPerEpoch
                                      ? m_spinClosedLoopMaxStimPerEpoch->value()
                                      : m_closedLoopMaxStimPerEpoch;

    saveElectrodeConfig();
    syncExperimentRoutingConfig();

    appendLog(QStringLiteral("闭环实验配置已应用：Epoch=%1 s | MaxStim/Epoch=%2 | SenseA(a)=[%3] SenseA(b)=[%4] SenseB(a')=[%5] SenseB(b')=[%6] | StimA(a)=%7 StimA(b)=%8 StimB(a')=%9 StimB(b')=%10")
                  .arg(colletion_time, 0, 'f', 2)
                  .arg(m_closedLoopMaxStimPerEpoch)
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

void MainWindow::onSelectRecordingLocation()
{
    const QString startDir = !m_sessionRootDir.isEmpty() ? m_sessionRootDir : QDir::currentPath();
    const QString selectedDir = QFileDialog::getExistingDirectory(
        this,
        QString::fromUtf8(u8"选择录制位置"),
        startDir,
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (selectedDir.isEmpty()) return;

    m_sessionRootDir = selectedDir;
    saveSessionConfig();

    if (m_btnSelectSaveDir) {
        m_btnSelectSaveDir->setToolTip(m_sessionRootDir);
    }

    appendLog(QStringLiteral("Session root selected: %1").arg(m_sessionRootDir));
}

void MainWindow::onOpenDevice()
{
    commitManualStimEdits();
    saveManualStimConfig();

    QString path = QFileDialog::getOpenFileName(
        this,
        tr("选择 ConfigRHSController_7310.bit"),
        QString(),
        tr("Bitfile (*.bit);;All Files (*.*)")
        );
    if (path.isEmpty()) return;

    if (!m_engine->openDevice(path)) {
        appendLog("打开设备失败");
        return;
    }

    m_manualStimConfigApplied = false;
    m_manualStimConfigDirty = true;
    m_manualStimLoadedForCurrentRun = false;
    m_manualStimAppliedSummary.clear();
    m_manualStimAppliedSignature.clear();
    ensureStimStepSizeAppliedForMode(QStringLiteral("打开设备"));
    appendLog("打开设备成功");
}

void MainWindow::onStart()
{
    if (!m_engine) return;

    commitManualStimEdits();
    saveManualStimConfig();
    if (!ensureStimStepSizeAppliedForMode(QStringLiteral("普通采集"))) {
        return;
    }

    const bool wasAcquiring = m_engine->isContinuousRunning();
    const bool wasClosedLoop = m_closedLoopExperimentActive;
    const bool wasVoidStim = m_voidStimActive;
    if (wasVoidStim) {
        stopVoidStimReplay(false);
    }
    if (m_engine && m_engine->isRecording() && m_managedSessionMode != ManagedSessionMode::None) {
        m_engine->stopBinaryRecording();
        if (m_stimLog) {
            m_stimLog->stop();
        }
    }
    if (m_coordinator) m_coordinator->endRun();
    finalizeManagedSession();
    if (m_coordinator) m_coordinator->cancelPendingStimPhase();
    if (m_experiment) m_experiment->stop();
    m_closedLoopExperimentActive = false;

    if (!wasAcquiring) {
        commitManualStimEdits();
        m_manualStimLoadedForCurrentRun = false;

        const QString currentSignature = buildManualStimSignature();
        const bool shouldPreloadManualStim = m_manualStimConfigApplied
            && !m_manualStimAppliedSignature.isEmpty()
            && (m_manualStimAppliedSignature == currentSignature);

        if (shouldPreloadManualStim) {
            QString appliedSummary;
            if (configureManualStimHardware(&appliedSummary)) {
                m_manualStimConfigDirty = false;
                m_manualStimLoadedForCurrentRun = true;
                m_manualStimAppliedSummary = appliedSummary;
            } else {
                appendLog(QStringLiteral("普通采集启动前未能装载手动刺激配置，将仅启动波形采集"));
            }
        }

        m_engine->startContinuousAcquisition();
    }
    if (!m_engine->isContinuousRunning()) return;

    if (m_dockTimeline) m_dockTimeline->hide();
    else if (m_timeline) m_timeline->hide();

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
    if (m_closedLoopExperimentActive) {
        if (m_timeline) {
            m_timeline->setEpochSec(colletion_time);
            m_timeline->show();
            ensureTimelineVisible();
        }
        appendLog(QStringLiteral("Closed-loop experiment is already running."));
        return;
    }
    if (m_voidStimActive) {
        appendLog(QStringLiteral("A replay stimulation experiment is running; stop it before starting closed-loop."));
        return;
    }
    if (!ensureSessionRootSelected()) {
        appendLog(QStringLiteral("Please select a session root folder first."));
        return;
    }

    commitManualStimEdits();
    saveManualStimConfig();
    m_closedLoopMaxStimPerEpoch = m_spinClosedLoopMaxStimPerEpoch
                                      ? m_spinClosedLoopMaxStimPerEpoch->value()
                                      : m_closedLoopMaxStimPerEpoch;
    saveElectrodeConfig();
    syncExperimentRoutingConfig();
    if (!ensureStimStepSizeAppliedForMode(QStringLiteral("闭环实验"))) {
        return;
    }

    const bool wasAcquiring = m_engine->isContinuousRunning();
    if (!wasAcquiring) {
        m_engine->startContinuousAcquisition();
    }
    if (!m_engine->isContinuousRunning()) return;

    if (!prepareManagedSession(QStringLiteral("closed_loop"))) {
        appendLog(QStringLiteral("Failed to create a closed-loop session folder."));
        if (!wasAcquiring) {
            m_engine->stopAcquisition();
        }
        return;
    }
    if (!startManagedRecordingInActiveSession()) {
        appendLog(QStringLiteral("Failed to start managed recording for closed-loop session."));
        resetManagedSessionState();
        if (!wasAcquiring) {
            m_engine->stopAcquisition();
        }
        return;
    }

    m_managedSessionMode = ManagedSessionMode::ClosedLoop;
    m_closedLoopCompletedRounds = 0;
    m_managedSessionClock.restart();
    m_managedSessionClockActive = true;

    m_manualStimConfigApplied = false;
    m_manualStimConfigDirty = true;
    m_manualStimLoadedForCurrentRun = false;
    m_manualStimAppliedSummary.clear();
    m_manualStimAppliedSignature.clear();
    if (m_coordinator) {
        m_coordinator->cancelPendingStimPhase();
        m_coordinator->beginRun();
    }

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
        m_experiment->setTargetRounds(m_spinClosedLoopRounds ? m_spinClosedLoopRounds->value() : 1);
        m_experiment->start();
    }
    m_closedLoopExperimentActive = true;

    if (m_timeline) {
        m_timeline->setEpochSec(colletion_time);
        m_timeline->show();
        ensureTimelineVisible();
    }

    if (!wasAcquiring) {
        appendLog(QString("已开始闭环实验采集：AB epoch=%1 s, MaxStim/Epoch=%2")
                      .arg(colletion_time, 0, 'f', 2)
                      .arg(m_closedLoopMaxStimPerEpoch));
    } else {
        appendLog(QString("已在当前采集上启动闭环实验：AB epoch=%1 s, MaxStim/Epoch=%2")
                      .arg(colletion_time, 0, 'f', 2)
                      .arg(m_closedLoopMaxStimPerEpoch));
    }
}

void MainWindow::onVoidStim()
{
    if (!m_engine) return;
    if (m_closedLoopExperimentActive) {
        appendLog(QStringLiteral("Closed-loop experiment is running; stop it before starting virtual stimulation."));
        return;
    }
    if (m_voidStimActive) {
        appendLog(QStringLiteral("A replay stimulation experiment is already running."));
        return;
    }
    if (!ensureSessionRootSelected()) {
        appendLog(QStringLiteral("Please select a session root folder first."));
        return;
    }

    const QString startDir =
        !m_selectedReplayPlanPath.isEmpty()
            ? QFileInfo(m_selectedReplayPlanPath).absolutePath()
            : (!m_sessionRootDir.isEmpty() ? m_sessionRootDir : QDir::currentPath());
    const QString planPath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("Select stim_plan.json"),
        startDir,
        QStringLiteral("Stim Plan JSON (*.json);;All Files (*.*)"));
    if (planPath.isEmpty()) {
        return;
    }

    QByteArray jsonBytes;
    if (!loadStimPlanJson(planPath, &jsonBytes)) {
        appendLog(QStringLiteral("Failed to load stim plan JSON: %1").arg(planPath));
        return;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
    if (!doc.isObject()) {
        appendLog(QStringLiteral("Stim plan JSON format is invalid."));
        return;
    }

    const QJsonObject root = doc.object();
    const auto jsonToString = [](const QJsonValue &value) -> QString {
        return value.toVariant().toString().trimmed();
    };
    const auto jsonToLongLong = [&jsonToString](const QJsonValue &value, qint64 fallback) -> qint64 {
        bool ok = false;
        const qint64 direct = value.toVariant().toLongLong(&ok);
        if (ok) return direct;
        const QString text = jsonToString(value);
        if (text.isEmpty()) return fallback;
        const qint64 parsed = text.toLongLong(&ok);
        return ok ? parsed : fallback;
    };
    const auto jsonToInt = [&jsonToString](const QJsonValue &value, int fallback) -> int {
        bool ok = false;
        const int direct = value.toVariant().toInt(&ok);
        if (ok) return direct;
        const QString text = jsonToString(value);
        if (text.isEmpty()) return fallback;
        const int parsed = text.toInt(&ok);
        return ok ? parsed : fallback;
    };
    const auto jsonToDouble = [&jsonToString](const QJsonValue &value, double fallback) -> double {
        bool ok = false;
        const double direct = value.toVariant().toDouble(&ok);
        if (ok) return direct;
        const QString text = jsonToString(value);
        if (text.isEmpty()) return fallback;
        const double parsed = text.toDouble(&ok);
        return ok ? parsed : fallback;
    };

    qint64 experimentDurationMs = jsonToLongLong(root.value(QStringLiteral("experiment_duration_ms")), -1);
    if (experimentDurationMs <= 0) {
        const double epochSec = jsonToDouble(root.value(QStringLiteral("epoch_sec")), colletion_time);
        const int roundsCompleted =
            qMax(1, jsonToInt(root.value(QStringLiteral("rounds_completed")),
                              jsonToInt(root.value(QStringLiteral("rounds_target")), 1)));
        experimentDurationMs = qMax<qint64>(1, qRound64(epochSec * 2000.0 * roundsCompleted));
    }

    struct ReplayEvent {
        int itemIndex = -1;
        int displayIndex = -1;
        qint64 timeMs = 0;
        QString electrode;
        int amp_uA = 0;
        int pulses = 1;
        int triggerSource = 0;
    };

    QVector<ReplayEvent> replayEvents;
    const QJsonArray events = root.value(QStringLiteral("events")).toArray();
    replayEvents.reserve(events.size());
    for (int i = 0; i < events.size(); ++i) {
        const QJsonObject obj = events.at(i).toObject();

        int sourcePhaseIndex = jsonToInt(obj.value(QStringLiteral("source_phase_index")), -1);
        if (sourcePhaseIndex < 0) {
            const QString phaseText = jsonToString(obj.value(QStringLiteral("source_phase"))).toUpper();
            if (phaseText == QStringLiteral("A")) sourcePhaseIndex = 0;
            else if (phaseText == QStringLiteral("B")) sourcePhaseIndex = 1;
        }
        if (sourcePhaseIndex != 0) {
            continue;
        }

        qint64 timeMs = jsonToLongLong(obj.value(QStringLiteral("fired_time_ms")), -1);
        if (timeMs < 0) timeMs = jsonToLongLong(obj.value(QStringLiteral("time_ms")), -1);
        if (timeMs < 0) timeMs = jsonToLongLong(obj.value(QStringLiteral("planned_time_ms")), -1);
        if (timeMs < 0) {
            continue;
        }

        ReplayEvent event;
        event.itemIndex = jsonToInt(obj.value(QStringLiteral("item_index")), i);
        event.timeMs = timeMs;
        event.electrode = jsonToString(obj.value(QStringLiteral("target_electrode")));
        event.amp_uA = jsonToInt(obj.value(QStringLiteral("amp_uA")), 0);
        event.pulses = qMax(1, jsonToInt(obj.value(QStringLiteral("pulses")), 1));
        event.triggerSource = jsonToInt(obj.value(QStringLiteral("trigger_source")), 1);
        if (event.electrode.isEmpty() || event.amp_uA <= 0) {
            continue;
        }

        replayEvents.push_back(event);
    }

    std::sort(replayEvents.begin(), replayEvents.end(),
              [](const ReplayEvent &a, const ReplayEvent &b) {
                  return a.timeMs < b.timeMs;
              });
    for (int i = 0; i < replayEvents.size(); ++i) {
        replayEvents[i].displayIndex = i;
    }

    commitManualStimEdits();
    saveManualStimConfig();
    if (!ensureStimStepSizeAppliedForMode(QStringLiteral("虚空刺激"))) {
        return;
    }

    if (m_coordinator) {
        m_coordinator->cancelPendingStimPhase();
        m_coordinator->endRun();
    }
    if (m_experiment) {
        m_experiment->stop();
    }

    const bool wasAcquiring = m_engine->isContinuousRunning();
    if (!wasAcquiring) {
        m_engine->startContinuousAcquisition();
    }
    if (!m_engine->isContinuousRunning()) {
        appendLog(QStringLiteral("Failed to start acquisition for virtual stimulation."));
        return;
    }

    if (!prepareManagedSession(QStringLiteral("void_stim"))) {
        appendLog(QStringLiteral("Failed to create a virtual stimulation session folder."));
        if (!wasAcquiring) {
            m_engine->stopAcquisition();
        }
        return;
    }
    if (!startManagedRecordingInActiveSession()) {
        appendLog(QStringLiteral("Failed to start managed recording for virtual stimulation."));
        resetManagedSessionState();
        if (!wasAcquiring) {
            m_engine->stopAcquisition();
        }
        return;
    }

    m_selectedReplayPlanPath = planPath;
    m_managedSessionMode = ManagedSessionMode::VoidStim;
    m_managedSessionClock.restart();
    m_managedSessionClockActive = true;
    m_voidStimActive = true;
    m_closedLoopExperimentActive = false;

    if (m_timeline) {
        QVector<StimTimelineOverlay::Item> items;
        items.reserve(replayEvents.size());
        for (const ReplayEvent &event : replayEvents) {
            StimTimelineOverlay::Item item;
            item.itemIndex = event.displayIndex;
            item.offsetMs = double(event.timeMs);
            item.amp_uA = event.amp_uA;
            item.pulses = event.pulses;
            item.ch = -1;
            item.spike_uV = 0.0;
            item.electrode = event.electrode;
            item.fired = false;
            items.push_back(item);
        }
        m_timeline->setPlanView(QStringLiteral("虚空刺激  A驱动回放  events=%1  dur=%2s")
                                    .arg(items.size())
                                    .arg(double(experimentDurationMs) / 1000.0, 0, 'f', 2),
                                double(experimentDurationMs) / 1000.0,
                                items);
        ensureTimelineVisible();
    }

    const quint64 replayToken = ++m_voidStimReplayToken;
    for (const ReplayEvent &event : replayEvents) {
        if (m_stimLog) {
            m_stimLog->logPlanned(0, 0, event.itemIndex, double(event.timeMs),
                                  event.electrode, event.amp_uA, event.pulses, -1, 0.0);
        }

        const int delayMs = int(qMax<qint64>(0, event.timeMs));
        QTimer::singleShot(delayMs, this,
                           [this, replayToken, event]() {
                               if (replayToken != m_voidStimReplayToken || !m_voidStimActive) return;
                               if (!m_engine) return;
                               if (m_timeline) {
                                   m_timeline->markFired(0, event.displayIndex);
                               }
                               if (m_stimLog) {
                                   m_stimLog->logFired(0, 0, event.itemIndex,
                                                       event.electrode, event.amp_uA, event.pulses);
                               }
                               m_engine->applyAdaptiveStim(event.electrode,
                                                           event.amp_uA,
                                                           event.pulses,
                                                           event.triggerSource);
                           });
    }

    QTimer::singleShot(int(qMax<qint64>(1, experimentDurationMs)), this,
                       [this, replayToken]() {
                           if (replayToken != m_voidStimReplayToken || !m_voidStimActive) return;
                           onVoidStimReplayCompleted();
                       });

    appendLog(QStringLiteral("Virtual stimulation started from %1: A-driven events=%2, duration=%3 ms, recording=%4")
                  .arg(planPath)
                  .arg(replayEvents.size())
                  .arg(experimentDurationMs)
                  .arg(m_activeRecordingPath));
}

void MainWindow::onFixedStimExperiment()
{
    if (!m_engine) return;
    if (m_closedLoopExperimentActive) {
        appendLog(QStringLiteral("Closed-loop experiment is running; stop it before starting fixed-stim experiment."));
        return;
    }
    if (m_voidStimActive) {
        appendLog(QStringLiteral("A stimulation experiment is already running."));
        return;
    }
    if (!ensureSessionRootSelected()) {
        appendLog(QStringLiteral("Please select a session root folder first."));
        return;
    }

    commitManualStimEdits();
    saveManualStimConfig();
    saveElectrodeConfig();
    saveSessionConfig();
    if (!ensureStimStepSizeAppliedForMode(QStringLiteral("固定刺激实验"))) {
        return;
    }

    const qint64 collectPreMs = qMax<qint64>(0, qRound64((m_spinFixedStimCollectPreSec ? m_spinFixedStimCollectPreSec->value() : 60.0) * 1000.0));
    const qint64 stimWindowMs = qMax<qint64>(1, qRound64((m_spinFixedStimWindowSec ? m_spinFixedStimWindowSec->value() : 60.0) * 1000.0));
    const qint64 collectPostMs = qMax<qint64>(0, qRound64((m_spinFixedStimCollectPostSec ? m_spinFixedStimCollectPostSec->value() : 60.0) * 1000.0));
    const qint64 idleMs = qMax<qint64>(0, qRound64((m_spinFixedStimIdleSec ? m_spinFixedStimIdleSec->value() : 60.0) * 1000.0));
    const qint64 roundDurationMs = collectPreMs + stimWindowMs + collectPostMs + idleMs;

    const QString electrodeName = manualStimElectrodeName();
    const int triggerSource = m_spinManualTriggerSource ? m_spinManualTriggerSource->value() : 0;
    const double fixedAmp_uA = m_spinFixedStimAmp ? m_spinFixedStimAmp->value() : 20.0;
    const int fixedPhaseUs = m_spinFixedStimPhaseUs ? m_spinFixedStimPhaseUs->value() : 500;
    const double fixedFreqHz = m_spinFixedStimFreqHz ? m_spinFixedStimFreqHz->value() : 10.0;
    const int targetRounds = m_spinFixedStimRounds ? m_spinFixedStimRounds->value() : 1;

    const int periodUs = qMax(1, qRound(1000000.0 / fixedFreqHz));
    const int stimActiveUs = fixedPhaseUs * 2;
    if (periodUs <= stimActiveUs) {
        appendLog(QStringLiteral("Fixed-stim frequency is too high for the selected phase width. Reduce frequency or phase width."));
        return;
    }
    const int pulsesPerTrain = qMax(1, qRound((stimWindowMs / 1000.0) * fixedFreqHz));

    const qint64 experimentDurationMs = qint64(targetRounds) * roundDurationMs;
    if (experimentDurationMs > std::numeric_limits<int>::max()) {
        appendLog(QStringLiteral("Fixed-stim experiment is too long for the current scheduler. Please reduce rounds."));
        return;
    }

    if (m_coordinator) {
        m_coordinator->cancelPendingStimPhase();
        m_coordinator->endRun();
    }
    if (m_experiment) {
        m_experiment->stop();
    }

    const bool wasAcquiring = m_engine->isContinuousRunning();
    if (!wasAcquiring) {
        m_engine->startContinuousAcquisition();
    }
    if (!m_engine->isContinuousRunning()) {
        appendLog(QStringLiteral("Failed to start acquisition for fixed-stim experiment."));
        return;
    }

    if (!prepareManagedSession(QStringLiteral("fixed_stim"))) {
        appendLog(QStringLiteral("Failed to create a fixed-stim session folder."));
        if (!wasAcquiring) {
            m_engine->stopAcquisition();
        }
        return;
    }
    if (!startManagedRecordingInActiveSession()) {
        appendLog(QStringLiteral("Failed to start managed recording for fixed-stim experiment."));
        resetManagedSessionState();
        if (!wasAcquiring) {
            m_engine->stopAcquisition();
        }
        return;
    }

    m_selectedReplayPlanPath.clear();
    m_managedSessionMode = ManagedSessionMode::FixedStim;
    m_managedSessionClock.restart();
    m_managedSessionClockActive = true;
    m_voidStimActive = true;
    m_closedLoopExperimentActive = false;
    m_activeFixedStimElectrode = electrodeName;
    m_activeFixedStimAmp_uA = fixedAmp_uA;
    m_activeFixedStimPhaseUs = fixedPhaseUs;
    m_activeFixedStimFreqHz = fixedFreqHz;
    m_activeFixedStimTriggerSource = triggerSource;
    m_activeFixedStimRounds = targetRounds;
    m_activeFixedStimPulsesPerTrain = pulsesPerTrain;
    m_activeFixedStimCollectPreMs = collectPreMs;
    m_activeFixedStimWindowMs = stimWindowMs;
    m_activeFixedStimCollectPostMs = collectPostMs;
    m_activeFixedStimIdleMs = idleMs;

    const quint64 replayToken = ++m_voidStimReplayToken;
    for (int roundIndex = 0; roundIndex < targetRounds; ++roundIndex) {
        const qint64 stimStartMs = qint64(roundIndex) * roundDurationMs + collectPreMs;
        if (stimStartMs > std::numeric_limits<int>::max()) {
            appendLog(QStringLiteral("Fixed-stim schedule exceeds the current timer limit. Please reduce rounds."));
            onStop();
            return;
        }

        StimEventRecord record;
        record.epochId = roundIndex + 1;
        record.phaseIndex = -1;
        record.itemIndex = roundIndex;
        record.plannedTimeMs = stimStartMs;
        record.electrode = electrodeName;
        record.amp_uA = qRound(fixedAmp_uA);
        record.pulses = pulsesPerTrain;
        record.triggerSource = triggerSource;
        m_managedStimEvents.push_back(record);

        if (m_stimLog) {
            m_stimLog->logPlanned(-1, -1, roundIndex, double(stimStartMs),
                                  electrodeName, qRound(fixedAmp_uA), pulsesPerTrain, -1, 0.0);
        }

        QTimer::singleShot(int(stimStartMs), this,
                           [this, replayToken, roundIndex, stimStartMs,
                            electrodeName, fixedAmp_uA, fixedPhaseUs,
                            fixedFreqHz, triggerSource, pulsesPerTrain, stimWindowMs]() {
                               if (replayToken != m_voidStimReplayToken || !m_voidStimActive) return;
                               if (!m_engine) return;

                               const qint64 firedTimeMs =
                                   m_managedSessionClockActive ? m_managedSessionClock.elapsed() : stimStartMs;
                               for (int i = m_managedStimEvents.size() - 1; i >= 0; --i) {
                                   StimEventRecord &record = m_managedStimEvents[i];
                                   if (record.itemIndex == roundIndex && record.plannedTimeMs == stimStartMs) {
                                       record.firedTimeMs = firedTimeMs;
                                       break;
                                   }
                               }
                               if (m_stimLog) {
                                   m_stimLog->logFired(-1, -1, roundIndex,
                                                       electrodeName, qRound(fixedAmp_uA), pulsesPerTrain);
                               }
                               m_engine->applyFixedTrainStim(electrodeName,
                                                             fixedAmp_uA,
                                                             fixedPhaseUs,
                                                             fixedFreqHz,
                                                             int(stimWindowMs),
                                                             triggerSource);
                           });
    }

    QTimer::singleShot(int(qMax<qint64>(1, experimentDurationMs)), this,
                       [this, replayToken]() {
                           if (replayToken != m_voidStimReplayToken || !m_voidStimActive) return;
                           onFixedStimReplayCompleted();
                       });

    appendLog(QStringLiteral("Fixed-stim experiment started: target=%1, amplitude=%2 uA, phase=%3 us, frequency=%4 Hz, rounds=%5, pre=%6 ms, stim=%7 ms, post=%8 ms, idle=%9 ms, duration=%10 ms, recording=%11")
                  .arg(electrodeName)
                  .arg(fixedAmp_uA, 0, 'f', m_spinFixedStimAmp ? m_spinFixedStimAmp->decimals() : 1)
                  .arg(fixedPhaseUs)
                  .arg(fixedFreqHz, 0, 'f', 3)
                  .arg(targetRounds)
                  .arg(collectPreMs)
                  .arg(stimWindowMs)
                  .arg(collectPostMs)
                  .arg(idleMs)
                  .arg(experimentDurationMs)
                  .arg(m_activeRecordingPath));
}

void MainWindow::onStop()
{
    const ManagedSessionMode modeBeforeStop = m_managedSessionMode;
    const bool wasClosedLoop = m_closedLoopExperimentActive;
    const bool wasVoidStim = m_voidStimActive;
    const bool wasAcquiring = m_engine && m_engine->isContinuousRunning();
    const bool hadManagedSession = (m_managedSessionMode != ManagedSessionMode::None);
    const bool stopManagedRecording = m_engine && m_engine->isRecording() && hadManagedSession;
    const bool keepContinuousAcquisition =
        (modeBeforeStop == ManagedSessionMode::ClosedLoop) ||
        (modeBeforeStop == ManagedSessionMode::VoidStim) ||
        (modeBeforeStop == ManagedSessionMode::FixedStim) ||
        wasClosedLoop || wasVoidStim;

    stopVoidStimReplay(false);
    if (m_coordinator) {
        m_coordinator->cancelPendingStimPhase();
        m_coordinator->endRun();
    }
    if (m_experiment) m_experiment->stop();
    m_closedLoopExperimentActive = false;

    if (stopManagedRecording) {
        m_engine->stopBinaryRecording();
        if (m_stimLog) {
            m_stimLog->stop();
        }
    }
    if (m_engine && wasAcquiring && !keepContinuousAcquisition) {
        m_engine->stopAcquisition();
    }
    if (m_dockTimeline) m_dockTimeline->hide();
    else if (m_timeline) m_timeline->hide();

    finalizeManagedSession();
    if (modeBeforeStop == ManagedSessionMode::ClosedLoop || wasClosedLoop) {
        appendLog(QStringLiteral("Closed-loop experiment stopped; continuous acquisition remains running."));
        return;
    }
    if (modeBeforeStop == ManagedSessionMode::FixedStim) {
        appendLog(QStringLiteral("Fixed-stim experiment stopped; continuous acquisition remains running."));
        return;
    }
    if (modeBeforeStop == ManagedSessionMode::VoidStim || wasVoidStim) {
        appendLog(QStringLiteral("Virtual stimulation stopped; continuous acquisition remains running."));
        return;
    }
    if (wasAcquiring) {
        appendLog(QStringLiteral("Continuous acquisition stopped."));
        return;
    }
    appendLog(QStringLiteral("Acquisition is already stopped."));
    return;

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
    commitManualStimEdits();
    saveManualStimConfig();

    if (!m_engine || !m_engine->rhx() || !m_engine->stimController()) {
        appendLog(QStringLiteral("未触发普通采集刺激：请先打开设备"));
        return;
    }

    if (m_closedLoopExperimentActive) {
        appendLog(QStringLiteral("闭环实验运行中，不执行普通采集手动触发"));
        return;
    }

    if (!m_engine->isContinuousRunning()) {
        appendLog(QStringLiteral("未触发普通采集刺激：请先开始普通采集"));
        return;
    }

    const QString summary = buildManualStimSummary();
    const QString signature = buildManualStimSignature();
    const bool signatureMatchesCurrent = (!m_manualStimAppliedSignature.isEmpty() && m_manualStimAppliedSignature == signature);
    m_manualStimConfigDirty = !signatureMatchesCurrent;
    if (!signatureMatchesCurrent) {
        appendLog(QStringLiteral("普通采集刺激参数已变更，请先点击\"应用刺激配置\"再触发"));
        return;
    }
    if (!m_manualStimLoadedForCurrentRun) {
        appendLog(QStringLiteral("当前这轮普通采集尚未装载手动刺激配置，请先重新点击\"普通采集\"或\"应用刺激配置\""));
        return;
    }

    const int triggerSource = m_spinManualTriggerSource ? m_spinManualTriggerSource->value() : 0;
    m_engine->triggerStim(triggerSource, true);
    m_engine->triggerStim(triggerSource, false);
    appendLog(QStringLiteral("已手动触发一次普通采集刺激：%1").arg(m_manualStimAppliedSummary.isEmpty() ? summary : m_manualStimAppliedSummary));
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

void MainWindow::onStimPlanned(int epochId,
                               int phaseIndex,
                               int itemIndex,
                               qint64 plannedTimeMs,
                               const QString &electrode,
                               int amp_uA,
                               int pulses,
                               int ch,
                               double spike_uV,
                               int triggerSource)
{
    if (m_managedSessionMode != ManagedSessionMode::ClosedLoop) return;

    StimEventRecord record;
    record.epochId = epochId;
    record.phaseIndex = phaseIndex;
    record.itemIndex = itemIndex;
    record.plannedTimeMs = plannedTimeMs;
    record.electrode = electrode;
    record.amp_uA = amp_uA;
    record.pulses = pulses;
    record.ch = ch;
    record.spike_uV = spike_uV;
    record.triggerSource = triggerSource;
    m_managedStimEvents.push_back(record);
}

void MainWindow::onStimFired(int epochId,
                             int phaseIndex,
                             int itemIndex,
                             qint64 firedTimeMs,
                             const QString &electrode,
                             int amp_uA,
                             int pulses,
                             int ch,
                             double spike_uV,
                             int triggerSource)
{
    if (m_managedSessionMode != ManagedSessionMode::ClosedLoop) return;

    for (int i = m_managedStimEvents.size() - 1; i >= 0; --i) {
        StimEventRecord &record = m_managedStimEvents[i];
        if (record.epochId == epochId &&
            record.phaseIndex == phaseIndex &&
            record.itemIndex == itemIndex) {
            record.firedTimeMs = firedTimeMs;
            record.electrode = electrode;
            record.amp_uA = amp_uA;
            record.pulses = pulses;
            record.ch = ch;
            record.triggerSource = triggerSource;
            record.spike_uV = spike_uV;
            return;
        }
    }

    StimEventRecord record;
    record.epochId = epochId;
    record.phaseIndex = phaseIndex;
    record.itemIndex = itemIndex;
    record.firedTimeMs = firedTimeMs;
    record.electrode = electrode;
    record.amp_uA = amp_uA;
    record.pulses = pulses;
    record.ch = ch;
    record.spike_uV = spike_uV;
    record.triggerSource = triggerSource;
    m_managedStimEvents.push_back(record);
}

void MainWindow::onClosedLoopRoundCompleted(int completedRounds, int targetRounds)
{
    m_closedLoopCompletedRounds = completedRounds;
    appendLog(QStringLiteral("Closed-loop round completed: %1/%2")
                  .arg(completedRounds)
                  .arg(targetRounds));
}

void MainWindow::onClosedLoopExperimentCompleted(int completedRounds)
{
    m_closedLoopCompletedRounds = completedRounds;
    appendLog(QStringLiteral("Closed-loop experiment completed after %1 round(s); stopping managed recording and returning to continuous acquisition.")
                  .arg(completedRounds));

    QTimer::singleShot(0, this, [this]() { onStop(); });
}

void MainWindow::onVoidStimReplayCompleted()
{
    if (!m_voidStimActive) return;
    appendLog(QStringLiteral("Virtual stimulation replay completed; stopping managed recording and returning to continuous acquisition."));
    QTimer::singleShot(0, this, [this]() { onStop(); });
}

void MainWindow::onFixedStimReplayCompleted()
{
    if (!m_voidStimActive) return;
    appendLog(QStringLiteral("Fixed-stim experiment completed; stopping managed recording and returning to continuous acquisition."));
    QTimer::singleShot(0, this, [this]() { onStop(); });
}

void MainWindow::handleError(const QString &msg)
{
    appendLog(QStringLiteral("错误：") + msg);
}

void MainWindow::handleLog(const QString &msg)
{
    appendLog(msg);
}
