#pragma once

#include <QCoreApplication>
#include <QObject>
#include <QTimer>
#include <QVector>
#include <deque>
#include <QDebug>

#include "okFrontPanel.h"
#include "rhxcontroller.h"
#include "rhxregisters.h"
#include "rhxdatablock.h"
#include "controller.h"

// 后面我们先只用 stream 0，如果你打开多个 stream，再扩展即可


class AcquisitionEngine : public QObject
{
    Q_OBJECT
public:
    explicit AcquisitionEngine(QObject *parent = nullptr);
    ~AcquisitionEngine();

    // 打开并初始化硬件（等价于你 main 里面前半部分）
    bool openDevice(const QString &bitfilePath);

    // 启用/关闭某个数据流（0~7）
    void enableStream(int stream, bool enabled);

    // 开始/停止连续采集（SPI 连续跑，USB 定时读取）
    void startContinuousAcquisition();
    void stopAcquisition();

    // 简单的刺激配置接口：把电极参数交给底层 Controller
    // 这里直接用你已有的 Controller / ElectrodeParameters 风格
    void configureStim(const QString &electrodeName,
                       int firstPhaseAmplitude,
                       int secondPhaseAmplitude,
                       int firstPhaseDuration_us,
                       int secondPhaseDuration_us,
                       int interPhaseDelay_us,
                       int numPulses,
                       int triggerSource);

    // 触发某个 triggerSource 的刺激
    void triggerStim(int triggerSource, bool on);

    // ⭐ 新增：根据算法结果构造刺激并触发
    void applyAdaptiveStim(const QString &electrodeName,
                           int amplitude_uA,
                           int numPulses,
                           int triggerSource);

    // 暴露底层指针，方便以后复杂操作
    RHXController* rhx() const { return m_rhxController; }
    Controller* stimController() const { return m_stimController; }

signals:
    // 原来就有的：
    void newSamples(const QVector<uint32_t> &timeStamps,
                    const QVector<QVector<int>> &channelData);

    // ⭐ 新增：专门给 stream2 用的信号（第二路 data stream）
    void newSamplesStream2(const QVector<uint32_t> &timeStamps,
                           const QVector<QVector<int>> &channelData);


    // 状态/错误信息
    void errorOccurred(const QString &msg);
    void logMessage(const QString &msg);

private slots:
    void onUsbTimer();

private:
    void cleanup();
    void processDataQueue();
    void pauseContinuousForStim();      // 只暂停连续采集（给刺激用）
    void resumeContinuousAfterStim();   // 刺激后恢复连续采集

private:
    RHXController   *m_rhxController   = nullptr;
    Controller      *m_stimController  = nullptr;

    QTimer           m_usbTimer;
    std::deque<RHXDataBlock*> m_dataQueue;

    bool m_continuousRunning = false;   // 当前是否处于连续采集模式
    bool m_deviceOpened = false;
    int  m_numEnabledStreams = 0;
    int  m_channelsPerStream = 16; // 对 RHS，官方文档就是 16
};
