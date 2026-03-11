#include "controller.h"

#include <QThread>

namespace {

constexpr int kNever = 65535;
constexpr int kStimStepSize = StimStepSize500nA;
constexpr int kRunningStimUpdateMinUs = 500;
constexpr int kRunningStimUpdateGuardUs = 200;
constexpr double kFallbackSampleRateHz = 30000.0;

int runningStimUpdateDelayUs(AbstractRHXController* controller, int commandSequenceLength)
{
    const double sampleRateHz = controller ? controller->getSampleRate() : kFallbackSampleRateHz;
    if (sampleRateHz <= 0.0) return 1000;

    const double sequenceUs = std::ceil((1.0e6 * commandSequenceLength) / sampleRateHz);
    return qMax(kRunningStimUpdateMinUs, int(sequenceUs) + kRunningStimUpdateGuardUs);
}

}

Controller::Controller(AbstractRHXController* rhxController_)
    : rhxController(rhxController_)
{
}

void Controller::setStimSequenceParameters(ElectrodeParameters *parameters)
{
    if (!parameters) return;
    if (rhxController->isSynthetic() || rhxController->isPlayback()) return;

    const bool controllerIsRunning = rhxController->isRunning();
    const int stream = parameters->stream;
    const int channel = parameters->channel;
    const double sampleRateHz = rhxController->getSampleRate();
    const double timestep = (sampleRateHz > 0.0) ? (1.0e6 / sampleRateHz) : (1.0e6 / kFallbackSampleRateHz);
    const double currentstep = RHXRegisters::stimStepSizeToDouble(static_cast<StimStepSize>(kStimStepSize)) * 1.0e6;
    const int numOfPulses = qMax(1, parameters->numOfPulses);

    const bool edgeTriggered = (parameters->triggerEdgeOrlevel == TriggerEdge);
    const bool triggerOnLow = (parameters->triggerHighOrlow == TriggerLow);
    const StimShape stimShape = static_cast<StimShape>(parameters->stimShape);
    const bool negativeFirst = (parameters->StimPolarity == NegativeFirst);

    rhxController->configureStimTrigger(stream,
                                        channel,
                                        parameters->triggerSource,
                                        parameters->enabled,
                                        edgeTriggered,
                                        triggerOnLow);

    rhxController->configureStimPulses(stream,
                                       channel,
                                       numOfPulses,
                                       stimShape,
                                       negativeFirst);

    const int preStimAmpSettle = qRound(parameters->preStimAmpSettle / timestep);
    const int postStimAmpSettle = qRound(parameters->postStimAmpSettle / timestep);
    const int postTriggerDelay = qRound(parameters->postTriggerDelay / timestep);
    const int firstPhaseDuration = qRound(parameters->firstPhaseDuration / timestep);
    const int secondPhaseDuration = qRound(parameters->secondPhaseDuration / timestep);
    const int interphaseDelay = qRound(parameters->interphaseDelay / timestep);
    const int refractoryPeriod = qRound(parameters->refractoryPeriod / timestep);
    const int postStimChargeRecovOn = qRound(parameters->postStimChargeRecovOn / timestep);
    const int postStimChargeRecovOff = qRound(parameters->postStimChargeRecovOff / timestep);
    const int pulseTrainPeriod = qRound(parameters->pulseTrainPeriod / timestep);

    int eventStartStim = 0;
    int eventStimPhase2 = kNever;
    int eventStimPhase3 = kNever;
    int eventEndStim = 0;
    int eventEnd = 0;
    int eventRepeatStim = kNever;
    int eventAmpSettleOn = kNever;
    int eventAmpSettleOff = 0;
    int eventAmpSettleOnRepeat = kNever;
    int eventAmpSettleOffRepeat = kNever;
    int eventChargeRecovOn = kNever;
    int eventChargeRecovOff = 0;

    switch (stimShape) {
    case Biphasic:
        eventStartStim = postTriggerDelay;
        eventStimPhase2 = eventStartStim + firstPhaseDuration;
        eventStimPhase3 = kNever;
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
        eventStartStim = postTriggerDelay;
        eventStimPhase2 = kNever;
        eventStimPhase3 = kNever;
        eventEndStim = eventStartStim + firstPhaseDuration;
        eventEnd = eventEndStim + refractoryPeriod;
        break;
    }

    if (parameters->pulseOrTrain == PulseTrain) {
        eventRepeatStim = eventStartStim + pulseTrainPeriod;
    }

    if (parameters->enableAmpSettle) {
        eventAmpSettleOn = eventStartStim - preStimAmpSettle;
        eventAmpSettleOff = eventEndStim + postStimAmpSettle;
        if (!parameters->maintainAmpSettle && eventRepeatStim != kNever) {
            eventAmpSettleOnRepeat = eventRepeatStim - preStimAmpSettle;
            eventAmpSettleOffRepeat = eventAmpSettleOff;
        }
    }

    if (parameters->enableChargeRecovery) {
        eventChargeRecovOn = eventEndStim + postStimChargeRecovOn;
        eventChargeRecovOff = eventEndStim + postStimChargeRecovOff;
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

    RHXRegisters chipRegisters(rhxController->getType(),
                               rhxController->getSampleRateEnum(),
                               static_cast<StimStepSize>(kStimStepSize));
    int commandSequenceLength = 0;
    std::vector<unsigned int> commandList;

    const int firstPhaseMagnitude = qRound(parameters->firstPhaseAmplitude / currentstep);
    const int secondPhaseMagnitude = qRound(parameters->secondPhaseAmplitude / currentstep);

    int posMag = 0;
    int negMag = 0;
    if (negativeFirst) {
        negMag = firstPhaseMagnitude;
        posMag = secondPhaseMagnitude;
    } else {
        posMag = firstPhaseMagnitude;
        negMag = secondPhaseMagnitude;
    }

    commandSequenceLength = chipRegisters.createCommandListSetStimMagnitudes(commandList,
                                                                             channel,
                                                                             posMag,
                                                                             0,
                                                                             negMag,
                                                                             0);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0);
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1,
                                          0,
                                          commandSequenceLength - 1);

    chipRegisters.createCommandListDummy(commandList,
                                         8192,
                                         chipRegisters.createRHXCommand(RHXRegisters::RHXCommandRegRead, 255));
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd2, 0);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd3, 0);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd4, 0);

    if (controllerIsRunning) {
        // In closed-loop operation we keep acquisition running, briefly allow manual aux commands
        // to program magnitudes on the selected stream, then restore automatic stim sequencing.
        rhxController->setStimCmdMode(false);
        rhxController->enableAuxCommandsOnOneStream(stream);
        QThread::usleep(runningStimUpdateDelayUs(rhxController, commandSequenceLength));
        rhxController->setStimCmdMode(true);
        return;
    }

    rhxController->setMaxTimeStep(commandSequenceLength);
    rhxController->setContinuousRunMode(false);
    rhxController->setStimCmdMode(false);
    rhxController->enableAuxCommandsOnOneStream(stream);

    rhxController->run();
    while (rhxController->isRunning()) {
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

    commandSequenceLength = chipRegisters.createCommandListRHSRegisterConfig(commandList, true);
    rhxController->uploadCommandList(commandList, AbstractRHXController::AuxCmd1, 0);
    rhxController->selectAuxCommandLength(AbstractRHXController::AuxCmd1, 0, commandSequenceLength - 1);
    rhxController->enableAuxCommandsOnOneStream(stream);
}

void Controller::stimTrigger(int triggerNumber, bool triggerOn)
{
    rhxController->setManualStimTrigger(triggerNumber, triggerOn);
}
