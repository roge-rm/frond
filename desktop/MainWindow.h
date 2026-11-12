#pragma once
#include <QJsonObject>
#include <QMainWindow>
#include <QSettings>
#include <QLockFile>
#include <QPointer>
#include <QProgressDialog>
#include "DesktopClient.h"
#include "DanaViewController.h"
#include "SdExchange.h"
#include "FirmwareInput.h"
#include "SessionStore.h"
#include "LinkClient.h"
#include "RunningBar.h"
#include "RunningPanels.h"
#include "PalmView.h"
#include "PrinterPanel.h"
#include "PrintJob.h"
#include <QTemporaryDir>
class ConnectionsWindow;
class CardsWindow;
class CardFilesWindow;
class QFileSystemWatcher;
class QDialog;
class QMessageBox;
class SettingsDialog;
class QHBoxLayout;
class QTimer;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QMimeData;
class QDragEnterEvent;
class QDropEvent;

class QLineEdit;
class QComboBox;
class QPushButton;
class QCheckBox;
class QPlainTextEdit;
class QLabel;
class QSpinBox;
class QSlider;
class DanaViewControls;
class MainWindow : public QMainWindow {
    Q_OBJECT
 public:
    // 'core68k' runs the DragonBall devices; empty finds it beside 'core' (frond-68k, frond-68k-core).
    explicit MainWindow(const QString& core, const QString& core68k = {});
    // frond-68k for DragonBall devices, else the ARM core.
    QString coreFor(frondFirmware::Device device) const;
    // Starts (or with launch false, imports) a session file; the command line and resume use it.
    void openSession(const QString& path, bool launch = false);
    // Copies a session file into the managed folder. Returns an error message, or empty.
    QString importSession(const QString& path);
    // Installs the files on the running device one after the other, in order. A failure doesn't
    // stop the rest; the failures are listed together at the end.
    void installFiles(const QStringList& paths);
 protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    bool event(QEvent* event) override;
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
    bool installNext();
    bool installResult(bool ok, const QString& code, const QString& message);
    QString serialArgument() const;
    void showSerial(const QString& state);
    void loadSerialSettings();
    QString bluetoothArgument() const;
    void showBluetooth(const QString& state);
    void loadBluetoothSettings();
    // off, radio (This Frond), socket:/abs, or link:<id>. This Frond and connections go through
    // the hub, which listens for the core on a socket per window.
    QString bluetoothChoice() const;
    bool bluetoothViaHub() const;
    void attachBluetooth();
    void applyBluetoothLink();
    void showBluetoothLink(const QJsonObject& status);
    QString irdaArgument() const;
    void showIrda(const QString& state);
    void loadIrdaSettings();
    QString usbArgument() const;
    void showUsb(const QString& state);
    void loadUsbSettings();
    // The Dana's USB printer. A received job is saved out of the core into printerHeld, so the
    // choices work on the bytes in hand; more printing adds to them.
    enum class PrinterStage { None, Collect, Discard, Attach, Chain };
    void printerPoll();
    void usbPortReply(bool switched, const QString& message);
    bool usbPortResync{false};
    // The start-up USB port choice and the state read after it are in flight.
    bool usbPortStart{false};
    // The Dana Wireless's radio choice goes to the core once at start (moduleStartSent), ahead of the USB port.
    bool moduleStartSent{false}, moduleStartPending{false};
    // One source of print jobs: the USB printer (command printer) or the infrared printer (irprinter).
    struct PrintSource {
        QString command;
        PrinterPanel* panel{nullptr};
        QByteArray held;
        frondPrint::Status status;
        PrinterStage stage{PrinterStage::None};
        std::unique_ptr<QTemporaryDir> dir;
    };
    bool printerResult(PrintSource& src, bool ok, const QString& message);
    void printerCollect(PrintSource& src);
    void printerShow(PrintSource& src);
    void printerClearHeld(PrintSource& src);
    void printerSavePdf(PrintSource& src);
    void printerPrint(PrintSource& src);
    void printerSaveRaw(PrintSource& src);
    frond::PrintText printerText(const PrintSource& src) const;
    // Asks before the infrared printer's unsaved output is thrown away by another choice.
    bool leavePrinterOk();
    // Which wanted source printerPoll asks next.
    unsigned pollTurn{0};
    // Peripheral choices are per machine, in its settings file; machines from before take the
    // old global choices once.
    void migratePeripherals(frondSessions::Preferences* prefs);
    void savePeripherals();
    QString irdaChoice() const;  // off, pty, tcp:PORT, link:local or link:<id>
    void refreshIrdaChoices();
    void attachInfrared();
    void showIrdaLink(const QJsonObject& status);
    QString serialChoice() const;  // off, pty, tcp:PORT, link:local or link:<id>
    void attachSerial();
    void showSerialLink(const QJsonObject& status);
    void showConnections();
    void failure(const QString& message, const QString& details = {});
    // A status message: beside the name in the bar while a machine runs, else in the status bar.
    void notify(const QString& text, int ms = 5000);
    void barChosen(runningRules::Id id);
    void barPressed(RunningBar::Button button);
    void runShortcut(runningRules::Shortcut what);
    void togglePanel(int page);
    void panelChanged(int page);
    void fitWindow();
    // The core's reply to window-id: embeds the Palm, or keeps its own window.
    void embedReply(bool ok, const QString& message);
    void afterDevice();
    void setBarShown(bool shown);
    // Full screen: the Palm fills the screen, the bar and an open panel float over it.
    void toggleFullScreen();
    void enterFullScreen();
    void leaveFullScreen();
    void showOverlayBar(bool on);
    void placeOverlays();
    void setPanelOpen(bool open);
    void liveDisplay(const QString& what);
    void updateMips();
    void pollMips();
    void listApps();
    void databasesReply(bool ok, const QString& message);
    void deleteApplication(const QString& name);
    void deleteLookup(bool ok, const QString& message, const std::vector<runningRules::Database>& all);
    void deleteNext();
    void deleteReply(bool ok, const QString& message);
    // Internal on the Storage panel: Files… on the running machine's RAM databases, each change one
    // queued run of install, export-db or delete-db.
    void openInternal();
    void listInternal();
    void startInternal(const QString& kind, const QList<QPair<QString, QString>>& commands, const QString& folder = {});
    void internalNext();
    void internalReply(bool ok, const QString& message);
    void internalDone();
    void exportDatabase(const QString& name);
    void otherDatabase();
    QStringList installPaths(const QMimeData* data) const;
    // Runs `lines` as an input replay, for Screenshot and Power.
    bool replay(const QString& lines, const QString& kind);
    void screenshot();
    void showDevice(const QString& id);
    QString deviceLabel() const;
    frondFirmware::Device currentDevice() const;
    QString outputFile(const QString& title, const QString& filter, const QString& defaultName);
    DesktopClient client;
    DanaViewController danaView{client, this};
    DanaViewControls* danaControls;
    LinkClient link;
    QPointer<ConnectionsWindow> connectionsWindow;
    RunningBar* runningBar{nullptr};
    PanelHost* panels{nullptr};
    AppsPanel* appsPanel{nullptr};
    QWidget* drivers{nullptr};  // hidden home of the controls the bar and the menu use
    QWidget* runArea{nullptr};  // the handle strip, the Palm and the panels
    PalmHolder* palmHolder{nullptr};
    BarHandle* barHandle{nullptr};
    QHBoxLayout* bodyLayout{nullptr};  // the Palm and, outside full screen, the panel
    QTimer* overlayTimer{nullptr};     // hides the bar 3 s after the pointer leaves it
    QTimer* edgeTimer{nullptr};        // watches for the pointer at the top edge while the bar is hidden
    bool fullScreenOn{false}, overlayBarShown{false};
    bool fullScreenWasMaximized{false};
    QSize fullScreenReturnSize;  // the window's size to go back to, without a panel
    bool embedWanted{false}, embedTried{false}, embedded{false};
    bool fitOffPending{false};  // `display fit=none` sent after the embedding failed
    bool panelWasOpen{false};
    int baseWidth{0};  // the window's width with no panel open, while the Palm is embedded
    bool contrastPending{false};
    QCheckBox *smoothToggle{nullptr}, *mipsToggle{nullptr}, *keyboardShow{nullptr};
    QSlider* sizeSlider{nullptr};
    QComboBox *jogBox{nullptr}, *speedBox{nullptr};
    QLabel *sizeLabel{nullptr}, *jogLabel{nullptr}, *speedLabel{nullptr};
    QByteArray listGeometry;    // the list window's geometry while a machine runs
    std::vector<runningRules::Database> knownDatabases;
    bool appsStale{false};      // the Applications list needs a fresh db-list
    struct DeleteApp {
        bool active{false}, lookup{false}, first{false};
        QString name;
        QStringList queue, failed;
    } deleteApp;
    QPointer<CardFilesWindow> internalFiles;
    QLabel* internalHeading{nullptr};
    QPushButton* internalButton{nullptr};
    bool internalListWanted{false};
    struct InternalJob {
        bool active{false};
        QString kind, folder;  // "install", "export" or "delete"; where an export goes
        QList<QPair<QString, QString>> queue;
        QString current;
        QStringList failed, zips;
        int done{0};
        std::shared_ptr<QTemporaryDir> temp;
    } internalJob;
    QPushButton *saveButton{nullptr}, *saveCopyButton{nullptr}, *logButton{nullptr}, *resetButton{nullptr}, *hardResetButton{nullptr};
    QMenu* helpMenu{nullptr};
    bool machineMenuShown{false};
    std::unique_ptr<QTemporaryDir> replayFiles;  // the replay file the core is reading
    QString replayKind, replayShot;              // "screenshot" or "power", and the screenshot's file name
    bool replayMicrophone{false};                // the microphone was on, and the replay turned it off
    QFileSystemWatcher* storeWatcher{nullptr};
    QString irdaSocket;   // the core's IR socket while Infrared uses a connection
    QString irdaLinkText; // the hub's status for it
    QString serialLinkText; // the same for Serial
    QString bluetoothLinkText; // and for Bluetooth through the hub
    QJsonObject bluetoothAttach;  // the bt-attach sent, so an unchanged one isn't sent again
    bool bluetoothListening{false};  // the hub listens on this window's socket
    QString bluetoothCore{"off"};  // what the core's Bluetooth is set to now
    QString bluetoothCoreChoice{"off"};  // the choice that gave it
    QString bluetoothSent;  // the bluetooth-mode argument waiting for its reply
    bool irdaQueued = false;  // an infrared choice waiting for the core to finish a request
    QSettings settings;
    QString core, core68k, sessionPath, savingPath, pendingCopy, copyStaging;
    bool hardResetAfterSave{false};
    std::unique_ptr<QLockFile> sessionLock, copyLock;
    QComboBox *network, *serial, *usb, *irda;
    QSpinBox* serialPort;
    QSpinBox* usbPort;
    QSpinBox* irdaPort;
    QLabel* irdaState;
    QLabel* serialState;
    QLabel* usbState;
    PrinterPanel* printerPanel;
    QTimer* printerTimer;
    QTimer* mipsTimer;
    PrintSource usbPrint, irPrint;
    QLineEdit* bluetoothSocket;
    QComboBox* bluetooth;
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
    QPushButton *cardPick, *sdEject, *sdReinsert, *sdExport, *sdExchange, *sdRetry, *linkedInsert;
    QLabel* linkedState;
    QCheckBox *microphoneToggle, *soundToggle, *holdToggle, *cardReadOnly;
    QLabel *serialLabel, *networkLabel, *keyboardLabel, *usbLabel, *bluetoothLabel, *microphoneLabel, *contrastLabel;
    QSlider* contrast;
    QPushButton *rotateButton, *hotSyncButton;
    bool protectionKnown{false};
    QLabel* holdLabel;
    bool holdKnown{false}, holdOn{false};
    QLabel *sdStatus, *storageHeading;
    QPushButton* danaCards;
    // Files… on the running machine's SD card: eject, save, take the card out into the machine
    // folder's sd.frondsd, edit it there in place, then put it back in (ejected again if it was out).
    enum class ExchangeStage { None, Eject, Save, TakeOut, Open, PutIn, EjectAgain };
    ExchangeStage exchangeStage{ExchangeStage::None};
    QString exchangeIdentity;           // what the take-out reported, for the put-in
    bool exchangeProtected{false}, exchangeWasInserted{false};
    QPointer<CardFilesWindow> exchangeDialog;
    void openExchangeCard();
    void exchangeClosed();
    QStringList installQueue, installFailed;  // files waiting, and "name: reason" for those that failed
    int installTotal{0}, installHint{0};
    QString installCurrent;
    QProgressDialog* cardProgress{nullptr};  // shown while an SD create, import or export runs
    QPointer<QMessageBox> failureBox;  // the error showing now; later errors are added to it
    bool cardPresent{false}, cardInserted{false}, sdKnown{false};
    bool msExportPending{false};
    // The running device selects the available peripheral controls.
    frondFirmware::Device runningDevice{frondFirmware::Device::tx};
    bool microphoneArmed{false};
    QLabel* microphoneState;
    QComboBox* cameraBox; QLabel *cameraLabel, *cameraState;
    bool cameraRead{false};  // camera-state is to be asked once the running commands are done
    QString cameraWebcam;  // the webcam last chosen
    void fillCamera();
    void showCamera(const QString& state);
    void chooseCamera(int index);
    QString networkTip, keyboardTip, serialTip, usbTip, irdaTip;
    QWidget* home;
    bool startupHandled{false}, busy{false}, stopping{false}, closeAfterStop{false}, allowClose{false};
    // Cards: the final save before quit succeeded, so a linked card can go back to the drawer.
    bool finalSaved{false};
    // A linked card held by another machine while this one runs with an empty slot.
    std::unique_ptr<frondCards::FreeWatcher> linkedWatcher;
    QString linkedHolder, linkedWatcherId;
    // A card change on the running machine: eject, take out to the drawer, put the next one in.
    struct CardMove {
        enum Stage { None, Eject, TakeOut, PutIn } stage{None};
        QString in;  // the card going in, empty for none
    } cardMove;
    QPointer<CardsWindow> cardsWindow;
    QString runningMachineDir() const;
    QString runningCardName() const;
    void pickCard();
    void changeCard(const QString& in);
    void putInCard(const QString& in);
    bool continueCardMove(const QString& command, const QString& message);
    void cardMoveFailed(const QString& command, const QString& message);
 public:
    // File > Cards: one window, raised if open.
    CardsWindow* showCards();
    // Save a Copy to `path` once its file dialog has picked it.
    void saveCopyTo(const QString& path);
    // Tests: called on the export's worker before it copies anything.
    std::function<void()> exportHook;
    // Whether the running machine's linked card is in the drawer now (C5 shows "Insert").
    bool linkedCardFree() const { return linkedWatcher && linkedWatcher->lastState(); }
};
