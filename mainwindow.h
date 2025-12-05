#pragma once

#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "acquisitionengine.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onOpenDevice();
    void onStart();
    void onStop();

    void handleNewSamples(const QVector<uint32_t> &timeStamps,
                          const QVector<QVector<int>> &channelData);

    void handleError(const QString &msg);
    void handleLog(const QString &msg);

private:
    void setupUi();
    void appendLog(const QString &msg);

private:
    QWidget          *m_central = nullptr;
    QVBoxLayout      *m_layout  = nullptr;
    QPushButton      *m_btnOpen = nullptr;
    QPushButton      *m_btnStart = nullptr;
    QPushButton      *m_btnStop = nullptr;
    QPlainTextEdit   *m_logView = nullptr;

    AcquisitionEngine *m_engine = nullptr;
};
