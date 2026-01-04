#include "stimtimelineoverlay.h"
#include <QPainter>
#include <QtMath>

StimTimelineOverlay::StimTimelineOverlay(QWidget *parent) : QWidget(parent)
{
    setWindowTitle("Stim Timeline");
    setWindowFlags(Qt::Tool | Qt::WindowStaysOnTopHint);
    resize(520, 180);
}



void StimTimelineOverlay::setEpochPlan(int epochId, int phaseIndex, double epochSec,
                                       const QVector<Item> &items)
{
    m_epochId = epochId;
    m_phaseIndex = phaseIndex;
    m_epochSec = qMax(0.1, epochSec);
    m_items = items;
    update();
}

void StimTimelineOverlay::setEpochSec(double epochSec)
{
    m_epochSec = epochSec;
}

void StimTimelineOverlay::markFired(int epochId, int itemIndex)
{
    if (epochId != m_epochId) return;
    for (auto &it : m_items) {
        if (it.itemIndex == itemIndex) {
            it.fired = true;
            break;
        }
    }
    update();
}

void StimTimelineOverlay::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(30,30,30));

    const int W = width();
    const int H = height();
    const int padL=40, padR=12, padT=28, padB=22;

    p.setPen(QColor(230,230,230));
    p.drawText(8, 18, QString("epoch=%1  phase=%2  dur=%3s")
                          .arg(m_epochId).arg(m_phaseIndex).arg(m_epochSec,0,'f',2));

    const int x0 = padL;
    const int x1 = W - padR;
    const int yAxis = H - padB;
    const int yTop = padT;

    // axis
    p.setPen(QColor(120,120,120));
    p.drawLine(x0, yAxis, x1, yAxis);

    // ticks
    int ticks = 5;
    for (int k=0;k<=ticks;++k){
        double t = m_epochSec * (double(k)/ticks);
        int x = x0 + int((x1-x0) * (t/m_epochSec));
        p.drawLine(x, yAxis, x, yAxis+6);
        p.drawText(x-10, yAxis+18, QString::number(t,'f',1));
    }

    // events
    int maxAmp = 1;
    for (const auto &it : m_items) maxAmp = qMax(maxAmp, it.amp_uA);

    for (const auto &it : m_items) {
        double tSec = it.offsetMs / 1000.0;
        int x = x0 + int((x1-x0) * (tSec / m_epochSec));
        int h = int((yAxis - yTop) * (double(it.amp_uA) / maxAmp));
        int y1 = yAxis - h;

        p.setPen(it.fired ? QColor(255,200,0) : QColor(0,210,170));
        p.drawLine(x, yAxis, x, y1);

        p.setPen(QColor(200,200,200));
        p.drawText(x+4, y1+12, QString("%1uA").arg(it.amp_uA));
    }
}
