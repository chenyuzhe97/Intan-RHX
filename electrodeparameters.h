#ifndef ELECTRODEPARAMETERS_H
#define ELECTRODEPARAMETERS_H

#include <string>

class ElectrodeParameters
{
public:
    explicit ElectrodeParameters(std::string electrodeName);

    void SetAllParameters(
        int streamNum, int channelNum,
        int preSettle, int postSettle,
        int triggerDelay, int firstDur, int secondDur,
        int interDelay, int refractory,
        int chargeRecovOn, int chargeRecovOff,
        int pulsePeriod);

    void SetElectrodeInfo(int streamNum, int channelNum);
    void SetStimulationTiming(int postTriggerDelay, int firstPhaseDuration, int secondPhaseDuration,
                              int interphaseDelay, int refractoryPeriod = 30);
    void SetStimulationAmplitude(int firstPhaseAmplitude, int secondPhaseAmplitude);
    void SetStimulationSource(int source);

    int stream;
    int channel;

    int preStimAmpSettle;
    int postStimAmpSettle;
    int postTriggerDelay;
    int firstPhaseDuration;
    int secondPhaseDuration;
    int interphaseDelay;
    int refractoryPeriod;
    int postStimChargeRecovOn;
    int postStimChargeRecovOff;
    int pulseTrainPeriod;

    int triggerSource;
    int triggerEdgeOrlevel;
    int triggerHighOrlow;
    bool enabled;

    int firstPhaseAmplitude;
    int secondPhaseAmplitude;
    int stimShape;
    int StimPolarity;
    int pulseOrTrain;

    int numOfPulses;

    bool enableChargeRecovery;
    bool enableAmpSettle;
    bool maintainAmpSettle;
};

#endif // ELECTRODEPARAMETERS_H
