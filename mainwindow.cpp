// mainwindow.cpp
#include "mainwindow.h"

#include <QFileDialog>
#include <QDateTime>
#include <QDir>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();

    // 核心对象
    m_engine     = new AcquisitionEngine(this);
    m_experiment = new ExperimentControllerAB(m_engine, this);
    m_abAlgo     = new ABAlgorithm(this);

    if (m_abAlgo) {
        m_abAlgo->setSampleRateHz(m_sampleRate);
    }

    // 高速波形显示：直接把数据喂给 widget（不要再走 QtCharts）
    connect(m_engine, &AcquisitionEngine::newSamples,
            m_viewA,   &StackedWaveWidget::pushBlock);
    connect(m_engine, &AcquisitionEngine::newSamplesStream2,
            m_viewB,   &StackedWaveWidget::pushBlock);

    connect(m_engine, &AcquisitionEngine::errorOccurred,
            this,     &MainWindow::handleError);
    connect(m_engine, &AcquisitionEngine::logMessage,
            this,     &MainWindow::handleLog);

    connect(m_experiment, &ExperimentControllerAB::logMessage,
            this,         &MainWindow::handleLog);

    // AB epoch 数据回调（用于计划刺激 + timeline + csv）
    connect(m_experiment, &ExperimentControllerAB::epochReady,
            this,         &MainWindow::onABEpochReady);

    // timeline + log writer
    m_timeline = new StimTimelineOverlay();
    m_timeline->show();

    m_stimLog = new StimLogWriter(this);
    // 默认先写到当前目录（开始录制时会切换到 recording.xxx.stim.csv）
    m_stimLog->start(QDir::currentPath() + "/stim_log.csv");

    // 初次定位 timeline 到窗口右侧
    QTimer::singleShot(0, this, [this](){ ensureTimelineVisible(); });
}

MainWindow::~MainWindow()
{
    if (m_experiment) m_experiment->stop();
    if (m_engine)     m_engine->stopAcquisition();
    if (m_stimLog)    m_stimLog->stop();
    if (m_timeline)   m_timeline->close();
}

void MainWindow::setupUi()
{
    m_central = new QWidget(this);
    m_layout  = new QVBoxLayout(m_central);

    // ===== 顶部按钮行 =====
    {
        QHBoxLayout *row = new QHBoxLayout();

        m_btnOpen     = new QPushButton(tr("打开设备"), this);
        m_btnStart    = new QPushButton(tr("开始采集"), this);
        m_btnStop     = new QPushButton(tr("停止采集"), this);
        m_btnStimOnce = new QPushButton(tr("发一次刺激 (trigger=0)"), this);
        m_btnRecStart = new QPushButton(tr("开始录制(bin)"), this);
        m_btnRecStop  = new QPushButton(tr("停止录制"), this);

        row->addWidget(m_btnOpen);
        row->addWidget(m_btnStart);
        row->addWidget(m_btnStop);
        row->addWidget(m_btnStimOnce);
        row->addWidget(m_btnRecStart);
        row->addWidget(m_btnRecStop);

        row->addSpacing(16);

        row->addWidget(new QLabel(tr("Epoch:"), this));
        m_spinEpochSec = new QDoubleSpinBox(this);
        m_spinEpochSec->setRange(0.1, 3600.0);
        m_spinEpochSec->setDecimals(2);
        m_spinEpochSec->setSingleStep(0.5);
        m_spinEpochSec->setSuffix(" s");
        m_spinEpochSec->setValue(colletion_time);
        row->addWidget(m_spinEpochSec);

        row->addStretch(1);
        m_layout->addLayout(row);
    }

    // ===== 显示范围控制行（不做复杂滤波，保持显示快）=====
    {
        QHBoxLayout *row = new QHBoxLayout();

        row->addWidget(new QLabel(tr("Stream0 显示范围(±uV):"), this));
        m_spinGainA = new QDoubleSpinBox(this);
        m_spinGainA->setRange(10.0, 200000.0);
        m_spinGainA->setDecimals(0);
        m_spinGainA->setSingleStep(100.0);
        m_spinGainA->setValue(500.0);
        m_spinGainA->setSuffix(" µV");
        row->addWidget(m_spinGainA);

        row->addSpacing(20);

        row->addWidget(new QLabel(tr("Stream2 显示范围(±uV):"), this));
        m_spinGainB = new QDoubleSpinBox(this);
        m_spinGainB->setRange(10.0, 200000.0);
        m_spinGainB->setDecimals(0);
        m_spinGainB->setSingleStep(100.0);
        m_spinGainB->setValue(500.0);
        m_spinGainB->setSuffix(" µV");
        row->addWidget(m_spinGainB);

        row->addStretch(1);
        m_layout->addLayout(row);
    }

    // ===== 左右分屏：Stream0 / Stream2 多通道显示 =====
    m_split = new QSplitter(Qt::Horizontal, this);

    m_viewA = new StackedWaveWidget(this);
    m_viewB = new StackedWaveWidget(this);

    m_viewA->configure(m_channelsPerStream, m_sampleRate, m_visibleWindowSec);
    m_viewB->configure(m_channelsPerStream, m_sampleRate, m_visibleWindowSec);

    m_viewA->setTitle("Stream0 (A) - 16ch");
    m_viewB->setTitle("Stream2 (B) - 16ch");

    m_viewA->setGainUv(m_spinGainA->value());
    m_viewB->setGainUv(m_spinGainB->value());

    m_split->addWidget(m_viewA);
    m_split->addWidget(m_viewB);
    m_split->setStretchFactor(0, 1);
    m_split->setStretchFactor(1, 1);

    m_layout->addWidget(m_split, 3);

    // ===== 底部日志 =====
    m_logView = new QPlainTextEdit(this);
    m_logView->setReadOnly(true);
    m_layout->addWidget(m_logView, 1);

    setCentralWidget(m_central);

    // ===== 信号槽 =====
    connect(m_btnOpen,     &QPushButton::clicked, this, &MainWindow::onOpenDevice);
    connect(m_btnStart,    &QPushButton::clicked, this, &MainWindow::onStart);
    connect(m_btnStop,     &QPushButton::clicked, this, &MainWindow::onStop);
    connect(m_btnStimOnce, &QPushButton::clicked, this, &MainWindow::onStimOnce);
    connect(m_btnRecStart, &QPushButton::clicked, this, &MainWindow::onRecStart);
    connect(m_btnRecStop,  &QPushButton::clicked, this, &MainWindow::onRecStop);

    connect(m_spinEpochSec,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this,
            &MainWindow::onEpochDurationChanged);

    connect(m_spinGainA,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this,
            &MainWindow::onGainAChanged);

    connect(m_spinGainB,
            QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this,
            &MainWindow::onGainBChanged);
}

void MainWindow::appendLog(const QString &msg)
{
    const QString line = QDateTime::currentDateTime()
    .toString("hh:mm:ss.zzz  ") + msg;
    m_logView->appendPlainText(line);
}

void MainWindow::ensureTimelineVisible()
{
    if (!m_timeline) return;
    // 放到主窗口右侧一点
    const QRect g = this->geometry();
    m_timeline->move(g.topRight() + QPoint(20, 40));
    m_timeline->raise();
}

void MainWindow::onOpenDevice()
{
    QString path = QFileDialog::getOpenFileName(
        this,
        tr("选择 ConfigRHSController_7310.bit"),
        QString(),
        tr("Bitfile (*.bit);;All Files (*.*)"));

    if (path.isEmpty()) return;

    if (!m_engine->openDevice(path)) {
        appendLog("打开设备失败");
    } else {
        appendLog("打开设备成功");
    }
}

void MainWindow::onStart()
{
    if (!m_engine) return;

    // 1) 启动连续采集
    m_engine->startContinuousAcquisition();

    // 2) 启动 AB epoch 控制器
    if (m_experiment) {
        m_experiment->setEpochDuration(colletion_time);
        m_experiment->start();
    }

    ensureTimelineVisible();

    appendLog(QString("开始采集 + AB epoch=%1 s").arg(colletion_time, 0, 'f', 2));
}

void MainWindow::onStop()
{
    if (m_experiment) m_experiment->stop();
    if (m_engine)     m_engine->stopAcquisition();
    appendLog("已停止采集");
}

void MainWindow::onRecStart()
{
    if (!m_engine) return;

    QString file = QFileDialog::getSaveFileName(
        this,
        tr("选择录制文件保存路径"),
        QDir::currentPath() + "/recording.bar",
        tr("Binary Recording (*.bar);;All Files (*.*)"));

    if (file.isEmpty()) return;

    if (m_engine->startBinaryRecording(file)) {
        appendLog("开始录制到文件：" + file);

        // 刺激日志写到同名 csv
        if (m_stimLog) {
            m_stimLog->start(file + ".stim.csv");
            appendLog("Stim log 写入：" + file + ".stim.csv");
        }
    } else {
        appendLog("开始录制失败");
    }
}

void MainWindow::onRecStop()
{
    if (!m_engine) return;
    m_engine->stopBinaryRecording();
    appendLog("已停止录制");

    if (m_stimLog) {
        m_stimLog->stop();
        // 也可以选择继续写到默认 stim_log.csv，这里按“录制结束就停止写文件”
        m_stimLog->start(QDir::currentPath() + "/stim_log.csv");
        appendLog("Stim log 切回默认：stim_log.csv");
    }
}

void MainWindow::onStimOnce()
{
    if (!m_engine) return;
    int triggerSource = 0;
    m_engine->triggerStim(triggerSource, true);
    appendLog("已触发刺激：trigger=0（使用当前已配置波形）");
}

void MainWindow::onEpochDurationChanged(double sec)
{
    colletion_time = sec;

    if (m_experiment) {
        m_experiment->setEpochDuration(colletion_time);
    }

    appendLog(QString("Epoch 时长设置为 %1 s").arg(colletion_time, 0, 'f', 2));
}

void MainWindow::onGainAChanged(double halfRangeUv)
{
    if (m_viewA) m_viewA->setGainUv(halfRangeUv);
}

void MainWindow::onGainBChanged(double halfRangeUv)
{
    if (m_viewB) m_viewB->setGainUv(halfRangeUv);
}

void MainWindow::onABEpochReady(int phaseIndex,
                                const QVector<uint32_t> &timeStamps,
                                const QVector<QVector<int>> &channelData)
{
    const QString phaseName = (phaseIndex == 0)
    ? "PhaseA (A端 stream0)"
    : "PhaseB (B端 stream2)";

    if (!m_abAlgo || !m_engine) {
        appendLog(phaseName + ": m_abAlgo 或 m_engine 为空，跳过");
        return;
    }
    if (timeStamps.isEmpty() || channelData.isEmpty()) {
        appendLog(phaseName + ": epoch 数据为空，跳过");
        return;
    }

    // 1) 算法分析
    const QVector<ABAlgorithm::Result> allResults =
        m_abAlgo->analyzeEpoch(phaseIndex, timeStamps, channelData);

    if (allResults.isEmpty()) {
        appendLog(phaseName + ": 本 epoch 未检测到尖峰事件");
        return;
    }

    // 2) phase 决定刺激目标与 trigger
    QString targetElectrode;
    int triggerSource = 0;

    if (phaseIndex == 0) {
        targetElectrode = "B1";   // A 看 → 刺激 B
        triggerSource   = 1;
    } else {
        targetElectrode = "A1";   // B 看 → 刺激 A
        triggerSource   = 0;
    }

    const uint32_t epochStartTs = timeStamps.first();
    const double fs = (m_sampleRate > 0.0 ? m_sampleRate : 30000.0);

    // 3) 筛候选（needStim & amp>0）
    QVector<ABAlgorithm::Result> candidates;
    candidates.reserve(allResults.size());
    for (const auto &r : allResults) {
        if (!r.needStim) continue;
        if (r.suggestedAmplitude_uA <= 0) continue;
        candidates.push_back(r);
    }

    if (candidates.isEmpty()) {
        appendLog(phaseName + ": 检测到事件，但全部判定不需刺激");
        return;
    }

    // 4) 取“强度最大 10 个”
    std::sort(candidates.begin(), candidates.end(),
              [](const ABAlgorithm::Result &a, const ABAlgorithm::Result &b) {
                  return a.spikeAmplitude_uV > b.spikeAmplitude_uV;
              });

    const int maxStimPerEpoch = 10;
    if (candidates.size() > maxStimPerEpoch) candidates.resize(maxStimPerEpoch);

    // 5) 再按时间顺序
    std::sort(candidates.begin(), candidates.end(),
              [](const ABAlgorithm::Result &a, const ABAlgorithm::Result &b) {
                  return a.triggerTime < b.triggerTime;
              });

    const int epochId = ++m_epochCounter;

    appendLog(QString("%1: 检测=%2，候选=%3，计划刺激=%4（epochId=%5）")
                  .arg(phaseName)
                  .arg(allResults.size())
                  .arg(candidates.size())
                  .arg(candidates.size())
                  .arg(epochId));

    // 6) 更新 timeline 计划 + 写 planned 日志 + 调度 singleShot
    QVector<StimTimelineOverlay::Item> items;
    items.reserve(candidates.size());

    for (int i = 0; i < candidates.size(); ++i) {
        const auto r = candidates[i]; // 复制一份，避免 lambda 引用悬空

        int numPulses = (r.suggestedNumPulses > 0) ? r.suggestedNumPulses : 1;

        double offsetSec = 0.0;
        if (r.triggerTime >= epochStartTs) {
            offsetSec = double(r.triggerTime - epochStartTs) / fs;
        }
        const double offsetMsD = offsetSec * 1000.0;
        int delayMs = int(offsetMsD);
        if (delayMs < 0) delayMs = 0;

        StimTimelineOverlay::Item it;
        it.itemIndex = i;
        it.offsetMs = offsetMsD;
        it.amp_uA = r.suggestedAmplitude_uA;
        it.pulses = numPulses;
        it.ch = r.channelIndex;
        it.spike_uV = r.spikeAmplitude_uV;
        it.electrode = targetElectrode;
        it.fired = false;
        items.push_back(it);

        if (m_stimLog) {
            m_stimLog->logPlanned(epochId, phaseIndex, i,
                                  offsetMsD,
                                  targetElectrode,
                                  it.amp_uA, it.pulses,
                                  it.ch, it.spike_uV);
        }

        // 安排在本 epoch 内相对时刻触发
        QTimer::singleShot(delayMs, this,
                           [this, epochId, phaseIndex, itemIdx=i,
                            targetElectrode, triggerSource,
                            amp=it.amp_uA, pulses=it.pulses]() {

                               if (m_timeline) m_timeline->markFired(epochId, itemIdx);
                               if (m_stimLog)  m_stimLog->logFired(epochId, phaseIndex, itemIdx,
                                                       targetElectrode, amp, pulses);

                               if (!m_engine) return;
                               m_engine->applyAdaptiveStim(targetElectrode, amp, pulses, triggerSource);
                           });
    }

    if (m_timeline) {
        m_timeline->setEpochPlan(epochId, phaseIndex, colletion_time, items);
        ensureTimelineVisible();
    }
}

void MainWindow::handleError(const QString &msg)
{
    appendLog(QStringLiteral("错误：") + msg);
}

void MainWindow::handleLog(const QString &msg)
{
    appendLog(msg);
}
