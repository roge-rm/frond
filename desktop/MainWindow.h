#pragma once
#include <QMainWindow>
#include <QSettings>
#include <QLockFile>
#include "DesktopClient.h"
#include "SdExchange.h"
class QDialog;

class QLineEdit;
class QComboBox;
class QPushButton;
class QCheckBox;
class QRadioButton;
class QPlainTextEdit;
class QLabel;
class MainWindow : public QMainWindow {
    Q_OBJECT
 public:
    explicit MainWindow(const QString& core);
    void openSession(const QString& path, bool launch = false);
 protected:
    void closeEvent(QCloseEvent* event) override;
 private:
    void start();
    void chooseSession();
    void save(bool saveAs = false);
    void stop(bool closeApp);
    void askStop(const QString& message);
    void refresh();
    void beginExchange();
    void exchangeResult(bool ok, const QString& message);
    void endExchange();
    void remember(const QString& path);
    void send(const QString& command, const QString& argument = {});
    void failure(const QString& message);
    QString outputFile(const QString& title, const QString& filter, const QString& defaultName);
    DesktopClient client;
    QSettings settings;
    QString core, sessionPath, savingPath;
    std::unique_ptr<QLockFile> sessionLock;
    QComboBox *sessions, *network;
    QLineEdit *nor, *nand;
    QRadioButton *savedMode, *newMode;
    QPushButton *startButton, *stopButton, *forceButton;
    QCheckBox *audio, *fastAudio, *doubleSize, *keyboard;
    QPlainTextEdit* log;
    QList<QAction*> runningActions;
    QAction *sdCreate, *sdImport, *sdEject, *sdReinsert, *sdExport, *sdExchange;
    QLabel* sdStatus;
    enum class ExchangeStage { None, Eject, Backup, Export, Insert };
    ExchangeStage exchangeStage{ExchangeStage::None};
    std::unique_ptr<QTemporaryDir> exchangeFiles, exchangeRecovery;
    std::unique_ptr<SdExchange> exchange;
    QDialog* exchangeDialog{nullptr};
    QString exchangeBackup;
    bool cardPresent{false}, cardInserted{false};
    QWidget* setup;
    bool startupHandled{false}, busy{false}, stopping{false}, closeAfterStop{false}, allowClose{false};
};
