// mainwindow.h
#pragma once

#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QComboBox>
#include <QLabel>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include "acquisitionengine.h"
#include "abalgorithm.h"
#include "experimentcontrollerab.h"

#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onOpenDevice();
    void onStart();
    void onStop();
    void onRecStart();
    void onRecStop();

    void onABEpochReady(int phaseIndex,
                        const QVector<uint32_t> &timeStamps,
                        const QVector<QVector<int>> &channelData);

    void handleNewSamples(const QVector<uint32_t> &timeStamps,
                          const QVector<QVector<int>> &channelData);

    void handleNewSamplesStream2(const QVector<uint32_t> &timeStamps,
                                 const QVector<QVector<int>> &channelData);

    void handleError(const QString &msg);
    void handleLog(const QString &msg);

    // 单通道（Stream0）通道选择
    void onChannelChanged(int index);
    void onStimOnce();

    // Stream0 带通滤波
    void onBandpassToggled(bool checked);
    void onBandpassParamChanged(double value);

    // Stream0 Y 轴范围
    void onAutoYChanged(bool checked);
    void onYRangeEdited(double value);

    // ⭐ 新增：Stream2 Y 轴范围
    void onAutoY2Changed(bool checked);
    void onY2RangeEdited(double value);

    // ⭐ 新增：Stream2 带通滤波
    void onBandpass2Toggled(bool checked);
    void onBandpass2ParamChanged(double value);

private:
    void setupUi();
    void setupSinglePlot();    // Stream0 单通道图
    void setupStream2Plot();   // Stream2 单通道图
    void setupMultiPlot();     // 多通道图
    void appendLog(const QString &msg);

private:
    QWidget          *m_central = nullptr;
    QVBoxLayout      *m_layout  = nullptr;
    QPushButton      *m_btnOpen = nullptr;
    QPushButton      *m_btnStart = nullptr;
    QPushButton      *m_btnStop = nullptr;
    QPushButton      *m_btnStim  = nullptr;
    QPushButton      *m_btnRecStart = nullptr;
    QPushButton      *m_btnRecStop  = nullptr;
    QPlainTextEdit   *m_logView = nullptr;

    AcquisitionEngine        *m_engine     = nullptr;
    ABAlgorithm              *m_abAlgo     = nullptr;
    ExperimentControllerAB   *m_experiment = nullptr;

    // ===== Stream0 单通道图 =====
    QChartView      *m_chartView = nullptr;
    QChart          *m_chart     = nullptr;
    QLineSeries     *m_series    = nullptr;
    QValueAxis      *m_axisX     = nullptr;
    QValueAxis      *m_axisY     = nullptr;
    QVector<QPointF> m_buffer;
    double           m_visibleWindowSec = 2.0;
    double           m_sampleRate       = 30000.0;

    // ==== Stream0 带通滤波 UI ====
    QCheckBox      *m_chkBandpass = nullptr;
    QDoubleSpinBox *m_spinBpLow   = nullptr;
    QDoubleSpinBox *m_spinBpHigh  = nullptr;

    // ==== Stream0 带通滤波参数 ====
    bool   m_enableBandpass = false;
    double m_bpLowHz        = 300.0;
    double m_bpHighHz       = 3000.0;

    // Stream0 通道选择
    QComboBox  *m_comboChannel   = nullptr;
    int         m_currentChannel = 0;

    // ===== Stream2 单通道图 =====
    QChartView      *m_chartViewStream2 = nullptr;
    QChart          *m_chartStream2     = nullptr;
    QLineSeries     *m_seriesStream2    = nullptr;
    QValueAxis      *m_axisX2           = nullptr;
    QValueAxis      *m_axisY2           = nullptr;
    QComboBox       *m_comboStream2Ch   = nullptr;
    int              NUM_CH_STREAM2     = 16;
    QVector<QPointF> m_bufferStream2;

    double m_stream2WindowSec = 2.0;  // 只看最近 2 s
    int    m_currentChStream2 = 0;    // 当前选择的 stream2 通道（0~15）

    // ===== 多通道图 =====
    int             NUM_CHANNELS   = 16;
    QChartView     *m_multiChartView = nullptr;
    QChart         *m_multiChart     = nullptr;
    QValueAxis     *m_multiAxisX     = nullptr;
    QValueAxis     *m_multiAxisY     = nullptr;
    QVector<QLineSeries*>     m_multiSeries;
    QVector<QVector<QPointF>> m_multiBuffers;
    double          m_multiWindowSec = 2.0;

    // ==== Stream0 Y 轴范围控制 ====
    QCheckBox      *m_chkAutoY    = nullptr;
    QDoubleSpinBox *m_spinYMin    = nullptr;
    QDoubleSpinBox *m_spinYMax    = nullptr;

    bool   m_autoY      = true;      // true = 自适应，false = 固定
    double m_fixedYMin  = -7000.0;
    double m_fixedYMax  =  7000.0;

    // ==== Stream2 Y 轴范围控制 ====
    QCheckBox      *m_chkAutoY2   = nullptr;
    QDoubleSpinBox *m_spinY2Min   = nullptr;
    QDoubleSpinBox *m_spinY2Max   = nullptr;

    bool   m_autoY2     = true;
    double m_fixedY2Min = -7000.0;
    double m_fixedY2Max =  7000.0;

    // ==== Stream2 带通滤波 UI + 参数 ====
    QCheckBox      *m_chkBandpass2 = nullptr;
    QDoubleSpinBox *m_spinBp2Low   = nullptr;
    QDoubleSpinBox *m_spinBp2High  = nullptr;

    bool   m_enableBandpass2 = false;
    double m_bp2LowHz        = 300.0;
    double m_bp2HighHz       = 3000.0;
};
