#include "fft_window.h"
#include "stackedwavewidget.h"

#include <QPainter>
#include <QtMath>
#include <complex>
#include <vector>

static void fft_radix2(std::vector<std::complex<double>> &a)
{
    const int n = (int)a.size();
    int j = 0;
    for (int i=1;i<n;i++){
        int bit = n>>1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (int len=2; len<=n; len<<=1){
        double ang = -2*M_PI/len;
        std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (int i=0;i<n;i+=len){
            std::complex<double> w(1.0, 0.0);
            for (int k=0;k<len/2;k++){
                auto u = a[i+k];
                auto v = a[i+k+len/2] * w;
                a[i+k] = u + v;
                a[i+k+len/2] = u - v;
                w *= wlen;
            }
        }
    }
}

FftWindow::FftWindow(StackedWaveWidget *src, QWidget *parent)
    : QWidget(parent), m_src(src)
{
    setWindowTitle("FFT");
    setWindowFlags(Qt::Tool | Qt::WindowStaysOnTopHint);
    resize(520, 260);

    connect(&m_timer, &QTimer::timeout, this, &FftWindow::tick);
    setUpdateHz(10);
}

void FftWindow::setChannel(int ch){ m_ch = qMax(0, ch); }
void FftWindow::setFftSize(int nPow2){ m_N = qMax(256, nPow2); }
void FftWindow::setMaxFreq(double hz){ m_maxFreq = qMax(10.0, hz); }

void FftWindow::setUpdateHz(int hz)
{
    hz = qBound(1, hz, 60);
    m_timer.setInterval(int(1000.0 / hz));
    m_timer.start();
}

void FftWindow::tick()
{
    compute();
    update();
}

void FftWindow::compute()
{
    if (!m_src) return;

    m_time.resize(m_N);
    if (!m_src->copySamplesForFft(m_ch, m_N, m_time)) return;

    // Hann window
    std::vector<std::complex<double>> a(m_N);
    for (int i=0;i<m_N;i++){
        double w = 0.5 - 0.5*std::cos(2*M_PI*i/(m_N-1));
        a[i] = std::complex<double>(m_time[i]*w, 0.0);
    }

    fft_radix2(a);

    const double fs = m_src->sampleRateHz();
    const int half = m_N/2;
    const int binsMax = qMin(half, int(m_maxFreq * m_N / fs));

    m_freq.resize(binsMax);
    m_magDb.resize(binsMax);

    for (int k=1;k<binsMax;k++){
        double re = a[k].real();
        double im = a[k].imag();
        double mag = std::sqrt(re*re + im*im) / m_N;
        double db = 20.0 * std::log10(mag + 1e-12);
        m_freq[k] = (fs * k) / m_N;
        m_magDb[k] = db;
    }
    if (binsMax>0){ m_freq[0]=0; m_magDb[0]=m_magDb.value(1, -120); }
}

void FftWindow::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(18,18,18));
    p.setRenderHint(QPainter::Antialiasing, true);

    p.setPen(QColor(235,235,235));
    p.drawText(8, 18, QString("ch=%1  N=%2  maxF=%3Hz").arg(m_ch).arg(m_N).arg(m_maxFreq,0,'f',0));

    const int padL=44, padR=12, padT=26, padB=22;
    const int W = width(), H = height();
    const int pw = W - padL - padR;
    const int ph = H - padT - padB;
    if (pw<20 || ph<20 || m_magDb.isEmpty()) return;

    // axes
    p.setPen(QColor(70,70,70));
    p.drawRect(padL, padT, pw, ph);

    // Y range (dB)
    double dbMin = -120, dbMax = 0;
    for (double v : m_magDb){ dbMin = qMin(dbMin, v); dbMax = qMax(dbMax, v); }
    dbMin = qMin(dbMin, -80.0);
    dbMax = qMax(dbMax, -20.0);

    auto yMap = [&](double db){
        double t = (db - dbMin) / (dbMax - dbMin + 1e-9);
        return padT + ph - int(t*ph);
    };
    auto xMap = [&](double f){
        double t = f / m_maxFreq;
        return padL + int(t*pw);
    };

    // curve
    p.setPen(QColor(0,210,170));
    QPainterPath path;
    path.moveTo(xMap(m_freq[0]), yMap(m_magDb[0]));
    for (int i=1;i<m_magDb.size();++i){
        path.lineTo(xMap(m_freq[i]), yMap(m_magDb[i]));
    }
    p.drawPath(path);

    // labels
    p.setPen(QColor(200,200,200));
    p.drawText(6, padT+12, QString("%1 dB").arg(dbMax,0,'f',0));
    p.drawText(6, padT+ph, QString("%1 dB").arg(dbMin,0,'f',0));
}
