#include "mainwindow.h"
#include <QFileDialog>
#include <QDateTime>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();
    setupPlot();

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

    m_layout->addWidget(m_btnOpen);
    m_layout->addWidget(m_btnStart);
    m_layout->addWidget(m_btnStop);
    m_layout->addWidget(m_logView, 1);

    setCentralWidget(m_central);

    connect(m_btnOpen,  &QPushButton::clicked,
            this,       &MainWindow::onOpenDevice);
    connect(m_btnStart, &QPushButton::clicked,
            this,       &MainWindow::onStart);
    connect(m_btnStop,  &QPushButton::clicked,
            this,       &MainWindow::onStop);
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

    // 简单起见，先画 stream0 的 CH0（对应 channelData[0]）
    const int ch = 0;
    const QVector<int> &chData = channelData[ch];

    // 保险起见，确保长度一致
    int N = qMin(timeStamps.size(), chData.size());
    if (N <= 0) return;

    // 下采样，避免点数太多拖慢 UI
    const int decim = 10;  // 每 10 个点取一个，可以自己调

    for (int i = 0; i < N; i += decim) {
        uint32_t ts = timeStamps[i];
        int raw      = chData[i];

        // 时间：Intan 的 timeStamp 通常就是 sampleIndex（从 0 开始）
        double tSec = double(ts) / m_sampleRate;

        // 原始值转换为 µV：和你 Python 解析一样 (val - 32768)*0.195
        double uV = (double(raw) - 32768.0) * 0.195;

        m_buffer.append(QPointF(tSec, uV));
    }

    if (m_buffer.isEmpty()) return;

    // 只保留最近 m_visibleWindowSec 秒的数据
    double tMax = m_buffer.last().x();
    double tMin = tMax - m_visibleWindowSec;
    if (tMin < 0.0) tMin = 0.0;

    while (!m_buffer.isEmpty() && m_buffer.first().x() < tMin) {
        m_buffer.removeFirst();
    }

    // 更新曲线
    m_series->replace(m_buffer);

    // 自适应 Y 轴范围
    double yMin = m_buffer.first().y();
    double yMax = yMin;
    for (const auto &p : m_buffer) {
        if (p.y() < yMin) yMin = p.y();
        if (p.y() > yMax) yMax = p.y();
    }
    // 给一点边距
    double margin = 0.1 * (yMax - yMin + 1e-9);
    m_axisY->setRange(yMin - margin, yMax + margin);

    // 更新 X 轴范围
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
