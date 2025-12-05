#include "controller.h"

Controller::Controller(AbstractRHXController* rhxController_):rhxController(rhxController_){}

void Controller::setStimSequenceParameters(ElectrodeParameters *parameters)
{
    if (rhxController->isSynthetic() || rhxController->isPlayback()) return;

    const int Never = 65535;

    int stream = parameters->stream;
    int channel = parameters->channel;

    double timestep = 33.3333;
    double currentstep = 0.5;
    int numOfPulses = parameters->numOfPulses;

    rhxController->configureStimTrigger(stream,channel,parameters->triggerSource,parameters->enabled,1,0);
    rhxController->configureStimPulses(stream,channel,numOfPulses,(StimShape)0,0);

    int preStimAmpSettle = parameters->preStimAmpSettle / timestep;
    int postStimAmpSettle = parameters->postStimAmpSettle / timestep;
    int postTriggerDelay = parameters->postTriggerDelay / timestep;
    int firstPhaseDuration = parameters->firstPhaseDuration / timestep;
    int secondPhaseDuration = parameters->secondPhaseDuration / timestep;
    int interphaseDelay = parameters->interphaseDelay / timestep;
    int refractoryPeriod = parameters->refractoryPeriod / timestep;
    int postStimChargeRecovOn = parameters->postStimChargeRecovOn / timestep;
    int postStimChargeRecovOff = parameters->postStimChargeRecovOff / timestep;
    int pulseTrainPeriod = parameters->pulseTrainPeriod / timestep;

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
        eventStartStim = postTriggerDelay;
        eventStimPhase2 = eventStartStim + firstPhaseDuration;
        eventStimPhase3 = Never;
        eventEndStim = eventStimPhase2 + secondPhaseDuration;
        eventEnd = eventEndStim + refractoryPeriod;
        break;
    case BiphasicWithInterphaseDelay:
        eventStartStim = postTriggerDelay;
        eventStimPhase2 = eventStartStim + firstPhaseDuration;
        eventStimPhase3 = eventStimPhase2 + interphaseDelay;
        eventEndStim = eventStimPhase3 + secondPhaseDuration;
        eventEnd = eventEndStim + refractoryPeriod;
        break;
    case Triphasic:
        eventStartStim = postTriggerDelay;
        eventStimPhase2 = eventStartStim + firstPhaseDuration;
        eventStimPhase3 = eventStimPhase2 + secondPhaseDuration;
        eventEndStim = eventStimPhase3 + firstPhaseDuration;
        eventEnd = eventEndStim + refractoryPeriod;
        break;
    case Monophasic:

        return;
    }


    if (parameters->pulseOrTrain == 1) {
        eventRepeatStim = eventStartStim + pulseTrainPeriod;
    } else {
        eventRepeatStim = Never;
    }

    if (parameters->enableAmpSettle) {
        eventAmpSettleOn = eventStartStim - preStimAmpSettle;
        eventAmpSettleOff = eventEndStim + postStimAmpSettle;
        if (parameters->maintainAmpSettle) {
            eventAmpSettleOnRepeat = Never;
            eventAmpSettleOffRepeat = Never;
        } else {
            eventAmpSettleOnRepeat = eventRepeatStim - preStimAmpSettle;
            eventAmpSettleOffRepeat = postStimAmpSettle;
        }
    } else {
        eventAmpSettleOn = Never;
        eventAmpSettleOff = 0;
        eventAmpSettleOnRepeat = Never;
        eventAmpSettleOffRepeat = Never;
    }

    if (parameters->enableChargeRecovery) {
        eventChargeRecovOn = eventEndStim + postStimChargeRecovOn;
        eventChargeRecovOff = eventEndStim + postStimChargeRecovOff;
    } else {
        eventChargeRecovOn = Never;
        eventChargeRecovOff = 0;
    }

    rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOn, eventAmpSettleOn);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventStartStim, eventStartStim);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventStimPhase2, eventStimPhase2);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventStimPhase3, eventStimPhase3);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventEndStim, eventEndStim);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventRepeatStim, eventRepeatStim);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOff, eventAmpSettleOff);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventChargeRecovOn, eventChargeRecovOn);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventChargeRecovOff, eventChargeRecovOff);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOnRepeat, eventAmpSettleOnRepeat);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventAmpSettleOffRepeat, eventAmpSettleOffRepeat);
    rhxController->programStimReg(stream, channel, AbstractRHXController::EventEnd, eventEnd);

    rhxController->enableAuxCommandsOnOneStream(stream);


    RHXRegisters chipRegisters(rhxController->getType(), rhxController->getSampleRate());
    int commandSequenceLength;
    std::vector<unsigned int> commandList;

    int firstPhaseAmplitude = round(parameters->firstPhaseAmplitude / currentstep);
    int secondPhaseAmplitude = round(parameters->secondPhaseAmplitude / currentstep);
    int posMag = firstPhaseAmplitude;
    int negMag = secondPhaseAmplitude;

    commandSequenceLength = chipRegisters.createCommandListSetStimMagnitudes(commandList, channel, posMag, 0, negMag, 0);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0);  // RHS - bank doesn't matter
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1, 0, commandSequenceLength - 1);

    chipRegisters.createCommandListDummy(commandList, 8192, chipRegisters.createRHXCommand(RHXRegisters::RHXCommandRegRead, 255));
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd2, 0);  // RHS - bank doesn't matter
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd3, 0);  // RHS - bank doesn't matter
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd4, 0);  // RHS - bank doesn't matter

    rhxController->setMaxTimeStep(commandSequenceLength);
    rhxController->setContinuousRunMode(false);
    rhxController->setStimCmdMode(false);
    rhxController->enableAuxCommandsOnOneStream(stream);

    rhxController->run();
    while (rhxController->isRunning() ) {}

    commandSequenceLength = chipRegisters.createCommandListRHSRegisterRead(commandList);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0);  // RHS - bank doesn't matter
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1, 0, commandSequenceLength - 1);
    rhxController->run();
    while (rhxController->isRunning()) {}

    RHXDataBlock dataBlock(rhxController->getType(), rhxController->getNumEnabledDataStreams());
    rhxController->readDataBlock(&dataBlock);
    rhxController->readDataBlock(&dataBlock);

    commandSequenceLength = chipRegisters.createCommandListRHSRegisterConfig(commandList, true);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0);  // RHS - bank doesn't matter
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1, 0, commandSequenceLength - 1);
    rhxController->enableAuxCommandsOnAllStreams();

}

void Controller::stimTrigger(int triggerNumber, bool triggerOn)
{
    rhxController->setManualStimTrigger(triggerNumber,triggerOn);
}
