#pragma once

#include <QObject>
#include <QTimer>
#include <QVector>

class AcquisitionEngine;

/**
 * AB 闭环的“epoch 数据打包器”：
 * - PhaseA：采集 A 端（stream0）所有通道的 1 个 epoch（例如 5 s）
 * - PhaseB：采集 B 端（stream2）所有通道的 1 个 epoch（例如 5 s）
 * - 每个 epoch 结束时，发出信号，把那一段的 timeStamps + channelData 整包给你
 * - 不在内部做算法和刺激决策，这些都交给外部（比如 MainWindow）处理
 */
class ExperimentControllerAB : public QObject
{
    Q_OBJECT
public:
    explicit ExperimentControllerAB(AcquisitionEngine *engine,
                                    QObject *parent = nullptr);

    void start();
    void stop();

    // 设置每个 epoch 的长度（秒），比如 5s 或 30s
    void setEpochDuration(double seconds);

signals:
    // 每个 epoch 完成时发出：
    //  phaseIndex: 0 = PhaseA（A 端，stream0），1 = PhaseB（B 端，stream2）
    //  timeStamps.size() == N
    //  channelData.size() == numChannels
    //  channelData[ch].size() == N
    void epochReady(int phaseIndex,
                    const QVector<uint32_t> &timeStamps,
                    const QVector<QVector<int>> &channelData);

    void logMessage(const QString &msg);

private slots:
    // 来自 A 端（stream0 = AcquisitionEngine::newSamples）
    void onNewSamplesStream0(const QVector<uint32_t> &timeStamps,
                             const QVector<QVector<int>> &channelData);

    // 来自 B 端（stream2 = AcquisitionEngine::newSamplesStream2）
    void onNewSamplesStream2(const QVector<uint32_t> &timeStamps,
                             const QVector<QVector<int>> &channelData);

    // epoch 时间到了，切换 A/B 阶段，并把刚才这段数据丢出去
    void onEpochTimeout();

private:
    enum Phase {
        PhaseA = 0,   // 当前 epoch 针对 A 端：收集 stream0
        PhaseB = 1    // 当前 epoch 针对 B 端：收集 stream2
    };

    AcquisitionEngine *m_engine = nullptr;

    QTimer  m_epochTimer;
    bool    m_running = false;
    Phase   m_phase   = PhaseA;

    double  m_epochDurationSec = 5.0;

    // A 端（stream0）当前 epoch 的缓存
    QVector<uint32_t>        m_tsBufferA;
    QVector<QVector<int>>    m_chBuffersA;
    int                      m_numChannelsA = 0;

    // B 端（stream2）当前 epoch 的缓存
    QVector<uint32_t>        m_tsBufferB;
    QVector<QVector<int>>    m_chBuffersB;
    int                      m_numChannelsB = 0;
};
