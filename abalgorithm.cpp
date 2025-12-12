#include "abalgorithm.h"
#include <QtMath>

ABAlgorithm::ABAlgorithm(QObject *parent)
    : QObject(parent)
{
}

ABAlgorithm::Result ABAlgorithm::analyzeEpoch(
    int phaseIndex,
    const QVector<uint32_t> &timeStamps,
    const QVector<QVector<int>> &channelData)
{
    Q_UNUSED(phaseIndex);

    Result res;
    int numCh = channelData.size();
    int N     = timeStamps.size();
    if (numCh == 0 || N == 0) {
        return res;
    }

    res.channelRms.resize(numCh);

    for (int ch = 0; ch < numCh; ++ch) {
        const auto &data = channelData[ch];
        if (data.isEmpty()) {
            res.channelRms[ch] = 0.0;
            continue;
        }

        // 1) int -> uV
        QVector<double> x;
        x.resize(data.size());
        for (int i = 0; i < data.size(); ++i) {
            x[i] = (double(data[i]) - 32768.0) * 0.195;  // 原来的换算
        }

        // 2) 选择滤波方式：举例用带通 1-40 Hz
        QVector<double> xf = bandPassFilter(x, 1.0, 40.0);
        // 你也可以换成：
        // auto xf = lowPassFilter(x, 40.0);
        // auto xf = highPassFilter(x, 1.0);

        // 3) 对滤波后的数据算 RMS
        double sumSq = 0.0;
        int Nch = xf.size();
        for (int i = 0; i < Nch; ++i) {
            double uV = xf[i];
            sumSq += uV * uV;
        }
        double rms = qSqrt(sumSq / double(Nch));
        res.channelRms[ch] = rms;
    }

    // 4) 全通道平均 RMS & 刺激判定逻辑保持不变
    double sum = 0.0;
    for (double v : res.channelRms) sum += v;
    res.globalRms = sum / double(numCh);

    if (res.globalRms > m_globalRmsThreshold) {
        res.needStim = true;

        int amp = int(res.globalRms);
        if (amp < m_minAmp_uA) amp = m_minAmp_uA;
        if (amp > m_maxAmp_uA) amp = m_maxAmp_uA;

        res.suggestedAmplitude_uA = amp;
        res.suggestedNumPulses    = 1;
    } else {
        res.needStim = false;
        res.suggestedAmplitude_uA = 0;
        res.suggestedNumPulses    = 0;
    }

    return res;
}




// ========== 一阶低通 ==========
// 连续 RC 低通：H(s) = 1 / (1 + sRC)
// 离散形式：y[n] = y[n-1] + alpha * (x[n] - y[n-1])
// 其中 alpha = dt / (RC + dt) = 1 - exp(-2πfc/fs) 近似
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

// ========== 一阶高通 ==========
// 连续 RC 高通：H(s) = sRC / (1 + sRC)
// 离散形式：y[n] = alpha * (y[n-1] + x[n] - x[n-1])
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
    double alpha = RC / (RC + dt);   // 也在 0~1 之间

    y[0] = 0.0;  // 高通起始可以设为 0
    for (int n = 1; n < x.size(); ++n) {
        y[n] = alpha * (y[n-1] + x[n] - x[n-1]);
    }
    return y;
}

// ========== 带通 ==========
// 先高通(去掉直流和慢趋势), 再低通(去掉高频噪声)
QVector<double> ABAlgorithm::bandPassFilter(
    const QVector<double> &x,
    double lowCutHz,
    double highCutHz) const
{
    QVector<double> y;
    if (x.isEmpty() || lowCutHz <= 0.0 || highCutHz <= lowCutHz)
        return y;

    // 先高通，去掉低于 lowCutHz 的成分
    QVector<double> tmp = highPassFilter(x, lowCutHz);
    // 再低通，去掉高于 highCutHz 的成分
    y = lowPassFilter(tmp, highCutHz);

    return y;
}
