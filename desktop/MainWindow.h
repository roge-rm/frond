#pragma once
#include <QMainWindow>
#include <QSettings>
#include <QLockFile>
#include "DesktopClient.h"
#include "SdExchange.h"
#include "FirmwareInput.h"
class QDialog;

class QLineEdit;
class QComboBox;
class QPushButton;
class QCheckBox;
class QRadioButton;
class QPlainTextEdit;
class QLabel;
class QSpinBox;
class QSlider;
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
    QString serialArgument() const;
    void showSerial(const QString& state);
    void loadSerialSettings();
    QString bluetoothArgument() const;
    void showBluetooth(const QString& state);
    void loadBluetoothSettings();
    QString irdaArgument() const;
    void showIrda(const QString& state);
    void loadIrdaSettings();
    QString usbArgument() const;
    void showUsb(const QString& state);
    void loadUsbSettings();
    void failure(const QString& message);
    void showDevice(const QString& id);
    QString deviceLabel() const;
    frondFirmware::Device currentDevice() const;
    QString outputFile(const QString& title, const QString& filter, const QString& defaultName);
    DesktopClient client;
    QSettings settings;
    QString core, sessionPath, savingPath;
    std::unique_ptr<QLockFile> sessionLock;
    QComboBox *sessions, *network, *serial, *usb, *irda, *deviceChoice;
    QSpinBox* serialPort;
    QSpinBox* usbPort;
    QSpinBox* irdaPort;
    QLabel* irdaState;
    QLabel* serialState;
    QLabel* usbState;
    QLineEdit *nor, *nand, *bluetoothSocket;
    QCheckBox* bluetooth;
    QLabel* bluetoothState;
    QRadioButton *savedMode, *newMode;
    QPushButton *startButton, *stopButton, *forceButton;
    QCheckBox *audio, *fastAudio, *smoothLcd, *keyboard;
    QSlider* windowScale;
    QLabel* scaleLabel;
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
    // The running device selects the available peripheral controls.
    bool runningE2{false};
    QString networkTip, keyboardTip, serialTip, usbTip, irdaTip, fastAudioTip;
    QWidget* setup;
    bool startupHandled{false}, busy{false}, stopping{false}, closeAfterStop{false}, allowClose{false};
};
