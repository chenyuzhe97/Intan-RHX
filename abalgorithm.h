#pragma once

#include <QObject>
#include <QVector>
#include <QtMath>


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
        bool   needStim = false;          // 是否建议刺激
        double globalRms = 0.0;           // 全通道平均 RMS
        QVector<double> channelRms;       // 每通道 RMS

        // ⭐ 新增：建议的刺激参数（单位都用 uA / 脉冲个数）
        int    suggestedAmplitude_uA = 0; // 建议刺激电流幅度
        int    suggestedNumPulses    = 1; // 建议脉冲个数
    };

    Result analyzeEpoch(int phaseIndex,
                        const QVector<uint32_t> &timeStamps,
                        const QVector<QVector<int>> &channelData);

    void setThreshold(double thr) { m_globalRmsThreshold = thr; }
    // ===== 新增三个滤波接口（对单通道数据）=====
    QVector<double> lowPassFilter(
        const QVector<double> &x,
        double cutoffHz) const;

    QVector<double> highPassFilter(
        const QVector<double> &x,
        double cutoffHz) const;

    QVector<double> bandPassFilter(
        const QVector<double> &x,
        double lowCutHz,
        double highCutHz) const;

    // 如果你需要设置采样率，可以加：
    void setSampleRate(double fs) { m_sampleRateHz = fs; }

private:
    double m_sampleRateHz        = 30000.0;  // 默认 1 kHz，自行改
    double m_globalRmsThreshold = 50.0;   // µV
    int    m_minAmp_uA = 20;              // 最小刺激幅度
    int    m_maxAmp_uA = 200;             // 最大刺激幅度
};
