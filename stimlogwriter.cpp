#include "stimlogwriter.h"
#include <QDateTime>

StimLogWriter::StimLogWriter(QObject *parent) : QObject(parent) {}

bool StimLogWriter::start(const QString &csvPath)
{
    stop();
    m_file.setFileName(csvPath);
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return false;
    }
    m_ts.setDevice(&m_file);

    if (m_file.size() == 0) {
        m_ts << "time_iso,type,epochId,phase,item,offset_ms,electrode,amp_uA,pulses,ch,spike_uV\n";
        m_ts.flush();
        m_headerWritten = true;
    }
    return true;
}

void StimLogWriter::stop()
{
    if (m_file.isOpen()) m_file.close();
}

void StimLogWriter::logPlanned(int epochId, int phaseIndex, int itemIndex,
                               double offsetMs, const QString &electrode,
                               int amp_uA, int pulses, int ch, double spike_uV)
{
    if (!m_file.isOpen()) return;
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    m_ts << now << ",planned," << epochId << "," << phaseIndex << "," << itemIndex
         << "," << offsetMs << "," << electrode << "," << amp_uA << "," << pulses
         << "," << ch << "," << spike_uV << "\n";
    m_ts.flush();
}

void StimLogWriter::logFired(int epochId, int phaseIndex, int itemIndex,
                             const QString &electrode, int amp_uA, int pulses)
{
    if (!m_file.isOpen()) return;
    const QString now = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    m_ts << now << ",fired," << epochId << "," << phaseIndex << "," << itemIndex
         << ",," << electrode << "," << amp_uA << "," << pulses
         << ",,\n";
    m_ts.flush();
}
