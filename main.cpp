//------------------------------------------------------------------------------
//
//  Intan Technologies RHX Data Acquisition Software
//  Version 3.3.1
//
//  Copyright (c) 2020-2023 Intan Technologies
//
//  This file is part of the Intan Technologies RHX Data Acquisition Software.
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published
//  by the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//  This software is provided 'as-is', without any express or implied warranty.
//  In no event will the authors be held liable for any damages arising from
//  the use of this software.
//
//  See <http://www.intantech.com> for documentation and product information.
//
//------------------------------------------------------------------------------

// #include <QApplication>
// #include "boardselectdialog.h"

// int main(int argc, char *argv[])
// {
//     QApplication app(argc, argv);

// #ifdef __APPLE__
//     app.setStyle(QStyleFactory::create("Fusion"));
// #endif

//     BoardSelectDialog boardSelectDialog;
//     return app.exec();
// }

// #include <iostream>
// #include <fstream>
// #include <vector>

// using namespace std;

// #include "okFrontPanel.h"
// #include "rhxcontroller.h"
// #include "rhxregisters.h"
// #include "rhxdatablock.h"
// #include <QDebug>


// int main(int argc,char* argv[])
// {
//     // 使用 20 kHz 的每放大器采样率创建 RHX 控制器。
//     RHXController *rhxController = new RHXController(ControllerStimRecord,
//                                                      SampleRate20000Hz);
//     // 1. 基础构造与状态
//     // 如果此控制器是“合成的”（即用于演示目的，没有连接真实硬件），则返回 true；如果是真实的 XEM7310 板，则返回 false
//     qDebug() <<"控制器状态(0代表真实,1代表虚拟)："<<rhxController->isSynthetic();
//     // 如果此控制器用于数据回放（不需要新的数据采集），则返回 true
//     qDebug() <<"数据回放状态："<<rhxController->isPlayback();

//     // 返回当前控制器的运行模式（枚举值：LiveMode, SyntheticMode, PlaybackMode)
//     qDebug() << "当前数据的运行模式为："<<rhxController->acquisitionMode();
//     qDebug() << "当前刺激模式为：" << rhxController->getType();
//     qDebug() << "当前采样率为：" << rhxController->getSampleRate();


//     // 2. 设备连接与初始化
//     // 列出当前连接到计算机的所有 Opal Kelly 设备的序列号
//     vector<string> availableDevices = rhxController->listAvailableDeviceSerials();
//     qDebug() << "设备数量：" << availableDevices.size();
//     qDebug() << "设备名称为：";
//     for(const auto &deviceSerial : availableDevices)
//         qDebug() << QString::fromStdString(deviceSerial);

//     // int open(): 打开找到的第一个 Opal Kelly 板,也可以传入设备名称
//     if (rhxController->open(availableDevices[0]))
//         qDebug() <<"设备"<<QString::fromStdString(availableDevices[0])<<"已成功打开";
//     else
//     {
//         qDebug() <<"设备打开失败";
//         return 0;
//     }

//     // 将 RhythmStim USB-7310 配置文件（bitfile）上传到 FPGA。成功返回 true
//     if(rhxController->uploadFPGABitfile("ConfigRHSController_7310.bit"))
//         qDebug()<< "FPGA上传成功";
//     else
//     {
//         qDebug()<< "FPGA上传失败";
//         return 0;
//     }

//     // 将 FPGA 寄存器初始化为默认值
//     rhxController->initialize();
//     qDebug() << "当前刺激模式为：" << rhxController->getType();

//     // 重置 FPGA。这会清除所有辅助命令 RAM 库、清除 USB FIFO，并将每通道采样率重置为默认值 30.0 kS/s 。
//     // rhxController->resetBoard();

//     // 执行 FPGA 的低级重置（通常在关闭应用程序时调用）
//     // rhxController->resetFpga();

//     // 3. 采样率控制
//     // 设置 RHS 芯片的每通道采样率。如果请求了不支持的采样率，则返回 false。支持的采样率包括 20kHz, 25kHz, 30kHz 等 。
//     // rhxController->setSampleRate(AmplifierSampleRate newSampleRate);

//     // 返回当前的每通道采样率（浮点数，单位 Hz）
//     qDebug()<<"当前设备采样率为："<< rhxController->getSampleRate();

//     // 返回与期望速率最接近的受支持采样率枚举 。
//     // static AmplifierSampleRate nearestSampleRate(double rate, double percentTolerance= 1.0)

//     // 4. 刺激参数辅助函数
//     // 返回与期望步长最接近的受支持刺激步长枚举
//     // static AmplifierSampleRate nearestStimStepSize(double step, double percentTolerance= 1.0)



//     return 0;
// }




// #include <iostream>
// #include <fstream>
// #include <vector>
// #include <QDebug>

// using namespace std;

// #include "okFrontPanel.h"
// #include "rhxcontroller.h"
// #include "rhxregisters.h"
// #include "rhxdatablock.h"
// #include "controller.h"


// void example(){

// }

// int main(int argc,char* argv[])
// {
//     // 使用 20 kHz 的每放大器采样率创建 RHX 控制器。
//     RHXController *rhxController = new RHXController(ControllerStimRecord,
//                                                      SampleRate20000Hz);

//     // 打开第一个检测到的 Opal Kelly 设备，加载 RhythmStim USB-7310 位文件。
//     vector<string> availableDevices = rhxController->listAvailableDeviceSerials();
//     rhxController->open(availableDevices[0]);

//     // 加载 RhythmStim USB-7310 位文件并初始化
//     rhxController->uploadFPGABitfile("ConfigRHSController_7310.bit");
//     rhxController->initialize();
//     rhxController->enableDataStream(0, true);

//     // 我们可以设置 MISO 采样延迟，它取决于采样率。
//     // 我们假设使用了 3 英尺（约 0.91 米）的电缆。
//     rhxController->setCableLengthFeet(PortA, 3.0);

//     // 让我们点亮一个 LED，以指示程序正在运行。
//     int ledArray[8] = {1, 0, 0, 0, 0, 0, 0, 0};
//     rhxController->setLedDisplay(ledArray);

//     Controller *controller = new Controller(rhxController);
//     // 配置电极编号用于测试
//     ElectrodeParameters *ele = new ElectrodeParameters("A1");

//     // 配置各种波形参数
//     ele->SetStimulationTiming(0,500,500,500);
//     ele->SetStimulationAmplitude(100,100);
//     ele->SetStimulationSource(0);

//     // 载入电极参数
//     controller->setStimSequenceParameters(ele);
//     rhxController->setStimCmdMode(true);
//     rhxController->setContinuousRunMode(true);
//     rhxController->run();
//     // 触发源为0的电极刺激
//     controller->stimTrigger(0,true);


//     // 从 USB 接口读取结果性的单个数据块。
//     RHXDataBlock *dataBlock =
//         new RHXDataBlock(rhxController->getType(),
//                          rhxController->getNumEnabledDataStreams());
//     rhxController->readDataBlock(dataBlock);
//     qDebug()<<"NumEnabledDataStreams"<<rhxController->getNumEnabledDataStreams();
//     // 显示来自数据流 0 的寄存器内容。
//     // dataBlock->print(0);

//     // 让我们将一秒钟的数据保存到磁盘上的一个二进制文件中。
//     ofstream saveOut;
//     saveOut.open("binary_save_file.dat", ios::binary | ios::out);

//     deque<RHXDataBlock*> dataQueue;
//     // 运行一秒钟。
//     bool usbDataRead;
//     do  {
//         controller->stimTrigger(0,true);
//         usbDataRead = rhxController->readDataBlocks(1, dataQueue);
//         if (dataQueue.size() >= 50) {  // 一次保存 50 个数据块
//             qDebug()<<"写入中";
//             rhxController->queueToFile(dataQueue, saveOut);
//         }
//     } while (usbDataRead || rhxController->isRunning());

//     rhxController->queueToFile(dataQueue, saveOut);

//     saveOut.close();

//     return 0;
// }

#include <QApplication>
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    MainWindow w;
    w.show();

    return a.exec();
}
