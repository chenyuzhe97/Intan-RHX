#pragma once
#include <QWidget>
#include <QVector>
#include <QTimer>
#include <QMutex>
#include <QRect>

#include "dsp_biquad.h"

class QResizeEvent;
class QScrollBar;

class StackedWaveWidget : public QWidget
{
    Q_OBJECT
public:
    explicit StackedWaveWidget(QWidget *parent=nullptr);

    // windowSec: 初始显示时窗
    // maxWindowSec: 允许 Ctrl+滚轮 放大的最大时窗（预分配 ring buffer 容量）
    void configure(int channels, double sampleRateHz, double windowSec, double maxWindowSec = 10.0);

    void setTitle(const QString &t) { m_title = t; }
    void setGainUv(double halfRangeUv);              // ±uV
    void setWindowSec(double sec);                   // 显示时窗（秒）
    void setOverviewLaneHeight(int px);              // 总览中每个框的高度

signals:
    void selectedChannelChanged(int ch);
    void overviewLaneHeightChanged(int px);

public slots:
    void pushBlock(const QVector<uint32_t> &timeStamps,
                   const QVector<QVector<int>> &channelData);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;

private:
    struct Ring {
        QVector<float> buf;
        int head = 0;
        bool full = false;

        void init(int cap){
            buf.fill(0.0f, cap);
            head = 0; full = false;
        }
        int cap()  const { return buf.size(); }
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

    struct ViewRange {
        int nAvail = 0;
        int nWin = 0;
        int start = 0;
        int end = 0;
        bool valid = false;
    };

    struct ChannelStats {
        float minUv = 0.0f;
        float maxUv = 0.0f;
        float rmsUv = 0.0f;
        float p2pUv = 0.0f;
        bool valid = false;
    };

    enum class ViewMode { Overview, Single };

    int pickChannelAtPos(const QPoint &pos, const QRect &contentRect) const;
    int overviewColumnCount(const QRect &contentRect) const;
    int overviewContentHeight(const QRect &contentRect) const;
    QRect overviewViewportRect() const;
    QRect overviewCardRect(const QRect &contentRect, int ch) const;
    ViewRange currentViewRangeLocked() const;
    ChannelStats computeStatsLocked(const Ring &ring, const ViewRange &range) const;
    void drawHeader(QPainter &p, const QRect &rect) const;
    void drawOverview(QPainter &p, const QRect &contentRect, const ViewRange &range) const;
    void drawSingle(QPainter &p, const QRect &contentRect, const ViewRange &range) const;
    void drawWaveform(QPainter &p,
                      const QRect &plotRect,
                      const Ring &ring,
                      const ViewRange &range,
                      const QColor &color,
                      double gainUv,
                      int zeroY) const;
    void drawTimeRuler(QPainter &p, const QRect &plotRect) const;
    void resetView();
    void autoScaleSelectedLocked(const ViewRange &range);
    void updateOverviewScrollBarLocked();

private:
    QVector<Ring> m_rings;
    int    m_channels = 16;
    double m_fs = 30000.0;

    // 显示控制
    double m_windowSec = 2.0;
    double m_windowSecMin = 0.05;
    double m_windowSecMax = 10.0;

    double m_gainUv = 500.0;     // ±gainUv
    double m_gainMin = 10.0;
    double m_gainMax = 200000.0;

    int m_overviewLaneHeight = 58;
    int m_overviewLaneHeightMin = 28;
    int m_overviewLaneHeightMax = 180;

    QString m_title;
    bool m_hasData = false;

    // 交互状态
    ViewMode m_mode = ViewMode::Overview;
    int m_selectedCh = 0;        // 点击选中的通道
    int m_focusCh = -1;          // 单通道模式的通道（>=0）
    int m_hoverCh = -1;

    // 平移（仅单通道模式生效）：向左看历史 = panSamples 增加
    int  m_panSamples = 0;
    bool m_dragging = false;
    int  m_dragStartX = 0;
    int  m_panSamplesAtDragStart = 0;

    QTimer m_repaint;
    QScrollBar *m_overviewScrollBar = nullptr;
    mutable QMutex m_mtx;

public:
    struct FilterSettings {
        bool enabled = false;
        enum class Type { Off, LowPass, HighPass, BandPass } type = Type::Off;

        double lp_hz = 3000.0;
        double hp_hz = 300.0;
        double bp_low_hz = 300.0;
        double bp_high_hz = 3000.0;

        bool notchEnabled = false;
        double notch_hz = 50.0;   // 50 or 60
        double notchQ = 30.0;
    };

    void setFilterSettings(const FilterSettings &s);
    FilterSettings filterSettings() const { return m_filt; }

    // FFT 需要用到
    bool copySamplesForFft(int ch, int N, QVector<float> &out) const;
    double sampleRateHz() const { return m_fs; }
    int selectedChannel() const { return m_selectedCh; }
    int overviewLaneHeight() const { return m_overviewLaneHeight; }

private:
    FilterSettings m_filt;

    // 每通道滤波状态（notch + main）
    QVector<Biquad> m_bqNotch;
    QVector<Biquad> m_bqMain;
};