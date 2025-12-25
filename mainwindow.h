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

#include "acquisitionengine.h"
#include "abalgorithm.h"
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
    void onStop();
    void onRecStart();
    void onRecStop();
    void onStimOnce();

    void onEpochDurationChanged(double sec);
    void onGainAChanged(double halfRangeUv);
    void onGainBChanged(double halfRangeUv);

    // DSP/FFT
    void applyDspSettings();
    void onToggleFftWindows();

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

private:
    QWidget        *m_central = nullptr;
    QVBoxLayout    *m_layout  = nullptr;

    // ===== buttons =====
    QPushButton    *m_btnOpen     = nullptr;
    QPushButton    *m_btnStart    = nullptr;
    QPushButton    *m_btnStop     = nullptr;
    QPushButton    *m_btnStimOnce = nullptr;
    QPushButton    *m_btnRecStart = nullptr;
    QPushButton    *m_btnRecStop  = nullptr;

    QDoubleSpinBox *m_spinEpochSec = nullptr;

    // ===== view gain =====
    QDoubleSpinBox *m_spinGainA = nullptr;   // ±uV
    QDoubleSpinBox *m_spinGainB = nullptr;   // ±uV

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
    AcquisitionEngine      *m_engine     = nullptr;
    ABAlgorithm            *m_abAlgo     = nullptr;
    ExperimentControllerAB *m_experiment = nullptr;

    // ===== stim timeline + csv =====
    StimTimelineOverlay *m_timeline = nullptr;
    StimLogWriter       *m_stimLog  = nullptr;

    int    m_epochCounter = 0;

    // ===== params =====
    double m_sampleRate = 30000.0;
    int    m_channelsPerStream = 16;
    double m_visibleWindowSec = 2.0;   // 初始显示 2s（Ctrl+滚轮可变）
    double m_maxWindowSec = 20.0;      // ring buffer 预留 20s

    // 拼写保留
    double colletion_time = 5.0;
};
