#include "stackedwavewidget.h"

#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QtMath>

StackedWaveWidget::StackedWaveWidget(QWidget *parent) : QWidget(parent)
{
    setMinimumSize(420, 360);
    setMouseTracking(true);

    m_repaint.setInterval(33); // ~30 fps
    // 每33ms 触发一次，触发重载后的update代码
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

    // 预分配足够的 ring buffer 容量（允许你 Ctrl+滚轮 放大时间窗）
    const int cap = qMax(2048, int(qCeil((m_windowSecMax + 0.5) * m_fs)));
    m_rings.resize(m_channels);
    for (int ch=0; ch<m_channels; ++ch) m_rings[ch].init(cap);

    m_hasData = false;
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
    // 注意：ring buffer 容量已经按 maxWindowSec 预分配，不需要重配
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
            // ADC -> uV（你之前的换算）
            float uV = float((double(channelData[ch][i]) - 32768.0) * 0.195);

            if (m_filt.notchEnabled) uV = m_bqNotch[ch].process(uV);
            if (m_filt.enabled && m_filt.type != FilterSettings::Type::Off) uV = m_bqMain[ch].process(uV);

            m_rings[ch].push(uV);
        }
    }

    m_hasData = true;
}

int StackedWaveWidget::pickChannelFromY(int y, int padT, int plotH) const
{
    if (plotH <= 0) return 0;
    const int yy = qBound(0, y - padT, plotH - 1);
    const double chH = double(plotH) / double(m_channels);
    int ch = int(yy / chH);
    ch = qBound(0, ch, m_channels - 1);
    return ch;
}

void StackedWaveWidget::resetView()
{
    m_mode = ViewMode::Stacked;
    m_focusCh = -1;
    m_panSamples = 0;
    m_dragging = false;
}

void StackedWaveWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(rect(), QColor(18,18,18));

    QMutexLocker lk(&m_mtx);
    if (!m_hasData || m_rings.isEmpty()) return;

    const int W = width();
    const int H = height();
    const int padT = 22, padB = 14, padL = 52, padR = 10;

    const int plotW = W - padL - padR;
    const int plotH = H - padT - padB;
    if (plotW < 20 || plotH < 20) return;

    // 顶部标题 & 状态
    p.setPen(QColor(235,235,235));
    QString status;
    if (m_mode == ViewMode::Stacked) {
        status = QString("%1 | Stacked | sel=%2 | win=%3s | gain=±%4uV (wheel) | Ctrl+wheel time zoom | dblclick focus")
                     .arg(m_title)
                     .arg(m_selectedCh)
                     .arg(m_windowSec,0,'f',2)
                     .arg(m_gainUv,0,'f',0);
    } else {
        status = QString("%1 | Single ch=%2 | win=%3s | gain=±%4uV | Shift+drag pan | dblclick back")
                     .arg(m_title)
                     .arg(m_focusCh)
                     .arg(m_windowSec,0,'f',2)
                     .arg(m_gainUv,0,'f',0);
    }
    p.drawText(6, 16, status);

    // 通用：可用样本数
    const int nAvail = m_rings[0].size();
    const int nWin = qMin(nAvail, int(m_windowSec * m_fs));
    if (nWin < 2) return;

    auto calcStartEnd = [&](int &start, int &end){
        end = nAvail - 1;
        if (m_mode == ViewMode::Single) {
            end = nAvail - 1 - m_panSamples;
        }
        end = qBound(nWin - 1, end, nAvail - 1);
        start = end - (nWin - 1);
        if (start < 0) start = 0;
    };

    int start = 0, end = 0;
    calcStartEnd(start, end);

    // 坐标轴/网格颜色
    const QColor grid(45,45,45);
    const QColor axis(70,70,70);
    const QColor wave(0,210,170);
    const QColor sel(255,200,0);

    // ===== Stacked 模式 =====
    if (m_mode == ViewMode::Stacked) {
        const double chH = double(plotH) / double(m_channels);

        // 网格
        p.setPen(grid);
        for (int ch=0; ch<m_channels; ++ch) {
            int y = padT + int((ch + 0.5) * chH);
            p.drawLine(padL, y, padL + plotW, y);
        }
        p.setPen(axis);
        p.drawLine(padL, padT, padL, padT + plotH);

        // 波形（按像素列 min/max）
        for (int ch=0; ch<m_channels; ++ch) {
            const auto &ring = m_rings[ch];
            const int baseY = padT + int((ch + 0.5) * chH);
            const double yScale = (chH * 0.45) / m_gainUv;

            p.setPen(ch == m_selectedCh ? sel : wave);

            for (int x=0; x<plotW; ++x) {
                int i0 = start + int(double(x)     / double(plotW) * (end - start + 1));
                int i1 = start + int(double(x + 1) / double(plotW) * (end - start + 1));
                if (i1 <= i0) i1 = i0 + 1;
                if (i1 > end + 1) i1 = end + 1;

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
        return;
    }

    // ===== Single 模式 =====
    const int ch = (m_focusCh >= 0 ? m_focusCh : m_selectedCh);
    const auto &ring = m_rings[qBound(0, ch, m_channels - 1)];

    // 轴
    p.setPen(axis);
    p.drawRect(padL, padT, plotW, plotH);

    // 中线
    const int midY = padT + plotH/2;
    p.setPen(grid);
    p.drawLine(padL, midY, padL + plotW, midY);

    // 波形（按像素列 min/max）
    p.setPen(sel);
    const double yScale = (plotH * 0.45) / m_gainUv;

    for (int x=0; x<plotW; ++x) {
        int i0 = start + int(double(x)     / double(plotW) * (end - start + 1));
        int i1 = start + int(double(x + 1) / double(plotW) * (end - start + 1));
        if (i1 <= i0) i1 = i0 + 1;
        if (i1 > end + 1) i1 = end + 1;

        float vmin = ring.atFromOldest(i0);
        float vmax = vmin;
        for (int i=i0+1; i<i1; ++i) {
            float v = ring.atFromOldest(i);
            if (v < vmin) vmin = v;
            if (v > vmax) vmax = v;
        }

        int y1 = midY - int(vmin * yScale);
        int y2 = midY - int(vmax * yScale);
        p.drawLine(padL + x, y1, padL + x, y2);
    }

    // 左侧标尺文字
    p.setPen(QColor(220,220,220));
    p.drawText(6, padT + 14, QString("ch %1").arg(ch));
    p.drawText(6, midY - 2, "0");
    p.drawText(6, padT + 14 + 14, QString("+%1").arg(m_gainUv,0,'f',0));
    p.drawText(6, padT + plotH - 4, QString("-%1").arg(m_gainUv,0,'f',0));
}

void StackedWaveWidget::mousePressEvent(QMouseEvent *e)
{
    const int padT = 22, padB = 14, padL = 52, padR = 10;
    const int plotW = width() - padL - padR;
    const int plotH = height() - padT - padB;
    if (plotW < 20 || plotH < 20) return;

    if (e->button() == Qt::LeftButton) {
        // 选中通道
        int ch = pickChannelFromY(e->pos().y(), padT, plotH);
        {
            QMutexLocker lk(&m_mtx);
            m_selectedCh = ch;
        }

        // 单通道模式下，Shift+拖拽 = 平移
        if (m_mode == ViewMode::Single && (e->modifiers() & Qt::ShiftModifier)) {
            m_dragging = true;
            m_dragStartX = e->pos().x();
            m_panSamplesAtDragStart = m_panSamples;
        }

        update();
    }
}

void StackedWaveWidget::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) return;

    QMutexLocker lk(&m_mtx);
    if (m_mode == ViewMode::Stacked) {
        m_mode = ViewMode::Single;
        m_focusCh = m_selectedCh;
        m_panSamples = 0;
    } else {
        resetView();
    }
    update();
}

void StackedWaveWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_dragging) return;

    QMutexLocker lk(&m_mtx);
    if (m_mode != ViewMode::Single) return;

    // 像素 -> 样本：拖拽多少像素换算为多少样本
    const int padL = 52, padR = 10;
    const int plotW = width() - padL - padR;
    if (plotW < 10) return;

    const int dx = e->pos().x() - m_dragStartX;
    // 右拖：看更“新”的数据 => pan 减小；左拖：看更“旧”的数据 => pan 增大
    const int nAvail = m_rings[0].size();
    const int nWin = qMin(nAvail, int(m_windowSec * m_fs));

    // 1 像素对应的样本数（近似）
    const double samplesPerPixel = double(nWin) / double(plotW);
    int deltaSamples = int(-dx * samplesPerPixel);

    int newPan = m_panSamplesAtDragStart + deltaSamples;

    // pan 的上限：不能超出历史数据范围
    const int maxPan = qMax(0, nAvail - nWin);
    newPan = qBound(0, newPan, maxPan);
    m_panSamples = newPan;

    update();
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
        // Ctrl+滚轮：时间窗 zoom
        applyZoom(m_windowSec, 0.8, m_windowSecMin, m_windowSecMax);

        // 调整 pan，避免 zoom 后越界
        const int nAvail = m_rings[0].size();
        const int nWin = qMin(nAvail, int(m_windowSec * m_fs));
        const int maxPan = qMax(0, nAvail - nWin);
        m_panSamples = qBound(0, m_panSamples, maxPan);
    } else {
        // 滚轮：幅度 zoom
        applyZoom(m_gainUv, 0.8, m_gainMin, m_gainMax);
    }

    update();
}

void StackedWaveWidget::contextMenuEvent(QContextMenuEvent *e)
{
    Q_UNUSED(e);
    QMutexLocker lk(&m_mtx);
    resetView();
    update();
}

void StackedWaveWidget::setFilterSettings(const FilterSettings &s)
{
    QMutexLocker lk(&m_mtx);
    m_filt = s;

    // 重新设计滤波器并 reset 状态
    for (int ch=0; ch<m_channels; ++ch) {
        m_bqNotch[ch].reset();
        m_bqMain[ch].reset();

        if (m_filt.notchEnabled) {
            m_bqNotch[ch] = Biquad::notch(m_fs, m_filt.notch_hz, m_filt.notchQ);
        }

        if (!m_filt.enabled || m_filt.type == FilterSettings::Type::Off) {
            m_bqMain[ch] = Biquad(); // identity
        } else if (m_filt.type == FilterSettings::Type::LowPass) {
            m_bqMain[ch] = Biquad::lowpass(m_fs, m_filt.lp_hz);
        } else if (m_filt.type == FilterSettings::Type::HighPass) {
            m_bqMain[ch] = Biquad::highpass(m_fs, m_filt.hp_hz);
        } else if (m_filt.type == FilterSettings::Type::BandPass) {
            // 用中心频率 + Q 来近似一个中通
            double f1 = qMax(1.0, m_filt.bp_low_hz);
            double f2 = qMax(f1+1.0, m_filt.bp_high_hz);
            double fc = std::sqrt(f1*f2);
            double Q  = fc / (f2 - f1 + 1e-9);
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

    // 取“当前视窗末端”(考虑单通道 pan)
    int end = nAvail - 1;
    if (m_mode == ViewMode::Single) end = nAvail - 1 - m_panSamples;
    end = qBound(N-1, end, nAvail-1);
    int start = end - (N - 1);

    out.resize(N);
    for (int i=0;i<N;i++){
        out[i] = m_rings[ch].atFromOldest(start + i);
    }
    return true;
}


