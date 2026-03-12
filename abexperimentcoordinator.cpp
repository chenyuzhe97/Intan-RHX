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

void ABExperimentCoordinator::setRoutingConfig(const RoutingConfig &config)
{
    m_config = config;
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
            used++;
        }
        if (used == 0) return {};
        out[i] = static_cast<int>(std::llround(static_cast<double>(sum) / static_cast<double>(used)));
    }

    return out;
}

QString ABExperimentCoordinator::phaseNameForIndex(int phaseIndex) const
{
    return (phaseIndex == 0)
        ? QStringLiteral("PhaseA (A端 stream0)")
        : QStringLiteral("PhaseB (B端 stream2)");
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
    const QString phaseName = phaseNameForIndex(phaseIndex);
    if (!m_algorithm || !m_engine) return;
    if (timeStamps.isEmpty() || channelData.isEmpty()) return;

    const QVector<int> &sel_a = (phaseIndex == 0) ? m_config.senseA_a : m_config.senseB_a;
    const QVector<int> &sel_b = (phaseIndex == 0) ? m_config.senseA_b : m_config.senseB_b;

    QVector<int> avg_a = meanSelectedChannels(channelData, sel_a);
    QVector<int> avg_b = meanSelectedChannels(channelData, sel_b);

    if (avg_a.isEmpty() || avg_b.isEmpty() || avg_a.size() != timeStamps.size() || avg_b.size() != timeStamps.size()) {
        emit logMessage(phaseName + QStringLiteral(": 平均信号生成失败（通道列表/数据长度不匹配）"));
        return;
    }

    QVector<QVector<int>> avgChannelData;
    avgChannelData.reserve(2);
    avgChannelData.push_back(avg_a);
    avgChannelData.push_back(avg_b);

    const QVector<ABAlgorithm::Result> allResults =
        m_algorithm->analyzeEpoch(phaseIndex, timeStamps, avgChannelData);

    if (allResults.isEmpty()) {
        emit logMessage(phaseName + QStringLiteral(": 本 epoch 未检测到事件"));
        return;
    }

    const int triggerSource = (phaseIndex == 0) ? 1 : 0;
    const uint32_t epochStartTs = timeStamps.first();
    const double fs = (m_sampleRateHz > 0.0) ? m_sampleRateHz : 30000.0;

    QVector<ABAlgorithm::Result> candidates;
    candidates.reserve(allResults.size());
    for (const auto &r : allResults) {
        if (!r.needStim) continue;
        if (r.suggestedAmplitude_uA <= 0) continue;
        candidates.push_back(r);
    }
    if (candidates.isEmpty()) {
        emit logMessage(phaseName + QStringLiteral(": 无需刺激"));
        return;
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const ABAlgorithm::Result &a, const ABAlgorithm::Result &b) {
                  return a.spikeAmplitude_uV > b.spikeAmplitude_uV;
              });
    const int maxStimPerEpoch = 10;
    if (candidates.size() > maxStimPerEpoch) candidates.resize(maxStimPerEpoch);

    std::sort(candidates.begin(), candidates.end(),
              [](const ABAlgorithm::Result &a, const ABAlgorithm::Result &b) {
                  return a.triggerTime < b.triggerTime;
              });

    const int epochId = ++m_epochCounter;

    QVector<StimTimelineOverlay::Item> items;
    items.reserve(candidates.size());

    for (int i = 0; i < candidates.size(); ++i) {
        const auto &r = candidates[i];

        const QString targetElectrode = electrodeForAvgIndex(phaseIndex, r.channelIndex);
        const int pulses = (r.suggestedNumPulses > 0) ? r.suggestedNumPulses : 1;

        double offsetSec = 0.0;
        if (r.triggerTime >= epochStartTs) {
            offsetSec = static_cast<double>(r.triggerTime - epochStartTs) / fs;
        }
        const double offsetMs = offsetSec * 1000.0;
        int delayMs = static_cast<int>(offsetMs);
        if (delayMs < 0) delayMs = 0;

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

        QTimer::singleShot(delayMs, this,
                           [this, epochId, itemIdx = i,
                            targetElectrode,
                            triggerSource,
                            amp = it.amp_uA,
                            pulses = it.pulses]() {
                               if (m_timeline) m_timeline->markFired(epochId, itemIdx);
                               if (!m_engine) return;
                               m_engine->applyAdaptiveStim(targetElectrode, amp, pulses, triggerSource);
                           });
    }

    if (m_timeline) {
        m_timeline->setEpochPlan(epochId, phaseIndex, m_epochDurationSec, items);
        m_timeline->show();
        m_timeline->raise();
    }

    emit logMessage(QStringLiteral("%1: epochId=%2 计划刺激=%3")
                        .arg(phaseName)
                        .arg(epochId)
                        .arg(items.size()));
}