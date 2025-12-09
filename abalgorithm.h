#pragma once

#include <QObject>
#include <QVector>

/**
 * AB 闭环的“算法模块”
 * 只关心数据，不关心 GUI、不关心 Intan 细节
 *
 * 输入：phaseIndex, timeStamps, channelData
 * 输出：要不要刺激？（needStim），以及你想要的中间结果（比如每通道 RMS）
 */
class ABAlgorithm : public QObject
{
    Q_OBJECT
public:
    explicit ABAlgorithm(QObject *parent = nullptr);

    struct Result {
        bool   needStim = false;           // 是否建议刺激
        double globalRms = 0.0;           // 全通道平均 RMS
        QVector<double> channelRms;       // 每个通道的 RMS
    };

    // 核心接口：给它一个 epoch，它给你一个决策结果
    Result analyzeEpoch(int phaseIndex,
                        const QVector<uint32_t> &timeStamps,
                        const QVector<QVector<int>> &channelData);

    // 简单一点：你可以设置一个全局 RMS 阈值
    void setThreshold(double thr) { m_globalRmsThreshold = thr; }

private:
    double m_globalRmsThreshold = 50.0;   // µV，粗暴一点先全局一个
};
