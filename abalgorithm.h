// abalgorithm.h
#pragma once

#include <QObject>
#include <QVector>

class ABAlgorithm : public QObject
{
    Q_OBJECT
public:
    explicit ABAlgorithm(QObject *parent = nullptr);

    struct Result {
        bool     needStim          = false;
        int      channelIndex      = -1;
        uint32_t triggerTime       = 0;
        double   spikeAmplitude_uV = 0.0;
        int      suggestedAmplitude_uA = 0;
        int      suggestedNumPulses    = 0;
    };

    // 你原来就有的主函数
    QVector<Result> analyzeEpoch(
        int phaseIndex,
        const QVector<uint32_t> &timeStamps,
        const QVector<QVector<int>> &channelData);

    // ⭐ 新增：外部（MainWindow）设置采样率
    inline void setSampleRateHz(double fs) { m_sampleRateHz = fs; }
    inline void setBandPassEnabled(bool enabled) { m_bpEnabled = enabled; }
    inline bool bandPassEnabled() const { return m_bpEnabled; }
    void setBandPassHz(double lowCutHz, double highCutHz);
    inline double bandPassLowCutHz() const { return m_bpLowCutHz; }
    inline double bandPassHighCutHz() const { return m_bpHighCutHz; }

    // ⭐ 如果你想在外面直接用带通滤波，也可以把这个声明成 public
    QVector<double> bandPassFilter(
        const QVector<double> &x,
        double lowCutHz,
        double highCutHz) const;

private:
    // 一阶低通 / 高通（已经在 .cpp 里实现）
    QVector<double> lowPassFilter(
        const QVector<double> &x,
        double cutoffHz) const;

    QVector<double> highPassFilter(
        const QVector<double> &x,
        double cutoffHz) const;

    void detectSpikesSingleChannel(
        const QVector<double> &filtered,
        const QVector<uint32_t> &timeStamps,
        int chIndex,
        QVector<Result> &outResults) const;

private:
    double m_sampleRateHz      = 30000.0;  // 采样率
    bool   m_bpEnabled         = true;
    double m_bpLowCutHz        = 300.0;
    double m_bpHighCutHz       = 3000.0;
    double m_spikeThreshold_uV = 50.0;
    double m_refractoryMs      = 1.0;

    int    m_minAmp_uA         = 10;
    int    m_maxAmp_uA         = 50;
    int    m_defaultNumPulses  = 1;
};
