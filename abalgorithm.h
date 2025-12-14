#pragma once

#include <QObject>
#include <QVector>
#include <QtMath>

class ABAlgorithm : public QObject
{
    Q_OBJECT
public:
    explicit ABAlgorithm(QObject *parent = nullptr);

    // ⭐ 尖峰事件结构体（一个事件 = 一个 Result）
    struct Result {
        bool     needStim          = false;
        uint32_t triggerTime       = 0;      // 事件发生的时间（ms）
        int      channelIndex      = -1;     // 触发事件的通道
        double   spikeAmplitude_uV = 0.0;    // 峰值（uV）

        int      suggestedAmplitude_uA = 0;  // 刺激电流
        int      suggestedNumPulses    = 1;  // 刺激脉冲数
    };

    // ⭐ 返回多个事件
    QVector<Result> analyzeEpoch(
        int phaseIndex,
        const QVector<uint32_t> &timeStamps,
        const QVector<QVector<int>> &channelData);

    // 参数设置
    void setSpikeThreshold(double thr) { m_spikeThreshold_uV = thr; }
    void setRefractoryMs(double ms)    { m_refractoryMs = ms; }
    void setSampleRate(double fs)      { m_sampleRateHz = fs; }

    // 三个滤波接口
    QVector<double> lowPassFilter(const QVector<double> &x, double cutoffHz) const;
    QVector<double> highPassFilter(const QVector<double> &x, double cutoffHz) const;
    QVector<double> bandPassFilter(const QVector<double> &x, double lowCutHz, double highCutHz) const;

private:
    double m_sampleRateHz       = 30000.0; // 30 kHz
    double m_spikeThreshold_uV  = 100.0;   // µV
    double m_refractoryMs       = 1.0;     // 不应期

    double m_bpLowCutHz         = 300.0;
    double m_bpHighCutHz        = 3000.0;

    int    m_minAmp_uA          = 20;
    int    m_maxAmp_uA          = 200;
    int    m_defaultNumPulses   = 1;

    // 单通道尖峰检测函数
    void detectSpikesSingleChannel(
        const QVector<double> &filtered,
        const QVector<uint32_t> &timeStamps,
        int chIndex,
        QVector<Result> &outResults) const;
};
