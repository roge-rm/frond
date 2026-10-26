#pragma once
#include <QMainWindow>
#include <QSettings>
#include <QLockFile>
#include "DesktopClient.h"

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
    QCheckBox *audio, *doubleSize;
    QPlainTextEdit* log;
    QList<QAction*> runningActions;
    QAction *sdCreate, *sdImport, *sdEject, *sdReinsert, *sdExport;
    QLabel* sdStatus;
    bool cardPresent{false}, cardInserted{false};
    QWidget* setup;
    bool startupHandled{false}, busy{false}, stopping{false}, closeAfterStop{false}, allowClose{false};
};
