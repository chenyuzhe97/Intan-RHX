#include "stackedwavewidget.h"

#include <QContextMenuEvent>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QtMath>

#include <cmath>

StackedWaveWidget::StackedWaveWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(560, 420);
    setMouseTracking(true);

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
    const int maxColumnsByWidth = qBound(1, contentRect.width() / 160, qMin(m_channels, 6));
    int columns = qBound(1,
                         int(qRound(std::sqrt(double(m_channels) * double(contentRect.width()) / double(qMax(1, contentRect.height()))))),
                         maxColumnsByWidth);

    auto cellHeightFor = [&](int cols) -> int {
        const int rows = qMax(1, (m_channels + cols - 1) / cols);
        const int gap = 10;
        return (contentRect.height() - gap * (rows - 1)) / rows;
    };

    while (columns < maxColumnsByWidth && cellHeightFor(columns) < 88) {
        ++columns;
    }
    while (columns > 1 && (contentRect.width() - 10 * (columns - 1)) / columns < 150) {
        --columns;
    }

    return qBound(1, columns, qMax(1, maxColumnsByWidth));
}

QRect StackedWaveWidget::overviewCardRect(const QRect &contentRect, int ch) const
{
    const int columns = overviewColumnCount(contentRect);
    const int rows = qMax(1, (m_channels + columns - 1) / columns);
    const int gap = 10;
    const int cellW = (contentRect.width() - gap * (columns - 1)) / columns;
    const int cellH = (contentRect.height() - gap * (rows - 1)) / rows;

    const int row = ch / columns;
    const int col = ch % columns;
    const int x = contentRect.left() + col * (cellW + gap);
    const int y = contentRect.top() + row * (cellH + gap);
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
        ? QStringLiteral("Overview cards (auto-scale)")
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

    const QString meta = QStringLiteral("%1   |   CH%2   |   %3 s   |   ±%4 µV   |   %5")
                             .arg(modeText)
                             .arg(m_selectedCh + 1, 2, 10, QChar('0'))
                             .arg(m_windowSec, 0, 'f', 2)
                             .arg(m_gainUv, 0, 'f', 0)
                             .arg(filterText);

    p.setPen(QColor(139, 169, 181));
    p.drawText(headerRect.adjusted(320, 0, -14, 0), Qt::AlignVCenter | Qt::AlignRight, meta);
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
    const QColor cardFill(16, 26, 36);
    const QColor headerFill(21, 34, 46);
    const QColor border(34, 54, 66);
    const QColor borderHover(52, 177, 160);
    const QColor borderSelected(240, 191, 76);
    const QColor wave(50, 214, 189);
    const QColor waveSelected(255, 207, 92);
    const QColor waveHover(110, 226, 214);

    for (int ch = 0; ch < m_channels; ++ch) {
        const QRect outer = overviewCardRect(contentRect, ch);
        if (outer.width() < 120 || outer.height() < 82) continue;

        const QRect headerRect = QRect(outer.left() + 10, outer.top() + 8, outer.width() - 20, 20);
        const QRect plotRect = outer.adjusted(10, 30, -10, -24);
        if (plotRect.width() < 24 || plotRect.height() < 24) continue;

        const bool isSelected = (ch == m_selectedCh);
        const bool isHover = (ch == m_hoverCh);
        const QColor edge = isSelected ? borderSelected : (isHover ? borderHover : border);

        p.save();
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(edge, isSelected ? 2.0 : 1.1));
        p.setBrush(cardFill);
        p.drawRoundedRect(outer, 12, 12);

        p.setPen(Qt::NoPen);
        p.setBrush(headerFill);
        p.drawRoundedRect(QRect(outer.left(), outer.top(), outer.width(), 28), 12, 12);
        p.drawRect(QRect(outer.left(), outer.top() + 14, outer.width(), 14));

        p.setPen(QColor(236, 242, 246));
        QFont chFont = p.font();
        chFont.setBold(true);
        p.setFont(chFont);
        p.drawText(headerRect.adjusted(0, 0, -96, 0), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("CH %1").arg(ch + 1, 2, 10, QChar('0')));

        const ChannelStats stats = computeStatsLocked(m_rings[ch], range);
        const double peakAbs = stats.valid ? qMax(std::abs(stats.minUv), std::abs(stats.maxUv)) : m_gainUv;
        const double overviewGainUv = qBound(35.0, peakAbs * 1.45, m_gainMax);

        p.setFont(QFont(chFont.family(), qMax(8, chFont.pointSize() - 2)));
        p.setPen(QColor(156, 186, 199));
        const QString statsText = stats.valid
            ? QStringLiteral("P2P %1  RMS %2")
                  .arg(stats.p2pUv, 0, 'f', 0)
                  .arg(stats.rmsUv, 0, 'f', 0)
            : QStringLiteral("Waiting");
        p.drawText(headerRect.adjusted(92, 0, 0, 0), Qt::AlignRight | Qt::AlignVCenter, statsText);

        p.setPen(QColor(42, 61, 74));
        p.setBrush(QColor(11, 18, 24));
        p.drawRoundedRect(plotRect, 9, 9);

        p.setClipRect(plotRect.adjusted(1, 1, -1, -1));
        p.setPen(QColor(36, 51, 62));
        for (int g = 1; g <= 2; ++g) {
            const int y = plotRect.top() + plotRect.height() * g / 3;
            p.drawLine(plotRect.left() + 5, y, plotRect.right() - 5, y);
        }
        for (int g = 1; g <= 3; ++g) {
            const int x = plotRect.left() + plotRect.width() * g / 4;
            p.drawLine(x, plotRect.top() + 5, x, plotRect.bottom() - 5);
        }
        p.setPen(QColor(64, 92, 104));
        p.drawLine(plotRect.left() + 5, plotRect.center().y(), plotRect.right() - 5, plotRect.center().y());

        const QColor curve = isSelected ? waveSelected : (isHover ? waveHover : wave);
        drawWaveform(p, plotRect.adjusted(5, 5, -5, -5), m_rings[ch], range, curve, overviewGainUv, plotRect.center().y());
        p.setClipping(false);

        p.setPen(QColor(110, 138, 151));
        p.setFont(QFont(chFont.family(), qMax(8, chFont.pointSize() - 3)));
        p.drawText(plotRect.adjusted(6, 0, -6, -4), Qt::AlignBottom | Qt::AlignLeft,
                   QStringLiteral("auto ±%1").arg(overviewGainUv, 0, 'f', 0));
        p.drawText(plotRect.adjusted(6, 0, -6, -4), Qt::AlignBottom | Qt::AlignRight,
                   QStringLiteral("%1 s").arg(m_windowSec, 0, 'f', 2));
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

void StackedWaveWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    QLinearGradient bg(0, 0, 0, height());
    bg.setColorAt(0.0, QColor(10, 18, 26));
    bg.setColorAt(1.0, QColor(5, 9, 13));
    p.fillRect(rect(), bg);

    const QRect contentRect = rect().adjusted(12, 54, -12, -14);
    drawHeader(p, rect());

    QMutexLocker lk(&m_mtx);
    if (m_rings.isEmpty() || contentRect.width() < 60 || contentRect.height() < 60) return;

    const ViewRange range = currentViewRangeLocked();
    if (m_mode == ViewMode::Overview) {
        drawOverview(p, contentRect, range);
    } else {
        drawSingle(p, contentRect, range);
    }
}

void StackedWaveWidget::mousePressEvent(QMouseEvent *e)
{
    const QRect contentRect = rect().adjusted(12, 54, -12, -14);
    if (e->button() != Qt::LeftButton || !contentRect.contains(e->pos())) return;

    int changedCh = -1;
    {
        QMutexLocker lk(&m_mtx);
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
        if (m_mode == ViewMode::Overview) {
            const QRect contentRect = rect().adjusted(12, 54, -12, -14);
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
        }
    }

    if (changedCh >= 0) emit selectedChannelChanged(changedCh);
    update();
}

void StackedWaveWidget::mouseMoveEvent(QMouseEvent *e)
{
    const QRect contentRect = rect().adjusted(12, 54, -12, -14);

    if (m_dragging) {
        QMutexLocker lk(&m_mtx);
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

    bool needUpdate = false;
    {
        QMutexLocker lk(&m_mtx);
        int hover = -1;
        if (contentRect.contains(e->pos()) && m_mode == ViewMode::Overview) {
            hover = pickChannelAtPos(e->pos(), contentRect);
        }
        if (hover != m_hoverCh) {
            m_hoverCh = hover;
            needUpdate = true;
        }
    }

    if (needUpdate) update();
}

void StackedWaveWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        m_dragging = false;
    }
}

void StackedWaveWidget::wheelEvent(QWheelEvent *e)
{
    QMutexLocker lk(&m_mtx);

    const bool ctrl = (e->modifiers() & Qt::ControlModifier);
    const int steps = (e->angleDelta().y() / 120);
    if (steps == 0) return;

    auto applyZoom = [&](double &val, double factor, double vmin, double vmax){
        if (steps > 0) val *= factor;
        else          val /= factor;
        val = qBound(vmin, val, vmax);
    };

    if (ctrl) {
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

    update();
}

void StackedWaveWidget::contextMenuEvent(QContextMenuEvent *e)
{
    int changedCh = -1;
    {
        QMutexLocker lk(&m_mtx);
        const QRect contentRect = rect().adjusted(12, 54, -12, -14);
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
    QAction *resetAction = menu.addAction(tr("重置视图"));

    QAction *selected = menu.exec(e->globalPos());
    if (!selected) return;

    {
        QMutexLocker lk(&m_mtx);
        if (selected == focusAction) {
            if (m_mode == ViewMode::Overview) {
                m_mode = ViewMode::Single;
                m_focusCh = m_selectedCh;
                m_panSamples = 0;
            } else {
                resetView();
            }
        } else if (selected == autoGainAction) {
            autoScaleSelectedLocked(currentViewRangeLocked());
        } else if (selected == resetAction) {
            resetView();
        }
    }

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