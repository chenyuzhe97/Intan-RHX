#include "stackedwavewidget.h"
#include <QPainter>
#include <QtMath>

StackedWaveWidget::StackedWaveWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(420, 360);
    m_repaint.setInterval(33); // ~30 fps
    connect(&m_repaint, &QTimer::timeout, this, QOverload<>::of(&StackedWaveWidget::update));
    m_repaint.start();
}

void StackedWaveWidget::setGainUv(double halfRangeUv)
{
    QMutexLocker lk(&m_mtx);
    m_gainUv = qMax(1.0, halfRangeUv);
}

void StackedWaveWidget::configure(int channels, double sampleRateHz, double windowSec)
{
    QMutexLocker lk(&m_mtx);
    m_channels = channels;
    m_fs = sampleRateHz;
    m_windowSec = windowSec;

    const int cap = qMax(1024, int(qCeil((windowSec + 0.5) * sampleRateHz)));
    m_rings.resize(m_channels);
    for (int ch=0; ch<m_channels; ++ch) m_rings[ch].init(cap);

    m_hasData = false;
}

void StackedWaveWidget::pushBlock(const QVector<uint32_t> &timeStamps,
                                  const QVector<QVector<int>> &channelData)
{
    if (timeStamps.isEmpty() || channelData.isEmpty()) return;

    const int N = timeStamps.size();
    const int C = qMin(m_channels, channelData.size());

    QMutexLocker lk(&m_mtx);

    for (int i=0; i<N; ++i) {
        for (int ch=0; ch<C; ++ch) {
            if (i >= channelData[ch].size()) continue;
            float uV = float((double(channelData[ch][i]) - 32768.0) * 0.195);
            m_rings[ch].push(uV);
        }
    }

    m_hasData = true;
}

void StackedWaveWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false); // 速度关键：关抗锯齿
    p.fillRect(rect(), QColor(18,18,18));

    QMutexLocker lk(&m_mtx);
    if (!m_hasData || m_rings.isEmpty()) return;

    const int W = width();
    const int H = height();
    const int padT = 18, padB = 10, padL = 44, padR = 10;

    p.setPen(QColor(220,220,220));
    p.drawText(6, 14, m_title);

    const int plotW = W - padL - padR;
    const int plotH = H - padT - padB;
    if (plotW < 20 || plotH < 20) return;

    const int C = m_channels;
    const double chH = double(plotH) / double(C);

    const int nAvail = m_rings[0].size();
    const int nWin = qMin(nAvail, int(m_windowSec * m_fs));
    if (nWin < 2) return;
    const int start = nAvail - nWin;

    // 网格
    p.setPen(QColor(45,45,45));
    for (int ch=0; ch<C; ++ch) {
        int y = padT + int((ch + 0.5) * chH);
        p.drawLine(padL, y, padL + plotW, y);
    }
    p.setPen(QColor(70,70,70));
    p.drawLine(padL, padT, padL, padT + plotH);

    // 波形：按像素列 min/max 下采样（超快）
    p.setPen(QColor(0, 210, 170));

    for (int ch=0; ch<C; ++ch) {
        const auto &ring = m_rings[ch];
        const int baseY = padT + int((ch + 0.5) * chH);
        const double yScale = (chH * 0.45) / m_gainUv;

        for (int x=0; x<plotW; ++x) {
            int i0 = start + int(double(x)     / double(plotW) * nWin);
            int i1 = start + int(double(x + 1) / double(plotW) * nWin);
            if (i1 <= i0) i1 = i0 + 1;
            if (i1 > start + nWin) i1 = start + nWin;

            float vmin = ring.atFromOldest(i0);
            float vmax = vmin;
            for (int i=i0+1; i<i1; ++i) {
                float v = ring.atFromOldest(i);
                if (v < vmin) vmin = v;
                if (v > vmax) vmax = v;
            }

            int y1 = baseY - int(vmin * yScale);
            int y2 = baseY - int(vmax * yScale);
            p.drawLine(padL + x, y1, padL + x, y2);
        }
    }
}
