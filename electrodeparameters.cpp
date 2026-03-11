#include "electrodeparameters.h"

#include <string>

ElectrodeParameters::ElectrodeParameters(std::string electrodeName)
{
    channel = 0;
    stream = 0;

    char group = electrodeName[0];
    int number = std::stoi(electrodeName.substr(1));

    if (group == 'A') {
        stream = 0;
    } else if (group == 'B') {
        stream = 2;
    } else if (group == 'C') {
        stream = 4;
    } else if (group == 'D') {
        stream = 6;
    }

    channel = 2 * number - 1;

    preStimAmpSettle = 0;
    postStimAmpSettle = 30;
    postTriggerDelay = 0;
    firstPhaseDuration = 3;
    secondPhaseDuration = 3;
    interphaseDelay = 3;
    refractoryPeriod = 30;
    postStimChargeRecovOn = 0;
    postStimChargeRecovOff = 0;
    pulseTrainPeriod = 300;

    triggerEdgeOrlevel = 0;
    triggerHighOrlow = 0;
    enabled = true;

    firstPhaseAmplitude = 0;
    secondPhaseAmplitude = 0;
    stimShape = 0;
    StimPolarity = 0;

    numOfPulses = 1;

    pulseOrTrain = 0;
    enableChargeRecovery = false;
    enableAmpSettle = true;
    maintainAmpSettle = false;
}

void ElectrodeParameters::SetElectrodeInfo(int streamNum, int channelNum)
{
    stream = streamNum;
    channel = channelNum;
}

void ElectrodeParameters::SetStimulationTiming(int postTriggerDelay, int firstPhaseDuration,
                                               int secondPhaseDuration, int interphaseDelay,
                                               int refractoryPeriod)
{
    this->postTriggerDelay = postTriggerDelay;
    this->firstPhaseDuration = firstPhaseDuration;
    this->secondPhaseDuration = secondPhaseDuration;
    this->interphaseDelay = interphaseDelay;
    this->refractoryPeriod = refractoryPeriod;
}

void ElectrodeParameters::SetStimulationAmplitude(int firstPhaseAmplitude, int secondPhaseAmplitude)
{
    this->firstPhaseAmplitude = firstPhaseAmplitude;
    this->secondPhaseAmplitude = secondPhaseAmplitude;
}

void ElectrodeParameters::SetStimulationSource(int source)
{
    triggerSource = source + 24;
}
