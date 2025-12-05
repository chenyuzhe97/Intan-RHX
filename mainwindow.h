// mainwindow.h
#pragma once

#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "acquisitionengine.h"

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

    void handleNewSamples(const QVector<uint32_t> &timeStamps,
                          const QVector<QVector<int>> &channelData);

    void handleError(const QString &msg);
    void handleLog(const QString &msg);

private:
    void setupUi();
    void setupPlot();
    void appendLog(const QString &msg);

private:
    QWidget          *m_central = nullptr;
    QVBoxLayout      *m_layout  = nullptr;
    QPushButton      *m_btnOpen = nullptr;
    QPushButton      *m_btnStart = nullptr;
    QPushButton      *m_btnStop = nullptr;
    QPlainTextEdit   *m_logView = nullptr;

    AcquisitionEngine *m_engine = nullptr;

    // ====== 新增：绘图相关 ======
    QChartView    *m_chartView = nullptr;
    QChart        *m_chart     = nullptr;
    QLineSeries   *m_series    = nullptr;
    QValueAxis    *m_axisX     = nullptr;
    QValueAxis    *m_axisY     = nullptr;

    // 滚动窗口数据缓冲（只保留最近 visibleWindowSec 秒）
    QVector<QPointF> m_buffer;
    double m_visibleWindowSec = 2.0;     // 显示最近 2 秒
    double m_sampleRate       = 30000.0; // 和 AcquisitionEngine 里一致
};
