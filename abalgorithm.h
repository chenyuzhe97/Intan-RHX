#pragma once

#include <QObject>
#include <QVector>
#include <QtMath>

/**
 * AB 闭环的“算法模块”
 * 只关心数据，不关心 GUI、不关心 Intan 细节
 *
 * 输入：phaseIndex, timeStamps, channelData
 * 输出：多个“尖峰事件” Result
 */
class ABAlgorithm : public QObject
{
    Q_OBJECT
public:
    explicit ABAlgorithm(QObject *parent = nullptr);

    struct Result {
        bool     needStim          = false;    // 是否建议刺激
        uint32_t triggerTime       = 0;        // 在这一 epoch 内的触发时间（直接用传进来的 timeStamps）
        int      channelIndex      = -1;       // 哪个通道出的尖峰
        double   spikeAmplitude_uV = 0.0;      // 该尖峰的峰值幅度（µV）

        int      suggestedAmplitude_uA = 0;    // 建议刺激电流幅度
        int      suggestedNumPulses    = 1;    // 建议脉冲个数
    };

    // ⭐ 现在返回多个 Result
    QVector<Result> analyzeEpoch(int phaseIndex,
                                 const QVector<uint32_t> &timeStamps,
                                 const QVector<QVector<int>> &channelData);

    // 如果你想把阈值和不应期开放给外部调：
    void setSpikeThreshold(double thr_uV) { m_spikeThreshold_uV = thr_uV; }
    void setRefractoryMs(double ms)       { m_refractoryMs = ms; }
    void setSampleRate(double fs)         { m_sampleRateHz = fs; }

    // ===== 三个滤波接口（对单通道数据）=====
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

private:
    // ====== 基本参数 ======
    double m_sampleRateHz   = 30000.0;   // 30 kHz 采样（Intan 风格）
    double m_spikeThreshold_uV = 100.0;  // 尖峰阈值，µV，先写死，之后根据数据调
    double m_refractoryMs      = 1.0;    // 不应期，ms，避免一个尖峰多次触发

    // 尖峰检测使用的带通范围，适合 spike（你可以根据实际改）
    double m_bpLowCutHz    = 300.0;
    double m_bpHighCutHz   = 3000.0;

    int    m_minAmp_uA     = 20;         // 刺激电流下限
    int    m_maxAmp_uA     = 200;        // 刺激电流上限
    int    m_defaultNumPulses = 1;

    // 单通道尖峰检测：在 filtered 上做阈值 + 不应期 + 找峰
    void detectSpikesSingleChannel(
        const QVector<double> &filtered,
        const QVector<uint32_t> &timeStamps,
        int chIndex,
        QVector<Result> &outResults) const;
};
