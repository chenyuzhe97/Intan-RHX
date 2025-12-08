#include "experimentcontrollerab.h"
#include "acquisitionengine.h"

#include <QtMath>

ExperimentControllerAB::ExperimentControllerAB(AcquisitionEngine *engine,
                                               QObject *parent)
    : QObject(parent),
    m_engine(engine)
{
    connect(&m_epochTimer, &QTimer::timeout,
            this, &ExperimentControllerAB::onEpochTimeout);

    connect(m_engine, &AcquisitionEngine::newSamples,
            this,       &ExperimentControllerAB::onNewSamples);
}

void ExperimentControllerAB::start()
{
    if (m_running) return;

    m_running = true;
    m_phase   = PhaseA;
    m_sumSquares = 0.0;
    m_count      = 0;

    int intervalMs = int(m_epochDurationSec * 1000.0);
    m_epochTimer.start(intervalMs);

    emit logMessage(
        QStringLiteral("A↔B 闭环已启动：PhaseA→CH%1, PhaseB→CH%2, epoch=%3 s")
            .arg(m_channelA)
            .arg(m_channelB)
            .arg(m_epochDurationSec, 0, 'f', 2));
}

void ExperimentControllerAB::stop()
{
    if (!m_running) return;

    m_running = false;
    m_epochTimer.stop();
    m_sumSquares = 0.0;
    m_count      = 0;

    emit logMessage("A↔B 闭环已停止");
}

void ExperimentControllerAB::setChannels(int chA, int chB)
{
    m_channelA = chA;
    m_channelB = chB;
}

void ExperimentControllerAB::setEpochDuration(double seconds)
{
    m_epochDurationSec = seconds;
    if (m_running) {
        m_epochTimer.start(int(m_epochDurationSec * 1000.0));
    }
}

void ExperimentControllerAB::setRmsThresholds(double thrA, double thrB)
{
    m_rmsThrA = thrA;
    m_rmsThrB = thrB;
}

void ExperimentControllerAB::setTriggers(int trigWhenAStimB,
                                         int trigWhenBStimA)
{
    m_triggerWhenAStimB = trigWhenAStimB;
    m_triggerWhenBStimA = trigWhenBStimA;
}

void ExperimentControllerAB::onNewSamples(const QVector<uint32_t> &timeStamps,
                                          const QVector<QVector<int>> &channelData)
{
    if (!m_running) return;
    if (channelData.isEmpty()) return;
    if (timeStamps.isEmpty()) return;

    int numCh = channelData.size();

    int ch = (m_phase == PhaseA) ? m_channelA : m_channelB;
    if (ch < 0 || ch >= numCh) return;

    const QVector<int> &chData = channelData[ch];
    int N = qMin(chData.size(), timeStamps.size());
    if (N <= 0) return;

    const int decim = 2;  // 适当下采样一丢丢

    for (int i = 0; i < N; i += decim) {
        int raw = chData[i];

        double uV = (double(raw) - 32768.0) * 0.195;
        m_sumSquares += uV * uV;
        m_count++;
    }
}

void ExperimentControllerAB::onEpochTimeout()
{
    if (!m_running) return;

    if (m_count == 0) {
        emit logMessage("本 epoch 没有数据，跳过 RMS 计算");
        return;
    }

    double rms = qSqrt(m_sumSquares / double(m_count));

    bool stimulated = false;

    if (m_phase == PhaseA) {
        // PhaseA：看 A 通道，如果 RMS_A > 阈值A → 刺激 B
        if (rms > m_rmsThrA && m_engine) {
            m_engine->triggerStim(m_triggerWhenAStimB, true);
            stimulated = true;
            emit logMessage(
                QString("PhaseA: CH%1 RMS=%2 µV > %3, 刺激B(trigger=%4)")
                    .arg(m_channelA)
                    .arg(rms, 0, 'f', 2)
                    .arg(m_rmsThrA, 0, 'f', 2)
                    .arg(m_triggerWhenAStimB));
        } else {
            emit logMessage(
                QString("PhaseA: CH%1 RMS=%2 µV ≤ %3, 未刺激")
                    .arg(m_channelA)
                    .arg(rms, 0, 'f', 2)
                    .arg(m_rmsThrA, 0, 'f', 2));
        }
    } else {
        // PhaseB：看 B 通道，如果 RMS_B > 阈值B → 刺激 A
        if (rms > m_rmsThrB && m_engine) {
            m_engine->triggerStim(m_triggerWhenBStimA, true);
            stimulated = true;
            emit logMessage(
                QString("PhaseB: CH%1 RMS=%2 µV > %3, 刺激A(trigger=%4)")
                    .arg(m_channelB)
                    .arg(rms, 0, 'f', 2)
                    .arg(m_rmsThrB, 0, 'f', 2)
                    .arg(m_triggerWhenBStimA));
        } else {
            emit logMessage(
                QString("PhaseB: CH%1 RMS=%2 µV ≤ %3, 未刺激")
                    .arg(m_channelB)
                    .arg(rms, 0, 'f', 2)
                    .arg(m_rmsThrB, 0, 'f', 2));
        }
    }

    emit epochFinished((m_phase == PhaseA) ? 0 : 1, rms, stimulated);

    // 为下一个 epoch 清零累积
    m_sumSquares = 0.0;
    m_count      = 0;

    // 切换 Phase：A ↔ B 互换
    m_phase = (m_phase == PhaseA) ? PhaseB : PhaseA;
}
