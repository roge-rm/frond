#include "MainWindow.h"
#include "FirmwareInput.h"
#include "SdExchangeDialog.h"
#include <QAction>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QScreen>
#include <QSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

MainWindow::MainWindow(const QString& core) : core(core) {
    setWindowTitle(tr("Frond"));
    resize(620, 380);
    auto* root = new QWidget(this);
    auto* layout = new QVBoxLayout(root);
    setup = new QGroupBox(tr("Palm TX"), root);
    auto* form = new QFormLayout(setup);
    savedMode = new QRadioButton(tr("Open a saved session"), setup);
    savedMode->setChecked(true);
    newMode = new QRadioButton(tr("Start with firmware images"), setup);
    form->addRow(savedMode);
    sessions = new QComboBox(setup);
    sessions->setObjectName("sessions");
    sessions->setEditable(true);
    sessions->addItems(settings.value("recentSessions").toStringList());
    auto* sessionRow = new QHBoxLayout;
    sessionRow->addWidget(sessions, 1);
    auto* browse = new QPushButton(tr("Browse…"), setup);
    sessionRow->addWidget(browse);
    form->addRow(tr("Session:"), sessionRow);
    connect(browse, &QPushButton::clicked, this, &MainWindow::chooseSession);
    form->addRow(newMode);
    auto firmware = [&](const QString& label, const QString& key) {
        auto* row = new QHBoxLayout;
        auto* field = new QLineEdit(settings.value(key).toString(), setup);
        auto* button = new QPushButton(tr("Browse…"), setup);
        row->addWidget(field, 1); row->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, field, label] {
            const auto file = QFileDialog::getOpenFileName(this, label, field->text(), tr("Firmware images (*.bin *.rom);;All files (*)"));
            if (!file.isEmpty()) { field->setText(file); newMode->setChecked(true); }
        });
        form->addRow(label, row);
        return field;
    };
    nor = firmware(tr("NOR image:"), "nor");
    nand = firmware(tr("NAND image:"), "nand");
    layout->addWidget(setup);
    doubleSize = new QCheckBox(tr("Double-size Palm display (when it fits)"), root);
    doubleSize->setToolTip(tr("Uses normal size if the Palm display and buttons would exceed the available screen height."));
    const auto* screen = QGuiApplication::primaryScreen();
    const bool fits = screen && screen->availableGeometry().height() >= 1180;
    doubleSize->setChecked(settings.value("doubleSize", fits).toBool());
    connect(doubleSize, &QCheckBox::toggled, this, [this](bool enabled) { settings.setValue("doubleSize", enabled); });
    layout->addWidget(doubleSize);
    audio = new QCheckBox(tr("Enable sound"), root);
    audio->setChecked(settings.value("audio", true).toBool());
    connect(audio, &QCheckBox::toggled, this, [this](bool enabled) {
        settings.setValue("audio", enabled);
        if (client.available()) send(enabled ? "audio-on" : "audio-off");
    });
    layout->addWidget(audio);
    fastAudio = new QCheckBox(tr("Experimental low-latency audio"), root);
    fastAudio->setObjectName("fastAudio");
    fastAudio->setToolTip(tr("Applies on the next start. Supports one 44.1 kHz stereo stream with compatible Palm TX firmware; unsupported audio falls back to normal playback and may briefly pause."));
    fastAudio->setChecked(settings.value("fastAudio", false).toBool());
    connect(fastAudio, &QCheckBox::toggled, this, [this](bool enabled) { settings.setValue("fastAudio", enabled); });
    layout->addWidget(fastAudio);
    sdStatus = new QLabel(tr("SD card: Not running"), root);
    sdStatus->setObjectName("sdState");
    layout->addWidget(sdStatus);
    network = new QComboBox(root);
    network->setObjectName("networkMode");
    network->addItem(tr("Network: Disabled"), "disabled");
    network->addItem(tr("Network: Virtual Wi-Fi (Frond)"), "wireless");
    network->addItem(tr("Network: Internet via host"), "internet");
    network->addItem(tr("Network: Simulated echo server"), "connected");
    network->addItem(tr("Network: Simulated connection timeout"), "timeout");
    network->addItem(tr("Network: Simulated disconnection"), "disconnected");
    network->setToolTip(tr("Virtual Wi-Fi uses the Palm networking stack and a Frond access point. Internet via host forwards a subset of 68k NetLib calls. Changing mode closes connections."));
    connect(network, qOverload<int>(&QComboBox::activated), this, [this](int index) {
        send("network-mode", network->itemData(index).toString());
    });
    layout->addWidget(network);
    keyboard = new QCheckBox(tr("Attach Palm infrared keyboard"),root);
    keyboard->setObjectName("keyboardAttached");
    keyboard->setToolTip(tr("Uses the Palm Universal Wireless Keyboard protocol. Install and enable its Palm OS driver first. Type in the Palm window; modifiers and key releases are sent to the driver."));
    connect(keyboard,&QCheckBox::clicked,this,[this](bool attached) { send("keyboard-attach",attached?"on":"off"); });
    layout->addWidget(keyboard);
    auto* serialRow = new QHBoxLayout;
    serial = new QComboBox(root);
    serial->setObjectName("serialMode");
    serial->addItem(tr("Serial port: Off"), "off");
    serial->addItem(tr("Serial port: PTY"), "pty");
    serial->addItem(tr("Serial port: TCP on 127.0.0.1"), "tcp");
    serialPort = new QSpinBox(root);
    serialPort->setObjectName("serialPort");
    serialPort->setRange(1, 65535);
    serialPort->setPrefix(tr("Port "));
    loadSerialSettings();
    serialRow->addWidget(serial, 1);
    serialRow->addWidget(serialPort);
    layout->addLayout(serialRow);
    serialState = new QLabel(root);
    serialState->setObjectName("serialState");
    serialState->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(serialState);
    // While running, the choice is saved only once the core has opened it.
    auto serialChanged = [this] {
        if (client.running()) {
            if (client.available()) send("serial-mode", serialArgument());
            return;
        }
        settings.setValue("serialMode", serial->currentData());
        settings.setValue("serialPort", serialPort->value());
        refresh();
    };
    connect(serial, qOverload<int>(&QComboBox::activated), this, serialChanged);
    connect(serialPort, &QSpinBox::editingFinished, this, [this, serialChanged] {
        if (client.running() ? serial->currentData() == "tcp" : serialPort->value() != settings.value("serialPort", 5023).toInt())
            serialChanged();
    });
    showSerial({});
    auto* usbRow = new QHBoxLayout;
    usb = new QComboBox(root);
    usb->setObjectName("usbMode");
    usb->addItem(tr("USB: Off"), "off");
    usb->addItem(tr("USB: PTY"), "pty");
    usb->addItem(tr("USB: TCP on 127.0.0.1"), "tcp");
    usb->setToolTip(tr("Plugs the TX into a host that moves its HotSync bytes to a PTY or TCP port, for pilot-link and the like."));
    usbPort = new QSpinBox(root);
    usbPort->setObjectName("usbPort");
    usbPort->setRange(1, 65535);
    usbPort->setPrefix(tr("Port "));
    loadUsbSettings();
    usbRow->addWidget(usb, 1);
    usbRow->addWidget(usbPort);
    layout->addLayout(usbRow);
    usbState = new QLabel(root);
    usbState->setObjectName("usbState");
    usbState->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(usbState);
    auto usbChanged = [this] {
        if (client.running()) {
            if (client.available()) send("usb-mode", usbArgument());
            return;
        }
        settings.setValue("usbMode", usb->currentData());
        settings.setValue("usbPort", usbPort->value());
        refresh();
    };
    connect(usb, qOverload<int>(&QComboBox::activated), this, usbChanged);
    connect(usbPort, &QSpinBox::editingFinished, this, [this, usbChanged] {
        if (client.running() ? usb->currentData() == "tcp" : usbPort->value() != settings.value("usbPort", 5024).toInt())
            usbChanged();
    });
    showUsb({});
    auto* resume = new QCheckBox(tr("Resume last session on startup"), root);
    resume->setObjectName("resume");
    resume->setChecked(settings.value("resume", true).toBool());
    connect(resume, &QCheckBox::toggled, this, [this](bool enabled) { settings.setValue("resume", enabled); });
    layout->addWidget(resume);
    auto* buttons = new QHBoxLayout;
    startButton = new QPushButton(tr("Start"), root);
    startButton->setObjectName("start"); startButton->setDefault(true);
    stopButton = new QPushButton(tr("Save and Stop"), root);
    stopButton->setObjectName("stop");
    forceButton = new QPushButton(tr("Force Stop…"), root);
    forceButton->hide();
    buttons->addStretch(); buttons->addWidget(forceButton); buttons->addWidget(stopButton); buttons->addWidget(startButton);
    layout->addLayout(buttons);
    setCentralWidget(root);
    connect(startButton, &QPushButton::clicked, this, &MainWindow::start);
    connect(stopButton, &QPushButton::clicked, this, [this] { stop(false); });
    connect(forceButton, &QPushButton::clicked, this, [this] {
        if (QMessageBox::warning(this, tr("Force Stop"), tr("Stop without saving? Changes since the last save will be lost."),
                                 QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Discard) {
            allowClose = closeAfterStop; client.forceStop();
        }
    });
    auto* file = menuBar()->addMenu(tr("&File"));
    auto* open = file->addAction(tr("&Open Session…"), this, &MainWindow::chooseSession);
    open->setShortcut(QKeySequence::Open);
    auto* saveAction = file->addAction(tr("&Save Session"), this, [this] { save(); });
    saveAction->setShortcut(QKeySequence::Save);
    auto* copy = file->addAction(tr("Save a &Copy…"), this, [this] { save(true); });
    file->addSeparator();
    auto* install = file->addAction(tr("&Install Application…"), this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("Install Application"), {}, tr("Palm databases (*.prc *.pdb);;All files (*)"));
        if (!path.isEmpty()) send("install", QFileInfo(path).absoluteFilePath());
    });
    auto* launch = file->addAction(tr("&Launch Application…"), this, [this] {
        bool ok;
        const auto name = QInputDialog::getText(this, tr("Launch Application"), tr("Palm database name:"), QLineEdit::Normal, {}, &ok);
        if (ok && !name.isEmpty()) send("launch-app", name);
    });
    auto* exportAction = file->addAction(tr("&Export RAM Databases…"), this, [this] {
        const auto path = outputFile(tr("Export RAM Databases"), tr("ZIP archives (*.zip)"), "Palm databases.zip");
        if (!path.isEmpty()) send("export", path);
    });
    auto* exportDatabase = file->addAction(tr("Export &Database…"), this, [this] {
        bool ok;
        const auto name=QInputDialog::getText(this,tr("Export Database"),tr("Palm database name:"),QLineEdit::Normal,{},&ok);
        if(!ok || name.isEmpty())return;
        const auto path=outputFile(tr("Export Database"),tr("ZIP archives (*.zip)"),name+".zip");
        if(!path.isEmpty())send("export-db",path+"\n"+name);
    });
    file->addSeparator();
    file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);
    auto* device = menuBar()->addMenu(tr("&Device"));
    auto* reset = device->addAction(tr("&Reset…"), this, [this] {
        if (QMessageBox::question(this, tr("Reset Palm TX"), tr("Reset the Palm TX now?")) == QMessageBox::Yes) send("reset");
    });
    auto* rotate = device->addAction(tr("&Rotate"), this, [this] { send("rotate"); });
    runningActions = {saveAction, copy, install, launch, exportAction, exportDatabase, reset, rotate};
    auto* cardMenu = menuBar()->addMenu(tr("&SD Card"));
    const auto replaceCard = [this] {
        return !cardPresent || QMessageBox::question(this, tr("Replace SD Card"),
            tr("Replace the ejected card? Export its image first if you want to keep its contents."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
    };
    sdCreate = cardMenu->addAction(tr("&Create Card…"), this, [this, replaceCard] {
        if (!replaceCard()) return;
        bool ok;
        const auto size = QInputDialog::getItem(this, tr("Create SD Card"), tr("Capacity (MiB):"),
                                              {"32", "64", "128"}, 0, false, &ok);
        if (ok) send("sd-create", size);
    });
    sdImport = cardMenu->addAction(tr("Insert &Image…"), this, [this, replaceCard] {
        if (!replaceCard()) return;
        const auto file = QFileDialog::getOpenFileName(this, tr("Insert SD Image"), {},
                                                     tr("SD images (*.img *.bin);;All files (*)"));
        if (!file.isEmpty()) send("sd-insert", QFileInfo(file).absoluteFilePath());
    });
    sdEject = cardMenu->addAction(tr("&Eject"), this, [this] {
        if (QMessageBox::question(this, tr("Eject SD Card"), tr("Close files on the Palm before ejecting. Eject now?"),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) send("sd-eject");
    });
    sdReinsert = cardMenu->addAction(tr("&Reinsert"), this, [this] { send("sd-reinsert"); });
    sdExport = cardMenu->addAction(tr("&Export Image…"), this, [this] {
        const auto file = outputFile(tr("Export SD Image"), tr("SD images (*.img)"), "Palm SD.img");
        if (!file.isEmpty()) send("sd-export", file);
    });
    cardMenu->addSeparator();
    sdExchange = cardMenu->addAction(tr("Exchange &Files…"), this, &MainWindow::beginExchange);
    sdExchange->setObjectName("sdExchange");
    auto* view = menuBar()->addMenu(tr("&View"));
    auto* logDialog = new QDialog(this);
    logDialog->setWindowTitle(tr("Emulator Log")); logDialog->resize(700, 400);
    auto* logLayout = new QVBoxLayout(logDialog);
    log = new QPlainTextEdit(logDialog); log->setReadOnly(true); log->setMaximumBlockCount(1000);
    logLayout->addWidget(log);
    view->addAction(tr("Emulator &Log"), logDialog, &QWidget::show);
    auto* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("Hardware &Keyboard Controls"), this, [this] {
        QMessageBox::information(this, tr("Hardware Keyboard Controls"),
            tr("With the Palm window focused and the infrared keyboard detached:\n\n"
               "Arrow keys: five-way navigation\nEnter: centre select\n"
               "F1: Home\nF2: Calendar\nF3: Contacts\nF4: Web\n"
               "Escape: Power (sleep and wake are experimental)\n\n"
               "Palm OS button assignments may change what the application buttons do."));
    });
    help->addAction(tr("&About Frond"), this, [this] {
        QMessageBox::about(this, tr("About Frond"), tr("Frond 0.3.0\nExperimental Palm TX emulator\nGPL-3.0-or-later"));
    });
    connect(&client, &DesktopClient::diagnostic, this, [this](const QString& text) {
        log->moveCursor(QTextCursor::End); log->insertPlainText(text);
    });
    connect(&client, &DesktopClient::error, this, &MainWindow::failure);
    connect(&client, &DesktopClient::stalled, this, [this] {
        forceButton->show(); statusBar()->showMessage(tr("The emulator is not responding. Waiting for it to finish…"));
    });
    connect(&client, &DesktopClient::started, this, [this] {
        busy = false; statusBar()->showMessage(tr("Running. Your session will be saved when you close Frond.")); refresh();
        if (stopping) save();
        else send("sd-state");
    });
    connect(&client, &DesktopClient::finished, this, [this, open] {
        if (exchangeDialog) exchangeDialog->reject();
        else endExchange();
        busy = false; stopping = false; cardPresent = cardInserted = false;
        sdStatus->setText(tr("SD card: Not running")); showSerial({}); loadSerialSettings(); showUsb({}); loadUsbSettings(); sessionLock.reset(); forceButton->hide(); refresh(); open->setEnabled(true);
        if (closeAfterStop) { allowClose = true; close(); }
        else statusBar()->showMessage(tr("Stopped."));
    });
    connect(&client, &DesktopClient::closeRequested, this, [this] { stop(true); });
    connect(&client, &DesktopClient::serialState, this, [this](const QString& state) { showSerial(state); });
    connect(&client, &DesktopClient::usbState, this, [this](const QString& state) { showUsb(state); });
    connect(&client, &DesktopClient::result, this, [this](const QString& command, bool ok, const QString& code, const QString& message) {
        busy = false; forceButton->hide(); refresh();
        if (!ok && exchangeStage != ExchangeStage::None) { exchangeResult(false, message); return; }
        if (!ok) {
            log->appendPlainText(command + ": " + message);
            if (command == "serial-mode") showSerial("off " + message);  // the core is now off
            if (command == "usb-mode") showUsb("off " + message);
            if (stopping) {
                if (command == "save" && savingPath == sessionPath) askStop(message);
                else save();
                return;
            }
            if (code == "not-ready" && (command == "export" || command == "export-db" || command == "install" || command == "launch-app"))
                failure(message + tr("\n\nWait for Palm OS to finish its current activity, then try again."));
            else failure(message);
            return;
        }
        if (command == "sd-state") { cardPresent = message != "none"; cardInserted = message == "inserted"; }
        else if (command == "sd-create" || command == "sd-insert" || command == "sd-reinsert") cardPresent = cardInserted = true;
        else if (command == "sd-eject") cardInserted = false;
        sdStatus->setText(!cardPresent ? tr("SD card: No card") : cardInserted ? tr("SD card: Inserted") : tr("SD card: Ejected"));
        if (exchangeStage != ExchangeStage::None) { exchangeResult(true, message); return; }
        if (command == "network-state" || command == "network-mode") {
            const auto index = network->findData(message);
            if (index >= 0) network->setCurrentIndex(index);
        }
        if (command=="keyboard-state" || command=="keyboard-attach") keyboard->setChecked(message=="attached");
        if (command == "serial-mode") {
            settings.setValue("serialMode", serial->currentData());
            if (serial->currentData() == "tcp") settings.setValue("serialPort", serialPort->value());
        }
        if (command == "serial-state" || command == "serial-mode") showSerial(message);
        if (command == "usb-mode") {
            settings.setValue("usbMode", usb->currentData());
            if (usb->currentData() == "tcp") settings.setValue("usbPort", usbPort->value());
        }
        if (command == "usb-state" || command == "usb-mode") showUsb(message);
        refresh();
        if (command == "network-state" && !stopping) { send("keyboard-state"); return; }
        if (command == "keyboard-state" && !stopping && serialArgument() != "off") { send("serial-state"); return; }
        if ((command == "keyboard-state" || command == "serial-state") && !stopping && usbArgument() != "off") { send("usb-state"); return; }
        if (command == "sd-state" && !stopping) { send("network-state"); return; }
        statusBar()->showMessage(command == "sd-state" ? tr("Running. Sessions save automatically on close.") : message);
        const bool savedWorkingCopy = command == "save" && savingPath == sessionPath;
        if (command == "save") {
            if (savedWorkingCopy) {
                remember(sessionPath); settings.setValue("lastSession", sessionPath); settings.sync();
            }
            savingPath.clear();
        }
        if (stopping && command != "quit") {
            if (savedWorkingCopy) send("quit");
            else save();
        }
    });
    connect(&client, &DesktopClient::started, open, [open] { open->setEnabled(false); });
    refresh();
    statusBar()->showMessage(tr("Ready. Sessions are saved automatically when you close Frond."));
    QTimer::singleShot(0, this, [this] {
        if (startupHandled) return;
        startupHandled = true;
        const auto last = settings.value("lastSession").toString();
        if (settings.value("resume", true).toBool() && QFileInfo::exists(last)) openSession(last, true);
    });
}
void MainWindow::refresh() {
    const bool running = client.running();
    setup->setEnabled(!running && !busy);
    startButton->setEnabled(!running && !busy);
    stopButton->setEnabled(running && !busy && !stopping);
    audio->setEnabled(!busy && !stopping);
    fastAudio->setEnabled(!running && !busy);
    doubleSize->setEnabled(!running && !busy);
    const bool enabled = client.available() && !busy && !stopping && exchangeStage == ExchangeStage::None;
    for (auto* action : runningActions) action->setEnabled(enabled);
    network->setEnabled(enabled);
    keyboard->setEnabled(enabled);
    const bool serialEditable = !busy && !stopping && (!running || enabled);
    serial->setEnabled(serialEditable);
    serialPort->setEnabled(serialEditable && serial->currentData() == "tcp");
    usb->setEnabled(serialEditable);
    usbPort->setEnabled(serialEditable && usb->currentData() == "tcp");
    sdCreate->setEnabled(enabled && !cardInserted); sdImport->setEnabled(enabled && !cardInserted);
    sdEject->setEnabled(enabled && cardInserted); sdReinsert->setEnabled(enabled && cardPresent && !cardInserted);
    sdExport->setEnabled(enabled && cardPresent && !cardInserted);
    sdExchange->setEnabled(enabled && cardPresent);
}
void MainWindow::openSession(const QString& path, bool launch) {
    startupHandled = true;
    if (client.running()) return;
    savedMode->setChecked(true);
    sessions->setCurrentText(QFileInfo(path).absoluteFilePath());
    if (launch) QTimer::singleShot(0, this, &MainWindow::start);
}
void MainWindow::chooseSession() {
    if (client.running()) return;
    const auto path = QFileDialog::getOpenFileName(this, tr("Open Session"), sessions->currentText(), tr("Frond sessions (*.frond *.session);;All files (*)"));
    if (!path.isEmpty()) openSession(path);
}
void MainWindow::start() {
    if (client.running()) return;
    startupHandled = true;
    if (!QFileInfo(core).isExecutable()) return failure(tr("The Frond emulator executable could not be found. Reinstall Frond or build both desktop and core targets."));
    QStringList arguments;
    QString source;
    if (savedMode->isChecked()) {
        source = QFileInfo(sessions->currentText()).absoluteFilePath();
        if (sessions->currentText().isEmpty() || !QFileInfo(source).isFile()) return failure(tr("Choose an existing session file."));
        source = QFileInfo(source).canonicalFilePath();
        arguments << source;
    } else {
        const auto firmwareError = frondFirmware::validateTxFiles(nor->text(), nand->text());
        if (!firmwareError.isEmpty()) return failure(firmwareError);
        arguments << "--experimental-tx" << QFileInfo(nor->text()).absoluteFilePath() << "--nand" << QFileInfo(nand->text()).absoluteFilePath();
        settings.setValue("nor", nor->text()); settings.setValue("nand", nand->text());
    }
    const auto data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/frond/sessions";
    if (!QDir().mkpath(data)) return failure(tr("Could not create the session storage directory."));
    // Compare canonical paths so an XDG data directory symlink does not clone on every resume.
    const auto managedRoot = QFileInfo(data).canonicalFilePath();
    sessionPath = source.startsWith(managedRoot + '/') ? source : managedRoot + "/Palm TX " +
        QDateTime::currentDateTime().toString("yyyy-MM-dd HH-mm-ss") + "-" + QUuid::createUuid().toString(QUuid::Id128).first(8) + ".frond";
    if (!audio->isChecked()) arguments << "--no-sound";
    if (fastAudio->isChecked()) arguments << "--experimental-fast-audio";
    if (!doubleSize->isChecked()) arguments << "--small-window";
    if (serialArgument() != "off") arguments << "--serial" << serialArgument();
    if (usbArgument() != "off") arguments << "--usb" << usbArgument();
    sessionLock = std::make_unique<QLockFile>(sessionPath + ".lock");
    sessionLock->setStaleLockTime(0);
    if (!sessionLock->tryLock()) {
        sessionLock.reset();
        return failure(tr("This working session is already open, or its lock file could not be created."));
    }
    stopping = closeAfterStop = allowClose = false;
    log->clear(); busy = true;
    const bool launched = client.start(core, arguments);
    if (!launched) { busy = false; sessionLock.reset(); }
    refresh(); statusBar()->showMessage(launched ? tr("Starting Palm TX…") : tr("Could not start Palm TX."));
}
QString MainWindow::serialArgument() const {
    const auto mode = serial->currentData().toString();
    return mode == "tcp" ? "tcp:" + QString::number(serialPort->value()) : mode;
}
// Live readout of the core's serial-state reply: "off", "off <error>" or
// "<pty|tcp> <endpoint> <connected|waiting>". Empty while no core is running.
// While running, the mode and port follow what the core actually has open.
void MainWindow::showSerial(const QString& state) {
    const auto parts = state.split(' ');
    if (!state.isEmpty()) {
        serial->setCurrentIndex(std::max(0, serial->findData(parts[0])));
        if (parts[0] == "tcp" && parts.size() > 1) {
            const auto port = parts[1].section(':', -1).toInt();
            if (port > 0) serialPort->setValue(port);
        }
        refresh();
    }
    if (state.isEmpty()) serialState->clear();
    else if (parts[0] == "off") serialState->setText(parts.size() > 1 ? tr("Serial: off, %1").arg(state.mid(4)) : QString());
    else if (parts.size() == 3)
        serialState->setText(tr("Serial: %1, %2").arg(parts[1], parts[2] == "connected" ? tr("connected") : tr("waiting for a connection")));
    else serialState->setText(tr("Serial: %1").arg(state));
}
void MainWindow::loadSerialSettings() {
    serial->setCurrentIndex(std::max(0, serial->findData(settings.value("serialMode", "off").toString())));
    serialPort->setValue(settings.value("serialPort", 5023).toInt());
}
QString MainWindow::usbArgument() const {
    const auto mode = usb->currentData().toString();
    return mode == "tcp" ? "tcp:" + QString::number(usbPort->value()) : mode;
}
// The core's usb-state reply: "off", "off <error>" or
// "<pty|tcp> <endpoint> <connected|waiting> <bus phase>".
void MainWindow::showUsb(const QString& state) {
    const auto parts = state.split(' ');
    if (!state.isEmpty()) {
        usb->setCurrentIndex(std::max(0, usb->findData(parts[0])));
        if (parts[0] == "tcp" && parts.size() > 1) {
            const auto port = parts[1].section(':', -1).toInt();
            if (port > 0) usbPort->setValue(port);
        }
        refresh();
    }
    if (state.isEmpty()) usbState->clear();
    else if (parts[0] == "off") usbState->setText(parts.size() > 1 ? tr("USB: off, %1").arg(state.mid(4)) : QString());
    else if (parts.size() == 4)
        usbState->setText(tr("USB: %1, %2, %3").arg(parts[1], parts[2] == "connected" ? tr("connected") : tr("waiting for a connection"),
                                                    parts[3] == "running" ? tr("TX enumerated") : tr("TX %1").arg(parts[3])));
    else usbState->setText(tr("USB: %1").arg(state));
}
void MainWindow::loadUsbSettings() {
    usb->setCurrentIndex(std::max(0, usb->findData(settings.value("usbMode", "off").toString())));
    usbPort->setValue(settings.value("usbPort", 5024).toInt());
}
void MainWindow::remember(const QString& path) {
    auto list = settings.value("recentSessions").toStringList();
    list.removeAll(path); list.prepend(path);
    while (list.size() > 12) list.removeLast();
    settings.setValue("recentSessions", list);
    sessions->clear(); sessions->addItems(list);
    savedMode->setChecked(true);
}
QString MainWindow::outputFile(const QString& title, const QString& filter, const QString& defaultName) {
    QFileDialog dialog(this, title, QDir::homePath() + '/' + defaultName, filter);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(filter.contains("*.frond") ? "frond" : filter.contains("*.img") ? "img" : "zip");
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return {};
    return QFileInfo(dialog.selectedFiles().first()).absoluteFilePath();
}
void MainWindow::save(bool saveAs) {
    if (!client.available()) return;
    const auto path = saveAs ? outputFile(tr("Save a Copy"), tr("Frond sessions (*.frond)"), "Palm TX.frond") : sessionPath;
    if (path.isEmpty()) return;
    savingPath = path; send("save", path);
}
void MainWindow::send(const QString& command, const QString& argument) {
    if (client.request(command, argument)) { busy = true; refresh(); statusBar()->showMessage(tr("Working…")); }
}
void MainWindow::stop(bool closeApp) {
    closeAfterStop = closeAfterStop || closeApp;
    if (!client.running()) { if (closeApp) { allowClose = true; close(); } return; }
    if (stopping) return;
    stopping = true; refresh();
    if (exchangeDialog) exchangeDialog->reject();
    else if (client.available() && exchangeStage == ExchangeStage::None) save();
    else statusBar()->showMessage(tr("Waiting to save the session…"));
}
void MainWindow::askStop(const QString& message) {
    const auto answer = QMessageBox::warning(this, tr("Session Was Not Saved"),
        message + "\n\n" + tr("Retry saving, keep Frond open, or discard changes?"),
        QMessageBox::Retry | QMessageBox::Cancel | QMessageBox::Discard, QMessageBox::Retry);
    if (answer == QMessageBox::Retry) save();
    else if (answer == QMessageBox::Discard) send("quit");
    else { stopping = closeAfterStop = false; refresh(); }
}
void MainWindow::failure(const QString& message) {
    QMessageBox::critical(this, tr("Frond"), message);
}
void MainWindow::closeEvent(QCloseEvent* event) {
    if (allowClose || !client.running()) { event->accept(); return; }
    event->ignore(); stop(true);
}

void MainWindow::beginExchange() {
    if (!client.available() || stopping || !cardPresent || exchangeStage != ExchangeStage::None) return;
    if (!SdExchange::available()) return failure(tr("Install mtools to exchange files (Debian: sudo apt install mtools)."));
    if (QMessageBox::question(this, tr("Close Palm Files Before Exchange"),
            tr("Save your work and close all files using the SD card on the Palm. "
               "Frond will eject the card and save a recovery session before editing a private image. "
               "Ejection cannot flush files left open by a Palm app. Continue?"),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    // The Palm window can request shutdown while the confirmation is open.
    if (!client.available() || stopping || !cardPresent || exchangeStage != ExchangeStage::None) return;
    const auto data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/frond/sd-exchange";
    const auto cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (!QDir().mkpath(data) || !QDir().mkpath(cache)) return failure(tr("Could not create SD exchange storage."));
    exchangeRecovery = std::make_unique<QTemporaryDir>(data + "/recovery-XXXXXX");
    exchangeFiles = std::make_unique<QTemporaryDir>(cache + "/sd-exchange-XXXXXX");
    if (!exchangeRecovery->isValid() || !exchangeFiles->isValid()) {
        endExchange(); return failure(tr("Could not create private SD exchange storage."));
    }
    exchangeBackup = exchangeRecovery->path() + "/Before exchange.frond";
    if (cardInserted) { exchangeStage = ExchangeStage::Eject; send("sd-eject"); }
    else { exchangeStage = ExchangeStage::Backup; send("save", exchangeBackup); }
}
void MainWindow::endExchange() {
    exchange.reset(); exchangeFiles.reset(); exchangeRecovery.reset();
    exchangeStage = ExchangeStage::None; busy = false; refresh();
}
void MainWindow::exchangeResult(bool ok, const QString& message) {
    if (ok && exchangeStage == ExchangeStage::Backup) exchangeRecovery->setAutoRemove(false);
    if (!ok || stopping) {
        endExchange();
        if (stopping) save();
        else failure(message + (QFileInfo::exists(exchangeBackup) ? tr("\nRecovery session: %1").arg(exchangeBackup) : QString{}));
        return;
    }
    if (exchangeStage == ExchangeStage::Eject) {
        exchangeStage = ExchangeStage::Backup; send("save", exchangeBackup);
    } else if (exchangeStage == ExchangeStage::Backup) {
        exchangeStage = ExchangeStage::Export; send("sd-export", exchangeFiles->path() + "/original.img");
    } else if (exchangeStage == ExchangeStage::Export) {
        exchange = std::make_unique<SdExchange>(exchangeFiles->path() + "/edit-XXXXXX");
        QString error;
        if (!exchange->open(exchangeFiles->path() + "/original.img", error)) {
            endExchange(); failure(error); return;
        }
        SdExchangeDialog dialog(*exchange, exchangeBackup, this);
        exchangeDialog = &dialog;
        const auto result = dialog.exec();
        exchangeDialog = nullptr;
        if (result == QDialog::Accepted && !stopping && client.running()) {
            exchangeStage = ExchangeStage::Insert; send("sd-insert", exchange->image());
        } else {
            endExchange();
            if (stopping && client.running()) save();
        }
    } else if (exchangeStage == ExchangeStage::Insert) {
        endExchange();
        statusBar()->showMessage(tr("Card inserted. Recovery session: %1").arg(exchangeBackup));
    }
}
