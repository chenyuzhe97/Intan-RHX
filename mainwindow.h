// mainwindow.h
#pragma once

#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QComboBox>
#include <QLabel>

#include "acquisitionengine.h"

#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

// Qt6 一般用 namespace QtCharts;

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

    void handleNewSamples(const QVector<uint32_t> &timeStamps,
                          const QVector<QVector<int>> &channelData);

    void handleError(const QString &msg);
    void handleLog(const QString &msg);

    // ⭐ 新增：通道选择变化时
    void onChannelChanged(int index);
    void onStimOnce();

private:
    void setupUi();
    void setupSinglePlot();   // ⭐ 单通道图
    void setupMultiPlot();    // ⭐ 多通道图
    void appendLog(const QString &msg);

private:
    QWidget          *m_central = nullptr;
    QVBoxLayout      *m_layout  = nullptr;
    QPushButton      *m_btnOpen = nullptr;
    QPushButton      *m_btnStart = nullptr;
    QPushButton      *m_btnStop = nullptr;
    QPushButton      *m_btnStim  = nullptr;
    QPlainTextEdit   *m_logView = nullptr;

    AcquisitionEngine *m_engine = nullptr;

    // ===== 单通道图（你原来的那套） =====
    QChartView    *m_chartView = nullptr;
    QChart        *m_chart     = nullptr;
    QLineSeries   *m_series    = nullptr;
    QValueAxis    *m_axisX     = nullptr;
    QValueAxis    *m_axisY     = nullptr;
    QVector<QPointF> m_buffer;
    double m_visibleWindowSec = 2.0;
    double m_sampleRate       = 30000.0;

    // ⭐ 新增：通道选择控件
    QComboBox     *m_comboChannel = nullptr;
    int            m_currentChannel = 0;    // 当前单通道视图使用的通道

    // ⭐ 新增：多通道图相关
    int NUM_CHANNELS = 16;     // RHS 一条 stream 16 通道

    QChartView    *m_multiChartView = nullptr;
    QChart        *m_multiChart     = nullptr;
    QValueAxis    *m_multiAxisX     = nullptr;
    QValueAxis    *m_multiAxisY     = nullptr;

    // 每个通道一条曲线
    QVector<QLineSeries*>        m_multiSeries;        // size = NUM_CHANNELS
    QVector<QVector<QPointF>>    m_multiBuffers;       // size = NUM_CHANNELS
    double m_multiWindowSec = 2.0;
};
