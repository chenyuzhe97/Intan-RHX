#include "experimentcontrollerab.h"
#include "acquisitionengine.h"

ExperimentControllerAB::ExperimentControllerAB(AcquisitionEngine *engine,
                                               QObject *parent)
    : QObject(parent),
    m_engine(engine)
{
    connect(&m_epochTimer, &QTimer::timeout,
            this, &ExperimentControllerAB::onEpochTimeout);

    // ⭐ A 端数据：来自 AcquisitionEngine::newSamples（stream0）
    connect(m_engine, &AcquisitionEngine::newSamples,
            this,      &ExperimentControllerAB::onNewSamplesStream0);

    // ⭐ B 端数据：来自 AcquisitionEngine::newSamplesStream2（stream2）
    connect(m_engine, &AcquisitionEngine::newSamplesStream2,
            this,      &ExperimentControllerAB::onNewSamplesStream2);
}

void ExperimentControllerAB::start()
{
    if (m_running) return;

    m_running = true;
    m_phase   = PhaseA;  // 从 A 端开始

    // 清空 A/B 缓存
    m_tsBufferA.clear();
    m_chBuffersA.clear();
    m_numChannelsA = 0;

    m_tsBufferB.clear();
    m_chBuffersB.clear();
    m_numChannelsB = 0;

    m_epochTimer.start(int(m_epochDurationSec * 1000.0));

    emit logMessage(
        QStringLiteral("AB epoch 采集已启动：先从 A 端 (stream0) 开始，每个 epoch = %1 s")
            .arg(m_epochDurationSec, 0, 'f', 2));
}

void ExperimentControllerAB::stop()
{
    if (!m_running) return;

    m_running = false;
    m_epochTimer.stop();

    m_tsBufferA.clear();
    m_chBuffersA.clear();
    m_numChannelsA = 0;

    m_tsBufferB.clear();
    m_chBuffersB.clear();
    m_numChannelsB = 0;

    emit logMessage("AB epoch 采集已停止");
}

void ExperimentControllerAB::setEpochDuration(double seconds)
{
    m_epochDurationSec = seconds;
    if (m_running) {
        m_epochTimer.start(int(m_epochDurationSec * 1000.0));
    }
}

// ================= A 端数据（stream0） =================

void ExperimentControllerAB::onNewSamplesStream0(
    const QVector<uint32_t> &timeStamps,
    const QVector<QVector<int>> &channelData)
{
    if (!m_running) return;
    if (m_phase != PhaseA) {
        // 当前不是 A 阶段，可以直接忽略 A 端数据，或者也可以缓存备用
        return;
    }

    if (timeStamps.isEmpty() || channelData.isEmpty()) return;

    int numCh = channelData.size();
    int N     = timeStamps.size();
    if (N <= 0) return;

    // 第一次收到 A 端数据时，初始化通道缓存
    if (m_numChannelsA == 0) {
        m_numChannelsA = numCh;
        m_chBuffersA.resize(m_numChannelsA);
    } else if (numCh != m_numChannelsA) {
        emit logMessage("警告：A 端数据块通道数变化，忽略本块");
        return;
    }

    // 追加时间戳
    m_tsBufferA.reserve(m_tsBufferA.size() + N);
    for (int i = 0; i < N; ++i) {
        m_tsBufferA.append(timeStamps[i]);
    }

    // 追加各通道数据
    for (int ch = 0; ch < m_numChannelsA; ++ch) {
        const QVector<int> &src = channelData[ch];
        int Nc = qMin(N, src.size());
        if (Nc <= 0) continue;

        QVector<int> &dst = m_chBuffersA[ch];
        dst.reserve(dst.size() + Nc);
        for (int i = 0; i < Nc; ++i) {
            dst.append(src[i]);
        }
    }
}

// ================= B 端数据（stream2） =================

void ExperimentControllerAB::onNewSamplesStream2(
    const QVector<uint32_t> &timeStamps,
    const QVector<QVector<int>> &channelData)
{
    if (!m_running) return;
    if (m_phase != PhaseB) {
        // 当前不是 B 阶段，可以忽略 B 端数据
        return;
    }

    if (timeStamps.isEmpty() || channelData.isEmpty()) return;

    int numCh = channelData.size();
    int N     = timeStamps.size();
    if (N <= 0) return;

    // 第一次收到 B 端数据时，初始化通道缓存
    if (m_numChannelsB == 0) {
        m_numChannelsB = numCh;
        m_chBuffersB.resize(m_numChannelsB);
    } else if (numCh != m_numChannelsB) {
        emit logMessage("警告：B 端数据块通道数变化，忽略本块");
        return;
    }

    // 追加时间戳
    m_tsBufferB.reserve(m_tsBufferB.size() + N);
    for (int i = 0; i < N; ++i) {
        m_tsBufferB.append(timeStamps[i]);
    }

    // 追加各通道数据
    for (int ch = 0; ch < m_numChannelsB; ++ch) {
        const QVector<int> &src = channelData[ch];
        int Nc = qMin(N, src.size());
        if (Nc <= 0) continue;

        QVector<int> &dst = m_chBuffersB[ch];
        dst.reserve(dst.size() + Nc);
        for (int i = 0; i < Nc; ++i) {
            dst.append(src[i]);
        }
    }
}

// ================= epoch 到点：发数据 + 切换 Phase =================

void ExperimentControllerAB::onEpochTimeout()
{
    if (!m_running) return;

    if (m_phase == PhaseA) {
        if (!m_tsBufferA.isEmpty() && !m_chBuffersA.isEmpty()) {
            emit logMessage(QStringLiteral("PhaseA 结束：A 端 epoch 就绪，样本数 = %1")
                                .arg(m_tsBufferA.size()));

            // ⭐ 把 A 端这 5s 的全部数据丢给外部
            emit epochReady(0, m_tsBufferA, m_chBuffersA);
        } else {
            emit logMessage("PhaseA 结束：A 端本 epoch 无数据");
        }

        // 清空，为下一次 A-phase 准备
        m_tsBufferA.clear();
        for (auto &buf : m_chBuffersA) buf.clear();

        // 切到 B-phase
        m_phase = PhaseB;
        emit logMessage("切换到 PhaseB（B 端 stream2）");

    } else { // PhaseB
        if (!m_tsBufferB.isEmpty() && !m_chBuffersB.isEmpty()) {
            emit logMessage(QStringLiteral("PhaseB 结束：B 端 epoch 就绪，样本数 = %1")
                                .arg(m_tsBufferB.size()));

            // ⭐ 把 B 端这 5s 的全部数据丢给外部
            emit epochReady(1, m_tsBufferB, m_chBuffersB);
        } else {
            emit logMessage("PhaseB 结束：B 端本 epoch 无数据");
        }

        // 清空，为下一次 B-phase 准备
        m_tsBufferB.clear();
        for (auto &buf : m_chBuffersB) buf.clear();

        // 切回 A-phase
        m_phase = PhaseA;
        emit logMessage("切换到 PhaseA（A 端 stream0）");
    }
}
