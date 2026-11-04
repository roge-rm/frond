#pragma once
#include <QMainWindow>
#include <QSettings>
#include <QLockFile>
#include <QProgressDialog>
#include "DesktopClient.h"
#include "SdExchange.h"
#include "FirmwareInput.h"
#include "SessionStore.h"
class QDialog;
class SettingsDialog;
class QListWidget;
class QListWidgetItem;
class QMenu;

class QLineEdit;
class QComboBox;
class QPushButton;
class QCheckBox;
class QPlainTextEdit;
class QLabel;
class QSpinBox;
class QSlider;
class MainWindow : public QMainWindow {
    Q_OBJECT
 public:
    explicit MainWindow(const QString& core);
    // Starts (or with launch false, imports) a session file; the command line and resume use it.
    void openSession(const QString& path, bool launch = false);
    // Copies a session file into the managed folder. Returns an error message, or empty.
    QString importSession(const QString& path);
 protected:
    void closeEvent(QCloseEvent* event) override;
 private:
    void start();
    void startSession(const frondSessions::Info& info);
    void launch(const QString& sessionPath, QStringList arguments, frondFirmware::Device device);
    void addDevice();
    bool ensureFirmware(frondFirmware::Device device);
    void importSessionDialog();
    void showFirmware(frondFirmware::Device device = frondFirmware::Device::automatic);
    void refreshSessions(const QString& select = {});
    void renameSession(const QString& path);
    void duplicateSession(const QString& path);
    void editSession(const QString& path);
    frondSessions::Preferences defaultPreferences(frondFirmware::Device device) const;
    QString loadPreferences(const QString& path, frondFirmware::Device device, frondSessions::Preferences* prefs);
    void exportSession(const QString& path);
    void deleteSession(const QString& path);
    QMenu* rowMenu(const QString& path, QWidget* parent);
    QString sessionFolder() const;
    QString firmwareRoot() const;
    frondSessions::Info infoFor(const QString& path) const;
    void save(bool saveAs = false);
    bool confirmReplace(const QString& dest);
    void stop(bool closeApp);
    void askStop(const QString& message);
    void refresh();
    bool memoryStick() const;
    bool storageSupported() const;
    QString cardCommand(const char* operation) const;
    void beginExchange();
    void exchangeResult(bool ok, const QString& message);
    void endExchange();
    QString sessionFile() const;
    bool send(const QString& command, const QString& argument = {});
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
    QString core, sessionPath, savingPath, pendingCopy, copyStaging;
    bool hardResetAfterSave{false};
    std::unique_ptr<QLockFile> sessionLock, copyLock;
    QComboBox *network, *serial, *usb, *irda;
    QSpinBox* serialPort;
    QSpinBox* usbPort;
    QSpinBox* irdaPort;
    QLabel* irdaState;
    QLabel* serialState;
    QLabel* usbState;
    QLineEdit* bluetoothSocket;
    QCheckBox* bluetooth;
    QLabel* bluetoothState;
    QListWidget* sessionList;
    QLabel* emptyNote;
    QPushButton* addButton;
    SettingsDialog* settingsDialog;
    QPushButton *startButton, *stopButton, *forceButton;
    QCheckBox* keyboard;
    frondSessions::Preferences runningPreferences;
    QPlainTextEdit* log;
    QList<QWidget*> runningButtons;
    QAction* importAction;
    QPushButton *sdCreate, *sdImport, *sdEject, *sdReinsert, *sdExport, *sdExchange, *sdRetry;
    QCheckBox *microphoneToggle, *soundToggle, *holdToggle, *cardReadOnly;
    bool protectionKnown{false};
    QLabel* holdLabel;
    bool holdKnown{false}, holdOn{false};
    QLabel *sdStatus, *storageHeading;
    enum class ExchangeStage { None, Eject, Backup, Export, Insert };
    ExchangeStage exchangeStage{ExchangeStage::None};
    std::unique_ptr<QTemporaryDir> exchangeFiles, exchangeRecovery;
    std::unique_ptr<SdExchange> exchange;
    QDialog* exchangeDialog{nullptr};
    QDialog* memoryStickFiles{nullptr};
    QProgressDialog* cardProgress{nullptr};  // shown while an SD create, import or export runs
    QString exchangeBackup;
    bool cardPresent{false}, cardInserted{false}, sdKnown{false};
    bool msExportPending{false};
    // The running device selects the available peripheral controls.
    frondFirmware::Device runningDevice{frondFirmware::Device::tx};
    bool microphoneArmed{false};
    QLabel* microphoneState;
    QString networkTip, keyboardTip, serialTip, usbTip, irdaTip;
    QWidget *home, *runningPage, *sessionBar = nullptr;
    bool startupHandled{false}, busy{false}, stopping{false}, closeAfterStop{false}, allowClose{false};
};
