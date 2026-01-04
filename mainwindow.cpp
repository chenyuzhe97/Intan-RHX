#include "mainwindow.h"

#include <QFileDialog>
#include <QDateTime>
#include <QDir>

#include <QSettings>
#include <QDockWidget>
#include <QGroupBox>
#include <QHeaderView>
#include <QFormLayout>
#include <QTableWidget>
#include <QToolButton>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
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

    // ===== electrode regions UI =====
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
    for (int ch0 : zeroBased) parts << QString::number(ch0 + 1);
    return parts.join(",");
}

bool MainWindow::parseChannels1Based(const QString &text, QVector<int> &outZeroBased, QString *err)
{
    const QString t = text.trimmed();
    if (t.isEmpty()) {
        if (err) *err = QStringLiteral("不能为空。示例：1,5,7");
        return false;
    }
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

void MainWindow::addRegionRow(QTableWidget *table, const RegionConfig &cfg)
{
    const int row = table->rowCount();
    table->insertRow(row);

    auto *itemName = new QTableWidgetItem(cfg.name);
    auto *itemSense = new QTableWidgetItem(formatChannels1Based(cfg.senseCh0));
    auto *itemStim = new QTableWidgetItem(cfg.stimElectrode);

    table->setItem(row, 0, itemName);
    table->setItem(row, 1, itemSense);
    table->setItem(row, 2, itemStim);
}

void MainWindow::writeRegionsToTable(QTableWidget *table, const QVector<RegionConfig> &regions)
{
    table->setRowCount(0);
    for (const auto &r : regions) addRegionRow(table, r);
    if (table->rowCount() == 0) table->setRowCount(0);
}

bool MainWindow::readRegionsFromTable(QTableWidget *table, QVector<RegionConfig> &out, QString *err) const
{
    out.clear();
    const int rows = table->rowCount();
    if (rows <= 0) {
        if (err) *err = QStringLiteral("至少需要 1 行区域配置");
        return false;
    }

    for (int i = 0; i < rows; ++i) {
        RegionConfig r;
        const QString name = table->item(i, 0) ? table->item(i, 0)->text().trimmed() : QString();
        const QString senseTxt = table->item(i, 1) ? table->item(i, 1)->text().trimmed() : QString();
        const QString stim = table->item(i, 2) ? table->item(i, 2)->text().trimmed() : QString();

        r.name = name.isEmpty() ? QString("R%1").arg(i + 1) : name;

        QString parseErr;
        if (!parseChannels1Based(senseTxt, r.senseCh0, &parseErr)) {
            if (err) *err = QStringLiteral("第 %1 行 Sense 通道列表错误：%2").arg(i + 1).arg(parseErr);
            return false;
        }

        if (stim.isEmpty()) {
            if (err) *err = QStringLiteral("第 %1 行 Stim 电极不能为空").arg(i + 1);
            return false;
        }
        r.stimElectrode = stim;

        out.push_back(r);
    }

    return true;
}

void MainWindow::moveSelectedRow(QTableWidget *table, int delta)
{
    const int row = table->currentRow();
    if (row < 0) return;
    const int newRow = row + delta;
    if (newRow < 0 || newRow >= table->rowCount()) return;

    // swap row contents
    for (int col = 0; col < table->columnCount(); ++col) {
        QTableWidgetItem *a = table->takeItem(row, col);
        QTableWidgetItem *b = table->takeItem(newRow, col);
        table->setItem(row, col, b);
        table->setItem(newRow, col, a);
    }
    table->setCurrentCell(newRow, 0);
}

static QToolButton* makeMiniBtn(const QString &text, const QString &tip, QWidget *parent)
{
    auto *b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setAutoRaise(true);
    return b;
}

void MainWindow::setupElectrodeConfigDock()
{
    m_dockElectrode = new QDockWidget(tr("Electrode Regions"), this);
    m_dockElectrode->setObjectName("dockElectrodeRegions");

    QWidget *panel = new QWidget(m_dockElectrode);
    QVBoxLayout *root = new QVBoxLayout(panel);

    auto buildTable = [&](QTableWidget *&tableOut) -> QTableWidget* {
        auto *t = new QTableWidget(panel);
        t->setColumnCount(3);
        t->setHorizontalHeaderLabels({tr("Region Name"), tr("Sense Channels (1-based)"), tr("Stim Electrode")});
        t->horizontalHeader()->setStretchLastSection(true);
        t->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        t->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        t->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        t->setSelectionBehavior(QAbstractItemView::SelectRows);
        t->setSelectionMode(QAbstractItemView::SingleSelection);
        t->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::AnyKeyPressed);
        tableOut = t;
        return t;
    };

    // ---- Phase0 group
    QGroupBox *g0 = new QGroupBox(tr("PhaseA: sense Mouse A (stream0)  → stimulate Mouse B"), panel);
    QVBoxLayout *g0v = new QVBoxLayout(g0);

    m_tablePhase0 = buildTable(m_tablePhase0);
    g0v->addWidget(m_tablePhase0);

    QHBoxLayout *b0 = new QHBoxLayout();
    m_btnAdd0  = makeMiniBtn("+", tr("Add region"), g0);
    m_btnDel0  = makeMiniBtn("-", tr("Remove selected region"), g0);
    m_btnUp0   = makeMiniBtn("↑", tr("Move up"), g0);
    m_btnDown0 = makeMiniBtn("↓", tr("Move down"), g0);
    b0->addWidget(m_btnAdd0);
    b0->addWidget(m_btnDel0);
    b0->addWidget(m_btnUp0);
    b0->addWidget(m_btnDown0);
    b0->addStretch(1);
    g0v->addLayout(b0);

    // ---- Phase1 group
    QGroupBox *g1 = new QGroupBox(tr("PhaseB: sense Mouse B (stream2)  → stimulate Mouse A"), panel);
    QVBoxLayout *g1v = new QVBoxLayout(g1);

    m_tablePhase1 = buildTable(m_tablePhase1);
    g1v->addWidget(m_tablePhase1);

    QHBoxLayout *b1 = new QHBoxLayout();
    m_btnAdd1  = makeMiniBtn("+", tr("Add region"), g1);
    m_btnDel1  = makeMiniBtn("-", tr("Remove selected region"), g1);
    m_btnUp1   = makeMiniBtn("↑", tr("Move up"), g1);
    m_btnDown1 = makeMiniBtn("↓", tr("Move down"), g1);
    b1->addWidget(m_btnAdd1);
    b1->addWidget(m_btnDel1);
    b1->addWidget(m_btnUp1);
    b1->addWidget(m_btnDown1);
    b1->addStretch(1);
    g1v->addLayout(b1);

    // ---- Apply
    m_btnApplyElectrode = new QPushButton(tr("Apply"), panel);

    root->addWidget(g0);
    root->addWidget(g1);
    root->addWidget(m_btnApplyElectrode);
    root->addStretch(1);

    panel->setLayout(root);
    m_dockElectrode->setWidget(panel);
    addDockWidget(Qt::RightDockWidgetArea, m_dockElectrode);

    connect(m_btnApplyElectrode, &QPushButton::clicked, this, &MainWindow::applyElectrodeConfigFromUi);

    connect(m_btnAdd0,  &QToolButton::clicked, this, &MainWindow::addRegionPhase0);
    connect(m_btnDel0,  &QToolButton::clicked, this, &MainWindow::removeRegionPhase0);
    connect(m_btnUp0,   &QToolButton::clicked, this, &MainWindow::moveUpPhase0);
    connect(m_btnDown0, &QToolButton::clicked, this, &MainWindow::moveDownPhase0);

    connect(m_btnAdd1,  &QToolButton::clicked, this, &MainWindow::addRegionPhase1);
    connect(m_btnDel1,  &QToolButton::clicked, this, &MainWindow::removeRegionPhase1);
    connect(m_btnUp1,   &QToolButton::clicked, this, &MainWindow::moveUpPhase1);
    connect(m_btnDown1, &QToolButton::clicked, this, &MainWindow::moveDownPhase1);
}

static QJsonArray regionsToJson(const QVector<MainWindow::RegionConfig> &regions)
{
    QJsonArray arr;
    for (const auto &r : regions) {
        QJsonObject o;
        o["name"] = r.name;
        o["sense"] = MainWindow::formatChannels1Based(r.senseCh0); // store as 1-based string
        o["stim"] = r.stimElectrode;
        arr.append(o);
    }
    return arr;
}

static QVector<MainWindow::RegionConfig> regionsFromJson(const QJsonArray &arr)
{
    QVector<MainWindow::RegionConfig> out;
    out.reserve(arr.size());
    for (const auto &v : arr) {
        if (!v.isObject()) continue;
        const QJsonObject o = v.toObject();
        MainWindow::RegionConfig r;
        r.name = o.value("name").toString();
        const QString senseTxt = o.value("sense").toString();
        QString err;
        if (!MainWindow::parseChannels1Based(senseTxt, r.senseCh0, &err)) {
            // skip invalid row
            continue;
        }
        r.stimElectrode = o.value("stim").toString();
        if (r.stimElectrode.trimmed().isEmpty()) continue;
        if (r.name.trimmed().isEmpty()) r.name = QString("R%1").arg(out.size() + 1);
        out.push_back(r);
    }
    return out;
}

void MainWindow::loadElectrodeConfig()
{
    // default: 2 regions like your old a/b
    if (m_regionsPhase0.isEmpty()) {
        m_regionsPhase0 = {
                           {"A_a", {0,1,2}, "B1"},
                           {"A_b", {3,4,5}, "B2"},
                           };
    }
    if (m_regionsPhase1.isEmpty()) {
        m_regionsPhase1 = {
                           {"B_a", {0,1,2}, "A1"},
                           {"B_b", {3,4,5}, "A2"},
                           };
    }

    QSettings s;
    const QByteArray j0 = s.value("electrode/regions_phase0").toByteArray();
    const QByteArray j1 = s.value("electrode/regions_phase1").toByteArray();

    if (!j0.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(j0);
        if (doc.isArray()) {
            auto regs = regionsFromJson(doc.array());
            if (!regs.isEmpty()) m_regionsPhase0 = regs;
        }
    }

    if (!j1.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(j1);
        if (doc.isArray()) {
            auto regs = regionsFromJson(doc.array());
            if (!regs.isEmpty()) m_regionsPhase1 = regs;
        }
    }

    if (m_tablePhase0) writeRegionsToTable(m_tablePhase0, m_regionsPhase0);
    if (m_tablePhase1) writeRegionsToTable(m_tablePhase1, m_regionsPhase1);
}

void MainWindow::saveElectrodeConfig() const
{
    QSettings s;
    s.setValue("electrode/regions_phase0", QJsonDocument(regionsToJson(m_regionsPhase0)).toJson(QJsonDocument::Compact));
    s.setValue("electrode/regions_phase1", QJsonDocument(regionsToJson(m_regionsPhase1)).toJson(QJsonDocument::Compact));
}

void MainWindow::applyElectrodeConfigFromUi()
{
    QString err;
    QVector<RegionConfig> r0, r1;

    if (!readRegionsFromTable(m_tablePhase0, r0, &err)) {
        QMessageBox::warning(this, tr("Invalid PhaseA regions"), err);
        return;
    }
    if (!readRegionsFromTable(m_tablePhase1, r1, &err)) {
        QMessageBox::warning(this, tr("Invalid PhaseB regions"), err);
        return;
    }

    m_regionsPhase0 = r0;
    m_regionsPhase1 = r1;
    saveElectrodeConfig();

    appendLog(QString("Electrode regions applied. PhaseA regions=%1, PhaseB regions=%2")
                  .arg(m_regionsPhase0.size()).arg(m_regionsPhase1.size()));
}

void MainWindow::addRegionPhase0()
{
    RegionConfig cfg;
    cfg.name = QString("A_R%1").arg(m_tablePhase0->rowCount() + 1);
    cfg.senseCh0 = {0};
    cfg.stimElectrode = "B1";
    addRegionRow(m_tablePhase0, cfg);
}

void MainWindow::removeRegionPhase0()
{
    const int r = m_tablePhase0->currentRow();
    if (r < 0) return;
    m_tablePhase0->removeRow(r);
}

void MainWindow::moveUpPhase0()   { moveSelectedRow(m_tablePhase0, -1); }
void MainWindow::moveDownPhase0() { moveSelectedRow(m_tablePhase0, +1); }

void MainWindow::addRegionPhase1()
{
    RegionConfig cfg;
    cfg.name = QString("B_R%1").arg(m_tablePhase1->rowCount() + 1);
    cfg.senseCh0 = {0};
    cfg.stimElectrode = "A1";
    addRegionRow(m_tablePhase1, cfg);
}

void MainWindow::removeRegionPhase1()
{
    const int r = m_tablePhase1->currentRow();
    if (r < 0) return;
    m_tablePhase1->removeRow(r);
}

void MainWindow::moveUpPhase1()   { moveSelectedRow(m_tablePhase1, -1); }
void MainWindow::moveDownPhase1() { moveSelectedRow(m_tablePhase1, +1); }



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

    // 0) 选择本 phase 的区域列表（支持任意数量，不再强制 2 对 2）
    const QVector<RegionConfig> &regions = (phaseIndex == 0) ? m_regionsPhase0 : m_regionsPhase1;
    if (regions.isEmpty()) {
        appendLog(phaseName + ": 区域列表为空，请在右侧 Electrode Regions 里配置至少 1 行并 Apply");
        return;
    }

    // 1) 按区域列表，生成 N 路平均信号 avgChannelData[i]
    QVector<QVector<int>> avgChannelData;
    avgChannelData.reserve(regions.size());

    for (int i = 0; i < regions.size(); ++i) {
        const auto &reg = regions[i];
        QVector<int> avg = meanSelectedChannels(channelData, reg.senseCh0);
        if (avg.isEmpty() || avg.size() != timeStamps.size()) {
            appendLog(QString("%1: 区域[%2] 平均信号生成失败（通道列表/数据长度不匹配）")
                          .arg(phaseName).arg(reg.name));
            return;
        }
        avgChannelData.push_back(avg);
    }

    // 2) 把“多路平均信号”送进算法（ABAlgorithm 需要支持 channelData.size() 可变）
    const QVector<ABAlgorithm::Result> allResults =
        m_abAlgo->analyzeEpoch(phaseIndex, timeStamps, avgChannelData);

    if (allResults.isEmpty()) {
        appendLog(phaseName + ": 本 epoch 未检测到事件");
        return;
    }

    // 3) phase->triggerSource 不变：PhaseA 刺激老鼠B 用 triggerSource=1；PhaseB 刺激老鼠A 用 triggerSource=0
    const int triggerSource = (phaseIndex == 0) ? 1 : 0;

    const uint32_t epochStartTs = timeStamps.first();
    const double fs = (m_sampleRate > 0.0 ? m_sampleRate : 30000.0);

    // 4) 只保留需要刺激的候选
    QVector<ABAlgorithm::Result> candidates;
    candidates.reserve(allResults.size());
    for (const auto &r : allResults) {
        if (!r.needStim) continue;
        if (r.suggestedAmplitude_uA <= 0) continue;
        // r.channelIndex 代表 avgChannelData 的索引，也就是 regions 的索引
        if (r.channelIndex < 0 || r.channelIndex >= regions.size()) continue;
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

        const QString targetElectrode = regions[r.channelIndex].stimElectrode;

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
        it.ch = r.channelIndex;          // 现在是 0..(regions.size()-1)
        it.spike_uV = r.spikeAmplitude_uV;
        it.electrode = targetElectrode;
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

    appendLog(QString("%1: epochId=%2 计划刺激=%3 (regions=%4)")
                  .arg(phaseName).arg(epochId).arg(items.size()).arg(regions.size()));
}


void MainWindow::handleError(const QString &msg)
{
    appendLog(QStringLiteral("错误：") + msg);
}

void MainWindow::handleLog(const QString &msg)
{
    appendLog(msg);
}
