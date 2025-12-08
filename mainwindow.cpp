#include "mainwindow.h"
#include <QFileDialog>
#include <QDateTime>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();
    setupSinglePlot();   // ⭐ 初始化单通道图
    setupMultiPlot();    // ⭐ 初始化多通道叠加图

    m_engine = new AcquisitionEngine(this);

    connect(m_engine, &AcquisitionEngine::newSamples,
            this, &MainWindow::handleNewSamples);
    connect(m_engine, &AcquisitionEngine::errorOccurred,
            this, &MainWindow::handleError);
    connect(m_engine, &AcquisitionEngine::logMessage,
            this, &MainWindow::handleLog);
}


MainWindow::~MainWindow()
{
}

void MainWindow::setupPlot()
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
    m_axisY->setRange(-100.0, 100.0);  // 初始值，后面会自适应

    m_chart->addAxis(m_axisX, Qt::AlignBottom);
    m_chart->addAxis(m_axisY, Qt::AlignLeft);
    m_series->attachAxis(m_axisX);
    m_series->attachAxis(m_axisY);

    m_chartView = new QChartView(m_chart, this);
    m_chartView->setRenderHint(QPainter::Antialiasing);

    // 把图加到原来的 layout 里（在 logView 上面）
    // 假设 setupUi() 里最后 add 的是 m_logView：
    m_layout->insertWidget(0, m_chartView, 1); // 放在最上面，占比较大空间
}


void MainWindow::setupUi()
{
    m_central = new QWidget(this);
    m_layout  = new QVBoxLayout(m_central);

    m_btnOpen  = new QPushButton(tr("打开设备"), m_central);
    m_btnStart = new QPushButton(tr("开始采集"), m_central);
    m_btnStop  = new QPushButton(tr("停止采集"), m_central);
    m_logView  = new QPlainTextEdit(m_central);
    m_logView->setReadOnly(true);

    // 顶部按钮
    m_layout->addWidget(m_btnOpen);
    m_layout->addWidget(m_btnStart);
    m_layout->addWidget(m_btnStop);

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

    // ⭐ 通道选择信号
    connect(m_comboChannel,
            QOverload<int>::of(&QComboBox::currentIndexChanged),
            this,
            &MainWindow::);
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
    m_engine->startContinuousAcquisition();
}

void MainWindow::onStop()
{
    m_engine->stopAcquisition();
}
void MainWindow::handleNewSamples(const QVector<uint32_t> &timeStamps,
                                  const QVector<QVector<int>> &channelData)
{
    if (channelData.isEmpty()) return;
    if (timeStamps.isEmpty()) return;

    // 保险：通道数取实际和 NUM_CHANNELS 的较小值
    int numCh = qMin(NUM_CHANNELS, channelData.size());
    int N = timeStamps.size();
    if (N <= 0) return;

    // ====== 1. 单通道（当前选中的通道） ======
    int chSel = qBound(0, m_currentChannel, numCh - 1);
    const QVector<int> &chSelData = channelData[chSel];

    int Nsingle = qMin(N, chSelData.size());
    const int decimSingle = 10;  // 单通道下采样
    for (int i = 0; i < Nsingle; i += decimSingle) {
        uint32_t ts = timeStamps[i];
        int raw      = chSelData[i];

        double tSec = double(ts) / m_sampleRate;
        double uV   = (double(raw) - 32768.0) * 0.195;

        m_buffer.append(QPointF(tSec, uV));
    }

    if (!m_buffer.isEmpty()) {
        double tMax = m_buffer.last().x();
        double tMin = tMax - m_visibleWindowSec;
        if (tMin < 0.0) tMin = 0.0;

        while (!m_buffer.isEmpty() && m_buffer.first().x() < tMin) {
            m_buffer.removeFirst();
        }

        m_series->replace(m_buffer);

        // Y 轴自适应
        double yMin = m_buffer.first().y();
        double yMax = yMin;
        for (const auto &p : m_buffer) {
            if (p.y() < yMin) yMin = p.y();
            if (p.y() > yMax) yMax = p.y();
        }
        double margin = 0.1 * (yMax - yMin + 1e-9);
        m_axisY->setRange(yMin - margin, yMax + margin);

        m_axisX->setRange(tMin, tMax);
    }

    // ====== 2. 多通道叠加视图 ======
    // 每个通道一个 buffer，加垂直偏移
    // 先简单假设所有 channelData[ch] 长度 >= N
    const double spacing = 400.0; // 每通道之间的垂直间距（µV）

    for (int ch = 0; ch < numCh; ++ch) {
        const QVector<int> &chData = channelData[ch];
        int Nch = qMin(N, chData.size());
        const int decimMulti = 20; // 多通道可以更强一点的下采样

        QVector<QPointF> &buf = m_multiBuffers[ch];

        for (int i = 0; i < Nch; i += decimMulti) {
            uint32_t ts = timeStamps[i];
            int raw      = chData[i];

            double tSec = double(ts) / m_sampleRate;
            double uV   = (double(raw) - 32768.0) * 0.195;

            // 加通道偏移，把每个通道错开堆叠
            double yPlot = uV + ch * spacing;

            buf.append(QPointF(tSec, yPlot));
        }
    }

    // 清理 & 更新多通道曲线
    if (!m_multiBuffers.isEmpty() && !m_multiBuffers[0].isEmpty()) {
        double tMax = m_multiBuffers[0].last().x();
        double tMin = tMax - m_multiWindowSec;
        if (tMin < 0.0) tMin = 0.0;

        double globalMin = 1e9;
        double globalMax = -1e9;

        for (int ch = 0; ch < numCh; ++ch) {
            QVector<QPointF> &buf = m_multiBuffers[ch];

            // 删除窗口外的数据
            while (!buf.isEmpty() && buf.first().x() < tMin) {
                buf.removeFirst();
            }

            if (!buf.isEmpty()) {
                // 更新每个 series
                if (m_multiSeries[ch]) {
                    m_multiSeries[ch]->replace(buf);
                }

                // 更新全局 y 范围
                for (const auto &p : buf) {
                    if (p.y() < globalMin) globalMin = p.y();
                    if (p.y() > globalMax) globalMax = p.y();
                }
            }
        }

        if (globalMin < globalMax) {
            double margin = 0.1 * (globalMax - globalMin + 1e-9);
            m_multiAxisY->setRange(globalMin - margin, globalMax + margin);
        }

        m_multiAxisX->setRange(tMin, tMax);
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

void MainWindow::onChannelChanged(int index)
{
    int ch = m_comboChannel->currentData().toInt();
    m_currentChannel = ch;
    appendLog(QString("切换单通道显示到 CH%1").arg(ch));

    // 切换时可以清空单通道缓冲，避免残留旧通道的形状
    m_buffer.clear();
    m_series->clear();
}
