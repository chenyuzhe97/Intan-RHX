#include "experimentcontroller.h"
#include "acquisitionengine.h"

#include <QtMath>

ExperimentController::ExperimentController(AcquisitionEngine *engine,
                                           QObject *parent)
    : QObject(parent),
    m_engine(engine)
{
    // epoch 定时器：每个 epoch 结束时触发 onEpochTimeout()
    connect(&m_epochTimer, &QTimer::timeout,
            this, &ExperimentController::onEpochTimeout);

    // 连接采集引擎的新数据
    // 注意：这里要求 AcquisitionEngine 有信号：
    //   void newSamples(const QVector<uint32_t>&,
    //                   const QVector<QVector<int>> &);
    connect(m_engine, &AcquisitionEngine::newSamples,
            this, &ExperimentController::onNewSamples);
}

void ExperimentController::start()
{
    if (m_running) return;

    m_running = true;
    m_sumSquares = 0.0;
    m_count      = 0;

    int intervalMs = int(m_epochDurationSec * 1000.0);
    m_epochTimer.start(intervalMs);

    emit logMessage(
        QStringLiteral("闭环实验已启动：通道 CH%1，epoch=%2 s，RMS阈值=%3 µV")
            .arg(m_channel)
            .arg(m_epochDurationSec, 0, 'f', 2)
            .arg(m_rmsThreshold,     0, 'f', 2));
}

void ExperimentController::stop()
{
    if (!m_running) return;

    m_running = false;
    m_epochTimer.stop();
    m_sumSquares = 0.0;
    m_count      = 0;

    emit logMessage("闭环实验已停止");
}

void ExperimentController::setChannel(int ch)
{
    m_channel = ch;
}

void ExperimentController::setEpochDuration(double seconds)
{
    m_epochDurationSec = seconds;
    if (m_running) {
        m_epochTimer.start(int(m_epochDurationSec * 1000.0));
    }
}

void ExperimentController::setRmsThreshold(double microVolts)
{
    m_rmsThreshold = microVolts;
}

void ExperimentController::onNewSamples(const QVector<uint32_t> &timeStamps,
                                        const QVector<QVector<int>> &channelData)
{
    if (!m_running) return;
    if (channelData.isEmpty()) return;
    if (timeStamps.isEmpty()) return;

    int numCh = channelData.size();
    if (m_channel < 0 || m_channel >= numCh) return;

    const QVector<int> &chData = channelData[m_channel];
    int N = qMin(chData.size(), timeStamps.size());
    if (N <= 0) return;

    // 可以选择下采样，如果觉得太重，比如每 2 个样本取 1 个
    const int decim = 2;

    for (int i = 0; i < N; i += decim) {
        int raw = chData[i];

        // 转为 µV：和你 Python 一样
        double uV = (double(raw) - 32768.0) * 0.195;

        m_sumSquares += uV * uV;
        m_count++;
    }
}

void ExperimentController::onEpochTimeout()
{
    if (!m_running) return;

    if (m_count == 0) {
        emit logMessage("本 epoch 没有数据，跳过 RMS 计算");
        return;
    }

    double rms = qSqrt(m_sumSquares / double(m_count));

    bool stimulated = false;
    if (rms > m_rmsThreshold) {
        // 超过阈值 → 发一次刺激
        if (m_engine) {
            m_engine->triggerStim(m_triggerSource, true);
            stimulated = true;
        }
    }

    emit epochRmsComputed(rms, stimulated);

    // 清空累计，为下一个 epoch 做准备
    m_sumSquares = 0.0;
    m_count      = 0;
}
