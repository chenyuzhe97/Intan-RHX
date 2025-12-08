#pragma once

#include <QObject>
#include <QTimer>

class AcquisitionEngine;

/**
 * 简单闭环控制器：
 * - 监听 AcquisitionEngine::newSamples
 * - 针对指定通道计算 RMS
 * - 每个 epoch（默认 5 秒）结束时，根据 RMS 决定是否发一次刺激 trigger
 */
class ExperimentController : public QObject
{
    Q_OBJECT
public:
    explicit ExperimentController(AcquisitionEngine *engine,
                                  QObject *parent = nullptr);

    void start();   // 开始闭环
    void stop();    // 停止闭环

    void setChannel(int ch);              // 选哪一个通道做闭环
    void setEpochDuration(double seconds); // epoch 长度（秒）
    void setRmsThreshold(double microVolts); // 触发刺激的 RMS 阈值（µV）

signals:
    void logMessage(const QString &msg);
    void epochRmsComputed(double rms, bool stimulated);

private slots:
    void onNewSamples(const QVector<uint32_t> &timeStamps,
                      const QVector<QVector<int>> &channelData);
    void onEpochTimeout();

private:
    AcquisitionEngine *m_engine = nullptr;

    QTimer  m_epochTimer;
    bool    m_running = false;

    int     m_channel = 0;          // 用哪一路通道做闭环
    double  m_sampleRate = 30000.0; // 和 RHXController 里设置的保持一致
    double  m_epochDurationSec = 5.0;
    double  m_rmsThreshold   = 50.0; // 阈值：例如 50 µV

    // 用“累加”的方式计算 RMS，不保存整段波形，省内存&CPU
    double  m_sumSquares = 0.0;
    qint64  m_count      = 0;

    int     m_triggerSource = 0;    // 用哪一路 trigger（和你刷新刺激时保持一致）
};
