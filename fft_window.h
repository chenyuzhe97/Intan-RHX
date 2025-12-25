#pragma once
#include <QWidget>
#include <QTimer>
#include <QVector>
#include <QPainterPath>

#include "stackedwavewidget.h"

class StackedWaveWidget;

class FftWindow : public QWidget
{
    Q_OBJECT
public:
    explicit FftWindow(StackedWaveWidget *src, QWidget *parent=nullptr);

    void setChannel(int ch);
    void setFftSize(int nPow2);     // 1024/2048/4096...
    void setMaxFreq(double hz);     // 只画到 maxFreq
    void setUpdateHz(int hz);       // 更新频率

protected:
    void paintEvent(QPaintEvent*) override;

private slots:
    void tick();

private:
    void compute();

private:
    StackedWaveWidget *m_src = nullptr;
    QTimer m_timer;

    int m_ch = 0;
    int m_N = 2048;
    double m_maxFreq = 5000.0;

    QVector<float>  m_time;
    QVector<double> m_magDb;  // dB
    QVector<double> m_freq;   // Hz
};
