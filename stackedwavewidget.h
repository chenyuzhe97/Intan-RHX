#pragma once
#include <QWidget>
#include <QVector>
#include <QTimer>
#include <QMutex>

class StackedWaveWidget : public QWidget
{
    Q_OBJECT
public:
    explicit StackedWaveWidget(QWidget *parent=nullptr);

    void configure(int channels, double sampleRateHz, double windowSec);
    void setTitle(const QString &t) { m_title = t; }
    void setGainUv(double halfRangeUv);

public slots:
    void pushBlock(const QVector<uint32_t> &timeStamps,
                   const QVector<QVector<int>> &channelData);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    struct Ring {
        QVector<float> buf;
        int head = 0;
        bool full = false;

        void init(int cap){
            buf.fill(0.0f, cap);
            head = 0; full = false;
        }
        int size() const { return full ? buf.size() : head; }
        void push(float v){
            if (buf.isEmpty()) return;
            buf[head] = v;
            head = (head + 1) % buf.size();
            if (head == 0) full = true;
        }
        float atFromOldest(int idx) const {
            const int c = buf.size();
            const int start = full ? head : 0;
            return buf[(start + idx) % c];
        }
    };

    QVector<Ring> m_rings;
    int    m_channels = 16;
    double m_fs = 30000.0;
    double m_windowSec = 2.0;
    double m_gainUv = 500.0;
    QString m_title;

    bool m_hasData = false;

    QTimer m_repaint;
    QMutex m_mtx;
};
