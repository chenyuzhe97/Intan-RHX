#pragma once
#include <QObject>
#include <QFile>
#include <QTextStream>

class StimLogWriter : public QObject
{
    Q_OBJECT
public:
    explicit StimLogWriter(QObject *parent=nullptr);

    bool start(const QString &csvPath);
    void stop();

    void logPlanned(int epochId, int phaseIndex, int itemIndex,
                    double offsetMs, const QString &electrode,
                    int amp_uA, int pulses, int ch, double spike_uV);

    void logFired(int epochId, int phaseIndex, int itemIndex,
                  const QString &electrode, int amp_uA, int pulses);

private:
    QFile m_file;
    QTextStream m_ts;
    bool m_headerWritten = false;
};
