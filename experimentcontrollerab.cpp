#include "experimentcontrollerab.h"

#include "acquisitionengine.h"

ExperimentControllerAB::ExperimentControllerAB(AcquisitionEngine *engine,
                                               QObject *parent)
    : QObject(parent),
      m_engine(engine)
{
    connect(&m_epochTimer, &QTimer::timeout,
            this, &ExperimentControllerAB::onEpochTimeout);

    connect(m_engine, &AcquisitionEngine::newSamples,
            this, &ExperimentControllerAB::onNewSamplesStream0);
    connect(m_engine, &AcquisitionEngine::newSamplesStream2,
            this, &ExperimentControllerAB::onNewSamplesStream2);
}

void ExperimentControllerAB::start()
{
    if (m_running) return;

    m_running = true;
    m_waitingForStim = false;
    m_completedRounds = 0;
    m_nextPhase = PhaseB;
    m_lastCollectedPhase = PhaseA;

    clearPhaseBuffers(PhaseA);
    clearPhaseBuffers(PhaseB);
    startCollectionPhase(PhaseA);

    emit logMessage(QStringLiteral("Closed-loop experiment started: epoch=%1 s, rounds=%2")
                        .arg(m_epochDurationSec, 0, 'f', 2)
                        .arg(m_targetRounds));
}

void ExperimentControllerAB::stop()
{
    if (!m_running) return;

    m_running = false;
    m_waitingForStim = false;
    m_epochTimer.stop();

    clearPhaseBuffers(PhaseA);
    clearPhaseBuffers(PhaseB);

    emit logMessage(QStringLiteral("Closed-loop experiment stopped."));
}

void ExperimentControllerAB::setEpochDuration(double seconds)
{
    if (seconds > 0.0) {
        m_epochDurationSec = seconds;
    }

    if (m_running && !m_waitingForStim) {
        m_epochTimer.start(qMax(1, int(m_epochDurationSec * 1000.0)));
    }
}

void ExperimentControllerAB::setTargetRounds(int rounds)
{
    m_targetRounds = qMax(1, rounds);
}

void ExperimentControllerAB::onStimPhaseFinished()
{
    if (!m_running || !m_waitingForStim) return;

    m_waitingForStim = false;

    emit logMessage(QStringLiteral("%1 stimulation phase finished.")
                        .arg(phaseName(m_nextPhase)));

    if (m_lastCollectedPhase == PhaseB) {
        ++m_completedRounds;
        emit roundCompleted(m_completedRounds, m_targetRounds);

        if (m_completedRounds >= m_targetRounds) {
            m_running = false;
            clearPhaseBuffers(PhaseA);
            clearPhaseBuffers(PhaseB);
            emit logMessage(QStringLiteral("Closed-loop target rounds reached: %1")
                                .arg(m_completedRounds));
            emit experimentCompleted(m_completedRounds);
            return;
        }
    }

    startCollectionPhase(m_nextPhase);
}

void ExperimentControllerAB::onNewSamplesStream0(
    const QVector<uint32_t> &timeStamps,
    const QVector<QVector<int>> &channelData)
{
    if (!m_running || m_waitingForStim || m_phase != PhaseA) return;
    if (timeStamps.isEmpty() || channelData.isEmpty()) return;

    const int numCh = channelData.size();
    const int N = timeStamps.size();
    if (N <= 0) return;

    if (m_numChannelsA == 0) {
        m_numChannelsA = numCh;
        m_chBuffersA.resize(m_numChannelsA);
    } else if (numCh != m_numChannelsA) {
        emit logMessage(QStringLiteral("PhaseA channel count changed unexpectedly; block ignored."));
        return;
    }

    m_tsBufferA.reserve(m_tsBufferA.size() + N);
    for (uint32_t ts : timeStamps) {
        m_tsBufferA.append(ts);
    }

    for (int ch = 0; ch < m_numChannelsA; ++ch) {
        const QVector<int> &src = channelData[ch];
        const int Nc = qMin(N, src.size());
        if (Nc <= 0) continue;

        QVector<int> &dst = m_chBuffersA[ch];
        dst.reserve(dst.size() + Nc);
        for (int i = 0; i < Nc; ++i) {
            dst.append(src[i]);
        }
    }
}

void ExperimentControllerAB::onNewSamplesStream2(
    const QVector<uint32_t> &timeStamps,
    const QVector<QVector<int>> &channelData)
{
    if (!m_running || m_waitingForStim || m_phase != PhaseB) return;
    if (timeStamps.isEmpty() || channelData.isEmpty()) return;

    const int numCh = channelData.size();
    const int N = timeStamps.size();
    if (N <= 0) return;

    if (m_numChannelsB == 0) {
        m_numChannelsB = numCh;
        m_chBuffersB.resize(m_numChannelsB);
    } else if (numCh != m_numChannelsB) {
        emit logMessage(QStringLiteral("PhaseB channel count changed unexpectedly; block ignored."));
        return;
    }

    m_tsBufferB.reserve(m_tsBufferB.size() + N);
    for (uint32_t ts : timeStamps) {
        m_tsBufferB.append(ts);
    }

    for (int ch = 0; ch < m_numChannelsB; ++ch) {
        const QVector<int> &src = channelData[ch];
        const int Nc = qMin(N, src.size());
        if (Nc <= 0) continue;

        QVector<int> &dst = m_chBuffersB[ch];
        dst.reserve(dst.size() + Nc);
        for (int i = 0; i < Nc; ++i) {
            dst.append(src[i]);
        }
    }
}

void ExperimentControllerAB::onEpochTimeout()
{
    if (!m_running || m_waitingForStim) return;

    m_epochTimer.stop();
    m_waitingForStim = true;
    m_lastCollectedPhase = m_phase;
    m_nextPhase = (m_phase == PhaseA) ? PhaseB : PhaseA;

    if (m_phase == PhaseA) {
        emit logMessage(QStringLiteral("PhaseA collection finished: samples=%1")
                            .arg(m_tsBufferA.size()));
        emit epochReady(0, m_tsBufferA, m_chBuffersA);
        clearPhaseBuffers(PhaseA);
    } else {
        emit logMessage(QStringLiteral("PhaseB collection finished: samples=%1")
                            .arg(m_tsBufferB.size()));
        emit epochReady(1, m_tsBufferB, m_chBuffersB);
        clearPhaseBuffers(PhaseB);
    }

    emit logMessage(QStringLiteral("Waiting for %1 stimulation phase to finish before next collection.")
                        .arg(phaseName(m_nextPhase)));
}

void ExperimentControllerAB::clearPhaseBuffers(Phase phase)
{
    if (phase == PhaseA) {
        m_tsBufferA.clear();
        m_chBuffersA.clear();
        m_numChannelsA = 0;
        return;
    }

    m_tsBufferB.clear();
    m_chBuffersB.clear();
    m_numChannelsB = 0;
}

void ExperimentControllerAB::startCollectionPhase(Phase phase)
{
    m_phase = phase;
    m_waitingForStim = false;
    clearPhaseBuffers(phase);
    m_epochTimer.start(qMax(1, int(m_epochDurationSec * 1000.0)));

    emit logMessage(QStringLiteral("%1 collection started for %2 s.")
                        .arg(phaseName(phase))
                        .arg(m_epochDurationSec, 0, 'f', 2));
}

QString ExperimentControllerAB::phaseName(Phase phase) const
{
    return (phase == PhaseA)
        ? QStringLiteral("PhaseA")
        : QStringLiteral("PhaseB");
}
