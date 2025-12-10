#ifndef CONTROLLER_H
#define CONTROLLER_H


#include "rhxcontroller.h"
#include "stimparameters.h"
#include "ElectrodeParameters.h"
#include "rhxregisters.h"
#include "math.h"
#include <QDebug>

class Controller
{
public:
    Controller(AbstractRHXController* rhxController_);

    // 设置对应通道刺激电极的参数
    void setChannel(ElectrodeParameters* ampParameters);

    // 传入已经设置好的刺激电极的channel
    void setStimSequenceParameters(ElectrodeParameters* Parameters);

    // 触发电极
    void stimTrigger(int triggerNumber,bool triggerOn);


public:
    AbstractRHXController* rhxController;

};

#endif // CONTROLLER_H
