// mainwindow.h
#pragma once

#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QSplitter>
#include <QTimer>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QSpinBox>

#include "acquisitionengine.h"
#include "abalgorithm.h"
#include "abexperimentcoordinator.h"
#include "experimentcontrollerab.h"

#include "stackedwavewidget.h"
#include "stimtimelineoverlay.h"
#include "stimlogwriter.h"
#include "fft_window.h"

class QCloseEvent;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onOpenDevice();
    void onSelectRecordingLocation();
    void onStart();
    void onStartClosedLoop();
    void onVoidStim();
    void onFixedStimExperiment();
    void onDualFixedStimExperiment();
    void onStop();
    void onRecStart();
    void onRecStop();
    void onStimOnce();
    void applyManualStimConfigFromUi();

    void onEpochDurationChanged(double sec);
    void onGainAChanged(double halfRangeUv);
    void onGainBChanged(double halfRangeUv);
    void onOverviewLaneHeightChanged(int px);

    // DSP/FFT
    void applyDspSettings();
    void onToggleFftWindows();

    // Closed-loop config dock
    void applyElectrodeConfigFromUi();

    void onABEpochReady(int phaseIndex,
                        const QVector<uint32_t> &timeStamps,
                        const QVector<QVector<int>> &channelData);
    void onClosedLoopRoundCompleted(int completedRounds, int targetRounds);
    void onClosedLoopExperimentCompleted(int completedRounds);
    void onStimPlanned(int epochId,
                       int phaseIndex,
                       int itemIndex,
                       qint64 plannedTimeMs,
                       const QString &electrode,
                       int amp_uA,
                       int pulses,
                       int ch,
                       double spike_uV,
                       int triggerSource);
    void onStimFired(int epochId,
                     int phaseIndex,
                     int itemIndex,
                     qint64 firedTimeMs,
                     const QString &electrode,
                     int amp_uA,
                     int pulses,
                     int ch,
                     double spike_uV,
                     int triggerSource);
    void onVoidStimReplayCompleted();
    void onFixedStimReplayCompleted();

    void handleError(const QString &msg);
    void handleLog(const QString &msg);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void setupUi();
    void appendLog(const QString &msg);
    void ensureTimelineVisible();
    void ensureFftVisible();
    void syncExperimentRoutingConfig();

    // ===== Manual stim (GUI) =====
    void setupManualStimDock();
    void loadManualStimConfig();
    void saveManualStimConfig() const;
    void commitManualStimEdits();
    QString buildManualStimSummary() const;
    QString buildManualStimSignature() const;
    QString manualStimElectrodeName() const;
    StimStepSize selectedStimStepSize() const;
    bool ensureStimStepSizeAppliedForMode(const QString &modeLabel);
    void updateFixedStimAmplitudeControl();
    bool configureManualStimHardware(QString *summary = nullptr);

    // ===== Closed-loop config (GUI) =====
    void setupElectrodeConfigDock();
    void setupFixedStimDock();
    void loadElectrodeConfig();
    void saveElectrodeConfig() const;
    void loadSessionConfig();
    void saveSessionConfig() const;
    void updateDualFixedStimParamModeUi();

    static QString formatChannels1Based(const QVector<int> &zeroBased);
    static bool parseChannels1Based(const QString &text, QVector<int> &outZeroBased, QString *err = nullptr);
    bool ensureSessionRootSelected();
    bool prepareManagedSession(const QString &sessionPrefix);
    bool startManagedRecordingInActiveSession();
    void finalizeManagedSession();
    void resetManagedSessionState();
    bool writeStimPlanJson(qint64 durationMs) const;
    bool writeFixedStimSessionJson(qint64 durationMs) const;
    bool loadStimPlanJson(const QString &filePath, QByteArray *jsonBytes = nullptr) const;
    void stopVoidStimReplay(bool logMessage);

private:
    struct StimEventRecord {
        int epochId = -1;
        int phaseIndex = -1;
        int itemIndex = -1;
        qint64 plannedTimeMs = -1;
        qint64 firedTimeMs = -1;
        QString electrode;
        int amp_uA = 0;
        int phaseUs = 0;
        double frequencyHz = 0.0;
        int pulses = 0;
        int ch = -1;
        double spike_uV = 0.0;
        int triggerSource = 0;
    };

    enum class ManagedSessionMode {
        None,
        ClosedLoop,
        VoidStim,
        FixedStim,
        FixedStimDual
    };

    QWidget        *m_central = nullptr;
    QVBoxLayout    *m_layout  = nullptr;

    // ===== buttons =====
    QPushButton    *m_btnOpen            = nullptr;
    QPushButton    *m_btnSelectSaveDir   = nullptr;
    QPushButton    *m_btnStart           = nullptr;
    QPushButton    *m_btnStartClosedLoop = nullptr;
    QPushButton    *m_btnVoidStim        = nullptr;
    QPushButton    *m_btnFixedStim       = nullptr;
    QPushButton    *m_btnDualFixedStim   = nullptr;
    QPushButton    *m_btnStop            = nullptr;
    QPushButton    *m_btnStimOnce        = nullptr;
    QPushButton    *m_btnRecStart        = nullptr;
    QPushButton    *m_btnRecStop         = nullptr;
    QPushButton    *m_btnShowAllChannels = nullptr;
    QPushButton    *m_btnApplyManualStim = nullptr;

    QDoubleSpinBox *m_spinEpochSec = nullptr;
    QSpinBox       *m_spinClosedLoopRounds = nullptr;
    QSpinBox       *m_spinClosedLoopMaxStimPerEpoch = nullptr;
    QSpinBox       *m_spinClosedLoopPhaseUs = nullptr;
    QSpinBox       *m_spinVoidStimPhaseUs = nullptr;
    QCheckBox      *m_chkClosedLoopBpEnabled = nullptr;
    QDoubleSpinBox *m_spinClosedLoopBpLowHz = nullptr;
    QDoubleSpinBox *m_spinClosedLoopBpHighHz = nullptr;

    // ===== view gain =====
    QDoubleSpinBox *m_spinGainA = nullptr;   // ±uV
    QDoubleSpinBox *m_spinGainB = nullptr;   // ±uV
    QSpinBox       *m_spinOverviewLane = nullptr;

    // ===== DSP controls =====
    QCheckBox *m_chkFilter = nullptr;
    QComboBox *m_cmbFilterType = nullptr;   // Off/BandPass/LowPass/HighPass

    QDoubleSpinBox *m_spBP1 = nullptr;      // band low
    QDoubleSpinBox *m_spBP2 = nullptr;      // band high
    QDoubleSpinBox *m_spLP  = nullptr;      // lowpass
    QDoubleSpinBox *m_spHP  = nullptr;      // highpass

    QCheckBox *m_chkNotch = nullptr;
    QComboBox *m_cmbNotchHz = nullptr;      // 50/60
    QDoubleSpinBox *m_spNotchQ = nullptr;

    QPushButton *m_btnFFT = nullptr;
    FftWindow *m_fftA = nullptr;
    FftWindow *m_fftB = nullptr;

    // ===== split view =====
    QSplitter      *m_split = nullptr;
    StackedWaveWidget *m_viewA = nullptr;    // Stream0
    StackedWaveWidget *m_viewB = nullptr;    // Stream2

    QPlainTextEdit *m_logView = nullptr;

    // ===== core =====
    AcquisitionEngine       *m_engine      = nullptr;
    ABAlgorithm             *m_abAlgo      = nullptr;
    ABExperimentCoordinator *m_coordinator = nullptr;
    ExperimentControllerAB  *m_experiment  = nullptr;

    // ===== stim timeline + csv =====
    StimTimelineOverlay *m_timeline = nullptr;
    QDockWidget         *m_dockTimeline = nullptr;
    StimLogWriter       *m_stimLog  = nullptr;

    // ===== params =====
    double m_sampleRate = 30000.0;
    int    m_channelsPerStream = 16;
    double m_visibleWindowSec = 2.0;   // 初始显示 2s（Ctrl+滚轮可变）
    double m_maxWindowSec = 20.0;      // ring buffer 预留 20s

    bool   m_closedLoopExperimentActive = false;
    bool   m_voidStimActive = false;

    // 拼写保留
    double colletion_time = 60.0;
    int    m_closedLoopCompletedRounds = 0;
    int    m_closedLoopMaxStimPerEpoch = 10;
    int    m_closedLoopPhaseUs = 60;
    int    m_voidStimPhaseUs = 60;
    bool   m_closedLoopBpEnabled = true;
    double m_closedLoopBpLowHz = 300.0;
    double m_closedLoopBpHighHz = 3000.0;

    QString            m_sessionRootDir;
    QString            m_activeSessionDir;
    QString            m_activeRecordingPath;
    QString            m_activeStimPlanPath;
    QString            m_selectedReplayPlanPath;
    ManagedSessionMode m_managedSessionMode = ManagedSessionMode::None;
    QElapsedTimer      m_managedSessionClock;
    bool               m_managedSessionClockActive = false;
    QVector<StimEventRecord> m_managedStimEvents;
    quint64            m_voidStimReplayToken = 0;
    QString            m_activeFixedStimElectrode;
    double             m_activeFixedStimAmp_uA = 0.0;
    int                m_activeFixedStimPhaseUs = 0;
    double             m_activeFixedStimFreqHz = 0.0;
    int                m_activeFixedStimTriggerSource = 0;
    int                m_activeFixedStimRounds = 0;
    int                m_activeFixedStimPulsesPerTrain = 0;
    qint64             m_activeFixedStimCollectPreMs = 60000;
    qint64             m_activeFixedStimWindowMs = 60000;
    qint64             m_activeFixedStimCollectPostMs = 60000;
    qint64             m_activeFixedStimIdleMs = 60000;
    bool               m_activeDualFixedSharedParams = true;
    QString            m_activeDualFixedStimElectrodeA;
    QString            m_activeDualFixedStimElectrodeB;
    double             m_activeDualFixedStimAmpA_uA = 0.0;
    double             m_activeDualFixedStimAmpB_uA = 0.0;
    int                m_activeDualFixedStimPhaseAUs = 0;
    int                m_activeDualFixedStimPhaseBUs = 0;
    double             m_activeDualFixedStimFreqAHz = 0.0;
    double             m_activeDualFixedStimFreqBHz = 0.0;
    int                m_activeDualFixedStimPulsesPerTrainA = 0;
    int                m_activeDualFixedStimPulsesPerTrainB = 0;

    // ====== 普通采集刺激配置 ======
    QDockWidget *m_dockManualStim = nullptr;
    QComboBox   *m_cmbStimStepSize = nullptr;
    QComboBox   *m_cmbManualStimPrefix = nullptr;
    QSpinBox    *m_spinManualStimElectrode = nullptr;
    QSpinBox    *m_spinManualTriggerSource = nullptr;
    QSpinBox    *m_spinManualFirstAmp = nullptr;
    QSpinBox    *m_spinManualSecondAmp = nullptr;
    QSpinBox    *m_spinManualPulseCount = nullptr;
    QSpinBox    *m_spinManualFirstPhaseUs = nullptr;
    QSpinBox    *m_spinManualSecondPhaseUs = nullptr;
    QSpinBox    *m_spinManualInterphaseUs = nullptr;
    bool         m_manualStimConfigApplied = false;
    bool         m_manualStimConfigDirty = true;
    bool         m_manualStimLoadedForCurrentRun = false;
    QString      m_manualStimAppliedSummary;
    QString      m_manualStimAppliedSignature;

    // ====== 你要自定义的：每个 phase 用哪些通道做区域平均 ======
    // 注意：内部存的是 0-based index（用于 channelData[ch]）。GUI 显示/输入用 1-based。
    // phaseIndex==0: 来自老鼠A的感受电极( stream0 )，算 a/b 两条平均信号
    QVector<int> kSense_A_a = {0, 2, 4, 6};      // GUI 默认显示：1,3,5,7
    QVector<int> kSense_A_b = {8, 10, 12, 14};   // GUI 默认显示：9,11,13,15

    // phaseIndex==1: 来自老鼠B的感受电极( stream2 )，算 a'/b' 两条平均信号
    QVector<int> kSense_B_a = {0, 2, 4, 6};      // GUI 默认显示：1,3,5,7
    QVector<int> kSense_B_b = {8, 10, 12, 14};   // GUI 默认显示：9,11,13,15

    // ====== 你要自定义的：刺激电极名字（必须符合你 ElectrodeParameters 的命名规则）======
    // 你说后面固定 A 开头 / B 开头，所以 GUI 里只编辑数字后缀
    QString kStim_A_a = "A2";  // 刺激老鼠A a区 的刺激电极
    QString kStim_A_b = "A7";  // 刺激老鼠A b区 的刺激电极
    QString kStim_B_a = "B2";  // 刺激老鼠B a'区 的刺激电极
    QString kStim_B_b = "B7";  // 刺激老鼠B b'区 的刺激电极

    // ====== Closed-loop Config Dock (GUI widgets) ======
    QDockWidget *m_dockElectrode = nullptr;
    QDockWidget *m_dockFixedStim = nullptr;

    QLineEdit *m_editSense_A_a = nullptr;
    QLineEdit *m_editSense_A_b = nullptr;
    QLineEdit *m_editSense_B_a = nullptr;
    QLineEdit *m_editSense_B_b = nullptr;

    QDoubleSpinBox *m_spinFixedStimAmp = nullptr;
    QSpinBox  *m_spinFixedStimPhaseUs = nullptr;
    QDoubleSpinBox *m_spinFixedStimFreqHz = nullptr;
    QSpinBox  *m_spinFixedStimRounds = nullptr;
    QSpinBox  *m_spinFixedStimElectrode = nullptr;
    QSpinBox  *m_spinDualFixedStimElectrodeA = nullptr;
    QSpinBox  *m_spinDualFixedStimElectrodeB = nullptr;
    QCheckBox *m_chkDualFixedSharedParams = nullptr;
    QDoubleSpinBox *m_spinDualFixedStimAmpA = nullptr;
    QSpinBox  *m_spinDualFixedStimPhaseUsA = nullptr;
    QDoubleSpinBox *m_spinDualFixedStimFreqHzA = nullptr;
    QDoubleSpinBox *m_spinDualFixedStimAmpB = nullptr;
    QSpinBox  *m_spinDualFixedStimPhaseUsB = nullptr;
    QDoubleSpinBox *m_spinDualFixedStimFreqHzB = nullptr;
    QWidget   *m_dualFixedIndependentParamsWidget = nullptr;
    QDoubleSpinBox *m_spinFixedStimCollectPreSec = nullptr;
    QDoubleSpinBox *m_spinFixedStimWindowSec = nullptr;
    QDoubleSpinBox *m_spinFixedStimCollectPostSec = nullptr;
    QDoubleSpinBox *m_spinFixedStimIdleSec = nullptr;

    QSpinBox  *m_spinStim_A_a = nullptr;
    QSpinBox  *m_spinStim_A_b = nullptr;
    QSpinBox  *m_spinStim_B_a = nullptr;
    QSpinBox  *m_spinStim_B_b = nullptr;

    QPushButton *m_btnApplyElectrode = nullptr;
};
