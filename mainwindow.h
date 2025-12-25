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

#include "acquisitionengine.h"
#include "abalgorithm.h"
#include "experimentcontrollerab.h"

#include "stackedwavewidget.h"
#include "stimtimelineoverlay.h"
#include "stimlogwriter.h"

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

    void onABEpochReady(int phaseIndex,
                        const QVector<uint32_t> &timeStamps,
                        const QVector<QVector<int>> &channelData);

    void handleError(const QString &msg);
    void handleLog(const QString &msg);

private:
    void setupUi();
    void appendLog(const QString &msg);
    void ensureTimelineVisible();

private:
    QWidget        *m_central = nullptr;
    QVBoxLayout    *m_layout  = nullptr;

    QPushButton    *m_btnOpen     = nullptr;
    QPushButton    *m_btnStart    = nullptr;
    QPushButton    *m_btnStop     = nullptr;
    QPushButton    *m_btnStimOnce = nullptr;
    QPushButton    *m_btnRecStart = nullptr;
    QPushButton    *m_btnRecStop  = nullptr;

    QDoubleSpinBox *m_spinEpochSec = nullptr;

    // 显示缩放（每个 stream 一套）
    QDoubleSpinBox *m_spinGainA = nullptr;   // ±uV
    QDoubleSpinBox *m_spinGainB = nullptr;   // ±uV

    QSplitter      *m_split = nullptr;
    StackedWaveWidget *m_viewA = nullptr;    // Stream0
    StackedWaveWidget *m_viewB = nullptr;    // Stream2

    QPlainTextEdit *m_logView = nullptr;

    AcquisitionEngine      *m_engine     = nullptr;
    ABAlgorithm            *m_abAlgo     = nullptr;
    ExperimentControllerAB *m_experiment = nullptr;

    // 刺激 timeline 悬浮窗 + CSV writer
    StimTimelineOverlay *m_timeline = nullptr;
    StimLogWriter       *m_stimLog  = nullptr;

    int    m_epochCounter = 0;

    // 参数
    double m_sampleRate = 30000.0;
    int    m_channelsPerStream = 16;
    double m_visibleWindowSec = 2.0;

    // 你原来变量名拼写是 colletion_time，我这里保留避免你其它代码引用崩掉
    double colletion_time = 5.0;
};
