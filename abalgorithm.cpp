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

    // 1. 计算每个通道 RMS
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

    // 2. 全通道平均 RMS（非常简单的一个 global 指标）
    double sum = 0.0;
    for (double v : res.channelRms) sum += v;
    res.globalRms = sum / double(numCh);

    // 3. VERY 简单的决策：global RMS > 阈值 就建议刺激
    res.needStim = (res.globalRms > m_globalRmsThreshold);

    return res;
}
