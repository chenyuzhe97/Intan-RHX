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

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void onOpenDevice();
    void onStart();
    void onStartClosedLoop();
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

    void handleError(const QString &msg);
    void handleLog(const QString &msg);

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
    QString buildManualStimSummary() const;
    QString manualStimElectrodeName() const;
    bool configureManualStimHardware(QString *summary = nullptr);

    // ===== Closed-loop config (GUI) =====
    void setupElectrodeConfigDock();
    void loadElectrodeConfig();
    void saveElectrodeConfig() const;

    static QString formatChannels1Based(const QVector<int> &zeroBased);
    static bool parseChannels1Based(const QString &text, QVector<int> &outZeroBased, QString *err = nullptr);

private:
    QWidget        *m_central = nullptr;
    QVBoxLayout    *m_layout  = nullptr;

    // ===== buttons =====
    QPushButton    *m_btnOpen            = nullptr;
    QPushButton    *m_btnStart           = nullptr;
    QPushButton    *m_btnStartClosedLoop = nullptr;
    QPushButton    *m_btnStop            = nullptr;
    QPushButton    *m_btnStimOnce        = nullptr;
    QPushButton    *m_btnRecStart        = nullptr;
    QPushButton    *m_btnRecStop         = nullptr;
    QPushButton    *m_btnApplyManualStim = nullptr;

    QDoubleSpinBox *m_spinEpochSec = nullptr;

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

    // 拼写保留
    double colletion_time = 60.0;

    // ====== 普通采集刺激配置 ======
    QDockWidget *m_dockManualStim = nullptr;
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
    QString      m_manualStimAppliedSummary;

    // ====== 你要自定义的：每个 phase 用哪些通道做区域平均 ======
    // 注意：内部存的是 0-based index（用于 channelData[ch]）。GUI 显示/输入用 1-based。
    // phaseIndex==0: 来自老鼠A的感受电极( stream0 )，算 a/b 两条平均信号
    QVector<int> kSense_A_a = {0, 4, 6};      // GUI 默认显示：1,5,7
    QVector<int> kSense_A_b = {8, 10, 14};    // GUI 默认显示：9,11,15

    // phaseIndex==1: 来自老鼠B的感受电极( stream2 )，算 a'/b' 两条平均信号
    QVector<int> kSense_B_a = {0, 4, 6};      // GUI 默认显示：1,5,7
    QVector<int> kSense_B_b = {8, 10, 14};    // GUI 默认显示：9,11,15

    // ====== 你要自定义的：刺激电极名字（必须符合你 ElectrodeParameters 的命名规则）======
    // 你说后面固定 A 开头 / B 开头，所以 GUI 里只编辑数字后缀
    QString kStim_A_a = "A2";  // 刺激老鼠A a区 的刺激电极
    QString kStim_A_b = "A7";  // 刺激老鼠A b区 的刺激电极
    QString kStim_B_a = "B2";  // 刺激老鼠B a'区 的刺激电极
    QString kStim_B_b = "B7";  // 刺激老鼠B b'区 的刺激电极

    // ====== Closed-loop Config Dock (GUI widgets) ======
    QDockWidget *m_dockElectrode = nullptr;

    QLineEdit *m_editSense_A_a = nullptr;
    QLineEdit *m_editSense_A_b = nullptr;
    QLineEdit *m_editSense_B_a = nullptr;
    QLineEdit *m_editSense_B_b = nullptr;

    QSpinBox  *m_spinStim_A_a = nullptr;
    QSpinBox  *m_spinStim_A_b = nullptr;
    QSpinBox  *m_spinStim_B_a = nullptr;
    QSpinBox  *m_spinStim_B_b = nullptr;

    QPushButton *m_btnApplyElectrode = nullptr;
};