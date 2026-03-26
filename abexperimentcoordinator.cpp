#include "abexperimentcoordinator.h"

#include <QTimer>

#include <algorithm>
#include <cmath>

#include "abalgorithm.h"
#include "acquisitionengine.h"
#include "stimlogwriter.h"
#include "stimtimelineoverlay.h"

ABExperimentCoordinator::ABExperimentCoordinator(AcquisitionEngine *engine,
                                                 ABAlgorithm *algorithm,
                                                 QObject *parent)
    : QObject(parent),
      m_engine(engine),
      m_algorithm(algorithm)
{
}

void ABExperimentCoordinator::setTimelineOverlay(StimTimelineOverlay *timeline)
{
    m_timeline = timeline;
    if (m_timeline) {
        m_timeline->setEpochSec(m_epochDurationSec);
    }
}

void ABExperimentCoordinator::setStimLogWriter(StimLogWriter *stimLog)
{
    m_stimLog = stimLog;
}

void ABExperimentCoordinator::setSampleRateHz(double sampleRateHz)
{
    if (sampleRateHz > 0.0) {
        m_sampleRateHz = sampleRateHz;
    }
}

void ABExperimentCoordinator::setEpochDurationSec(double epochDurationSec)
{
    if (epochDurationSec > 0.0) {
        m_epochDurationSec = epochDurationSec;
    }

    if (m_timeline) {
        m_timeline->setEpochSec(m_epochDurationSec);
    }
}

void ABExperimentCoordinator::setMaxStimPerEpoch(int maxStimPerEpoch)
{
    m_maxStimPerEpoch = qMax(2, maxStimPerEpoch);
    if ((m_maxStimPerEpoch % 2) != 0) {
        ++m_maxStimPerEpoch;
    }
}

void ABExperimentCoordinator::setRoutingConfig(const RoutingConfig &config)
{
    m_config = config;
}

void ABExperimentCoordinator::beginRun()
{
    ++m_scheduleToken;
    m_runClock.restart();
    m_runClockActive = true;
}

void ABExperimentCoordinator::endRun()
{
    ++m_scheduleToken;
    m_runClock.invalidate();
    m_runClockActive = false;
}

void ABExperimentCoordinator::cancelPendingStimPhase()
{
    ++m_scheduleToken;
}

QVector<int> ABExperimentCoordinator::meanSelectedChannels(const QVector<QVector<int>> &channelData,
                                                           const QVector<int> &sel) const
{
    if (channelData.isEmpty() || sel.isEmpty()) return {};

    const int N = channelData[0].size();
    if (N <= 0) return {};

    QVector<int> out;
    out.resize(N);

    for (int i = 0; i < N; ++i) {
        long long sum = 0;
        int used = 0;
        for (int ch : sel) {
            if (ch < 0 || ch >= channelData.size()) continue;
            const auto &v = channelData[ch];
            if (i < 0 || i >= v.size()) continue;
            sum += static_cast<long long>(v[i]);
            ++used;
        }
        if (used == 0) return {};
        out[i] = static_cast<int>(std::llround(static_cast<double>(sum) / static_cast<double>(used)));
    }

    return out;
}

QString ABExperimentCoordinator::phaseNameForIndex(int phaseIndex) const
{
    return (phaseIndex == 0)
        ? QStringLiteral("PhaseA (Aç»”?stream0)")
        : QStringLiteral("PhaseB (Bç»”?stream2)");
}

QString ABExperimentCoordinator::electrodeForAvgIndex(int phaseIndex, int avgIndex) const
{
    if (phaseIndex == 0) {
        return (avgIndex == 0) ? m_config.stimB_a : m_config.stimB_b;
    }

    return (avgIndex == 0) ? m_config.stimA_a : m_config.stimA_b;
}

void ABExperimentCoordinator::handleEpochReady(int phaseIndex,
                                               const QVector<uint32_t> &timeStamps,
                                               const QVector<QVector<int>> &channelData)
{
    const quint64 scheduleToken = ++m_scheduleToken;
    auto finishPhase = [this, scheduleToken](int delayMs) {
        QTimer::singleShot(qMax(0, delayMs), this, [this, scheduleToken]() {
            if (scheduleToken != m_scheduleToken) return;
            emit stimPhaseFinished();
        });
    };

    const QString phaseName = phaseNameForIndex(phaseIndex);
    if (!m_algorithm || !m_engine) {
        emit logMessage(phaseName + QStringLiteral(": coordinator not ready, skip stimulation."));
        finishPhase(0);
        return;
    }

    if (timeStamps.isEmpty() || channelData.isEmpty()) {
        emit logMessage(phaseName + QStringLiteral(": empty epoch, skip stimulation."));
        finishPhase(0);
        return;
    }

    const QVector<int> &sel_a = (phaseIndex == 0) ? m_config.senseA_a : m_config.senseB_a;
    const QVector<int> &sel_b = (phaseIndex == 0) ? m_config.senseA_b : m_config.senseB_b;

    QVector<int> avg_a = meanSelectedChannels(channelData, sel_a);
    QVector<int> avg_b = meanSelectedChannels(channelData, sel_b);

    if (avg_a.isEmpty() || avg_b.isEmpty() ||
        avg_a.size() != timeStamps.size() || avg_b.size() != timeStamps.size()) {
        emit logMessage(phaseName + QStringLiteral(": averaged channels are invalid, skip stimulation."));
        finishPhase(0);
        return;
    }

    QVector<QVector<int>> avgChannelData;
    avgChannelData.reserve(2);
    avgChannelData.push_back(avg_a);
    avgChannelData.push_back(avg_b);

    const QVector<ABAlgorithm::Result> allResults =
        m_algorithm->analyzeEpoch(phaseIndex, timeStamps, avgChannelData);

    if (allResults.isEmpty()) {
        emit logMessage(phaseName + QStringLiteral(": no candidate event in this epoch."));
        finishPhase(0);
        return;
    }

    const int triggerSource = (phaseIndex == 0) ? 1 : 0;
    const uint32_t epochStartTs = timeStamps.first();
    const double fs = (m_sampleRateHz > 0.0) ? m_sampleRateHz : 30000.0;
    const qint64 phaseStartTimeMs = m_runClockActive ? m_runClock.elapsed() : 0;

    QVector<ABAlgorithm::Result> candidatesA;
    QVector<ABAlgorithm::Result> candidatesB;
    candidatesA.reserve(allResults.size());
    candidatesB.reserve(allResults.size());
    for (const auto &r : allResults) {
        if (!r.needStim) continue;
        if (r.suggestedAmplitude_uA <= 0) continue;
        if (r.channelIndex == 0) {
            candidatesA.push_back(r);
        } else if (r.channelIndex == 1) {
            candidatesB.push_back(r);
        }
    }
    if (candidatesA.isEmpty() && candidatesB.isEmpty()) {
        emit logMessage(phaseName + QStringLiteral(": no stimulation scheduled."));
        finishPhase(0);
        return;
    }

    const int perRegionLimit = qMax(1, m_maxStimPerEpoch / 2);
    auto trimRegionCandidates = [perRegionLimit](QVector<ABAlgorithm::Result> &regionCandidates) {
        std::sort(regionCandidates.begin(), regionCandidates.end(),
                  [](const ABAlgorithm::Result &a, const ABAlgorithm::Result &b) {
                      return a.spikeAmplitude_uV > b.spikeAmplitude_uV;
                  });
        if (regionCandidates.size() > perRegionLimit) {
            regionCandidates.resize(perRegionLimit);
        }
    };
    trimRegionCandidates(candidatesA);
    trimRegionCandidates(candidatesB);

    QVector<ABAlgorithm::Result> candidates;
    candidates.reserve(candidatesA.size() + candidatesB.size());
    candidates += candidatesA;
    candidates += candidatesB;

    std::sort(candidates.begin(), candidates.end(),
              [](const ABAlgorithm::Result &a, const ABAlgorithm::Result &b) {
                  return a.triggerTime < b.triggerTime;
              });

    const int epochId = ++m_epochCounter;

    QVector<StimTimelineOverlay::Item> items;
    items.reserve(candidates.size());

    int maxDelayMs = 0;
    int maxPulses = 1;
    for (int i = 0; i < candidates.size(); ++i) {
        const auto &r = candidates[i];

        const QString targetElectrode = electrodeForAvgIndex(phaseIndex, r.channelIndex);
        const int pulses = (r.suggestedNumPulses > 0) ? r.suggestedNumPulses : 1;
        maxPulses = qMax(maxPulses, pulses);

        double offsetSec = 0.0;
        if (r.triggerTime >= epochStartTs) {
            offsetSec = static_cast<double>(r.triggerTime - epochStartTs) / fs;
        }
        const double offsetMs = offsetSec * 1000.0;
        int delayMs = static_cast<int>(offsetMs);
        if (delayMs < 0) delayMs = 0;
        maxDelayMs = qMax(maxDelayMs, delayMs);
        const qint64 plannedTimeMs = phaseStartTimeMs + delayMs;

        StimTimelineOverlay::Item it;
        it.itemIndex = i;
        it.offsetMs = offsetMs;
        it.amp_uA = r.suggestedAmplitude_uA;
        it.pulses = pulses;
        it.ch = r.channelIndex;
        it.spike_uV = r.spikeAmplitude_uV;
        it.electrode = targetElectrode;
        it.fired = false;
        items.push_back(it);

        if (m_stimLog) {
            m_stimLog->logPlanned(epochId, phaseIndex, i, offsetMs,
                                  targetElectrode, it.amp_uA, it.pulses, it.ch, it.spike_uV);
        }
        emit stimPlanned(epochId, phaseIndex, i, plannedTimeMs,
                         targetElectrode, it.amp_uA, it.pulses, it.ch, it.spike_uV, triggerSource);

        QTimer::singleShot(delayMs, this,
                           [this, scheduleToken, epochId, phaseIndex, itemIdx = i,
                            targetElectrode, triggerSource,
                            amp = it.amp_uA, pulses = it.pulses,
                            ch = it.ch, spikeUv = it.spike_uV]() {
                               if (scheduleToken != m_scheduleToken) return;
                               if (m_timeline) m_timeline->markFired(epochId, itemIdx);
                               if (!m_engine) return;
                               const qint64 firedTimeMs = m_runClockActive ? m_runClock.elapsed() : 0;
                               if (m_stimLog) {
                                   m_stimLog->logFired(epochId, phaseIndex, itemIdx,
                                                       targetElectrode, amp, pulses);
                               }
                               emit stimFired(epochId, phaseIndex, itemIdx, firedTimeMs,
                                              targetElectrode, amp, pulses, ch, spikeUv, triggerSource);
                               m_engine->applyAdaptiveStim(targetElectrode, amp, pulses, triggerSource);
                           });
    }

    if (m_timeline) {
        m_timeline->setEpochPlan(epochId, phaseIndex, m_epochDurationSec, items);
        m_timeline->show();
        m_timeline->raise();
    }

    const int finishDelayMs = maxDelayMs + qMax(10, maxPulses * 10);
    finishPhase(finishDelayMs);

    emit logMessage(QStringLiteral("%1: epochId=%2 planned stim count=%3 (a=%4, b=%5), stim phase ends in %6 ms")
                        .arg(phaseName)
                        .arg(epochId)
                        .arg(items.size())
                        .arg(candidatesA.size())
                        .arg(candidatesB.size())
                        .arg(finishDelayMs));
}
