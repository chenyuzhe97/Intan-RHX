#include "stackedwavewidget.h"

#include <QContextMenuEvent>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>

StackedWaveWidget::StackedWaveWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(560, 420);
    setMouseTracking(true);

    m_overviewScrollBar = new QScrollBar(Qt::Vertical, this);
    m_overviewScrollBar->hide();
    m_overviewScrollBar->setFocusPolicy(Qt::NoFocus);
    connect(m_overviewScrollBar, &QScrollBar::valueChanged, this, QOverload<>::of(&StackedWaveWidget::update));

    m_repaint.setInterval(33); // ~30 fps
    connect(&m_repaint, &QTimer::timeout, this, QOverload<>::of(&StackedWaveWidget::update));
    m_repaint.start();
}

void StackedWaveWidget::configure(int channels, double sampleRateHz, double windowSec, double maxWindowSec)
{
    QMutexLocker lk(&m_mtx);
    m_channels = qMax(1, channels);
    m_fs = qMax(1.0, sampleRateHz);

    m_windowSecMax = qMax(0.2, maxWindowSec);
    m_windowSec = qBound(m_windowSecMin, windowSec, m_windowSecMax);

    m_bqNotch.resize(m_channels);
    m_bqMain.resize(m_channels);
    for (int ch=0; ch<m_channels; ++ch) {
        m_bqNotch[ch].reset();
        m_bqMain[ch].reset();
    }

    const int cap = qMax(2048, int(qCeil((m_windowSecMax + 0.5) * m_fs)));
    m_rings.resize(m_channels);
    for (int ch=0; ch<m_channels; ++ch) m_rings[ch].init(cap);

    m_hasData = false;
    m_selectedCh = 0;
    m_hoverCh = -1;
    resetView();
    updateOverviewScrollBarLocked();
}

void StackedWaveWidget::setGainUv(double halfRangeUv)
{
    QMutexLocker lk(&m_mtx);
    m_gainUv = qBound(m_gainMin, halfRangeUv, m_gainMax);
}

void StackedWaveWidget::setWindowSec(double sec)
{
    QMutexLocker lk(&m_mtx);
    m_windowSec = qBound(m_windowSecMin, sec, m_windowSecMax);
}

void StackedWaveWidget::setOverviewLaneHeight(int px)
{
    bool changed = false;
    {
        QMutexLocker lk(&m_mtx);
        const int clamped = qBound(m_overviewLaneHeightMin, px, m_overviewLaneHeightMax);
        if (clamped != m_overviewLaneHeight) {
            m_overviewLaneHeight = clamped;
            updateOverviewScrollBarLocked();
            changed = true;
        }
    }

    if (changed) emit overviewLaneHeightChanged(m_overviewLaneHeight);
    if (changed) update();
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

            if (m_filt.notchEnabled) uV = m_bqNotch[ch].process(uV);
            if (m_filt.enabled && m_filt.type != FilterSettings::Type::Off) uV = m_bqMain[ch].process(uV);

            m_rings[ch].push(uV);
        }
    }

    m_hasData = true;
}

int StackedWaveWidget::overviewColumnCount(const QRect &contentRect) const
{
    Q_UNUSED(contentRect);
    return 1;
}

int StackedWaveWidget::overviewContentHeight(const QRect &contentRect) const
{
    Q_UNUSED(contentRect);
    const int gap = 8;
    return m_channels * m_overviewLaneHeight + qMax(0, m_channels - 1) * gap;
}

QRect StackedWaveWidget::overviewViewportRect() const
{
    QRect contentRect = rect().adjusted(12, 54, -12, -14);
    if (m_mode == ViewMode::Overview && m_overviewScrollBar && m_overviewScrollBar->isVisible()) {
        contentRect.adjust(0, 0, -(m_overviewScrollBar->width() + 8), 0);
    }
    return contentRect;
}

QRect StackedWaveWidget::overviewCardRect(const QRect &contentRect, int ch) const
{
    const int columns = overviewColumnCount(contentRect);
    const int gap = 8;
    const int scroll = (m_mode == ViewMode::Overview && m_overviewScrollBar && m_overviewScrollBar->isVisible())
        ? m_overviewScrollBar->value()
        : 0;

    const int cellW = (contentRect.width() - gap * (columns - 1)) / columns;
    const int cellH = m_overviewLaneHeight;

    const int row = ch / columns;
    const int col = ch % columns;
    const int x = contentRect.left() + col * (cellW + gap);
    const int y = contentRect.top() + row * (cellH + gap) - scroll;
    return QRect(x, y, cellW, cellH);
}

int StackedWaveWidget::pickChannelAtPos(const QPoint &pos, const QRect &contentRect) const
{
    if (m_mode == ViewMode::Single) {
        return qBound(0, (m_focusCh >= 0 ? m_focusCh : m_selectedCh), m_channels - 1);
    }

    for (int ch = 0; ch < m_channels; ++ch) {
        if (overviewCardRect(contentRect, ch).contains(pos)) {
            return ch;
        }
    }
    return -1;
}

StackedWaveWidget::ViewRange StackedWaveWidget::currentViewRangeLocked() const
{
    ViewRange range;
    if (m_rings.isEmpty()) return range;

    range.nAvail = m_rings[0].size();
    range.nWin = qMin(range.nAvail, int(m_windowSec * m_fs));
    if (range.nWin < 2) return range;

    int end = range.nAvail - 1;
    if (m_mode == ViewMode::Single) {
        end = range.nAvail - 1 - m_panSamples;
    }
    end = qBound(range.nWin - 1, end, range.nAvail - 1);

    range.end = end;
    range.start = qMax(0, range.end - (range.nWin - 1));
    range.valid = true;
    return range;
}

StackedWaveWidget::ChannelStats StackedWaveWidget::computeStatsLocked(const Ring &ring, const ViewRange &range) const
{
    ChannelStats stats;
    if (!range.valid) return stats;

    float vmin = ring.atFromOldest(range.start);
    float vmax = vmin;
    double sumSq = 0.0;

    for (int i = range.start; i <= range.end; ++i) {
        const float v = ring.atFromOldest(i);
        vmin = qMin(vmin, v);
        vmax = qMax(vmax, v);
        sumSq += double(v) * double(v);
    }

    stats.minUv = vmin;
    stats.maxUv = vmax;
    stats.p2pUv = vmax - vmin;
    stats.rmsUv = float(std::sqrt(sumSq / double(range.end - range.start + 1)));
    stats.valid = true;
    return stats;
}

void StackedWaveWidget::drawHeader(QPainter &p, const QRect &rect) const
{
    QRect headerRect = rect.adjusted(10, 10, -10, -rect.height() + 44);

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(43, 73, 88), 1));
    p.setBrush(QColor(11, 19, 27, 220));
    p.drawRoundedRect(headerRect, 12, 12);

    QFont titleFont = p.font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 1);
    p.setFont(titleFont);
    p.setPen(QColor(231, 238, 243));
    p.drawText(headerRect.adjusted(14, 0, -180, 0), Qt::AlignVCenter | Qt::AlignLeft, m_title);

    p.setFont(QFont(titleFont.family(), titleFont.pointSize() - 1, QFont::Medium));
    const QString modeText = (m_mode == ViewMode::Overview)
        ? QStringLiteral("Stacked lanes (wheel y-range / Alt-wheel scroll)")
        : QStringLiteral("Focus CH%1").arg(m_focusCh + 1, 2, 10, QChar('0'));

    QString filterText = QStringLiteral("RAW");
    if (m_filt.enabled && m_filt.type == FilterSettings::Type::BandPass) {
        filterText = QStringLiteral("BP %1-%2 Hz")
                         .arg(m_filt.bp_low_hz, 0, 'f', 0)
                         .arg(m_filt.bp_high_hz, 0, 'f', 0);
    } else if (m_filt.enabled && m_filt.type == FilterSettings::Type::LowPass) {
        filterText = QStringLiteral("LP %1 Hz").arg(m_filt.lp_hz, 0, 'f', 0);
    } else if (m_filt.enabled && m_filt.type == FilterSettings::Type::HighPass) {
        filterText = QStringLiteral("HP %1 Hz").arg(m_filt.hp_hz, 0, 'f', 0);
    }
    if (m_filt.notchEnabled) {
        filterText += QStringLiteral(" + Notch %1").arg(m_filt.notch_hz, 0, 'f', 0);
    }

    const QString meta = QStringLiteral("%1   |   CH%2   |   %3 s   |   ±%4 µV   |   lane %5 px   |   %6")
                             .arg(modeText)
                             .arg(m_selectedCh + 1, 2, 10, QChar('0'))
                             .arg(m_windowSec, 0, 'f', 2)
                             .arg(m_gainUv, 0, 'f', 0)
                             .arg(m_overviewLaneHeight)
                             .arg(filterText);

    p.setPen(QColor(139, 169, 181));
    p.drawText(headerRect.adjusted(360, 0, -14, 0), Qt::AlignVCenter | Qt::AlignRight, meta);
    p.restore();
}

void StackedWaveWidget::drawWaveform(QPainter &p,
                                     const QRect &plotRect,
                                     const Ring &ring,
                                     const ViewRange &range,
                                     const QColor &color,
                                     double gainUv,
                                     int zeroY) const
{
    if (!range.valid || plotRect.width() <= 2 || plotRect.height() <= 2) return;

    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(color);

    const double yScale = (plotRect.height() * 0.44) / qMax(1.0, gainUv);
    for (int x = 0; x < plotRect.width(); ++x) {
        int i0 = range.start + int(double(x) / double(plotRect.width()) * (range.end - range.start + 1));
        int i1 = range.start + int(double(x + 1) / double(plotRect.width()) * (range.end - range.start + 1));
        if (i1 <= i0) i1 = i0 + 1;
        if (i1 > range.end + 1) i1 = range.end + 1;

        float vmin = ring.atFromOldest(i0);
        float vmax = vmin;
        for (int i = i0 + 1; i < i1; ++i) {
            const float v = ring.atFromOldest(i);
            vmin = qMin(vmin, v);
            vmax = qMax(vmax, v);
        }

        const int y1 = zeroY - int(vmin * yScale);
        const int y2 = zeroY - int(vmax * yScale);
        p.drawLine(plotRect.left() + x, y1, plotRect.left() + x, y2);
    }
    p.restore();
}

void StackedWaveWidget::drawTimeRuler(QPainter &p, const QRect &plotRect) const
{
    p.save();
    p.setPen(QColor(86, 112, 125));

    const int ticks = 5;
    for (int i = 0; i <= ticks; ++i) {
        const double t = m_windowSec * double(i) / double(ticks);
        const int x = plotRect.left() + int(double(plotRect.width()) * double(i) / double(ticks));
        p.drawLine(x, plotRect.bottom() + 2, x, plotRect.bottom() + 7);
        p.drawText(x - 16, plotRect.bottom() + 20, QString::number(t, 'f', 1) + " s");
    }
    p.restore();
}

void StackedWaveWidget::drawOverview(QPainter &p, const QRect &contentRect, const ViewRange &range) const
{
    const QColor laneFillEven(14, 23, 31);
    const QColor laneFillOdd(10, 18, 25);
    const QColor laneBorder(35, 55, 68);
    const QColor laneHover(64, 194, 176);
    const QColor laneSelected(244, 196, 82);
    const QColor tagFill(12, 22, 30);
    const QColor wave(106, 246, 232);
    const QColor waveHover(166, 248, 239);
    const QColor waveSelected(255, 221, 128);

    for (int ch = 0; ch < m_channels; ++ch) {
        const QRect outer = overviewCardRect(contentRect, ch);
        if (outer.bottom() < contentRect.top() - 2 || outer.top() > contentRect.bottom() + 2) continue;
        if (outer.width() < 260 || outer.height() < 26) continue;

        const bool compact = outer.height() < 54;
        const int pad = compact ? 5 : 7;
        const int tagW = compact ? 88 : 124;
        const QRect tagRect(outer.left() + pad, outer.top() + pad, tagW, outer.height() - pad * 2);
        const int plotX = tagRect.right() + 8;
        const QRect plotRect(plotX,
                             outer.top() + pad,
                             outer.right() - pad - plotX + 1,
                             outer.height() - pad * 2);
        if (plotRect.width() < 40 || plotRect.height() < 16) continue;

        const bool isSelected = (ch == m_selectedCh);
        const bool isHover = (ch == m_hoverCh);
        const QColor edge = isSelected ? laneSelected : (isHover ? laneHover : laneBorder);

        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(edge, isSelected ? 1.7 : 1.0));
        p.setBrush((ch % 2 == 0) ? laneFillEven : laneFillOdd);
        p.drawRoundedRect(outer, 10, 10);

        p.setPen(Qt::NoPen);
        p.setBrush(tagFill);
        p.drawRoundedRect(tagRect, 8, 8);

        QFont labelFont = p.font();
        labelFont.setBold(true);
        labelFont.setPointSize(qMax(8, labelFont.pointSize() - (compact ? 1 : 0)));
        p.setFont(labelFont);
        p.setPen(QColor(221, 233, 239));
        p.drawText(tagRect.adjusted(8, compact ? 2 : 4, -6, 0), Qt::AlignLeft | Qt::AlignTop,
                   QStringLiteral("CH %1").arg(ch + 1, 2, 10, QChar('0')));

        const ChannelStats stats = computeStatsLocked(m_rings[ch], range);
        const double overviewScale = qBound(0.25, m_gainUv / 500.0, 12.0);
        double overviewGainUv = qBound(18.0, 120.0 * overviewScale, 5000.0);
        if (range.valid) {
            QVector<float> absSamples;
            const int step = qMax(1, (range.end - range.start + 1) / 180);
            absSamples.reserve((range.end - range.start + step) / step);
            for (int i = range.start; i <= range.end; i += step) {
                absSamples.push_back(std::abs(m_rings[ch].atFromOldest(i)));
            }

            if (!absSamples.isEmpty()) {
                auto p80It = absSamples.begin() + int(0.80 * double(absSamples.size() - 1));
                std::nth_element(absSamples.begin(), p80It, absSamples.end());
                const double p80Abs = *p80It;
                const double rmsDriven = stats.valid ? stats.rmsUv * 4.2 : p80Abs * 1.6;
                const double robustDriven = qMax(rmsDriven, p80Abs * 1.8);
                overviewGainUv = qBound(18.0, robustDriven * overviewScale, 5000.0);
            }
        }

        p.setFont(QFont(labelFont.family(), compact ? 7 : 8));
        p.setPen(QColor(150, 181, 194));
        const QString statText = stats.valid
            ? (compact
                ? QStringLiteral("R%1 P%2").arg(stats.rmsUv, 0, 'f', 0).arg(stats.p2pUv, 0, 'f', 0)
                : QStringLiteral("RMS %1\nP2P %2").arg(stats.rmsUv, 0, 'f', 0).arg(stats.p2pUv, 0, 'f', 0))
            : QStringLiteral("Waiting");
        p.drawText(tagRect.adjusted(8, compact ? 16 : 24, -6, -4),
                   compact ? (Qt::AlignLeft | Qt::AlignVCenter) : (Qt::AlignLeft | Qt::AlignTop),
                   statText);

        p.setPen(QPen(QColor(42, 61, 74), 1));
        p.setBrush(QColor(7, 12, 17));
        p.drawRoundedRect(plotRect, 8, 8);

        p.setClipRect(plotRect.adjusted(1, 1, -1, -1));
        p.setPen(QColor(27, 40, 49));
        for (int g = 1; g <= 4; ++g) {
            const int x = plotRect.left() + plotRect.width() * g / 5;
            p.drawLine(x, plotRect.top() + 3, x, plotRect.bottom() - 3);
        }
        if (!compact) {
            p.setPen(QColor(24, 36, 45));
            p.drawLine(plotRect.left() + 4, plotRect.top() + plotRect.height() / 4,
                       plotRect.right() - 4, plotRect.top() + plotRect.height() / 4);
            p.drawLine(plotRect.left() + 4, plotRect.bottom() - plotRect.height() / 4,
                       plotRect.right() - 4, plotRect.bottom() - plotRect.height() / 4);
        }
        p.setPen(QColor(74, 108, 122));
        p.drawLine(plotRect.left() + 4, plotRect.center().y(), plotRect.right() - 4, plotRect.center().y());

        const QRect waveRect = plotRect.adjusted(4, 4, -4, -4);
        const QColor curve = isSelected ? waveSelected : (isHover ? waveHover : wave);
        if (range.valid) {
            drawWaveform(p, waveRect, m_rings[ch], range, curve, overviewGainUv, waveRect.center().y());
        } else {
            p.setPen(QColor(124, 152, 165));
            p.drawText(plotRect, Qt::AlignCenter, QStringLiteral("Waiting for samples"));
        }
        p.setClipping(false);

        p.setPen(QColor(128, 157, 170));
        p.setFont(QFont(labelFont.family(), compact ? 7 : 8));
        const QString footer = compact
            ? QStringLiteral("+/-%1").arg(overviewGainUv, 0, 'f', 0)
            : QStringLiteral("auto +/-%1 uV   %2 s").arg(overviewGainUv, 0, 'f', 0).arg(m_windowSec, 0, 'f', 2);
        p.drawText(plotRect.adjusted(6, 0, -6, -3), Qt::AlignBottom | Qt::AlignRight, footer);
        p.restore();
    }
}

void StackedWaveWidget::drawSingle(QPainter &p, const QRect &contentRect, const ViewRange &range) const
{
    const int ch = qBound(0, (m_focusCh >= 0 ? m_focusCh : m_selectedCh), m_channels - 1);
    const QRect cardRect = contentRect;
    const QRect infoRect = cardRect.adjusted(16, 12, -16, -cardRect.height() + 40);
    const QRect plotRect = cardRect.adjusted(16, 50, -16, -34);

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(240, 191, 76), 1.6));
    p.setBrush(QColor(16, 25, 34));
    p.drawRoundedRect(cardRect, 18, 18);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(22, 35, 47));
    p.drawRoundedRect(QRect(cardRect.left(), cardRect.top(), cardRect.width(), 44), 18, 18);
    p.drawRect(QRect(cardRect.left(), cardRect.top() + 22, cardRect.width(), 22));

    QFont titleFont = p.font();
    titleFont.setBold(true);
    titleFont.setPointSize(titleFont.pointSize() + 1);
    p.setFont(titleFont);
    p.setPen(QColor(242, 246, 249));
    p.drawText(infoRect.adjusted(0, 0, -200, 0), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("Channel %1 Focus").arg(ch + 1, 2, 10, QChar('0')));

    const ChannelStats stats = computeStatsLocked(m_rings[ch], range);
    p.setFont(QFont(titleFont.family(), qMax(9, titleFont.pointSize() - 1)));
    p.setPen(QColor(156, 186, 199));
    const QString statText = stats.valid
        ? QStringLiteral("RMS %1   P2P %2   Pan %3 samples")
              .arg(stats.rmsUv, 0, 'f', 0)
              .arg(stats.p2pUv, 0, 'f', 0)
              .arg(m_panSamples)
        : QStringLiteral("Waiting for data");
    p.drawText(infoRect.adjusted(180, 0, 0, 0), Qt::AlignRight | Qt::AlignVCenter, statText);

    p.setPen(QPen(QColor(55, 76, 88), 1));
    p.setBrush(QColor(11, 18, 24));
    p.drawRoundedRect(plotRect, 12, 12);

    p.setClipRect(plotRect.adjusted(1, 1, -1, -1));
    p.setPen(QColor(35, 50, 61));
    for (int g = 1; g <= 4; ++g) {
        const int x = plotRect.left() + plotRect.width() * g / 5;
        p.drawLine(x, plotRect.top() + 8, x, plotRect.bottom() - 18);
    }
    for (int g = 1; g <= 4; ++g) {
        const int y = plotRect.top() + plotRect.height() * g / 5;
        p.drawLine(plotRect.left() + 8, y, plotRect.right() - 8, y);
    }
    p.setPen(QColor(76, 108, 121));
    p.drawLine(plotRect.left() + 8, plotRect.center().y(), plotRect.right() - 8, plotRect.center().y());

    drawWaveform(p, plotRect.adjusted(8, 8, -8, -24), m_rings[ch], range, QColor(255, 207, 92), m_gainUv, plotRect.center().y() - 8);
    p.setClipping(false);

    p.setPen(QColor(201, 213, 221));
    p.setFont(QFont(titleFont.family(), qMax(9, titleFont.pointSize() - 2)));
    p.drawText(plotRect.left() + 8, plotRect.top() + 18, QStringLiteral("+%1 µV").arg(m_gainUv, 0, 'f', 0));
    p.drawText(plotRect.left() + 8, plotRect.center().y() - 4, QStringLiteral("0"));
    p.drawText(plotRect.left() + 8, plotRect.bottom() - 26, QStringLiteral("-%1 µV").arg(m_gainUv, 0, 'f', 0));
    drawTimeRuler(p, plotRect.adjusted(12, 0, -12, -20));
    p.restore();
}

void StackedWaveWidget::resetView()
{
    m_mode = ViewMode::Overview;
    m_focusCh = -1;
    m_panSamples = 0;
    m_dragging = false;
    m_hoverCh = -1;
}

void StackedWaveWidget::autoScaleSelectedLocked(const ViewRange &range)
{
    if (!range.valid || m_rings.isEmpty()) return;

    const int ch = qBound(0, (m_focusCh >= 0 ? m_focusCh : m_selectedCh), m_channels - 1);
    float peakAbs = 0.0f;
    for (int i = range.start; i <= range.end; ++i) {
        peakAbs = qMax(peakAbs, std::abs(m_rings[ch].atFromOldest(i)));
    }
    if (peakAbs <= 1.0f) peakAbs = 1.0f;
    m_gainUv = qBound(m_gainMin, double(peakAbs) * 1.35, m_gainMax);
}

void StackedWaveWidget::updateOverviewScrollBarLocked()
{
    if (!m_overviewScrollBar) return;

    const QRect baseRect = rect().adjusted(12, 54, -12, -14);
    if (baseRect.width() < 40 || baseRect.height() < 40 || m_mode != ViewMode::Overview) {
        m_overviewScrollBar->hide();
        m_overviewScrollBar->setValue(0);
        return;
    }

    const int contentHeight = overviewContentHeight(baseRect);
    const bool needScroll = contentHeight > baseRect.height();
    if (!needScroll) {
        m_overviewScrollBar->hide();
        m_overviewScrollBar->setValue(0);
        return;
    }

    const int scrollW = 14;
    m_overviewScrollBar->setGeometry(baseRect.right() - scrollW + 1, baseRect.top() + 2, scrollW, baseRect.height() - 4);
    m_overviewScrollBar->setPageStep(baseRect.height());
    m_overviewScrollBar->setSingleStep(qMax(12, m_overviewLaneHeight / 3));
    m_overviewScrollBar->setRange(0, qMax(0, contentHeight - baseRect.height()));
    m_overviewScrollBar->show();
}

void StackedWaveWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    QLinearGradient bg(0, 0, 0, height());
    bg.setColorAt(0.0, QColor(10, 18, 26));
    bg.setColorAt(1.0, QColor(5, 9, 13));
    p.fillRect(rect(), bg);

    drawHeader(p, rect());

    QMutexLocker lk(&m_mtx);
    updateOverviewScrollBarLocked();
    const QRect contentRect = overviewViewportRect();
    if (m_rings.isEmpty() || contentRect.width() < 60 || contentRect.height() < 60) return;

    const ViewRange range = currentViewRangeLocked();
    if (m_mode == ViewMode::Overview) {
        drawOverview(p, contentRect, range);
    } else {
        drawSingle(p, contentRect, range);
    }
}

void StackedWaveWidget::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    QMutexLocker lk(&m_mtx);
    updateOverviewScrollBarLocked();
}

void StackedWaveWidget::mousePressEvent(QMouseEvent *e)
{
    int changedCh = -1;
    {
        QMutexLocker lk(&m_mtx);
        updateOverviewScrollBarLocked();
        const QRect contentRect = overviewViewportRect();
        if (e->button() != Qt::LeftButton || !contentRect.contains(e->pos())) return;

        const int ch = pickChannelAtPos(e->pos(), contentRect);
        if (ch >= 0 && ch != m_selectedCh) {
            m_selectedCh = ch;
            changedCh = ch;
        }

        if (m_mode == ViewMode::Single && (e->modifiers() & Qt::ShiftModifier)) {
            m_dragging = true;
            m_dragStartX = e->pos().x();
            m_panSamplesAtDragStart = m_panSamples;
        }
    }

    if (changedCh >= 0) emit selectedChannelChanged(changedCh);
    update();
}

void StackedWaveWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;

    int changedCh = -1;
    {
        QMutexLocker lk(&m_mtx);
        updateOverviewScrollBarLocked();
        if (m_mode == ViewMode::Overview) {
            const QRect contentRect = overviewViewportRect();
            const int ch = pickChannelAtPos(e->pos(), contentRect);
            if (ch >= 0 && ch != m_selectedCh) {
                m_selectedCh = ch;
                changedCh = ch;
            }
            m_mode = ViewMode::Single;
            m_focusCh = (ch >= 0) ? ch : m_selectedCh;
            m_panSamples = 0;
        } else {
            resetView();
            updateOverviewScrollBarLocked();
        }
    }

    if (changedCh >= 0) emit selectedChannelChanged(changedCh);
    update();
}

void StackedWaveWidget::mouseMoveEvent(QMouseEvent *e)
{
    QMutexLocker lk(&m_mtx);
    updateOverviewScrollBarLocked();
    const QRect contentRect = overviewViewportRect();

    if (m_dragging) {
        if (m_mode != ViewMode::Single || m_rings.isEmpty()) return;

        const int plotW = qMax(10, contentRect.width() - 32);
        const int dx = e->pos().x() - m_dragStartX;
        const int nAvail = m_rings[0].size();
        const int nWin = qMin(nAvail, int(m_windowSec * m_fs));
        const double samplesPerPixel = double(qMax(1, nWin)) / double(plotW);
        const int deltaSamples = int(-dx * samplesPerPixel);

        const int maxPan = qMax(0, nAvail - nWin);
        m_panSamples = qBound(0, m_panSamplesAtDragStart + deltaSamples, maxPan);
        update();
        return;
    }

    int hover = -1;
    if (contentRect.contains(e->pos()) && m_mode == ViewMode::Overview) {
        hover = pickChannelAtPos(e->pos(), contentRect);
    }
    if (hover != m_hoverCh) {
        m_hoverCh = hover;
        update();
    }
}

void StackedWaveWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        m_dragging = false;
    }
}

void StackedWaveWidget::wheelEvent(QWheelEvent *e)
{
    bool laneChanged = false;
    int laneHeight = m_overviewLaneHeight;

    {
        QMutexLocker lk(&m_mtx);
        updateOverviewScrollBarLocked();

        const bool ctrl = (e->modifiers() & Qt::ControlModifier);
        const bool shift = (e->modifiers() & Qt::ShiftModifier);
        const bool alt = (e->modifiers() & Qt::AltModifier);
        const int steps = (e->angleDelta().y() / 120);
        if (steps == 0) return;

        auto applyZoom = [&](double &val, double factor, double vmin, double vmax){
            if (steps > 0) val *= factor;
            else          val /= factor;
            val = qBound(vmin, val, vmax);
        };

        if (m_mode == ViewMode::Overview && alt) {
            if (m_overviewScrollBar && m_overviewScrollBar->isVisible()) {
                m_overviewScrollBar->setValue(m_overviewScrollBar->value() - steps * m_overviewScrollBar->singleStep());
            }
        } else if (m_mode == ViewMode::Overview && shift) {
            const int nextHeight = qBound(m_overviewLaneHeightMin,
                                          m_overviewLaneHeight + steps * 8,
                                          m_overviewLaneHeightMax);
            if (nextHeight != m_overviewLaneHeight) {
                m_overviewLaneHeight = nextHeight;
                laneHeight = nextHeight;
                laneChanged = true;
                updateOverviewScrollBarLocked();
            }
        } else if (ctrl) {
            applyZoom(m_windowSec, 0.82, m_windowSecMin, m_windowSecMax);

            if (!m_rings.isEmpty()) {
                const int nAvail = m_rings[0].size();
                const int nWin = qMin(nAvail, int(m_windowSec * m_fs));
                const int maxPan = qMax(0, nAvail - nWin);
                m_panSamples = qBound(0, m_panSamples, maxPan);
            }
        } else {
            applyZoom(m_gainUv, 0.82, m_gainMin, m_gainMax);
        }
    }

    if (laneChanged) emit overviewLaneHeightChanged(laneHeight);
    update();
}

void StackedWaveWidget::contextMenuEvent(QContextMenuEvent *e)
{
    int changedCh = -1;
    {
        QMutexLocker lk(&m_mtx);
        updateOverviewScrollBarLocked();
        const QRect contentRect = overviewViewportRect();
        const int ch = pickChannelAtPos(e->pos(), contentRect);
        if (ch >= 0 && ch != m_selectedCh) {
            m_selectedCh = ch;
            changedCh = ch;
        }
    }
    if (changedCh >= 0) emit selectedChannelChanged(changedCh);

    QMenu menu(this);
    QAction *focusAction = menu.addAction(m_mode == ViewMode::Overview ? tr("聚焦选中通道") : tr("返回总览"));
    QAction *autoGainAction = menu.addAction(tr("自动匹配当前通道幅度"));
    QAction *growLaneAction = nullptr;
    QAction *shrinkLaneAction = nullptr;
    QAction *resetLaneAction = nullptr;
    if (m_mode == ViewMode::Overview) {
        menu.addSeparator();
        growLaneAction = menu.addAction(tr("增大总览框"));
        shrinkLaneAction = menu.addAction(tr("减小总览框"));
        resetLaneAction = menu.addAction(tr("总览框恢复默认"));
    }
    menu.addSeparator();
    QAction *resetAction = menu.addAction(tr("重置视图"));

    QAction *selected = menu.exec(e->globalPos());
    if (!selected) return;

    bool laneChanged = false;
    int laneHeight = m_overviewLaneHeight;
    {
        QMutexLocker lk(&m_mtx);
        if (selected == focusAction) {
            if (m_mode == ViewMode::Overview) {
                m_mode = ViewMode::Single;
                m_focusCh = m_selectedCh;
                m_panSamples = 0;
                updateOverviewScrollBarLocked();
            } else {
                resetView();
                updateOverviewScrollBarLocked();
            }
        } else if (selected == autoGainAction) {
            autoScaleSelectedLocked(currentViewRangeLocked());
        } else if (selected == growLaneAction) {
            m_overviewLaneHeight = qBound(m_overviewLaneHeightMin, m_overviewLaneHeight + 10, m_overviewLaneHeightMax);
            laneHeight = m_overviewLaneHeight;
            laneChanged = true;
            updateOverviewScrollBarLocked();
        } else if (selected == shrinkLaneAction) {
            m_overviewLaneHeight = qBound(m_overviewLaneHeightMin, m_overviewLaneHeight - 10, m_overviewLaneHeightMax);
            laneHeight = m_overviewLaneHeight;
            laneChanged = true;
            updateOverviewScrollBarLocked();
        } else if (selected == resetLaneAction) {
            m_overviewLaneHeight = 58;
            laneHeight = m_overviewLaneHeight;
            laneChanged = true;
            updateOverviewScrollBarLocked();
        } else if (selected == resetAction) {
            resetView();
            updateOverviewScrollBarLocked();
        }
    }

    if (laneChanged) emit overviewLaneHeightChanged(laneHeight);
    update();
}

void StackedWaveWidget::setFilterSettings(const FilterSettings &s)
{
    QMutexLocker lk(&m_mtx);
    m_filt = s;

    for (int ch=0; ch<m_channels; ++ch) {
        m_bqNotch[ch].reset();
        m_bqMain[ch].reset();

        if (m_filt.notchEnabled) {
            m_bqNotch[ch] = Biquad::notch(m_fs, m_filt.notch_hz, m_filt.notchQ);
        }

        if (!m_filt.enabled || m_filt.type == FilterSettings::Type::Off) {
            m_bqMain[ch] = Biquad();
        } else if (m_filt.type == FilterSettings::Type::LowPass) {
            m_bqMain[ch] = Biquad::lowpass(m_fs, m_filt.lp_hz);
        } else if (m_filt.type == FilterSettings::Type::HighPass) {
            m_bqMain[ch] = Biquad::highpass(m_fs, m_filt.hp_hz);
        } else if (m_filt.type == FilterSettings::Type::BandPass) {
            const double f1 = qMax(1.0, m_filt.bp_low_hz);
            const double f2 = qMax(f1 + 1.0, m_filt.bp_high_hz);
            const double fc = std::sqrt(f1 * f2);
            const double Q = fc / (f2 - f1 + 1e-9);
            m_bqMain[ch] = Biquad::bandpass(m_fs, fc, Q);
        }
    }
}

bool StackedWaveWidget::copySamplesForFft(int ch, int N, QVector<float> &out) const
{
    QMutexLocker lk(&m_mtx);
    if (!m_hasData || m_rings.isEmpty()) return false;
    ch = qBound(0, ch, m_channels - 1);

    const int nAvail = m_rings[ch].size();
    if (nAvail < N) return false;

    int end = nAvail - 1;
    if (m_mode == ViewMode::Single) end = nAvail - 1 - m_panSamples;
    end = qBound(N-1, end, nAvail-1);
    const int start = end - (N - 1);

    out.resize(N);
    for (int i = 0; i < N; ++i) {
        out[i] = m_rings[ch].atFromOldest(start + i);
    }
    return true;
}