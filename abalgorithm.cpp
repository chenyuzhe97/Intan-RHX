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

    // 1) 每通道 RMS
    for (int ch = 0; ch < numCh; ++ch) {
        const auto &data = channelData[ch];
        if (data.isEmpty()) {
            res.channelRms[ch] = 0.0;
            continue;
        }

        double sumSq = 0.0;
        int Nch = data.size();
        for (int i = 0; i < Nch; ++i) {
            double uV = (double(data[i]) - 32768.0) * 0.195;
            sumSq += uV * uV;
        }
        double rms = qSqrt(sumSq / double(Nch));
        res.channelRms[ch] = rms;
    }

    // 2) 全通道平均 RMS
    double sum = 0.0;
    for (double v : res.channelRms) sum += v;
    res.globalRms = sum / double(numCh);

    // 3) VERY 简单的规则：global RMS > 阈值 ⇒ 刺激
    if (res.globalRms > m_globalRmsThreshold) {
        res.needStim = true;

        // 把 globalRms 直接当成 uA，再夹在 [min,max] 区间
        int amp = int(res.globalRms);
        if (amp < m_minAmp_uA) amp = m_minAmp_uA;
        if (amp > m_maxAmp_uA) amp = m_maxAmp_uA;

        res.suggestedAmplitude_uA = amp;
        res.suggestedNumPulses    = 1;    // 先固定 1 个，你之后可以按算法改
    } else {
        res.needStim = false;
        res.suggestedAmplitude_uA = 0;
        res.suggestedNumPulses    = 0;
    }

    return res;
}
