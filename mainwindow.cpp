#include "mainwindow.h"

#include <QFileDialog>
#include <QDateTime>
#include <QDir>
#include <QSettings>
#include <QDockWidget>
#include <QGroupBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QMessageBox>
#include <QRegularExpression>

static StackedWaveWidget::FilterSettings buildFilterSettingsFromUi(
    QCheckBox *chkFilter, QComboBox *cmbType,
    QDoubleSpinBox *bp1, QDoubleSpinBox *bp2,
    QDoubleSpinBox *lp, QDoubleSpinBox *hp,
    QCheckBox *chkNotch, QComboBox *cmbNotchHz, QDoubleSpinBox *notchQ)
{
    StackedWaveWidget::FilterSettings s;

    s.enabled = chkFilter->isChecked();

    const QString t = cmbType->currentText();
    if (t == "Off") s.type = StackedWaveWidget::FilterSettings::Type::Off;
    else if (t == "LowPass") s.type = StackedWaveWidget::FilterSettings::Type::LowPass;
    else if (t == "HighPass") s.type = StackedWaveWidget::FilterSettings::Type::HighPass;
    else s.type = StackedWaveWidget::FilterSettings::Type::BandPass;

    s.bp_low_hz  = bp1->value();
    s.bp_high_hz = bp2->value();
    s.lp_hz      = lp->value();
    s.hp_hz      = hp->value();

    s.notchEnabled = chkNotch->isChecked();
    s.notch_hz = (cmbNotchHz->currentText() == "60") ? 60.0 : 50.0;
    s.notchQ   = notchQ->value();
    return s;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();

    setupElectrodeConfigDock();
    loadElectrodeConfig();

    // ===== core =====
    m_engine     = new AcquisitionEngine(this);
    m_experiment = new ExperimentControllerAB(m_engine, this);
    m_abAlgo     = new ABAlgorithm(this);

    if (m_abAlgo) {
        m_abAlgo->setSampleRateHz(m_sampleRate);
    }

    // ===== wave feed =====
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

    connect(m_experiment, &ExperimentControllerAB::epochReady,
            this,         &MainWindow::onABEpochReady);

    // ===== timeline + stim csv =====
    m_timeline = new StimTimelineOverlay();
    m_timeline->setEpochSec(colletion_time);
    m_timeline->show();

    m_stimLog = new StimLogWriter(this);
    m_stimLog->start(QDir::currentPath() + "/stim_log.csv");

    // ===== FFT windows =====
    m_fftA = new FftWindow(m_viewA);
    m_fftB = new FftWindow(m_viewB);
    m_fftA->setFftSize(2048);
    m_fftB->setFftSize(2048);
    m_fftA->setMaxFreq(5000);
    m_fftB->setMaxFreq(5000);
    m_fftA->hide();
    m_fftB->hide();

    ensureTimelineVisible();
    applyDspSettings();
}

MainWindow::~MainWindow()
{
    if (m_experiment) m_experiment->stop();
    if (m_engine)     m_engine->stopAcquisition();

    if (m_stimLog) m_stimLog->stop();

    if (m_fftA) m_fftA->close();
    if (m_fftB) m_fftB->close();
    if (m_timeline) m_timeline->close();
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

        row->addSpacing(14);
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

    // ===== 显示范围（±uV）行 =====
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

    // ===== DSP/FFT 控件行 =====
    {
        QHBoxLayout *row = new QHBoxLayout();

        m_chkFilter = new QCheckBox(tr("Enable Filter"), this);
        m_chkFilter->setChecked(false);

        m_cmbFilterType = new QComboBox(this);
        m_cmbFilterType->addItems({"Off","BandPass","LowPass","HighPass"});
        m_cmbFilterType->setCurrentText("BandPass");

        m_spBP1 = new QDoubleSpinBox(this);
        m_spBP1->setRange(0.1, 20000.0);
        m_spBP1->setDecimals(1);
        m_spBP1->setValue(300.0);
        m_spBP1->setSuffix(" Hz");

        m_spBP2 = new QDoubleSpinBox(this);
        m_spBP2->setRange(0.1, 20000.0);
        m_spBP2->setDecimals(1);
        m_spBP2->setValue(3000.0);
        m_spBP2->setSuffix(" Hz");

        m_spLP = new QDoubleSpinBox(this);
        m_spLP->setRange(0.1, 20000.0);
        m_spLP->setDecimals(1);
        m_spLP->setValue(3000.0);
        m_spLP->setSuffix(" Hz");

        m_spHP = new QDoubleSpinBox(this);
        m_spHP->setRange(0.1, 20000.0);
        m_spHP->setDecimals(1);
        m_spHP->setValue(300.0);
        m_spHP->setSuffix(" Hz");

        m_chkNotch = new QCheckBox(tr("Notch"), this);
        m_chkNotch->setChecked(false);

        m_cmbNotchHz = new QComboBox(this);
        m_cmbNotchHz->addItems({"50Hz","60Hz"});
        m_cmbNotchHz->setCurrentText("50Hz");

        m_spNotchQ = new QDoubleSpinBox(this);
        m_spNotchQ->setRange(5.0, 200.0);
        m_spNotchQ->setDecimals(0);
        m_spNotchQ->setValue(30.0);

        m_btnFFT = new QPushButton(tr("FFT (A/B)"), this);

        row->addWidget(m_chkFilter);
        row->addWidget(m_cmbFilterType);

        row->addWidget(new QLabel(tr("BP:"), this));
        row->addWidget(m_spBP1);
        row->addWidget(m_spBP2);

        row->addSpacing(10);
        row->addWidget(new QLabel(tr("LP:"), this));
        row->addWidget(m_spLP);

        row->addSpacing(10);
        row->addWidget(new QLabel(tr("HP:"), this));
        row->addWidget(m_spHP);

        row->addSpacing(10);
        row->addWidget(m_chkNotch);
        row->addWidget(m_cmbNotchHz);
        row->addWidget(new QLabel(tr("Q"), this));
        row->addWidget(m_spNotchQ);

        row->addStretch(1);
        row->addWidget(m_btnFFT);

        m_layout->addLayout(row);
    }

    // ===== 左右分屏：两个 stream =====
    m_split = new QSplitter(Qt::Horizontal, this);

    m_viewA = new StackedWaveWidget(this);
    m_viewB = new StackedWaveWidget(this);

    // configure(ch, fs, initialWinSec, maxWinSec)
    m_viewA->configure(m_channelsPerStream, m_sampleRate, m_visibleWindowSec, m_maxWindowSec);
    m_viewB->configure(m_channelsPerStream, m_sampleRate, m_visibleWindowSec, m_maxWindowSec);

    m_viewA->setTitle("Stream0 (A) - 16ch  (dblclick=single, wheel=gain, Ctrl+wheel=time, Shift+drag=pan)");
    m_viewB->setTitle("Stream2 (B) - 16ch  (dblclick=single, wheel=gain, Ctrl+wheel=time, Shift+drag=pan)");

    m_viewA->setGainUv(m_spinGainA->value());
    m_viewB->setGainUv(m_spinGainB->value());

    m_split->addWidget(m_viewA);
    m_split->addWidget(m_viewB);
    m_split->setStretchFactor(0, 1);
    m_split->setStretchFactor(1, 1);

    m_layout->addWidget(m_split, 3);

    // ===== 日志 =====
    m_logView  = new QPlainTextEdit(this);
    m_logView->setReadOnly(true);
    m_layout->addWidget(m_logView, 1);

    setCentralWidget(m_central);

    // ===== connections =====
    connect(m_btnOpen,     &QPushButton::clicked, this, &MainWindow::onOpenDevice);
    connect(m_btnStart,    &QPushButton::clicked, this, &MainWindow::onStart);
    connect(m_btnStop,     &QPushButton::clicked, this, &MainWindow::onStop);
    connect(m_btnStimOnce, &QPushButton::clicked, this, &MainWindow::onStimOnce);
    connect(m_btnRecStart, &QPushButton::clicked, this, &MainWindow::onRecStart);
    connect(m_btnRecStop,  &QPushButton::clicked, this, &MainWindow::onRecStop);

    connect(m_spinEpochSec, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::onEpochDurationChanged);

    connect(m_spinGainA, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::onGainAChanged);
    connect(m_spinGainB, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &MainWindow::onGainBChanged);

    // DSP apply on any change
    connect(m_chkFilter, &QCheckBox::toggled, this, &MainWindow::applyDspSettings);
    connect(m_chkNotch,  &QCheckBox::toggled, this, &MainWindow::applyDspSettings);
    connect(m_cmbFilterType, &QComboBox::currentTextChanged, this, &MainWindow::applyDspSettings);
    connect(m_cmbNotchHz, &QComboBox::currentTextChanged, this, &MainWindow::applyDspSettings);

    connect(m_spBP1, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &MainWindow::applyDspSettings);
    connect(m_spBP2, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &MainWindow::applyDspSettings);
    connect(m_spLP,  QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &MainWindow::applyDspSettings);
    connect(m_spHP,  QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &MainWindow::applyDspSettings);
    connect(m_spNotchQ, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &MainWindow::applyDspSettings);

    connect(m_btnFFT, &QPushButton::clicked, this, &MainWindow::onToggleFftWindows);
}

void MainWindow::appendLog(const QString &msg)
{
    const QString line = QDateTime::currentDateTime().toString("hh:mm:ss.zzz  ") + msg;
    m_logView->appendPlainText(line);
}

void MainWindow::ensureTimelineVisible()
{
    if (!m_timeline) return;
    const QRect g = this->geometry();
    m_timeline->move(g.topRight() + QPoint(20, 40));
    m_timeline->raise();
}

void MainWindow::ensureFftVisible()
{
    const QRect g = this->geometry();
    if (m_fftA) m_fftA->move(g.topLeft() + QPoint(20, 80));
    if (m_fftB) m_fftB->move(g.topLeft() + QPoint(20, 380));
    if (m_fftA) m_fftA->raise();
    if (m_fftB) m_fftB->raise();
}

QVector<int> MainWindow::meanSelectedChannels(const QVector<QVector<int> > &channelData, const QVector<int> &sel)
{
    if (channelData.isEmpty() || sel.isEmpty()) return {};

    // 以第0通道长度作为基准（你这套采集一般各通道等长）
    const int N = channelData[0].size();
    if (N <= 0) return {};

    QVector<int> out;
    out.resize(N);

    for (int i = 0; i < N; ++i) {
        long long sum = 0;
        int used = 0;
        for (int ch : sel) {
            if (ch < 0 || ch >= channelData.size()) continue;
            const auto &v = channelData[ch];
            if (i < 0 || i >= v.size()) continue;
            sum += (long long)v[i];
            used++;
        }
        if (used == 0) return {}; // sel 全无效就直接失败
        out[i] = (int)std::llround((double)sum / (double)used);
    }
    return out;
}


QString MainWindow::formatChannels1Based(const QVector<int> &zeroBased)
{
    QStringList parts;
    parts.reserve(zeroBased.size());
    for (int ch0 : zeroBased) {
        parts << QString::number(ch0 + 1);
    }
    return parts.join(",");
}

bool MainWindow::parseChannels1Based(const QString &text, QVector<int> &outZeroBased, QString *err)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) {
        if (err) *err = QStringLiteral("不能为空。示例：1,5,7");
        return false;
    }

    // split by comma / space / semicolon
    const QRegularExpression re(QStringLiteral(R"([,\s;]+)"));
    const QStringList tokens = t.split(re, Qt::SkipEmptyParts);

    QVector<int> v;
    v.reserve(tokens.size());

    for (const QString &tok : tokens) {
        bool ok = false;
        const int val1 = tok.toInt(&ok);
        if (!ok) {
            if (err) *err = QStringLiteral("包含非数字：%1").arg(tok);
            return false;
        }
        if (val1 <= 0) {
            if (err) *err = QStringLiteral("通道必须为正整数(1-based)：%1").arg(val1);
            return false;
        }
        v.push_back(val1 - 1); // to 0-based
    }

    if (v.isEmpty()) {
        if (err) *err = QStringLiteral("未解析到任何通道。示例：1,5,7");
        return false;
    }

    outZeroBased = v;
    return true;
}

void MainWindow::setupElectrodeConfigDock()
{
    // Dock on the right
    m_dockElectrode = new QDockWidget(tr("Electrode Config"), this);
    m_dockElectrode->setObjectName("dockElectrodeConfig");

    QWidget *panel = new QWidget(m_dockElectrode);
    QVBoxLayout *root = new QVBoxLayout(panel);

    // ---- Sense group
    QGroupBox *gbSense = new QGroupBox(tr("Sense channels (UI is 1-based, internal is 0-based)"), panel);
    QFormLayout *senseLayout = new QFormLayout(gbSense);

    m_editSense_A_a = new QLineEdit(gbSense);
    m_editSense_A_b = new QLineEdit(gbSense);
    m_editSense_B_a = new QLineEdit(gbSense);
    m_editSense_B_b = new QLineEdit(gbSense);

    m_editSense_A_a->setPlaceholderText("e.g. 1,5,7");
    m_editSense_A_b->setPlaceholderText("e.g. 9,11,15");
    m_editSense_B_a->setPlaceholderText("e.g. 1,5,7");
    m_editSense_B_b->setPlaceholderText("e.g. 9,11,15");

    senseLayout->addRow(tr("Mouse A: a (stream0)"), m_editSense_A_a);
    senseLayout->addRow(tr("Mouse A: b (stream0)"), m_editSense_A_b);
    senseLayout->addRow(tr("Mouse B: a' (stream2)"), m_editSense_B_a);
    senseLayout->addRow(tr("Mouse B: b' (stream2)"), m_editSense_B_b);

    // ---- Stim group
    QGroupBox *gbStim = new QGroupBox(tr("Stim electrodes (prefix fixed A/B)"), panel);
    QFormLayout *stimLayout = new QFormLayout(gbStim);

    auto makeStimEditor = [&](const QString &prefix, QSpinBox *&spinOut) -> QWidget* {
        QWidget *w = new QWidget(gbStim);
        QHBoxLayout *hl = new QHBoxLayout(w);
        hl->setContentsMargins(0,0,0,0);
        QLabel *lab = new QLabel(prefix, w);
        spinOut = new QSpinBox(w);
        spinOut->setRange(1, 64); // TODO: 如果你的电极编号范围不是 1..64，自行改这里
        spinOut->setSingleStep(1);
        hl->addWidget(lab);
        hl->addWidget(spinOut, 1);
        w->setLayout(hl);
        return w;
    };

    stimLayout->addRow(tr("Stim A a:"),  makeStimEditor("A", m_spinStim_A_a));
    stimLayout->addRow(tr("Stim A b:"),  makeStimEditor("A", m_spinStim_A_b));
    stimLayout->addRow(tr("Stim B a':"), makeStimEditor("B", m_spinStim_B_a));
    stimLayout->addRow(tr("Stim B b':"), makeStimEditor("B", m_spinStim_B_b));

    // ---- Apply button
    m_btnApplyElectrode = new QPushButton(tr("Apply"), panel);

    root->addWidget(gbSense);
    root->addWidget(gbStim);
    root->addWidget(m_btnApplyElectrode);
    root->addStretch(1);

    panel->setLayout(root);
    m_dockElectrode->setWidget(panel);

    addDockWidget(Qt::RightDockWidgetArea, m_dockElectrode);

    connect(m_btnApplyElectrode, &QPushButton::clicked,
            this, &MainWindow::applyElectrodeConfigFromUi);

    // 初始化 UI 为当前默认值
    m_editSense_A_a->setText(formatChannels1Based(kSense_A_a));
    m_editSense_A_b->setText(formatChannels1Based(kSense_A_b));
    m_editSense_B_a->setText(formatChannels1Based(kSense_B_a));
    m_editSense_B_b->setText(formatChannels1Based(kSense_B_b));

    auto parseSuffix = [](const QString &name, const QChar expectedPrefix, int fallback) -> int {
        if (name.size() >= 2 && name[0] == expectedPrefix) {
            bool ok = false;
            const int n = name.mid(1).toInt(&ok);
            if (ok && n > 0) return n;
        }
        return fallback;
    };

    m_spinStim_A_a->setValue(parseSuffix(kStim_A_a, QChar('A'), 2));
    m_spinStim_A_b->setValue(parseSuffix(kStim_A_b, QChar('A'), 7));
    m_spinStim_B_a->setValue(parseSuffix(kStim_B_a, QChar('B'), 2));
    m_spinStim_B_b->setValue(parseSuffix(kStim_B_b, QChar('B'), 7));
}

void MainWindow::loadElectrodeConfig()
{
    if (!m_dockElectrode) return;

    QSettings s;

    // Sense texts are stored as 1-based string like "1,5,7"
    const QString tA_a = s.value("electrode/sense_A_a", m_editSense_A_a->text()).toString();
    const QString tA_b = s.value("electrode/sense_A_b", m_editSense_A_b->text()).toString();
    const QString tB_a = s.value("electrode/sense_B_a", m_editSense_B_a->text()).toString();
    const QString tB_b = s.value("electrode/sense_B_b", m_editSense_B_b->text()).toString();

    m_editSense_A_a->setText(tA_a);
    m_editSense_A_b->setText(tA_b);
    m_editSense_B_a->setText(tB_a);
    m_editSense_B_b->setText(tB_b);

    // Stim numbers
    m_spinStim_A_a->setValue(s.value("electrode/stim_A_a_num", m_spinStim_A_a->value()).toInt());
    m_spinStim_A_b->setValue(s.value("electrode/stim_A_b_num", m_spinStim_A_b->value()).toInt());
    m_spinStim_B_a->setValue(s.value("electrode/stim_B_a_num", m_spinStim_B_a->value()).toInt());
    m_spinStim_B_b->setValue(s.value("electrode/stim_B_b_num", m_spinStim_B_b->value()).toInt());

    // Apply to members (silent, invalid string -> keep old defaults)
    QString err;
    QVector<int> tmp;

    if (parseChannels1Based(m_editSense_A_a->text(), tmp, &err)) kSense_A_a = tmp;
    if (parseChannels1Based(m_editSense_A_b->text(), tmp, &err)) kSense_A_b = tmp;
    if (parseChannels1Based(m_editSense_B_a->text(), tmp, &err)) kSense_B_a = tmp;
    if (parseChannels1Based(m_editSense_B_b->text(), tmp, &err)) kSense_B_b = tmp;

    kStim_A_a = QString("A%1").arg(m_spinStim_A_a->value());
    kStim_A_b = QString("A%1").arg(m_spinStim_A_b->value());
    kStim_B_a = QString("B%1").arg(m_spinStim_B_a->value());
    kStim_B_b = QString("B%1").arg(m_spinStim_B_b->value());
}

void MainWindow::saveElectrodeConfig() const
{
    if (!m_dockElectrode) return;

    QSettings s;
    s.setValue("electrode/sense_A_a", m_editSense_A_a->text().trimmed());
    s.setValue("electrode/sense_A_b", m_editSense_A_b->text().trimmed());
    s.setValue("electrode/sense_B_a", m_editSense_B_a->text().trimmed());
    s.setValue("electrode/sense_B_b", m_editSense_B_b->text().trimmed());

    s.setValue("electrode/stim_A_a_num", m_spinStim_A_a->value());
    s.setValue("electrode/stim_A_b_num", m_spinStim_A_b->value());
    s.setValue("electrode/stim_B_a_num", m_spinStim_B_a->value());
    s.setValue("electrode/stim_B_b_num", m_spinStim_B_b->value());
}

void MainWindow::applyElectrodeConfigFromUi()
{
    QString err;
    QVector<int> tmp;

    if (!parseChannels1Based(m_editSense_A_a->text(), tmp, &err)) {
        QMessageBox::warning(this, tr("Invalid A a sense list"), err);
        return;
    }
    kSense_A_a = tmp;

    if (!parseChannels1Based(m_editSense_A_b->text(), tmp, &err)) {
        QMessageBox::warning(this, tr("Invalid A b sense list"), err);
        return;
    }
    kSense_A_b = tmp;

    if (!parseChannels1Based(m_editSense_B_a->text(), tmp, &err)) {
        QMessageBox::warning(this, tr("Invalid B a' sense list"), err);
        return;
    }
    kSense_B_a = tmp;

    if (!parseChannels1Based(m_editSense_B_b->text(), tmp, &err)) {
        QMessageBox::warning(this, tr("Invalid B b' sense list"), err);
        return;
    }
    kSense_B_b = tmp;

    // Stim strings
    kStim_A_a = QString("A%1").arg(m_spinStim_A_a->value());
    kStim_A_b = QString("A%1").arg(m_spinStim_A_b->value());
    kStim_B_a = QString("B%1").arg(m_spinStim_B_a->value());
    kStim_B_b = QString("B%1").arg(m_spinStim_B_b->value());

    saveElectrodeConfig();

    appendLog(QString("ElectrodeConfig applied. "
                      "SenseA(a)=[%1] SenseA(b)=[%2] SenseB(a')=[%3] SenseB(b')=[%4]; "
                      "StimA(a)=%5 StimA(b)=%6 StimB(a')=%7 StimB(b')=%8")
                  .arg(formatChannels1Based(kSense_A_a))
                  .arg(formatChannels1Based(kSense_A_b))
                  .arg(formatChannels1Based(kSense_B_a))
                  .arg(formatChannels1Based(kSense_B_b))
                  .arg(kStim_A_a).arg(kStim_A_b).arg(kStim_B_a).arg(kStim_B_b));
}
void MainWindow::applyDspSettings()
{
    if (!m_viewA || !m_viewB) return;

    auto s = buildFilterSettingsFromUi(
        m_chkFilter, m_cmbFilterType,
        m_spBP1, m_spBP2, m_spLP, m_spHP,
        m_chkNotch, m_cmbNotchHz, m_spNotchQ
        );

    m_viewA->setFilterSettings(s);
    m_viewB->setFilterSettings(s);

    // enable/disable fields for clarity
    const QString t = m_cmbFilterType->currentText();
    const bool en = m_chkFilter->isChecked() && (t != "Off");
    m_spBP1->setEnabled(en && t == "BandPass");
    m_spBP2->setEnabled(en && t == "BandPass");
    m_spLP->setEnabled(en && t == "LowPass");
    m_spHP->setEnabled(en && t == "HighPass");

    const bool enNotch = m_chkNotch->isChecked();
    m_cmbNotchHz->setEnabled(enNotch);
    m_spNotchQ->setEnabled(enNotch);
}

void MainWindow::onToggleFftWindows()
{
    if (!m_fftA || !m_fftB || !m_viewA || !m_viewB) return;

    // 跟随当前选中通道
    m_fftA->setChannel(m_viewA->selectedChannel());
    m_fftB->setChannel(m_viewB->selectedChannel());

    const bool show = !m_fftA->isVisible();
    if (show) {
        m_fftA->show();
        m_fftB->show();
        ensureFftVisible();
    } else {
        m_fftA->hide();
        m_fftB->hide();
    }
}

void MainWindow::onOpenDevice()
{
    QString path = QFileDialog::getOpenFileName(
        this,
        tr("选择 ConfigRHSController_7310.bit"),
        QString(),
        tr("Bitfile (*.bit);;All Files (*.*)")
        );
    if (path.isEmpty()) return;

    if (!m_engine->openDevice(path)) appendLog("打开设备失败");
    else appendLog("打开设备成功");
}

void MainWindow::onStart()
{
    if (!m_engine) return;

    m_engine->startContinuousAcquisition();

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
        tr("Binary Recording (*.bar);;All Files (*.*)")
        );
    if (file.isEmpty()) return;

    if (m_engine->startBinaryRecording(file)) {
        appendLog("开始录制到文件：" + file);
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
        m_stimLog->start(QDir::currentPath() + "/stim_log.csv");
        appendLog("Stim log 切回默认：stim_log.csv");
    }
}

void MainWindow::onStimOnce()
{
    if (!m_engine) return;
    int triggerSource = 0;
    m_engine->triggerStim(triggerSource, true);
    appendLog("已触发一次刺激：trigger=0");
}

void MainWindow::onEpochDurationChanged(double sec)
{
    colletion_time = sec;
    if (m_experiment) m_experiment->setEpochDuration(colletion_time);
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
    const QString phaseName = (phaseIndex == 0) ? "PhaseA (A端 stream0)" : "PhaseB (B端 stream2)";
    if (!m_abAlgo || !m_engine) return;
    if (timeStamps.isEmpty() || channelData.isEmpty()) return;

    // 1) 按 phase 选择通道列表，算两条平均信号：avg[0]=a区，avg[1]=b区
    const QVector<int> &sel_a = (phaseIndex == 0) ? kSense_A_a : kSense_B_a;
    const QVector<int> &sel_b = (phaseIndex == 0) ? kSense_A_b : kSense_B_b;

    QVector<int> avg_a = meanSelectedChannels(channelData, sel_a);
    QVector<int> avg_b = meanSelectedChannels(channelData, sel_b);

    if (avg_a.isEmpty() || avg_b.isEmpty() || avg_a.size() != timeStamps.size() || avg_b.size() != timeStamps.size()) {
        appendLog(phaseName + ": 平均信号生成失败（通道列表/数据长度不匹配）");
        return;
    }

    QVector<QVector<int>> avgChannelData;
    avgChannelData.reserve(2);
    avgChannelData.push_back(avg_a); // chIndex=0 -> a区平均
    avgChannelData.push_back(avg_b); // chIndex=1 -> b区平均

    // 2) 把“2路平均信号”送进你现有算法（算法不用改，仍然做滤波+尖峰检测）
    const QVector<ABAlgorithm::Result> allResults =
        m_abAlgo->analyzeEpoch(phaseIndex, timeStamps, avgChannelData);

    if (allResults.isEmpty()) {
        appendLog(phaseName + ": 本 epoch 未检测到事件");
        return;
    }

    // 3) phase->triggerSource 不变：你说“配置24/25，触发0/1”，那这里继续沿用
    //    仍然：PhaseA 刺激老鼠B 用 triggerSource=1；PhaseB 刺激老鼠A 用 triggerSource=0
    const int triggerSource = (phaseIndex == 0) ? 1 : 0;

    // 4) 按 a/b 区域决定刺激电极名字（关键：一条平均信号对应一根刺激电极）
    auto electrodeForAvgIndex = [&](int avgIndex) -> QString {
        if (phaseIndex == 0) { // A -> 刺激 B
            return (avgIndex == 0) ? kStim_B_a : kStim_B_b;
        } else {               // B -> 刺激 A
            return (avgIndex == 0) ? kStim_A_a : kStim_A_b;
        }
    };

    const uint32_t epochStartTs = timeStamps.first();
    const double fs = (m_sampleRate > 0.0 ? m_sampleRate : 30000.0);

    // 5) 只保留需要刺激的候选
    QVector<ABAlgorithm::Result> candidates;
    candidates.reserve(allResults.size());
    for (const auto &r : allResults) {
        if (!r.needStim) continue;
        if (r.suggestedAmplitude_uA <= 0) continue;
        candidates.push_back(r);
    }
    if (candidates.isEmpty()) {
        appendLog(phaseName + ": 无需刺激");
        return;
    }

    // 强度最大 10 个
    std::sort(candidates.begin(), candidates.end(),
              [](const ABAlgorithm::Result &a, const ABAlgorithm::Result &b) {
                  return a.spikeAmplitude_uV > b.spikeAmplitude_uV;
              });
    const int maxStimPerEpoch = 10;
    if (candidates.size() > maxStimPerEpoch) candidates.resize(maxStimPerEpoch);

    // 按时间排序
    std::sort(candidates.begin(), candidates.end(),
              [](const ABAlgorithm::Result &a, const ABAlgorithm::Result &b) {
                  return a.triggerTime < b.triggerTime;
              });

    const int epochId = ++m_epochCounter;

    QVector<StimTimelineOverlay::Item> items;
    items.reserve(candidates.size());

    for (int i = 0; i < candidates.size(); ++i) {
        const auto &r = candidates[i];

        const QString targetElectrode = electrodeForAvgIndex(r.channelIndex); // 0->a区电极, 1->b区电极

        int pulses = (r.suggestedNumPulses > 0) ? r.suggestedNumPulses : 1;

        double offsetSec = 0.0;
        if (r.triggerTime >= epochStartTs) offsetSec = double(r.triggerTime - epochStartTs) / fs;
        const double offsetMs = offsetSec * 1000.0;
        int delayMs = int(offsetMs);
        if (delayMs < 0) delayMs = 0;

        StimTimelineOverlay::Item it;
        it.itemIndex = i;
        it.offsetMs = offsetMs;
        it.amp_uA = r.suggestedAmplitude_uA;
        it.pulses = pulses;
        it.ch = r.channelIndex;          // 这里现在是 0/1，代表 a平均/b平均
        it.spike_uV = r.spikeAmplitude_uV;
        it.electrode = targetElectrode;  // 关键：每条事件写入对应的刺激电极
        it.fired = false;
        items.push_back(it);

        if (m_stimLog) {
            m_stimLog->logPlanned(epochId, phaseIndex, i, offsetMs,
                                  targetElectrode, it.amp_uA, it.pulses, it.ch, it.spike_uV);
        }

        QTimer::singleShot(delayMs, this,
                           [this, epochId, itemIdx=i,
                            targetElectrode,
                            triggerSource,
                            amp=it.amp_uA, pulses=it.pulses]() {
                               if (m_timeline) m_timeline->markFired(epochId, itemIdx);
                               if (!m_engine) return;
                               m_engine->applyAdaptiveStim(targetElectrode, amp, pulses, triggerSource);
                           });
    }

    if (m_timeline) {
        m_timeline->setEpochPlan(epochId, phaseIndex, colletion_time, items);
        ensureTimelineVisible();
    }

    appendLog(QString("%1: epochId=%2 计划刺激=%3")
                  .arg(phaseName).arg(epochId).arg(items.size()));
}


void MainWindow::handleError(const QString &msg)
{
    appendLog(QStringLiteral("错误：") + msg);
}

void MainWindow::handleLog(const QString &msg)
{
    appendLog(msg);
}
