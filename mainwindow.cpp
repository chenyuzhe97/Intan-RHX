#include "mainwindow.h"
#include <QFileDialog>
#include <QDateTime>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();
    setupSinglePlot();   // 你的画图初始化

    m_engine = new AcquisitionEngine(this);
    m_experiment = new ExperimentController(m_engine, this);

    connect(m_engine, &AcquisitionEngine::newSamples,
            this,      &MainWindow::handleNewSamples);
    connect(m_engine, &AcquisitionEngine::errorOccurred,
            this,      &MainWindow::handleError);
    connect(m_engine, &AcquisitionEngine::logMessage,
            this,      &MainWindow::handleLog);

    // 闭环控制器的 log 直接接到日志视图
    connect(m_experiment, &ExperimentController::logMessage,
            this,         &MainWindow::handleLog);

    // 每个 epoch 结束时，把 RMS 和是否刺激显示出来
    connect(m_experiment, &ExperimentController::epochRmsComputed,
            this, [this](double rms, bool stimulated) {
                appendLog(QString("Epoch RMS = %1 µV, %2")
                              .arg(rms, 0, 'f', 2)
                              .arg(stimulated ? "已发刺激" : "未刺激"));
            });
}


MainWindow::~MainWindow()
{
}



void MainWindow::setupUi()
{
    m_central = new QWidget(this);
    m_layout  = new QVBoxLayout(m_central);

    m_btnOpen  = new QPushButton(tr("打开设备"), m_central);
    m_btnStart = new QPushButton(tr("开始采集"), m_central);
    m_btnStop  = new QPushButton(tr("停止采集"), m_central);
    m_btnStim  = new QPushButton(tr("发一次刺激 (A1)"), m_central);
    m_logView  = new QPlainTextEdit(m_central);
    m_logView->setReadOnly(true);

    // 顶部按钮
    m_layout->addWidget(m_btnOpen);
    m_layout->addWidget(m_btnStart);
    m_layout->addWidget(m_btnStop);
    m_layout->addWidget(m_btnStim);   // ⭐ 加在按钮区域


    // ⭐ 中间插一个“单通道图”占位（真正的图在 setupSinglePlot 里 addWidget）
    // 这里先不插，等 setupSinglePlot 调用 insertWidget

    // ⭐ 通道选择控件
    QHBoxLayout *channelLayout = new QHBoxLayout();
    QLabel *label = new QLabel(tr("单通道显示通道："), m_central);
    m_comboChannel = new QComboBox(m_central);
    for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
        m_comboChannel->addItem(QString("CH%1").arg(ch), ch);
    }
    channelLayout->addWidget(label);
    channelLayout->addWidget(m_comboChannel);
    channelLayout->addStretch(1);

    m_layout->addLayout(channelLayout);

    // ⭐ 多通道图占位（真正 add 在 setupMultiPlot 里）

    // 底部 log
    m_layout->addWidget(m_logView, 1);

    setCentralWidget(m_central);

    connect(m_btnOpen,  &QPushButton::clicked,
            this,       &MainWindow::onOpenDevice);
    connect(m_btnStart, &QPushButton::clicked,
            this,       &MainWindow::onStart);
    connect(m_btnStop,  &QPushButton::clicked,
            this,       &MainWindow::onStop);
    connect(m_btnStim,  &QPushButton::clicked,
            this,       &MainWindow::onStimOnce);

    // ⭐ 通道选择信号
    connect(m_comboChannel,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &MainWindow::onChannelChanged);
}


void MainWindow::setupSinglePlot()
{
    m_series = new QLineSeries(this);
    m_chart  = new QChart();
    m_chart->addSeries(m_series);
    m_chart->legend()->hide();

    m_axisX = new QValueAxis(this);
    m_axisX->setTitleText("Time (s)");
    m_axisX->setRange(0.0, m_visibleWindowSec);

    m_axisY = new QValueAxis(this);
    m_axisY->setTitleText("Amplitude (µV)");
    m_axisY->setRange(-100.0, 100.0);

    m_chart->addAxis(m_axisX, Qt::AlignBottom);
    m_chart->addAxis(m_axisY, Qt::AlignLeft);
    m_series->attachAxis(m_axisX);
    m_series->attachAxis(m_axisY);

    m_chartView = new QChartView(m_chart, this);
    m_chartView->setRenderHint(QPainter::Antialiasing);

    // 把单通道图插到按钮下面（布局最上面），索引 0 或 1 视你按钮多少个
    // 这里假设按钮三行已经在最上面，我们想把图放在按钮之后
    m_layout->insertWidget(3, m_chartView, 2);
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

    // 1️⃣ 先配置一次默认刺激参数（安全：此时还未 startContinuousAcquisition）
    QString electrodeName = "A1";
    int firstAmp_uA       = 100;
    int secondAmp_uA      = 100;
    int firstDur_us       = 500;
    int secondDur_us      = 500;
    int interDelay_us     = 500;
    int numPulses         = 1;
    int triggerSource     = 0;

    m_engine->configureStim(electrodeName,
                            firstAmp_uA,
                            secondAmp_uA,
                            firstDur_us,
                            secondDur_us,
                            interDelay_us,
                            numPulses,
                            triggerSource);

    // 2️⃣ 开 continuous 采集
    m_engine->startContinuousAcquisition();

    // 3️⃣ 启动闭环控制（每 5 秒算一次 RMS → 决定是否 trigger）
    if (m_experiment) {
        // 可选：让闭环用当前 GUI 选的通道，比如 m_currentChannel
        m_experiment->setChannel(m_currentChannel);
        m_experiment->start();
    }
}

void MainWindow::onStop()
{
    if (m_experiment) {
        m_experiment->stop();
    }

    if (m_engine) {
        m_engine->stopAcquisition();
    }
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
