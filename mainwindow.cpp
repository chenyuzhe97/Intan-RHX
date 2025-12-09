#include "mainwindow.h"
#include <QFileDialog>
#include <QDateTime>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();
    setupSinglePlot();
    setupStream2Plot();

    m_engine     = new AcquisitionEngine(this);
    m_experiment = new ExperimentControllerAB(m_engine, this);

    connect(m_engine, &AcquisitionEngine::newSamples,
            this,      &MainWindow::handleNewSamples);
    connect(m_engine, &AcquisitionEngine::newSamplesStream2,
            this,      &MainWindow::handleNewSamplesStream2);
    connect(m_engine, &AcquisitionEngine::errorOccurred,
            this,      &MainWindow::handleError);
    connect(m_engine, &AcquisitionEngine::logMessage,
            this,      &MainWindow::handleLog);

    connect(m_experiment, &ExperimentControllerAB::logMessage,
            this,         &MainWindow::handleLog);

    // ⭐ 关键：把 AB 每个 epoch 的所有数据接到这里
    connect(m_experiment, &ExperimentControllerAB::epochReady,
            this,         &MainWindow::onABEpochReady);
}


MainWindow::~MainWindow()
{
}


void MainWindow::setupUi()
{
    m_central = new QWidget(this);
    m_layout  = new QVBoxLayout(m_central);

    // ===== 顶部按钮一行 =====
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    m_btnOpen  = new QPushButton(tr("打开设备"), this);
    m_btnStart = new QPushButton(tr("开始采集"), this);
    m_btnStop  = new QPushButton(tr("停止采集"), this);
    m_btnStim  = new QPushButton(tr("发一次刺激 (A1)"), this);

    buttonLayout->addWidget(m_btnOpen);
    buttonLayout->addWidget(m_btnStart);
    buttonLayout->addWidget(m_btnStop);
    buttonLayout->addWidget(m_btnStim);
    buttonLayout->addStretch(1);  // 右边空出来一点

    m_layout->addLayout(buttonLayout);

    // ===== Stream0 单通道：通道选择 + 图，占位 =====
    // 通道下拉框（stream0）
    QHBoxLayout *chLayout0 = new QHBoxLayout();
    QLabel *label0 = new QLabel(tr("Stream 0 通道："), this);
    m_comboChannel = new QComboBox(this);
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        m_comboChannel->addItem(QString("CH%1").arg(ch), ch);
    }
    chLayout0->addWidget(label0);
    chLayout0->addWidget(m_comboChannel);
    chLayout0->addStretch(1);
    m_layout->addLayout(chLayout0);

    // 图本身在 setupSinglePlot() 里插入

    // ===== Stream2 单通道：通道选择 + 图，占位 =====
    // 通道下拉框（stream2）
    QHBoxLayout *chLayout2 = new QHBoxLayout();
    QLabel *label2 = new QLabel(tr("Stream 2 通道："), this);
    m_comboStream2Ch = new QComboBox(this);
    for (int ch = 0; ch < 16; ++ch) {   // 一个 data stream 16 个通道
        m_comboStream2Ch->addItem(QString("CH%1").arg(ch), ch);
    }
    chLayout2->addWidget(label2);
    chLayout2->addWidget(m_comboStream2Ch);
    chLayout2->addStretch(1);
    m_layout->addLayout(chLayout2);

    // 图本身在 setupStream2Plot() 里插入

    // ===== 底部日志框 =====
    m_logView  = new QPlainTextEdit(this);
    m_logView->setReadOnly(true);
    m_layout->addWidget(m_logView, 1);

    setCentralWidget(m_central);

    // ===== 按钮信号槽 =====
    connect(m_btnOpen,  &QPushButton::clicked,
            this,       &MainWindow::onOpenDevice);
    connect(m_btnStart, &QPushButton::clicked,
            this,       &MainWindow::onStart);
    connect(m_btnStop,  &QPushButton::clicked,
            this,       &MainWindow::onStop);
    connect(m_btnStim,  &QPushButton::clicked,
            this,       &MainWindow::onStimOnce);

    // ===== 单通道（Stream0）通道选择 =====
    connect(m_comboChannel,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &MainWindow::onChannelChanged);

    // ===== Stream2 通道选择 =====
    connect(m_comboStream2Ch,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            [this](int index) {
                m_currentChStream2 = m_comboStream2Ch->itemData(index).toInt();
                appendLog(QString("切换 Stream2 显示到 CH%1").arg(m_currentChStream2));
                m_bufferStream2.clear();
                if (m_seriesStream2) m_seriesStream2->clear();
            });
}

void MainWindow::setupSinglePlot()
{
    m_series = new QLineSeries(this);
    m_chart  = new QChart();
    m_chart->addSeries(m_series);
    m_chart->legend()->hide();
    m_chart->setTitle(tr("Stream 0 单通道实时波形"));

    m_axisX = new QValueAxis(this);
    m_axisX->setTitleText("Time (s)");
    m_axisX->setRange(0.0, m_visibleWindowSec);

    m_axisY = new QValueAxis(this);
    m_axisY->setTitleText("Voltage (µV)");   // ⭐ 改成和 stream2 一致
    m_axisY->setRange(-100.0, 100.0);

    m_chart->addAxis(m_axisX, Qt::AlignBottom);
    m_chart->addAxis(m_axisY, Qt::AlignLeft);
    m_series->attachAxis(m_axisX);
    m_series->attachAxis(m_axisY);

    m_chartView = new QChartView(m_chart, this);
    m_chartView->setRenderHint(QPainter::Antialiasing);

    // 按钮行 + Stream0 通道选择布局之后，索引大概是 1 或 2；
    // 我们把单通道图插在“Stream0 通道选择”后面：
    int insertIndex = 2; // 0: 按钮行, 1: Stream0 通道布局, 2: 这里
    m_layout->insertWidget(insertIndex, m_chartView, 2);
}
void MainWindow::setupStream2Plot()
{
    m_chartStream2 = new QChart();
    m_chartStream2->legend()->hide();
    m_chartStream2->setTitle(tr("Stream 2 单通道实时波形"));

    m_axisX2 = new QValueAxis(this);
    m_axisX2->setTitleText("Time (s)");
    m_axisX2->setRange(0.0, m_stream2WindowSec);

    m_axisY2 = new QValueAxis(this);
    m_axisY2->setTitleText("Voltage (µV)");  // ⭐ 和上面统一
    m_axisY2->setRange(-200.0, 200.0);

    m_chartStream2->addAxis(m_axisX2, Qt::AlignBottom);
    m_chartStream2->addAxis(m_axisY2, Qt::AlignLeft);

    m_seriesStream2 = new QLineSeries(this);
    m_chartStream2->addSeries(m_seriesStream2);
    m_seriesStream2->attachAxis(m_axisX2);
    m_seriesStream2->attachAxis(m_axisY2);

    m_chartViewStream2 = new QChartView(m_chartStream2, this);
    m_chartViewStream2->setRenderHint(QPainter::Antialiasing);

    // 插在 “Stream2 通道选择布局” 后面
    // layout 顺序：0 按钮行，1 Stream0通道布局，2 Stream0图，3 Stream2通道布局，4 待插 Stream2图，5 日志
    int logIndex = m_layout->indexOf(m_logView);
    int insertIndex = (logIndex > 0) ? logIndex : m_layout->count();
    // 日志在最后一个，所以 Stream2 曲线插在日志前面
    m_layout->insertWidget(insertIndex, m_chartViewStream2, 2);
}


void MainWindow::setupMultiPlot()
{
    m_multiChart = new QChart();
    m_multiChart->legend()->hide();

    m_multiAxisX = new QValueAxis(this);
    m_multiAxisX->setTitleText("Time (s)");
    m_multiAxisX->setRange(0.0, m_multiWindowSec);

    m_multiAxisY = new QValueAxis(this);
    m_multiAxisY->setTitleText("Channel stack");
    // 先给一个大概的范围，后面会根据通道数自动调整
    m_multiAxisY->setRange(-200.0, NUM_CHANNELS * 400.0);

    m_multiChart->addAxis(m_multiAxisX, Qt::AlignBottom);
    m_multiChart->addAxis(m_multiAxisY, Qt::AlignLeft);

    // 准备多通道 series & buffer
    m_multiSeries.resize(NUM_CHANNELS);
    m_multiBuffers.resize(NUM_CHANNELS);

    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        auto *series = new QLineSeries(this);
        series->setName(QString("CH%1").arg(ch));
        m_multiChart->addSeries(series);
        series->attachAxis(m_multiAxisX);
        series->attachAxis(m_multiAxisY);
        m_multiSeries[ch] = series;
    }

    m_multiChartView = new QChartView(m_multiChart, this);
    m_multiChartView->setRenderHint(QPainter::Antialiasing);

    // 放在 log 之前
    // 当前 layout 顺序：按钮…（0,1,2）+ 单通道图(3) + 通道layout(4) + log(5)
    // 想要多通道图在 log 上面，可以 insert 在 log 前一位
    int logIndex = m_layout->indexOf(m_logView);
    if (logIndex < 0) logIndex = m_layout->count();
    m_layout->insertWidget(logIndex, m_multiChartView, 2);
}



void MainWindow::appendLog(const QString &msg)
{
    QString line = QDateTime::currentDateTime()
    .toString("hh:mm:ss.zzz  ") + msg;
    m_logView->appendPlainText(line);
}

void MainWindow::onOpenDevice()
{
    // 你可以写死 bitfile 路径，也可以弹框选择
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

    // 1）配置好你要的刺激波形（如果暂时只想先看数据，可以先注释掉）
    /*
    m_engine->configureStim("A1", 100, 100, 500, 500, 500, 1, 0); // trigger 0 给 A
    m_engine->configureStim("B1", 100, 100, 500, 500, 500, 1, 1); // trigger 1 给 B
    */

    // 2）启动连续采集
    m_engine->startContinuousAcquisition();

    // 3）启动 AB epoch 控制器（比如 5s）
    if (m_experiment) {
        m_experiment->setEpochDuration(5.0);
        m_experiment->start();
    }

    appendLog("开始采集 + AB 5s epoch 采集");
}

void MainWindow::onStop()
{
    if (m_experiment) {
        m_experiment->stop();
    }
    if (m_engine) {
        m_engine->stopAcquisition();
    }
    appendLog("已停止采集");
}

void MainWindow::onABEpochReady(int phaseIndex,
                                const QVector<uint32_t> &timeStamps,
                                const QVector<QVector<int>> &channelData)
{
    int numCh = channelData.size();
    int N     = timeStamps.size();

    QString phaseName = (phaseIndex == 0) ? "PhaseA(A 端 stream0)" : "PhaseB(B 端 stream2)";
    appendLog(QString("%1: 收到一个 epoch，通道数=%2, 样本点数=%3")
                  .arg(phaseName).arg(numCh).arg(N));

    // ===== 示例1：你可以在这里对“整个 5s 所有通道”做处理 =====
    // 下面这个只是示例：算每个通道的 RMS
    for (int ch = 0; ch < numCh; ++ch) {
        const auto &data = channelData[ch];
        if (data.isEmpty()) continue;

        double sumSq = 0.0;
        for (int i = 0; i < data.size(); ++i) {
            double uV = (double(data[i]) - 32768.0) * 0.195;
            sumSq += uV * uV;
        }
        double rms = std::sqrt(sumSq / double(data.size()));

        appendLog(QString("  %1 CH%2: RMS = %3 µV")
                      .arg(phaseIndex == 0 ? "A" : "B")
                      .arg(ch)
                      .arg(rms, 0, 'f', 2));
    }

    // ===== 示例2：这里根据分析结果决定要不要刺激 =====
    // 例如：
    /*
    bool needStim = yourAlgorithm(phaseIndex, timeStamps, channelData);

    if (needStim) {
        int trigger = (phaseIndex == 0) ? 1 : 0; // A phase → 刺激 B, B phase → 刺激 A
        m_engine->triggerStim(trigger, true);
        appendLog(QString("%1: 算法判定需要刺激，触发 trigger=%2")
                  .arg(phaseName).arg(trigger));
    }
    */
}



void MainWindow::handleNewSamples(const QVector<uint32_t> &timeStamps,
                                  const QVector<QVector<int>> &channelData)
{
    if (channelData.isEmpty()) return;
    if (timeStamps.isEmpty()) return;

    int numCh = channelData.size();
    int N = timeStamps.size();
    if (N <= 0) return;

    // ====== 只做单通道：当前选中的 ch ======
    int chSel = qBound(0, m_currentChannel, numCh - 1);
    const QVector<int> &chSelData = channelData[chSel];

    int Nsingle = qMin(N, chSelData.size());
    if (Nsingle <= 0) return;

    // 下采样因子：越大越省 CPU（对应显示越“稀疏”）
    const int decim = 30;  // 30kHz / 30 ≈ 每秒 1000 点，2 秒 ≈ 2000 点，完全够看

    for (int i = 0; i < Nsingle; i += decim) {
        uint32_t ts = timeStamps[i];
        int raw      = chSelData[i];

        // Intan timeStamp 一般是样本计数，直接除采样率=秒
        double tSec = double(ts) / m_sampleRate;

        // 转 µV（你 Python 就是这么干的）
        double uV   = (double(raw) - 32768.0) * 0.195;

        m_buffer.append(QPointF(tSec, uV));
    }

    if (m_buffer.isEmpty()) return;

    // ====== 真正只保留“最近 2 秒”的数据 ======
    double tMax = m_buffer.last().x();
    double tMin = tMax - m_visibleWindowSec;
    if (tMin < 0.0) tMin = 0.0;

    // 把更早的数据从前面扔掉
    while (!m_buffer.isEmpty() && m_buffer.first().x() < tMin) {
        m_buffer.removeFirst();
    }

    // 用这一小段 buffer 更新曲线
    m_series->replace(m_buffer);

    // 自适应 Y 轴范围
    double yMin = m_buffer.first().y();
    double yMax = yMin;
    for (const auto &p : m_buffer) {
        if (p.y() < yMin) yMin = p.y();
        if (p.y() > yMax) yMax = p.y();
    }
    double margin = 0.1 * (yMax - yMin + 1e-9);
    m_axisY->setRange(yMin - margin, yMax + margin);

    // X 轴固定滑动窗口 [tMin, tMax]，就是“只看最后 2 秒”
    m_axisX->setRange(tMin, tMax);
}
void MainWindow::handleNewSamplesStream2(const QVector<uint32_t> &timeStamps,
                                         const QVector<QVector<int>> &channelData)
{
    if (channelData.isEmpty()) return;
    if (timeStamps.isEmpty()) return;

    int numCh = channelData.size();
    int N = timeStamps.size();
    if (N <= 0) return;

    int chSel = qBound(0, m_currentChStream2, numCh - 1);
    const QVector<int> &chData = channelData[chSel];
    int Nsingle = qMin(N, chData.size());
    if (Nsingle <= 0) return;

    const int decim = 30;  // 下采样，减负载

    for (int i = 0; i < Nsingle; i += decim) {
        uint32_t ts = timeStamps[i];
        int raw      = chData[i];

        double tSec = double(ts) / m_sampleRate;
        double uV   = (double(raw) - 32768.0) * 0.195;

        m_bufferStream2.append(QPointF(tSec, uV));
    }

    if (m_bufferStream2.isEmpty()) return;

    // 只保留最近 m_stream2WindowSec 秒
    double tMax = m_bufferStream2.last().x();
    double tMin = tMax - m_stream2WindowSec;
    if (tMin < 0.0) tMin = 0.0;

    while (!m_bufferStream2.isEmpty() && m_bufferStream2.first().x() < tMin) {
        m_bufferStream2.removeFirst();
    }

    // 更新曲线
    m_seriesStream2->replace(m_bufferStream2);

    // 自动 Y 轴范围
    double yMin = m_bufferStream2.first().y();
    double yMax = yMin;
    for (const auto &p : m_bufferStream2) {
        if (p.y() < yMin) yMin = p.y();
        if (p.y() > yMax) yMax = p.y();
    }
    double margin = 0.1 * (yMax - yMin + 1e-9);
    m_axisY2->setRange(yMin - margin, yMax + margin);

    // X 轴范围
    m_axisX2->setRange(tMin, tMax);
}


void MainWindow::handleError(const QString &msg)
{
    appendLog(QStringLiteral("错误：") + msg);
}

void MainWindow::handleLog(const QString &msg)
{
    appendLog(msg);
}

void MainWindow::onChannelChanged(int index)
{
    int ch = m_comboChannel->currentData().toInt();
    m_currentChannel = ch;
    appendLog(QString("切换单通道显示到 CH%1").arg(ch));

    // 切换时可以清空单通道缓冲，避免残留旧通道的形状
    m_buffer.clear();
    m_series->clear();
}
void MainWindow::onStimOnce()
{
    if (!m_engine) return;

    int triggerSource = 0;
    m_engine->triggerStim(triggerSource, true);
    appendLog("已触发刺激（使用当前已配置波形，trigger=0）");
}
