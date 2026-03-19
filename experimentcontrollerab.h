#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

class AcquisitionEngine;

class ExperimentControllerAB : public QObject
{
    Q_OBJECT
public:
    explicit ExperimentControllerAB(AcquisitionEngine *engine,
                                    QObject *parent = nullptr);

    void start();
    void stop();
    void setEpochDuration(double seconds);
    void setTargetRounds(int rounds);

signals:
    void epochReady(int phaseIndex,
                    const QVector<uint32_t> &timeStamps,
                    const QVector<QVector<int>> &channelData);

    void logMessage(const QString &msg);
    void roundCompleted(int completedRounds, int targetRounds);
    void experimentCompleted(int completedRounds);

public slots:
    void onStimPhaseFinished();

private slots:
    void onNewSamplesStream0(const QVector<uint32_t> &timeStamps,
                             const QVector<QVector<int>> &channelData);
    void onNewSamplesStream2(const QVector<uint32_t> &timeStamps,
                             const QVector<QVector<int>> &channelData);
    void onEpochTimeout();

private:
    enum Phase {
        PhaseA = 0,
        PhaseB = 1
    };

    void clearPhaseBuffers(Phase phase);
    void startCollectionPhase(Phase phase);
    QString phaseName(Phase phase) const;

    AcquisitionEngine *m_engine = nullptr;

    QTimer m_epochTimer;
    bool   m_running = false;
    bool   m_waitingForStim = false;
    Phase  m_phase = PhaseA;
    Phase  m_nextPhase = PhaseB;
    Phase  m_lastCollectedPhase = PhaseA;

    double m_epochDurationSec = 5.0;
    int    m_targetRounds = 1;
    int    m_completedRounds = 0;

    QVector<uint32_t>     m_tsBufferA;
    QVector<QVector<int>> m_chBuffersA;
    int                   m_numChannelsA = 0;

    QVector<uint32_t>     m_tsBufferB;
    QVector<QVector<int>> m_chBuffersB;
    int                   m_numChannelsB = 0;
};
