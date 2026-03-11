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

    // ===== 妞ゅ爼鍎撮幐澶愭尦鐞?=====
    {
        QHBoxLayout *row = new QHBoxLayout();

        m_btnOpen     = new QPushButton(tr("Open Device"), this);
        m_btnStart    = new QPushButton(tr("Start"), this);
        m_btnStop     = new QPushButton(tr("Stop"), this);
        m_btnStimOnce = new QPushButton(tr("Stim Once (trigger=0)"), this);
        m_btnRecStart = new QPushButton(tr("Start Rec (.bin)"), this);
        m_btnRecStop  = new QPushButton(tr("Stop Rec"), this);

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

    // ===== 閺勫墽銇氶懠鍐ㄦ纯閿涘煪鐪淰閿涘顢?=====
    {
        QHBoxLayout *row = new QHBoxLayout();

        row->addWidget(new QLabel(tr("Stream0 閺勫墽銇氶懠鍐ㄦ纯(鍗V):"), this));
        m_spinGainA = new QDoubleSpinBox(this);
        m_spinGainA->setRange(10.0, 200000.0);
        m_spinGainA->setDecimals(0);
        m_spinGainA->setSingleStep(100.0);
        m_spinGainA->setValue(500.0);
        m_spinGainA->setSuffix(" 纰孷");
        row->addWidget(m_spinGainA);

        row->addSpacing(20);

        row->addWidget(new QLabel(tr("Stream2 閺勫墽銇氶懠鍐ㄦ纯(鍗V):"), this));
        m_spinGainB = new QDoubleSpinBox(this);
        m_spinGainB->setRange(10.0, 200000.0);
        m_spinGainB->setDecimals(0);
        m_spinGainB->setSingleStep(100.0);
        m_spinGainB->setValue(500.0);
        m_spinGainB->setSuffix(" 纰孷");
        row->addWidget(m_spinGainB);

        row->addStretch(1);
        m_layout->addLayout(row);
    }

    // ===== DSP/FFT 閹貉傛鐞?=====
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

    // ===== 瀹革箑褰搁崚鍡楃潌閿涙矮琚辨稉?stream =====
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

    // ===== 閺冦儱绻?=====
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

    // 娴犮儳顑?闁岸浜鹃梹鍨娴ｆ粈璐熼崺鍝勫櫙閿涘牅缍樻潻娆忣殰闁插洭娉︽稉鈧懜顒€鎮囬柅姘朵壕缁涘鏆遍敍?
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
        if (used == 0) return {}; // sel 閸忋劍妫ら弫鍫濇皑閻╁瓨甯存径杈Е
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
        if (err) *err = QStringLiteral("娑撳秷鍏樻稉铏光敄閵嗗倻銇氭笟瀣剁窗1,5,7");
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
            if (err) *err = QStringLiteral("閸栧懎鎯堥棃鐐存殶鐎涙绱?1").arg(tok);
            return false;
        }
        if (val1 <= 0) {
            if (err) *err = QStringLiteral("闁岸浜捐箛鍛淬€忔稉鐑橆劀閺佸瓨鏆?1-based)閿?1").arg(val1);
            return false;
        }
        v.push_back(val1 - 1); // to 0-based
    }

    if (v.isEmpty()) {
        if (err) *err = QStringLiteral("閺堫亣袙閺嬫劕鍩屾禒璁崇秿闁岸浜鹃妴鍌溿仛娓氬绱?,5,7");
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
        spinOut->setRange(1, 64); // TODO: 婵″倹鐏夋担鐘垫畱閻㈠灚鐎紓鏍у娇閼煎啫娲挎稉宥嗘Ц 1..64閿涘矁鍤滅悰灞炬暭鏉╂瑩鍣?
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

    // 閸掓繂顫愰崠?UI 娑撳搫缍嬮崜宥夌帛鐠併倕鈧?
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

    // 鐠虹喖娈㈣ぐ鎾冲闁鑵戦柅姘朵壕
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
        tr("Open RHS bitfile"),
        QString(),
        tr("Bitfile (*.bit);;All Files (*.*)")
        );
    if (path.isEmpty()) return;

    if (!m_engine->openDevice(path)) appendLog("Open device failed");
    else appendLog("Device opened");
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
    appendLog(QString("Started acquisition + AB epoch=%1 s").arg(colletion_time, 0, 'f', 2));
}

void MainWindow::onStop()
{
    if (m_experiment) m_experiment->stop();
    if (m_engine)     m_engine->stopAcquisition();
    appendLog("Stopped acquisition");
}

void MainWindow::onRecStart()
{
    if (!m_engine) return;

    QString file = QFileDialog::getSaveFileName(
        this,
        tr("Save binary recording"),
        QDir::currentPath() + "/recording.bar",
        tr("Binary Recording (*.bar);;All Files (*.*)")
        );
    if (file.isEmpty()) return;

    if (m_engine->startBinaryRecording(file)) {
        appendLog("Started binary recording: " + file);
        if (m_stimLog) {
            m_stimLog->start(file + ".stim.csv");
            appendLog("Stim log started: " + file + ".stim.csv");
        }
    } else {
        appendLog("Failed to start binary recording");
    }
}

void MainWindow::onRecStop()
{
    if (!m_engine) return;
    m_engine->stopBinaryRecording();
    appendLog("Stopped binary recording");

    if (m_stimLog) {
        m_stimLog->stop();
        m_stimLog->start(QDir::currentPath() + "/stim_log.csv");
        appendLog("Stim log reset to stim_log.csv");
    }
}

void MainWindow::onStimOnce()
{
    if (!m_engine) return;
    int triggerSource = 0;
    m_engine->triggerStim(triggerSource, true);
    m_engine->triggerStim(triggerSource, false);
    appendLog("Manual stimulation pulse sent (trigger=0)");
}

void MainWindow::onEpochDurationChanged(double sec)
{
    colletion_time = sec;
    if (m_experiment) m_experiment->setEpochDuration(colletion_time);
    m_timeline->setEpochSec(colletion_time);
    appendLog(QString("Epoch duration set to %1 s").arg(colletion_time, 0, 'f', 2));
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
    const QString phaseName = (phaseIndex == 0) ? "PhaseA (A缁?stream0)" : "PhaseB (B缁?stream2)";
    if (!m_abAlgo || !m_engine) return;
    if (timeStamps.isEmpty() || channelData.isEmpty()) return;

    // 1) 閹?phase 闁瀚ㄩ柅姘朵壕閸掓銆冮敍宀€鐣绘稉銈嗘蒋楠炲啿娼庢穱鈥冲娇閿涙瓫vg[0]=a閸栫尨绱漚vg[1]=b閸?
    const QVector<int> &sel_a = (phaseIndex == 0) ? kSense_A_a : kSense_B_a;
    const QVector<int> &sel_b = (phaseIndex == 0) ? kSense_A_b : kSense_B_b;

    QVector<int> avg_a = meanSelectedChannels(channelData, sel_a);
    QVector<int> avg_b = meanSelectedChannels(channelData, sel_b);

    if (avg_a.isEmpty() || avg_b.isEmpty() || avg_a.size() != timeStamps.size() || avg_b.size() != timeStamps.size()) {
        appendLog(phaseName + ": 楠炲啿娼庢穱鈥冲娇閻㈢喐鍨氭径杈Е閿涘牓鈧岸浜鹃崚妤勩€?閺佺増宓侀梹鍨娑撳秴灏柊宥忕礆");
        return;
    }

    QVector<QVector<int>> avgChannelData;
    avgChannelData.reserve(2);
    avgChannelData.push_back(avg_a); // chIndex=0 -> a閸栧搫閽╅崸?
    avgChannelData.push_back(avg_b); // chIndex=1 -> b閸栧搫閽╅崸?

    // 2) 閹跺ň鈧?鐠侯垰閽╅崸鍥︿繆閸欏皝鈧繈鈧浇绻樻担鐘靛箛閺堝鐣诲▔鏇礄缁犳纭舵稉宥囨暏閺€鐧哥礉娴犲秶鍔ч崑姘姢濞?鐏忔牕鍢插Λ鈧ù瀣剁礆
    const QVector<ABAlgorithm::Result> allResults =
        m_abAlgo->analyzeEpoch(phaseIndex, timeStamps, avgChannelData);

    if (allResults.isEmpty()) {
        appendLog(phaseName + ": no stimulation candidates in this epoch");
        return;
    }

    // 3) phase->triggerSource 娑撳秴褰夐敍姘稑鐠囩补鈧粓鍘ょ純?4/25閿涘矁袝閸?/1閳ユ繐绱濋柇锝堢箹闁插瞼鎴风紒顓熼儴閻?
    //    娴犲秶鍔ч敍姝卙aseA 閸掔儤绺洪懓渚€绱禕 閻?triggerSource=1閿涙备haseB 閸掔儤绺洪懓渚€绱禔 閻?triggerSource=0
    const int triggerSource = (phaseIndex == 0) ? 1 : 0;

    // 4) 閹?a/b 閸栧搫鐓欓崘鍐茬暰閸掔儤绺洪悽鍨€崥宥呯摟閿涘牆鍙ч柨顕嗙窗娑撯偓閺夆€抽挬閸у洣淇婇崣宄邦嚠鎼存柧绔撮弽鐟板煛濠碘偓閻㈠灚鐎敍?
    auto electrodeForAvgIndex = [&](int avgIndex) -> QString {
        if (phaseIndex == 0) { // A -> 閸掔儤绺?B
            return (avgIndex == 0) ? kStim_B_a : kStim_B_b;
        } else {               // B -> 閸掔儤绺?A
            return (avgIndex == 0) ? kStim_A_a : kStim_A_b;
        }
    };

    const uint32_t epochStartTs = timeStamps.first();
    const double fs = (m_sampleRate > 0.0 ? m_sampleRate : 30000.0);

    // 5) 閸欘亙绻氶悾娆撴付鐟曚礁鍩″┑鈧惃鍕偓娆撯偓?
    QVector<ABAlgorithm::Result> candidates;
    candidates.reserve(allResults.size());
    for (const auto &r : allResults) {
        if (!r.needStim) continue;
        if (r.suggestedAmplitude_uA <= 0) continue;
        candidates.push_back(r);
    }
    if (candidates.isEmpty()) {
        appendLog(phaseName + ": 閺冪娀娓堕崚鐑樼负");
        return;
    }

    // 瀵搫瀹抽張鈧径?10 娑?
    std::sort(candidates.begin(), candidates.end(),
              [](const ABAlgorithm::Result &a, const ABAlgorithm::Result &b) {
                  return a.spikeAmplitude_uV > b.spikeAmplitude_uV;
              });
    const int maxStimPerEpoch = 10;
    if (candidates.size() > maxStimPerEpoch) candidates.resize(maxStimPerEpoch);

    // 閹稿妞傞梻瀛樺笓鎼?
    std::sort(candidates.begin(), candidates.end(),
              [](const ABAlgorithm::Result &a, const ABAlgorithm::Result &b) {
                  return a.triggerTime < b.triggerTime;
              });

    const int epochId = ++m_epochCounter;

    QVector<StimTimelineOverlay::Item> items;
    items.reserve(candidates.size());

    for (int i = 0; i < candidates.size(); ++i) {
        const auto &r = candidates[i];

        const QString targetElectrode = electrodeForAvgIndex(r.channelIndex); // 0->a閸栬櫣鏁搁弸? 1->b閸栬櫣鏁搁弸?

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
        it.ch = r.channelIndex;          // 鏉╂瑩鍣烽悳鏉挎躬閺?0/1閿涘奔鍞悰?a楠炲啿娼?b楠炲啿娼?
        it.spike_uV = r.spikeAmplitude_uV;
        it.electrode = targetElectrode;  // 閸忔娊鏁敍姘槨閺夆€茬皑娴犺泛鍟撻崗銉ヮ嚠鎼存梻娈戦崚鐑樼负閻㈠灚鐎?
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

    appendLog(QString("%1: epochId=%2 鐠佲€冲灊閸掔儤绺?%3")
                  .arg(phaseName).arg(epochId).arg(items.size()));
}


void MainWindow::handleError(const QString &msg)
{
    appendLog(QStringLiteral("Error: ") + msg);
}

void MainWindow::handleLog(const QString &msg)
{
    appendLog(msg);
}
