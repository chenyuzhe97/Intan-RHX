#pragma once

#include <QObject>
#include <QTimer>

class AcquisitionEngine;

/**
 * A↔B 双端口交替闭环控制器：
 * - Phase A：采集通道 channelA，计算 RMS_A，若 > 阈值A → 刺激 B（triggerWhenAStimB）
 * - Phase B：采集通道 channelB，计算 RMS_B，若 > 阈值B → 刺激 A（triggerWhenBStimA）
 * - Phase A/B 轮流切换
 */
class ExperimentControllerAB : public QObject
{
    Q_OBJECT
public:
    explicit ExperimentControllerAB(AcquisitionEngine *engine,
                                    QObject *parent = nullptr);

    void start();   // 启动 A↔B 闭环
    void stop();    // 停止闭环

    // 设置 A/B 两个数据通道索引（在 channelData 里的索引）
    void setChannels(int chA, int chB);

    // 设置 epoch 时长（秒），每个 epoch 完成后切换一次 A/B
    void setEpochDuration(double seconds);

    // A/B 各自 RMS 阈值（单位 µV）
    void setRmsThresholds(double thrA, double thrB);

    // 在 PhaseA 结束时，若超阈值 → 用哪个 triggerSource 刺激 B
    // 在 PhaseB 结束时，若超阈值 → 用哪个 triggerSource 刺激 A
    void setTriggers(int trigWhenAStimB, int trigWhenBStimA);

signals:
    void logMessage(const QString &msg);
    void epochFinished(int phaseIndex, double rms, bool stimulated);
    // phaseIndex: 0 = PhaseA, 1 = PhaseB

private slots:
    void onNewSamples(const QVector<uint32_t> &timeStamps,
                      const QVector<QVector<int>> &channelData);
    void onEpochTimeout();

private:
    enum Phase {
        PhaseA = 0,
        PhaseB = 1
    };

    AcquisitionEngine *m_engine = nullptr;

    QTimer  m_epochTimer;
    bool    m_running = false;

    Phase   m_phase = PhaseA;    // 当前处于 A 还是 B 阶段

    int     m_channelA = 0;
    int     m_channelB = 1;      // 默认 A=CH0, B=CH1，你可以在外部 set

    double  m_sampleRate = 30000.0;
    double  m_epochDurationSec = 5.0;

    double  m_rmsThrA = 50.0;    // 阈值 A
    double  m_rmsThrB = 50.0;    // 阈值 B

    int     m_triggerWhenAStimB = 1; // 比如 A 阶段超阈值 → 用 trigger1 刺激 B
    int     m_triggerWhenBStimA = 0; // 比如 B 阶段超阈值 → 用 trigger0 刺激 A

    // 当前 phase 的 RMS 累计（共用一套累加器）
    double  m_sumSquares = 0.0;
    qint64  m_count      = 0;
};
