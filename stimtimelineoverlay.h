#pragma once
#include <QWidget>
#include <QVector>

class StimTimelineOverlay : public QWidget
{
    Q_OBJECT
public:
    struct Item {
        int itemIndex = 0;
        double offsetMs = 0.0;
        int amp_uA = 0;
        int pulses = 1;
        int ch = -1;
        double spike_uV = 0.0;
        QString electrode;
        bool fired = false;
    };

    explicit StimTimelineOverlay(QWidget *parent=nullptr);

    void setEpochPlan(int epochId, int phaseIndex, double epochSec,
                      const QVector<Item> &items);

    void markFired(int epochId, int itemIndex);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    int m_epochId = 0;
    int m_phaseIndex = 0;
    double m_epochSec = 5.0;
    QVector<Item> m_items;
};
