#ifndef ELECTRODEPARAMETERS_H
#define ELECTRODEPARAMETERS_H

#include <string>

class ElectrodeParameters
{
public:
    explicit ElectrodeParameters(std::string electrodeName);

    // 1. 批量设置所有参数（通过参数传递）
    void SetAllParameters(
        int streamNum, int channelNum,
        int preSettle, int postSettle,
        int triggerDelay, int firstDur, int secondDur,
        int interDelay, int refractory,
        int chargeRecovOn, int chargeRecovOff,
        int pulsePeriod);

    // 2. 设置电极基本信息
    void SetElectrodeInfo(int streamNum, int channelNum);

    // 3. 设置刺激时序参数
    void SetStimulationTiming(int postTriggerDelay, int firstPhaseDuration, int secondPhaseDuration,int refractoryPeriod);

    // 4. 设置电极刺激强度
    void SetStimulationAmplitude(int firstPhaseAmplitude,int secondPhaseAmplitude);

    // 5. 设置电极触发编号
    void SetStimulationSource(int source);



    // 成员变量（公开访问，也可以设置为private并提供getter/setter）
    int stream;   // 流顺序
    int channel;  // 注意拼写

    // 电极参数设置
    int preStimAmpSettle;
    int postStimAmpSettle;
    int postTriggerDelay;  // 刺激时序参数
    int firstPhaseDuration; // 刺激时序参数
    int secondPhaseDuration;  // 刺激时序参数
    int interphaseDelay;
    int refractoryPeriod;  // 刺激时序参数
    int postStimChargeRecovOn;
    int postStimChargeRecovOff;
    int pulseTrainPeriod;

    // 电极触发方式设置
    int triggerSource;
    int triggerEdgeOrlevel;
    int triggerHighOrlow;
    bool enabled;

    // 电极刺激相关设置
    int firstPhaseAmplitude;
    int secondPhaseAmplitude;
    int stimShape;
    int StimPolarity;
    int pulseOrTrain;

    // 刺激次数
    int numOfPulses;

    // 其他未知参数
    bool enableChargeRecovery;
    bool enableAmpSettle;
    bool maintainAmpSettle;

};

#endif // ELECTRODEPARAMETERS_H
