#pragma once

#include <QCoreApplication>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <QVector>
#include <deque>
#include <fstream>

#include "okFrontPanel.h"
#include "rhxcontroller.h"
#include "rhxregisters.h"
#include "rhxdatablock.h"
#include "controller.h"

class AcquisitionEngine : public QObject
{
    Q_OBJECT
public:
    explicit AcquisitionEngine(QObject *parent = nullptr);

    ~AcquisitionEngine()
    {
        stopBinaryRecording();
        stopAcquisition();
        cleanup();
    }

    bool openDevice(const QString &bitfilePath);
    void enableStream(int stream, bool enabled);

    void startContinuousAcquisition();
    void stopAcquisition();

    void configureStim(const QString &electrodeName,
                       int firstPhaseAmplitude,
                       int secondPhaseAmplitude,
                       int firstPhaseDuration_us,
                       int secondPhaseDuration_us,
                       int interPhaseDelay_us,
                       int numPulses,
                       int triggerSource);

    void triggerStim(int triggerSource, bool on);
    void pulseStim(int triggerSource);

    void applyAdaptiveStim(const QString &electrodeName,
                           int amplitude_uA,
                           int numPulses,
                           int triggerSource);

    RHXController* rhx() const { return m_rhxController; }
    Controller* stimController() const { return m_stimController; }

    bool startBinaryRecording(const QString &filePath);
    void stopBinaryRecording();

signals:
    void newSamples(const QVector<uint32_t> &timeStamps,
                    const QVector<QVector<int>> &channelData);

    void newSamplesStream2(const QVector<uint32_t> &timeStamps,
                           const QVector<QVector<int>> &channelData);

    void errorOccurred(const QString &msg);
    void logMessage(const QString &msg);

private slots:
    void onUsbTimer();

private:
    void cleanup();
    void processDataQueue();
    void pauseContinuousForStim();
    void resumeContinuousAfterStim();
    void writeBlockToRecording(RHXDataBlock *block);
    void writeBlockStream();

private:
    RHXController   *m_rhxController   = nullptr;
    Controller      *m_stimController  = nullptr;

    std::ofstream   m_recordStream;
    bool            m_isRecording = false;

    QTimer                     m_usbTimer;
    std::deque<RHXDataBlock*>  m_dataQueue;

    bool m_continuousRunning = false;
    bool m_deviceOpened      = false;
    int  m_numEnabledStreams = 0;
    int  m_channelsPerStream = 16;
};
