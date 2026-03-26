#include "abalgorithm.h"
#include <QtMath>

void ABAlgorithm::setBandPassHz(double lowCutHz, double highCutHz)
{
    const double safeLow = qMax(0.1, lowCutHz);
    const double safeHigh = qMax(safeLow + 0.1, highCutHz);
    m_bpLowCutHz = safeLow;
    m_bpHighCutHz = safeHigh;
}

ABAlgorithm::ABAlgorithm(QObject *parent)
    : QObject(parent)
{
}

// ========= 一阶低通 =========
QVector<double> ABAlgorithm::lowPassFilter(
    const QVector<double> &x,
    double cutoffHz) const
{
    QVector<double> y;
    y.resize(x.size());
    if (x.isEmpty() || cutoffHz <= 0.0 || m_sampleRateHz <= 0.0)
        return y;

    double dt = 1.0 / m_sampleRateHz;
    double RC = 1.0 / (2.0 * M_PI * cutoffHz);
    double alpha = dt / (RC + dt);   // 0~1 之间

    y[0] = x[0];
    for (int n = 1; n < x.size(); ++n) {
        y[n] = y[n-1] + alpha * (x[n] - y[n-1]);
    }
    return y;
}

// ========= 一阶高通 =========
QVector<double> ABAlgorithm::highPassFilter(
    const QVector<double> &x,
    double cutoffHz) const
{
    QVector<double> y;
    y.resize(x.size());
    if (x.isEmpty() || cutoffHz <= 0.0 || m_sampleRateHz <= 0.0)
        return y;

    double dt = 1.0 / m_sampleRateHz;
    double RC = 1.0 / (2.0 * M_PI * cutoffHz);
    double alpha = RC / (RC + dt);

    y[0] = 0.0;  // 高通起始可以设为 0
    for (int n = 1; n < x.size(); ++n) {
        y[n] = alpha * (y[n-1] + x[n] - x[n-1]);
    }
    return y;
}

// ========= 带通：先 HP 再 LP =========
QVector<double> ABAlgorithm::bandPassFilter(
    const QVector<double> &x,
    double lowCutHz,
    double highCutHz) const
{
    QVector<double> y;
    if (x.isEmpty() || lowCutHz <= 0.0 || highCutHz <= lowCutHz)
        return y;

    QVector<double> tmp = highPassFilter(x, lowCutHz);
    y = lowPassFilter(tmp, highCutHz);
    return y;
}

// ========= 单通道尖峰检测 =========
// 策略：
//  1) 用 m_spikeThreshold_uV 做阈值
//  2) 检测从 <thr 到 >=thr 的“上升沿”
//  3) 在一个小窗口内找局部峰值（真正尖峰顶）
//  4) 加不应期，避免重复
void ABAlgorithm::detectSpikesSingleChannel(
    const QVector<double> &filtered,
    const QVector<uint32_t> &timeStamps,
    int chIndex,
    QVector<Result> &outResults) const
{
    int N = filtered.size();
    if (N == 0 || timeStamps.size() != N)
        return;

    // 不应期对应的样本数
    int refractorySamples =
        int(m_refractoryMs * m_sampleRateHz / 1000.0);
    if (refractorySamples < 1) refractorySamples = 1;

    // 在上升沿后，往后看多少样本找峰
    // 这里用 0.8 ms 的窗口举例
    int peakWindowSamples =
        int(0.8 * m_sampleRateHz / 1000.0);
    if (peakWindowSamples < 1) peakWindowSamples = 1;

    double thr = m_spikeThreshold_uV;
    int lastSpikeIdx = -refractorySamples;

    for (int i = 1; i < N; ++i) {
        // 不应期限制
        if (i - lastSpikeIdx < refractorySamples)
            continue;

        double prev = filtered[i - 1];
        double curr = filtered[i];

        // 上升沿穿越阈值
        if (prev < thr && curr >= thr) {
            // 在 [i, i + peakWindowSamples) 区间寻找真正峰值
            int   peakIdx = i;
            double peakVal = curr;

            int jEnd = qMin(N, i + peakWindowSamples);
            for (int j = i + 1; j < jEnd; ++j) {
                if (filtered[j] > peakVal) {
                    peakVal = filtered[j];
                    peakIdx = j;
                }
            }

            lastSpikeIdx = peakIdx;

            Result r;
            r.needStim          = true;
            r.channelIndex      = chIndex;
            r.triggerTime       = timeStamps[peakIdx];
            r.spikeAmplitude_uV = peakVal;

            // ===== 简单版：根据峰值粗略映射刺激电流（你之后可以自己调算法）=====
            // 举例：peakVal = thr 时给最小电流；peakVal 高一些时线性增加一点点
            double over = qMax(0.0, peakVal - thr);    // 超出阈值多少 µV
            double k    = 0.05;                        // 每 1 µV 增加 0.05 uA（纯示例）

            int amp = int(m_minAmp_uA + k * over);
            if (amp < m_minAmp_uA) amp = m_minAmp_uA;
            if (amp > m_maxAmp_uA) amp = m_maxAmp_uA;

            r.suggestedAmplitude_uA = amp;
            r.suggestedNumPulses    = m_defaultNumPulses;

            outResults.append(r);
        }
    }
}

// ========= 主函数：一整个 epoch 内检测所有通道的所有尖峰 =========
QVector<ABAlgorithm::Result> ABAlgorithm::analyzeEpoch(
    int phaseIndex,
    const QVector<uint32_t> &timeStamps,
    const QVector<QVector<int>> &channelData)
{
    Q_UNUSED(phaseIndex);

    QVector<Result> allResults;

    int numCh = channelData.size();
    int N     = timeStamps.size();
    if (numCh == 0 || N == 0) {
        return allResults;
    }

    // 遍历每个通道：int -> µV -> 带通 -> 尖峰检测
    for (int ch = 0; ch < numCh; ++ch) {
        const auto &raw = channelData[ch];
        if (raw.isEmpty())
            continue;

        int nSamples = qMin(raw.size(), N); // 保守起见，取两者最小

        // 1) int -> uV
        QVector<double> x;
        x.resize(nSamples);
        for (int i = 0; i < nSamples; ++i) {
            x[i] = (double(raw[i]) - 32768.0) * 0.195;  // 和你原来一致
        }

        // 2) 带通滤波，用于 spike
        QVector<double> xf = m_bpEnabled ? bandPassFilter(x, m_bpLowCutHz, m_bpHighCutHz) : x;
        if (xf.size() != nSamples) {
            // 理论上是一样大的，这里防御性处理一下
            xf.resize(nSamples);
        }

        // 3) 尖峰检测，往 allResults 里 append
        detectSpikesSingleChannel(xf, timeStamps, ch, allResults);
    }

    return allResults;
}
