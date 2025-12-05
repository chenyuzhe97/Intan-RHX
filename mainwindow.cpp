#include "mainwindow.h"
#include <QFileDialog>
#include <QDateTime>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();

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
    // 这里只做一个简单示例：打印这一块里第 0 通道的最后一个样本
    if (channelData.isEmpty()) return;
    int lastIndex = timeStamps.size() - 1;
    if (lastIndex < 0) return;

    int value = channelData[0][lastIndex];
    appendLog(QString("新数据块：最后时间戳=%1, CH0=%2")
                  .arg(timeStamps[lastIndex])
                  .arg(value));

    // TODO：这里你可以把 channelData 推给 QCustomPlot / QtCharts 做实时波形
}

void MainWindow::handleError(const QString &msg)
{
    appendLog(QStringLiteral("错误：") + msg);
}

void MainWindow::handleLog(const QString &msg)
{
    appendLog(msg);
}
