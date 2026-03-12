#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <cstdint>

class AcquisitionEngine;
class ABAlgorithm;
class StimTimelineOverlay;
class StimLogWriter;

class ABExperimentCoordinator : public QObject
{
    Q_OBJECT
public:
    struct RoutingConfig {
        QVector<int> senseA_a;
        QVector<int> senseA_b;
        QVector<int> senseB_a;
        QVector<int> senseB_b;

        QString stimA_a;
        QString stimA_b;
        QString stimB_a;
        QString stimB_b;
    };

    explicit ABExperimentCoordinator(AcquisitionEngine *engine,
                                     ABAlgorithm *algorithm,
                                     QObject *parent = nullptr);

    void setTimelineOverlay(StimTimelineOverlay *timeline);
    void setStimLogWriter(StimLogWriter *stimLog);
    void setSampleRateHz(double sampleRateHz);
    void setEpochDurationSec(double epochDurationSec);
    void setRoutingConfig(const RoutingConfig &config);

    void handleEpochReady(int phaseIndex,
                          const QVector<uint32_t> &timeStamps,
                          const QVector<QVector<int>> &channelData);

signals:
    void logMessage(const QString &msg);

private:
    QVector<int> meanSelectedChannels(const QVector<QVector<int>> &channelData,
                                      const QVector<int> &sel) const;
    QString phaseNameForIndex(int phaseIndex) const;
    QString electrodeForAvgIndex(int phaseIndex, int avgIndex) const;

private:
    AcquisitionEngine   *m_engine = nullptr;
    ABAlgorithm         *m_algorithm = nullptr;
    StimTimelineOverlay *m_timeline = nullptr;
    StimLogWriter       *m_stimLog = nullptr;

    RoutingConfig m_config;

    int    m_epochCounter = 0;
    double m_sampleRateHz = 30000.0;
    double m_epochDurationSec = 5.0;
};