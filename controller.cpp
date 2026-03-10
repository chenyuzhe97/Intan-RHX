#include "controller.h"

Controller::Controller(AbstractRHXController* rhxController_):rhxController(rhxController_){}

void Controller::setStimSequenceParameters(ElectrodeParameters *parameters)
{
    if (rhxController->isSynthetic() || rhxController->isPlayback()) return;

    // ⭐ 先记住当前控制器是否在跑（continuous 采集中）
    bool controllerIsRunning = rhxController->isRunning();

    const int Never = 65535;

    int stream  = parameters->stream;
    int channel = parameters->channel;
    qDebug()<<"当前触发通道为：" << channel;
    qDebug()<<"当前触发流为:" << stream;
    double timestep    = 33.3333;
    double currentstep = 0.5;
    int numOfPulses    = parameters->numOfPulses;

    // ==== 1) 配置触发源 & 脉冲数（这部分无论是否在跑都可以做） ====
    rhxController->configureStimTrigger(stream,
                                        channel,
                                        parameters->triggerSource,
                                        parameters->enabled,
                                        1,
                                        0);
    qDebug()<<"触发源："<<parameters->triggerSource;

    rhxController->configureStimPulses(stream,
                                       channel,
                                       numOfPulses,
                                       (StimShape)0,
                                       1);

    // ==== 2) 计算各个时间事件 ====
    int preStimAmpSettle      = parameters->preStimAmpSettle      / timestep;
    int postStimAmpSettle     = parameters->postStimAmpSettle     / timestep;
    int postTriggerDelay      = parameters->postTriggerDelay      / timestep;
    int firstPhaseDuration    = parameters->firstPhaseDuration    / timestep;
    int secondPhaseDuration   = parameters->secondPhaseDuration   / timestep;
    int interphaseDelay       = parameters->interphaseDelay       / timestep;
    int refractoryPeriod      = parameters->refractoryPeriod      / timestep;
    int postStimChargeRecovOn = parameters->postStimChargeRecovOn / timestep;
    int postStimChargeRecovOff= parameters->postStimChargeRecovOff/ timestep;
    int pulseTrainPeriod      = parameters->pulseTrainPeriod      / timestep;

    int eventStartStim;
    int eventStimPhase2;
    int eventStimPhase3;
    int eventEndStim;
    int eventEnd;
    int eventRepeatStim;
    int eventAmpSettleOn;
    int eventAmpSettleOff;
    int eventAmpSettleOnRepeat;
    int eventAmpSettleOffRepeat;
    int eventChargeRecovOn;
    int eventChargeRecovOff;

    switch ((StimShape) parameters->stimShape) {
    case Biphasic:
        eventStartStim  = postTriggerDelay;
        eventStimPhase2 = eventStartStim + firstPhaseDuration;
        eventStimPhase3 = Never;
        eventEndStim    = eventStimPhase2 + secondPhaseDuration;
        eventEnd        = eventEndStim + refractoryPeriod;
        break;
    case BiphasicWithInterphaseDelay:
        eventStartStim  = postTriggerDelay;
        eventStimPhase2 = eventStartStim + firstPhaseDuration;
        eventStimPhase3 = eventStimPhase2 + interphaseDelay;
        eventEndStim    = eventStimPhase3 + secondPhaseDuration;
        eventEnd        = eventEndStim + refractoryPeriod;
        break;
    case Triphasic:
        eventStartStim  = postTriggerDelay;
        eventStimPhase2 = eventStartStim + firstPhaseDuration;
        eventStimPhase3 = eventStimPhase2 + secondPhaseDuration;
        eventEndStim    = eventStimPhase3 + firstPhaseDuration;
        eventEnd        = eventEndStim + refractoryPeriod;
        break;
    case Monophasic:
        // 暂时不支持单相就直接返回
        return;
    }

    if (parameters->pulseOrTrain == 1) {
        eventRepeatStim = eventStartStim + pulseTrainPeriod;
    } else {
        eventRepeatStim = Never;
    }

    if (parameters->enableAmpSettle) {
        eventAmpSettleOn  = eventStartStim - preStimAmpSettle;
        eventAmpSettleOff = eventEndStim + postStimAmpSettle;
        if (parameters->maintainAmpSettle) {
            eventAmpSettleOnRepeat  = Never;
            eventAmpSettleOffRepeat = Never;
        } else {
            eventAmpSettleOnRepeat  = eventRepeatStim - preStimAmpSettle;
            eventAmpSettleOffRepeat = postStimAmpSettle;
        }
    } else {
        eventAmpSettleOn       = Never;
        eventAmpSettleOff      = 0;
        eventAmpSettleOnRepeat = Never;
        eventAmpSettleOffRepeat= Never;
    }

    if (parameters->enableChargeRecovery) {
        eventChargeRecovOn  = eventEndStim + postStimChargeRecovOn;
        eventChargeRecovOff = eventEndStim + postStimChargeRecovOff;
    } else {
        eventChargeRecovOn  = Never;
        eventChargeRecovOff = 0;
    }

    // ==== 3) 把事件时间写进刺激寄存器（运行中也可以安全改） ====
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOn,        eventAmpSettleOn);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventStartStim,          eventStartStim);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventStimPhase2,         eventStimPhase2);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventStimPhase3,         eventStimPhase3);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventEndStim,            eventEndStim);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventRepeatStim,         eventRepeatStim);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOff,       eventAmpSettleOff);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventChargeRecovOn,      eventChargeRecovOn);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventChargeRecovOff,     eventChargeRecovOff);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOnRepeat,  eventAmpSettleOnRepeat);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOffRepeat, eventAmpSettleOffRepeat);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventEnd,                eventEnd);

    // rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOn,        0);
    // rhxController->programStimReg(stream, channel, AbstractRHXController::EventStartStim,          0);
    // rhxController->programStimReg(stream, channel, AbstractRHXController::EventStimPhase2,         3);
    // rhxController->programStimReg(stream, channel, AbstractRHXController::EventStimPhase3,         65535);
    // rhxController->programStimReg(stream, channel, AbstractRHXController::EventEndStim,            6);
    // rhxController->programStimReg(stream, channel, AbstractRHXController::EventRepeatStim,         65535);
    // rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOff,       36);
    // rhxController->programStimReg(stream, channel, AbstractRHXController::EventChargeRecovOn,      65535);
    // rhxController->programStimReg(stream, channel, AbstractRHXController::EventChargeRecovOff,     0);
    // rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOnRepeat,  65535);
    // rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOffRepeat, 30);
    // rhxController->programStimReg(stream, channel, AbstractRHXController::EventEnd,                36);

    rhxController->enableAuxCommandsOnOneStream(stream);

    // ==== 4) 设置幅度，生成并上传 Aux 命令序列（运行中也 OK） ====
    RHXRegisters chipRegisters(rhxController->getType(),
                               rhxController->getSampleRate(),StimStepSize500nA);
    int commandSequenceLength;
    std::vector<unsigned int> commandList;

    // 你这边改过的：μA → Intan 刺激 DAC 代码的转换
    qDebug()<<"原始刺激大小：" << parameters->firstPhaseAmplitude;
    int firstPhaseMagnitude  = qRound(parameters->firstPhaseAmplitude  / currentstep / 1000.0);
    int secondPhaseMagnitude = qRound(parameters->secondPhaseAmplitude / currentstep / 1000.0);

    qDebug()<<"当前刺激大小:" <<firstPhaseMagnitude << "mv";

    // int posMag = firstPhaseMagnitude;
    // int negMag = secondPhaseMagnitude;

    int posMag = parameters->firstPhaseAmplitude/5;
    int negMag = parameters->secondPhaseAmplitude/5;

    commandSequenceLength =
        chipRegisters.createCommandListSetStimMagnitudes(commandList,
                                                         channel,
                                                         posMag,
                                                         0,
                                                         negMag,
                                                         0);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0);
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1,
                                          0,
                                          commandSequenceLength - 1);

    chipRegisters.createCommandListDummy(
        commandList,
        8192,
        chipRegisters.createRHXCommand(RHXRegisters::RHXCommandRegRead, 255));
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd2, 0);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd3, 0);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd4, 0);

    // ================== 分界线 ==================
    // 上面的东西都是 "写配置"、"上传指令" —— continuous 状态下是安全的。
    // 下面这一大坨 run()/while()/register read，只在控制器不在跑的时候才做。
    // =======================================================

    if (controllerIsRunning) {
        // ⭐ 正在 continuous 采集：到这里就够了，不要打断采集。
        return;
    }

    // ====== 以下是“离线配置/校准模式”，只在未运行时执行 ======

    rhxController->setMaxTimeStep(commandSequenceLength);
    rhxController->setContinuousRunMode(false);
    rhxController->setStimCmdMode(false);
    rhxController->enableAuxCommandsOnOneStream(stream);

    rhxController->run();
    while (rhxController->isRunning()) {
        // 离线状态下这样阻塞没问题
    }

    commandSequenceLength = chipRegisters.createCommandListRHSRegisterRead(commandList);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0);
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1, 0, commandSequenceLength - 1);
    rhxController->run();
    while (rhxController->isRunning()) {
    }

    RHXDataBlock dataBlock(rhxController->getType(),
                           rhxController->getNumEnabledDataStreams());
    rhxController->readDataBlock(&dataBlock);
    rhxController->readDataBlock(&dataBlock);

    commandSequenceLength =
        chipRegisters.createCommandListRHSRegisterConfig(commandList, true);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0);
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1, 0, commandSequenceLength - 1);
    rhxController->enableAuxCommandsOnOneStream(stream);
}


void Controller::stimTrigger(int triggerNumber, bool triggerOn)
{
    rhxController->setManualStimTrigger(triggerNumber,triggerOn);
}
