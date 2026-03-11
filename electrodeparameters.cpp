#include "electrodeparameters.h"
#include <string>

ElectrodeParameters::ElectrodeParameters(std::string electrodeName) {
    // 假设 ElectrodeParameters 类中有 int channel 和 int stream 成员变量

    // 默认值，以防输入格式完全不匹配
    channel = 0;
    stream = 0;

    // 提取电极组（A, B, C, D）
    char group = electrodeName[0];

    // 提取数字部分并将其转换为整数。
    // 假设输入总是有效的，例如 "A1", "B8"
    // C++11/14/17 使用 std::stoi 是最直接的方法，如果不能用 stoi，则需要使用 atoi 或其他方法。
    int number = std::stoi(electrodeName.substr(1));

    // --- 核心逻辑：Stream 分配 ---

    if (group == 'A') {
        stream = 0;
    } else if (group == 'B') {
        stream = 2;
    } else if (group == 'C') {
        stream = 4; // C1-C8
    } else if (group == 'D') {
        stream = 6; // D1-D8
    } else {
        // 如果不是 A, B, C, D，则 stream 保持默认值 0 (或设置为错误码)
        // 为了简洁，这里不做特殊处理，保持默认值。
    }

    // --- 核心逻辑：Channel 分配 ---

    // 统一的通道规律：channel = 2 * n - 1
    // (A1/B1/C1/D1: 2*1 - 1 = 1; A2/B2/C2/D2: 2*2 - 1 = 3; ... A8: 2*8 - 1 = 15)
    // 假设 number 总是在 1 到 8 的有效范围内。
    channel = 2 * number - 1;


    preStimAmpSettle = 0;
    postStimAmpSettle = 30;
    postTriggerDelay = 0 ;
    firstPhaseDuration = 3;
    secondPhaseDuration = 3;
    interphaseDelay = 3;
    refractoryPeriod = 30;
    postStimChargeRecovOn = 0;
    postStimChargeRecovOff = 0;
    pulseTrainPeriod = 300;

    // 电极触发方式
    triggerEdgeOrlevel = 0;
    triggerHighOrlow = 0;
    enabled = true;

    // 设置电极强度
    firstPhaseAmplitude = 0;
    secondPhaseAmplitude = 0;
    stimShape = 0; // 双向波
    StimPolarity =0;

    // 刺激次数
    numOfPulses = 1;

    // 其他未知参数
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


