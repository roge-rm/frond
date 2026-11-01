#include "MainWindow.h"
#include "FirmwareInput.h"
#include "SdExchangeDialog.h"
#include "Dialogs.h"
#include "FirmwareStore.h"
#include <QListWidget>
#include <QMenu>
#include <QPainter>
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
#include <QGridLayout>
#include <QGroupBox>
#include <QShortcut>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
#include <QSlider>
#include <QSignalBlocker>
#include "WindowScale.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

MainWindow::MainWindow(const QString& core) : core(core) {
    setWindowTitle(tr("Frond"));
    resize(620, 650);
    restoreGeometry(settings.value("windowGeometry").toByteArray());
    auto* outer = new QWidget(this);
    auto* outerLayout = new QVBoxLayout(outer);
    home = new QWidget(outer);
    home->setObjectName("home");
    auto* homeLayout = new QVBoxLayout(home);
    homeLayout->setContentsMargins(0, 0, 0, 0);
    auto* title = new QLabel(tr("Devices"), home);
    auto titleFont = title->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.4); titleFont.setBold(true);
    title->setFont(titleFont);
    homeLayout->addWidget(title);
    sessionList = new QListWidget(home);
    sessionList->setObjectName("sessionList");
    sessionList->setContextMenuPolicy(Qt::CustomContextMenu);
    sessionList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sessionList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    sessionList->setMinimumHeight(260);
    homeLayout->addWidget(sessionList, 1);
    emptyNote = new QLabel(tr("No devices yet. Click Add device to set one up."), home);
    emptyNote->setObjectName("emptyNote");
    emptyNote->setWordWrap(true);
    homeLayout->addWidget(emptyNote);
    auto* homeButtons = new QHBoxLayout;
    addButton = new QPushButton(tr("Add device"), home);
    addButton->setObjectName("addDevice");
    startButton = new QPushButton(tr("Start"), home);
    startButton->setObjectName("start"); startButton->setDefault(true);
    homeButtons->addWidget(addButton); homeButtons->addStretch(); homeButtons->addWidget(startButton);
    homeLayout->addLayout(homeButtons);
    outerLayout->addWidget(home, 1);
    connect(addButton, &QPushButton::clicked, this, &MainWindow::addDevice);
    connect(startButton, &QPushButton::clicked, this, &MainWindow::start);
    connect(sessionList, &QListWidget::itemActivated, this, [this] { start(); });
    connect(sessionList, &QListWidget::currentItemChanged, this, [this] { refresh(); });
    connect(sessionList, &QWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        if (auto* item = sessionList->itemAt(at)) {
            sessionList->setCurrentItem(item);
            std::unique_ptr<QMenu> menu(rowMenu(item->data(Qt::UserRole).toString(), this));
            menu->exec(sessionList->viewport()->mapToGlobal(at));
        }
    });
    // The running controls go on this page while a Palm runs.
    auto* root = new QWidget(outer);
    runningPage = root;
    runningPage->setObjectName("running");
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(0, 0, 0, 0);
    outerLayout->addWidget(runningPage);
    settingsDialog = new SettingsDialog(this);
    windowScale = settingsDialog->windowScale;
    scaleLabel = settingsDialog->scaleLabel;
    smoothLcd = settingsDialog->smoothLcd;
    audio = settingsDialog->audio;
    fastAudio = settingsDialog->fastAudio;
    windowScale->setValue(std::clamp(settings.value("windowScale", 4).toInt(), 4, 8));
    connect(windowScale, &QSlider::valueChanged, this, [this](int quarters) {
        settings.setValue("windowScale", quarters);
        scaleLabel->setText(tr("Size: %1×").arg(quarters / 4.0));
    });
    smoothLcd->setChecked(settings.value("smoothLcd", false).toBool());
    connect(smoothLcd, &QCheckBox::toggled, this, [this](bool enabled) { settings.setValue("smoothLcd", enabled); });
    audio->setChecked(settings.value("audio", true).toBool());
    connect(audio, &QCheckBox::toggled, this, [this](bool enabled) {
        settings.setValue("audio", enabled);
        if (client.available()) send(enabled ? "audio-on" : "audio-off");
    });
    fastAudio->setToolTip(tr("Applies on the next start."));
    fastAudio->setChecked(settings.value("fastAudio", false).toBool());
    connect(fastAudio, &QCheckBox::toggled, this, [this](bool enabled) { settings.setValue("fastAudio", enabled); });
    settingsDialog->resume->setChecked(settings.value("resume", true).toBool());
    connect(settingsDialog->resume, &QCheckBox::toggled, this, [this](bool enabled) { settings.setValue("resume", enabled); });
    // The running page follows the Android Menu screen: Applications, SD card, Device, Session.
    auto section = [&](const QString& title, bool first = false) {
        auto* heading = new QLabel(title, root);
        auto font = heading->font(); font.setPointSizeF(font.pointSizeF() * 1.2); font.setBold(true);
        heading->setFont(font);
        if (!first) layout->addSpacing(8);
        layout->addWidget(heading);
    };
    auto button = [&](const QString& text, const char* name, bool plain = false) {
        auto* b = new QPushButton(text, root);
        b->setObjectName(name);
        if (!plain) runningButtons << b;
        return b;
    };
    section(tr("Applications"), true);
    auto* apps = new QGridLayout;
    auto* install = button(tr("Install application"), "installApp");
    auto* launchApp = button(tr("Launch application"), "launchApp");
    auto* exportAll = button(tr("Export RAM databases"), "exportRam");
    auto* exportOne = button(tr("Export database"), "exportDatabase");
    apps->addWidget(install, 0, 0); apps->addWidget(launchApp, 0, 1);
    apps->addWidget(exportAll, 1, 0); apps->addWidget(exportOne, 1, 1);
    layout->addLayout(apps);
    connect(install, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("Install application"), {}, tr("Palm databases (*.prc *.pdb);;All files (*)"));
        if (!path.isEmpty()) send("install", QFileInfo(path).absoluteFilePath());
    });
    connect(launchApp, &QPushButton::clicked, this, [this] {
        bool ok;
        const auto name = QInputDialog::getText(this, tr("Launch application"), tr("Palm database name"), QLineEdit::Normal, {}, &ok);
        if (ok && !name.isEmpty()) send("launch-app", name);
    });
    connect(exportAll, &QPushButton::clicked, this, [this] {
        const auto path = outputFile(tr("Export RAM databases"), tr("ZIP archives (*.zip)"), "Palm databases.zip");
        if (!path.isEmpty()) send("export", path);
    });
    connect(exportOne, &QPushButton::clicked, this, [this] {
        bool ok;
        const auto name = QInputDialog::getText(this, tr("Export database"), tr("Palm database name"), QLineEdit::Normal, {}, &ok);
        if (!ok || name.isEmpty()) return;
        const auto path = outputFile(tr("Export database"), tr("ZIP archives (*.zip)"), name + ".zip");
        if (!path.isEmpty()) send("export-db", path + "\n" + name);
    });

    // The buttons shown depend on the card's state; refresh() picks them.
    section(tr("SD card"));
    sdStatus = new QLabel(tr("Not known"), root);
    sdStatus->setObjectName("sdState");
    layout->addWidget(sdStatus);
    auto* cardRow = new QHBoxLayout;
    sdReinsert = button(tr("Insert"), "sdInsert", true);
    sdExchange = button(tr("Files"), "sdExchange", true);
    sdEject = button(tr("Eject"), "sdEject", true);
    sdExport = button(tr("Export image"), "sdExport", true);
    sdCreate = button(tr("New card"), "sdNew", true);
    sdImport = button(tr("Insert image"), "sdInsertImage", true);
    sdRetry = button(tr("Try again"), "sdRetry", true);
    for (auto* b : {sdReinsert, sdExchange, sdEject, sdExport, sdCreate, sdImport, sdRetry}) cardRow->addWidget(b);
    cardRow->addStretch();
    layout->addLayout(cardRow);
    const auto replaceCard = [this] {
        return !cardPresent || QMessageBox::question(this, tr("Replace the ejected card?"),
            tr("Its files are lost unless you export the image first."),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
    };
    connect(sdCreate, &QPushButton::clicked, this, [this, replaceCard] {
        if (!replaceCard()) return;
        bool ok;
        const auto size = QInputDialog::getItem(this, tr("New card"), tr("Size"), {"32 MB", "64 MB", "128 MB"}, 0, false, &ok);
        if (ok) send("sd-create", size.section(' ', 0, 0));
    });
    connect(sdImport, &QPushButton::clicked, this, [this, replaceCard] {
        if (!replaceCard()) return;
        const auto file = QFileDialog::getOpenFileName(this, tr("Insert image"), {}, tr("SD images (*.img *.bin);;All files (*)"));
        if (!file.isEmpty()) send("sd-insert", QFileInfo(file).absoluteFilePath());
    });
    connect(sdEject, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Eject the card?"), tr("Close any files open on the Palm first."),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) send("sd-eject");
    });
    connect(sdReinsert, &QPushButton::clicked, this, [this] { send("sd-reinsert"); });
    connect(sdExport, &QPushButton::clicked, this, [this] {
        const auto file = outputFile(tr("Export image"), tr("SD images (*.img)"), "SD card.img");
        if (!file.isEmpty()) send("sd-export", file);
    });
    connect(sdExchange, &QPushButton::clicked, this, &MainWindow::beginExchange);
    connect(sdRetry, &QPushButton::clicked, this, [this] { send("sd-state"); });

    section(tr("Device"));
    auto* grid = new QGridLayout;
    layout->addLayout(grid);
    grid->setHorizontalSpacing(12);
    auto label = [&](const QString& text, int row) { grid->addWidget(new QLabel(text, root), row, 0); };
    // One row per device: label, control, and the port beside it. Readout rows hide while empty.
    soundToggle = new QCheckBox(tr("On"), root);
    soundToggle->setObjectName("sound");
    label(tr("Sound"), 0); grid->addWidget(soundToggle, 0, 1, 1, 2);
    network = new QComboBox(root);
    network->setObjectName("networkMode");
    network->addItem(tr("Disabled"), "disabled");
    network->addItem(tr("Virtual Wi-Fi (Frond)"), "wireless");
    network->addItem(tr("Internet via host"), "internet");
    network->addItem(tr("Simulated echo server"), "connected");
    network->addItem(tr("Simulated connection timeout"), "timeout");
    network->addItem(tr("Simulated disconnection"), "disconnected");
    network->setToolTip(tr("Virtual Wi-Fi uses the Palm networking stack and a Frond access point. Internet via host forwards a subset of 68k NetLib calls. Changing mode closes connections."));
    connect(network, qOverload<int>(&QComboBox::activated), this, [this](int index) {
        send("network-mode", network->itemData(index).toString());
    });
    label(tr("Network"), 1); grid->addWidget(network, 1, 1, 1, 2);
    keyboard = new QCheckBox(tr("Attached"),root);
    keyboard->setObjectName("keyboardAttached");
    keyboard->setToolTip(tr("Uses the Palm Universal Wireless Keyboard protocol. Install and enable its Palm OS driver first. Type in the Palm window; modifiers and key releases are sent to the driver."));
    connect(keyboard,&QCheckBox::clicked,this,[this](bool attached) { send("keyboard-attach",attached?"on":"off"); });
    label(tr("Infrared keyboard"), 2); grid->addWidget(keyboard, 2, 1, 1, 2);
    serial = new QComboBox(root);
    serial->setObjectName("serialMode");
    serial->addItem(tr("Off"), "off");
    serial->addItem(tr("Terminal (PTY)"), "pty");
    serial->addItem(tr("Wait for a peer"), "tcp");
    serialPort = new QSpinBox(root);
    serialPort->setObjectName("serialPort");
    serialPort->setRange(1, 65535);
    serialPort->setPrefix(tr("Port "));
    loadSerialSettings();
    label(tr("Serial port"), 3); grid->addWidget(serial, 3, 1); grid->addWidget(serialPort, 3, 2);
    serialState = new QLabel(root);
    serialState->setObjectName("serialState");
    serialState->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addWidget(serialState, 4, 1, 1, 2);
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
    usb = new QComboBox(root);
    usb->setObjectName("usbMode");
    usb->addItem(tr("Off"), "off");
    usb->addItem(tr("Terminal (PTY)"), "pty");
    usb->addItem(tr("Wait for a peer"), "tcp");
    usb->setToolTip(tr("Plugs the Palm into a host that moves its HotSync bytes to a PTY or TCP port, for pilot-link and the like."));
    usbPort = new QSpinBox(root);
    usbPort->setObjectName("usbPort");
    usbPort->setRange(1, 65535);
    usbPort->setPrefix(tr("Port "));
    loadUsbSettings();
    label(tr("USB HotSync"), 5); grid->addWidget(usb, 5, 1); grid->addWidget(usbPort, 5, 2);
    usbState = new QLabel(root);
    usbState->setObjectName("usbState");
    usbState->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addWidget(usbState, 6, 1, 1, 2);
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
    irda=new QComboBox(root);irda->setObjectName("irdaMode");
    irda->addItem(tr("Off"),"off");
    irda->addItem(tr("Terminal (PTY)"),"pty");
    irda->addItem(tr("Wait for a peer"),"tcp");
    irda->setToolTip(tr("Link two Frond instances with tools/link-irda.py. Detach the infrared keyboard first."));
    irdaPort=new QSpinBox(root);irdaPort->setObjectName("irdaPort");
    irdaPort->setRange(1,65535);irdaPort->setPrefix(tr("Port "));loadIrdaSettings();
    label(tr("Infrared beaming"), 7); grid->addWidget(irda, 7, 1); grid->addWidget(irdaPort, 7, 2);
    irdaState=new QLabel(root);irdaState->setObjectName("irdaState");
    irdaState->setTextInteractionFlags(Qt::TextSelectableByMouse);grid->addWidget(irdaState, 8, 1, 1, 2);
    auto irdaChanged=[this] {
        if(client.running()){if(client.available())send("irda-mode",irdaArgument());return;}
        settings.setValue("irdaMode",irda->currentData());settings.setValue("irdaPort",irdaPort->value());refresh();
    };
    connect(irda,qOverload<int>(&QComboBox::activated),this,irdaChanged);
    connect(irdaPort,&QSpinBox::editingFinished,this,[this,irdaChanged]{if(irda->currentData()=="tcp")irdaChanged();});

    bluetooth = new QCheckBox(tr("On"), root);
    bluetooth->setObjectName("bluetoothEnabled");
    bluetoothSocket = new QLineEdit(root);
    bluetoothSocket->setObjectName("bluetoothSocket");
    bluetoothSocket->setPlaceholderText(tr("Absolute controller socket path"));
    loadBluetoothSettings();
    label(tr("Bluetooth"), 9); grid->addWidget(bluetooth, 9, 1, 1, 2); grid->addWidget(bluetoothSocket, 10, 1, 1, 2);
    bluetoothState = new QLabel(root); bluetoothState->setObjectName("bluetoothState");
    bluetoothState->setWordWrap(true); bluetoothState->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addWidget(bluetoothState, 11, 1, 1, 2);
    grid->setColumnStretch(1, 1);
    grid->setColumnMinimumWidth(2, 110);
    for (auto* c : {static_cast<QWidget*>(network), static_cast<QWidget*>(serial), static_cast<QWidget*>(usb), static_cast<QWidget*>(irda)})
        c->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* reset = button(tr("Reset"), "reset");
    auto* rotate = button(tr("Rotate"), "rotate");
    auto* deviceButtons = new QHBoxLayout;
    deviceButtons->addWidget(reset); deviceButtons->addWidget(rotate); deviceButtons->addStretch();
    layout->addLayout(deviceButtons);
    connect(reset, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Reset"), tr("Reset the %1 now?").arg(deviceLabel())) == QMessageBox::Yes) send("reset");
    });
    connect(rotate, &QPushButton::clicked, this, [this] { send("rotate"); });
    connect(soundToggle, &QCheckBox::clicked, this, [this](bool enabled) { audio->setChecked(enabled); });
    connect(audio, &QCheckBox::toggled, soundToggle, &QCheckBox::setChecked);
    soundToggle->setChecked(audio->isChecked());

    // Pinned under the scroll area, shown while a Palm runs.
    sessionBar = new QWidget(this);
    auto* sessionButtons = new QHBoxLayout(sessionBar);
    auto* saveButton = button(tr("Save"), "save");
    auto* copyButton = button(tr("Save a copy…"), "saveCopy");
    auto* logButton = button(tr("Emulator log"), "emulatorLog", true);
    auto* hardReset = button(tr("Hard reset"), "hardReset");
    for (auto* b : {saveButton, copyButton, logButton, hardReset}) { b->setParent(sessionBar); sessionButtons->addWidget(b); }
    connect(saveButton, &QPushButton::clicked, this, [this] { save(); });
    connect(hardReset, &QPushButton::clicked, this, [this] {
        QMessageBox box(QMessageBox::Warning, tr("Hard reset"), tr("Erase everything on the %1?").arg(deviceLabel()),
                        QMessageBox::NoButton, this);
        auto* erase = box.addButton(tr("Erase"), QMessageBox::DestructiveRole);
        auto* saveFirst = box.addButton(tr("Save first"), QMessageBox::AcceptRole);
        box.setDefaultButton(box.addButton(QMessageBox::Cancel));
        box.exec();
        if (box.clickedButton() == erase) send("hard-reset");
        else if (box.clickedButton() == saveFirst) {
            // Erases once the working copy saves; a failed save leaves the Palm as it is.
            save();
            hardResetAfterSave = !savingPath.isEmpty();
        }
    });
    connect(copyButton, &QPushButton::clicked, this, [this] { save(true); });
    auto* saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated, this, [this] {
        if (runningPage->isVisible() && client.available() && !busy && !stopping && exchangeStage == ExchangeStage::None) save();
    });
    auto* logDialog = new QDialog(this);
    logDialog->setWindowTitle(tr("Emulator log")); logDialog->resize(700, 400);
    auto* logLayout = new QVBoxLayout(logDialog);
    log = new QPlainTextEdit(logDialog); log->setReadOnly(true); log->setMaximumBlockCount(1000);
    logLayout->addWidget(log);
    connect(logButton, &QPushButton::clicked, logDialog, &QWidget::show);
    auto bluetoothChanged = [this] {
        if (client.running()) { if (client.available()) send("bluetooth-mode", bluetoothArgument()); return; }
        settings.setValue("bluetoothEnabled", bluetooth->isChecked());
        settings.setValue("bluetoothSocket", bluetoothSocket->text()); refresh();
    };
    connect(bluetooth, &QCheckBox::clicked, this, bluetoothChanged);
    connect(bluetoothSocket, &QLineEdit::editingFinished, this, bluetoothChanged);

    stopButton = new QPushButton(tr("Save and Stop"), sessionBar);
    stopButton->setObjectName("stop");
    forceButton = new QPushButton(tr("Force Stop…"), sessionBar);
    forceButton->hide();
    sessionButtons->addStretch(); sessionButtons->addWidget(forceButton); sessionButtons->addWidget(stopButton);
    layout->addStretch();
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(outer);
    auto* central = new QWidget(this);
    auto* centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->addWidget(scroll, 1);
    centralLayout->addWidget(sessionBar);
    sessionBar->hide();
    setCentralWidget(central);
    connect(stopButton, &QPushButton::clicked, this, [this] { stop(false); });
    connect(forceButton, &QPushButton::clicked, this, [this] {
        if (QMessageBox::warning(this, tr("Force Stop"), tr("Stop without saving? Changes since the last save will be lost."),
                                 QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Discard) {
            allowClose = closeAfterStop; client.forceStop();
        }
    });
    auto* file = menuBar()->addMenu(tr("&File"));
    importAction = file->addAction(tr("&Import session…"), this, &MainWindow::importSessionDialog);
    importAction->setShortcut(QKeySequence::Open);
    file->addAction(tr("&Firmware…"), this, [this] { showFirmware(); });
    file->addAction(tr("&Settings…"), this, [this] { settingsDialog->exec(); });
    file->addSeparator();
    file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);
    auto* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("Hardware &Keyboard Controls"), this, [this] {
        QMessageBox::information(this, tr("Hardware Keyboard Controls"),
            tr("With the Palm window focused and the infrared keyboard detached:\n\n"
               "Arrow keys: five-way navigation\nEnter: centre select\n"
               "F1: Home\nF2: Calendar\nF3: Contacts\nF4: Web\n"
               "On the Tungsten E2, F1 to F4 are Calendar, Contacts, Tasks and Note Pad.\n"
               "On the Zire 72, F1 to F4 are Calendar, Contacts, Music and Camera, and F5 is the side voice button.\n"
               "Escape: Power (sleep and wake are experimental)\n\n"
               "Palm OS button assignments may change what the application buttons do."));
    });
    help->addAction(tr("&About Frond"), this, [this] {
        QMessageBox::about(this, tr("About Frond"), tr("Frond 0.7.0\nExperimental Palm TX, Tungsten E2 and Zire 72 emulator\nGPL-3.0-or-later"));
    });
    connect(&client, &DesktopClient::diagnostic, this, [this](const QString& text) {
        log->moveCursor(QTextCursor::End); log->insertPlainText(text);
    });
    connect(&client, &DesktopClient::error, this, &MainWindow::failure);
    connect(&client, &DesktopClient::stalled, this, [this] {
        forceButton->show(); statusBar()->showMessage(tr("The emulator is not responding. Waiting for it to finish…"));
    });
    connect(&client, &DesktopClient::started, this, [this] {
        busy = false; hardResetAfterSave = false;
        statusBar()->showMessage(tr("Running. Your session will be saved when you close Frond.")); refresh();
        if (stopping) save();
        else send("device-state");
    });
    connect(&client, &DesktopClient::finished, this, [this] {
        if (exchangeDialog) exchangeDialog->reject();
        else endExchange();
        busy = false; stopping = false; hardResetAfterSave = false; cardPresent = cardInserted = false; runningDevice = frondFirmware::Device::tx;
        sdKnown = false; showSerial({}); loadSerialSettings(); showUsb({}); loadUsbSettings(); showIrda({}); loadIrdaSettings(); showBluetooth({}); loadBluetoothSettings(); sessionLock.reset(); forceButton->hide(); refreshSessions();
        if (closeAfterStop) { allowClose = true; close(); }
        else statusBar()->showMessage(tr("Stopped."));
    });
    connect(&client, &DesktopClient::closeRequested, this, [this] { stop(true); });
    connect(&client, &DesktopClient::serialState, this, [this](const QString& state) { showSerial(state); });
    connect(&client, &DesktopClient::usbState, this, [this](const QString& state) { showUsb(state); });
    connect(&client, &DesktopClient::bluetoothState, this, &MainWindow::showBluetooth);
    connect(&client,&DesktopClient::infraredState,this,[this](const QString& state){showIrda(state);});
    connect(&client, &DesktopClient::result, this, [this](const QString& command, bool ok, const QString& code, const QString& message) {
        busy = false; forceButton->hide(); refresh();
        if (!ok && exchangeStage != ExchangeStage::None) { exchangeResult(false, message); return; }
        if (!ok) {
            hardResetAfterSave = false;
            log->appendPlainText(command + ": " + message);
            if (command == "sd-state") sdKnown = false;
            if (command == "serial-mode") showSerial("off " + message);  // the core is now off
            if (command == "usb-mode") showUsb("off " + message);
            if(command=="irda-mode")showIrda("off error: "+message);
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
        if (command == "device-state") {
            showDevice(message);
            if (stopping) save();
            else send("sd-state");
            return;
        }
        if (command == "sd-state") { cardPresent = message != "none"; cardInserted = message == "inserted"; }
        else if (command == "sd-create" || command == "sd-insert" || command == "sd-reinsert") cardPresent = cardInserted = true;
        else if (command == "sd-eject") cardInserted = false;
        if (command.startsWith("sd-")) sdKnown = true;
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
        if(command=="irda-state" || command=="irda-mode")showIrda(message);
        if(command=="bluetooth-state" || command=="bluetooth-mode") {
            showBluetooth(message);
            if(command=="bluetooth-mode") {
                settings.setValue("bluetoothEnabled", bluetooth->isChecked());
                if(bluetooth->isChecked())settings.setValue("bluetoothSocket", bluetoothSocket->text());
            }
        }
        refresh();
        if (command == "network-state" && !stopping) { send("keyboard-state"); return; }
        if (command == "keyboard-state" && !stopping && frondFirmware::capabilities(runningDevice).serial && serialArgument() != "off") { send("serial-state"); return; }
        if ((command == "keyboard-state" || command == "serial-state") && !stopping && usbArgument() != "off") { send("usb-state"); return; }
        if (command == "sd-state" && !stopping) { send("network-state"); return; }
        statusBar()->showMessage(command == "save" ? tr("Saved.") : tr("Running. Sessions save automatically on close."));
        const bool savedWorkingCopy = command == "save" && savingPath == sessionPath;
        if (command == "save") {
            if (savedWorkingCopy) {
                settings.setValue("lastSession", sessionPath); settings.sync();
                QFile::remove(frondSessions::markerPath(sessionPath)); refreshSessions();
            }
            savingPath.clear();
        }
        if (command == "save" && hardResetAfterSave && !stopping) {
            hardResetAfterSave = false;
            if (savedWorkingCopy) { send("hard-reset"); return; }
        }
        if (stopping && command != "quit") {
            if (savedWorkingCopy) send("quit");
            else save();
        }
    });
    networkTip = network->toolTip(); keyboardTip = keyboard->toolTip(); serialTip = serial->toolTip();
    usbTip = usb->toolTip(); irdaTip=irda->toolTip();
    fastAudioTip = fastAudio->toolTip();
    refreshSessions();
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
    home->setVisible(!running);
    runningPage->setVisible(running);
    sessionBar->setVisible(running);
    for (auto* state : {serialState, usbState, irdaState, bluetoothState}) state->setVisible(!state->text().isEmpty());
    setWindowTitle(running && !sessionPath.isEmpty() ? QFileInfo(sessionPath).completeBaseName() : tr("Frond"));
    const auto* current = sessionList->currentItem();
    startButton->setEnabled(!running && !busy && current && !current->data(Qt::UserRole + 1).toBool());
    addButton->setEnabled(!running && !busy);
    stopButton->setEnabled(running && !busy && !stopping);
    audio->setEnabled(!busy && !stopping);
    const auto caps = frondFirmware::capabilities(currentDevice());
    const auto reason = tr("Not available on the %1 yet").arg(frondFirmware::deviceName(currentDevice()));
    auto note = [&](QWidget* w, bool supported, const QString& tip) { w->setToolTip(supported ? tip : reason); };
    note(network, caps.network, networkTip); note(keyboard, caps.keyboard, keyboardTip);
    note(serial, caps.serial, serialTip); note(serialPort, caps.serial, {});
    note(usb, caps.usb, usbTip); note(usbPort, caps.usb, {});
    note(irda,caps.infrared,irdaTip);note(irdaPort,caps.infrared,{});
    note(fastAudio, caps.fastAudio, fastAudioTip);
    fastAudio->setEnabled(!running && !busy && caps.fastAudio);
    windowScale->setEnabled(!running && !busy);
    smoothLcd->setEnabled(!running && !busy);
    if (!settings.contains("windowScale")) {
        const auto* display = QGuiApplication::primaryScreen();
        const auto available = display ? display->availableGeometry().size() : QSize(0, 0);
        const QSignalBlocker blocked(windowScale);
        windowScale->setValue(int(4 * fittingWindowScale(320,
            // Screen, silkscreen and button strip: E2 80 rows, Zire 72 96, TX 84 under 480.
            currentDevice() == frondFirmware::Device::e2 ? 520 : currentDevice() == frondFirmware::Device::zire72 ? 536 : 564,
            available.width(), available.height())));
    }
    scaleLabel->setText(tr("Size: %1×").arg(windowScale->value() / 4.0));
    const bool enabled = client.available() && !busy && !stopping && exchangeStage == ExchangeStage::None;
    for (auto* w : runningButtons) w->setEnabled(enabled);
    soundToggle->setEnabled(enabled);
    importAction->setEnabled(!running && !busy);
    network->setEnabled(enabled && caps.network);
    keyboard->setEnabled(enabled && caps.keyboard && irda->currentData()=="off");
    const bool editable = !busy && !stopping && (!running || enabled);
    serial->setEnabled(editable && caps.serial);
    serialPort->setEnabled(editable && caps.serial && serial->currentData() == "tcp");
    usb->setEnabled(editable && caps.usb);
    usbPort->setEnabled(editable && caps.usb && usb->currentData() == "tcp");
    irda->setEnabled(editable && caps.infrared && !keyboard->isChecked());
    irdaPort->setEnabled(editable && caps.infrared && !keyboard->isChecked() && irda->currentData()=="tcp");
    bluetooth->setEnabled(editable && caps.bluetooth);
    bluetoothSocket->setEnabled(editable && caps.bluetooth && bluetooth->isChecked());
    bluetooth->setToolTip(caps.bluetooth ? tr("See doc/bluetooth-provider.md for controller setup.") : reason);
    // Which card buttons show follows the card's state, as on Android.
    const bool known = sdKnown && running;
    sdStatus->setText(!known ? tr("Not known") : !cardPresent ? tr("No card") : cardInserted ? tr("Inserted") : tr("Ejected"));
    sdReinsert->setVisible(known && cardPresent && !cardInserted);
    sdExchange->setVisible(known && cardPresent);
    sdEject->setVisible(known && cardInserted);
    sdExport->setVisible(known && cardPresent && !cardInserted);
    sdCreate->setVisible(known && !cardInserted); sdImport->setVisible(known && !cardInserted);
    sdRetry->setVisible(!known);
    for (auto* b : {sdReinsert, sdExchange, sdEject, sdExport, sdCreate, sdImport, sdRetry}) b->setEnabled(enabled);
}
QString MainWindow::sessionFolder() const {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/frond/sessions";
}
QString MainWindow::firmwareRoot() const {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/frond/firmware";
}
QString MainWindow::sessionFile() const {
    const auto* item = sessionList->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}
frondSessions::Info MainWindow::infoFor(const QString& path) const { return frondSessions::describe(path); }

namespace {
// Draws its text on one line, cut with an ellipsis when the row is narrower.
class ElidedLabel : public QLabel {
 public:
    using QLabel::QLabel;
    QSize minimumSizeHint() const override { return {0, QLabel::sizeHint().height()}; }
 protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, foregroundRole()));
        painter.drawText(rect(), Qt::AlignVCenter | Qt::AlignLeft, fontMetrics().elidedText(text(), Qt::ElideRight, width()));
    }
};
}

// The session path with the folder resolved, as the settings keep it; the file need not exist.
static QString canonicalSession(const QString& path) {
    const QFileInfo file(path);
    const auto folder = file.dir().canonicalPath();
    return folder.isEmpty() ? file.absoluteFilePath() : folder + '/' + file.fileName();
}

// Rebuilds the list from the managed folder, keeping 'select' (else the current row) selected.
void MainWindow::refreshSessions(const QString& select) {
    const auto keep = select.isEmpty() ? sessionFile() : select;
    const QSignalBlocker blocked(sessionList);
    sessionList->clear();
    int row = 0, selectRow = -1;
    for (const auto& info : frondSessions::list(sessionFolder())) {
        const auto device = frondFirmware::deviceFromId(info.device);
        const auto label = info.old && device == frondFirmware::Device::automatic ? QString() : frondFirmware::deviceName(device);
        QStringList parts;
        if (!label.isEmpty()) parts << label;
        if (info.old) parts << tr("Saved by an older Frond");
        else if (info.saved) parts << tr("Saved %1").arg(QLocale().toString(info.when, QLocale::ShortFormat))
                                   << QLocale().formattedDataSize(info.size);
        else parts << tr("Not saved");
        // "<device> · Saved <date> · <size>"
        const auto detail = parts.join(QString::fromUtf8(" · "));
        auto* item = new QListWidgetItem(sessionList);
        item->setData(Qt::UserRole, info.path);
        item->setData(Qt::UserRole + 1, info.old);
        item->setToolTip(info.path);
        auto* line = new QWidget;
        auto* box = new QHBoxLayout(line);
        box->setContentsMargins(4, 2, 2, 2);
        auto* text = new QVBoxLayout;
        text->setSpacing(0);
        auto* name = new ElidedLabel(info.name, line);
        name->setObjectName("sessionName");
        auto nameFont = name->font(); nameFont.setBold(true); name->setFont(nameFont);
        auto* second = new ElidedLabel(detail, line);
        second->setObjectName("sessionDetail");
        if (info.old) { name->setEnabled(false); second->setEnabled(false); }
        else second->setForegroundRole(QPalette::PlaceholderText);
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        second->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        text->addWidget(name); text->addWidget(second);
        box->addLayout(text, 1);
        auto* more = new QPushButton(QString::fromUtf8("…"), line);
        more->setObjectName("rowMenu");
        more->setToolTip(tr("More"));
        more->setFixedWidth(32);
        more->setMenu(rowMenu(info.path, more));
        box->addWidget(more);
        item->setSizeHint(QSize(0, line->sizeHint().height() + 6));
        sessionList->setItemWidget(item, line);
        if (info.path == keep) selectRow = row;
        row++;
    }
    if (selectRow < 0 && row > 0) selectRow = 0;
    if (selectRow >= 0) sessionList->setCurrentRow(selectRow);
    sessionList->setVisible(row > 0);
    emptyNote->setVisible(row == 0);
    refresh();
}
// Rename, Export and Delete for one session; each action looks the session up again when used.
QMenu* MainWindow::rowMenu(const QString& path, QWidget* parent) {
    const auto info = infoFor(path);
    auto* menu = new QMenu(parent);
    auto* rename = menu->addAction(tr("Rename…"), this, [this, path] { QTimer::singleShot(0, this, [this, path] { renameSession(path); }); });
    rename->setObjectName("renameSession"); rename->setEnabled(!info.old);
    auto* exportAction = menu->addAction(tr("Export…"), this, [this, path] { QTimer::singleShot(0, this, [this, path] { exportSession(path); }); });
    exportAction->setObjectName("exportSession"); exportAction->setEnabled(info.saved && !info.old);
    auto* remove = menu->addAction(tr("Delete"), this, [this, path] { QTimer::singleShot(0, this, [this, path] { deleteSession(path); }); });
    remove->setObjectName("deleteSession");
    return menu;
}
void MainWindow::renameSession(const QString& path) {
    const auto info = infoFor(path);
    bool ok;
    const auto name = QInputDialog::getText(this, tr("Rename"), tr("Name"), QLineEdit::Normal, info.name, &ok);
    if (!ok) return;
    const auto error = frondSessions::rename(sessionFolder(), info, name);
    if (!error.isEmpty()) return failure(error);
    const auto renamed = frondSessions::filePath(sessionFolder(), frondSessions::cleanName(name));
    if (settings.value("lastSession").toString() == canonicalSession(path))
        settings.setValue("lastSession", canonicalSession(renamed));
    refreshSessions(renamed);
}
void MainWindow::exportSession(const QString& path) {
    const auto info = infoFor(path);
    if (!info.saved) return;
    const auto target = outputFile(tr("Export"), tr("Frond sessions (*.frond)"), info.name + ".frond");
    if (target.isEmpty()) return;
    const auto error = frondSessions::exportFile(info, target);
    if (!error.isEmpty()) failure(error);
    else statusBar()->showMessage(tr("Session exported."));
}
void MainWindow::deleteSession(const QString& path) {
    const auto info = infoFor(path);
    if (QMessageBox::question(this, tr("Delete"), tr("Delete %1?").arg(info.name),
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    const auto last = canonicalSession(path);
    const auto error = frondSessions::remove(info);
    if (!error.isEmpty()) failure(error);
    else if (settings.value("lastSession").toString() == last) settings.remove("lastSession");
    refreshSessions();
}
QString MainWindow::importSession(const QString& path) {
    QString name;
    const auto error = frondSessions::importFile(sessionFolder(), path, &name);
    if (error.isEmpty()) refreshSessions(frondSessions::filePath(sessionFolder(), name));
    return error;
}
void MainWindow::importSessionDialog() {
    if (client.running()) return;
    const auto path = QFileDialog::getOpenFileName(this, tr("Import session"), {}, tr("Frond sessions (*.frond *.session);;All files (*)"));
    if (path.isEmpty()) return;
    const auto error = importSession(path);
    if (!error.isEmpty()) failure(error);
    else statusBar()->showMessage(tr("Session imported."));
}
void MainWindow::showFirmware(frondFirmware::Device device) {
    FirmwareDialog dialog(firmwareRoot(), device, this);
    dialog.exec();
}
// True once the device's firmware is stored, asking for it through the Firmware dialog if not.
bool MainWindow::ensureFirmware(frondFirmware::Device device) {
    if (frondFirmware::ready(firmwareRoot(), device)) return true;
    FirmwareDialog dialog(firmwareRoot(), device, this);
    if (dialog.exec() != QDialog::Accepted) return false;
    if (frondFirmware::ready(firmwareRoot(), device)) return true;
    failure(tr("That firmware is for the %1. The %2 still needs its own.")
                .arg(frondFirmware::deviceName(dialog.imported()), frondFirmware::deviceName(device)));
    return false;
}
void MainWindow::addDevice() {
    if (client.running() || busy) return;
    AddDeviceDialog dialog(firmwareRoot(), this);
    if (dialog.exec() != QDialog::Accepted) return;
    const auto device = dialog.device();
    if (!ensureFirmware(device)) return;
    const auto name = frondSessions::reserve(sessionFolder(), frondFirmware::deviceName(device), frondFirmware::deviceId(device));
    if (name.isEmpty())
        return failure(tr("Could not create the session storage directory."));
    const auto path = frondSessions::filePath(sessionFolder(), name);
    refreshSessions(path);
    startSession(infoFor(path));
}
// The command line and resume: a session file anywhere. One outside the managed folder is
// opened as it is and saved as a new session in the folder.
void MainWindow::openSession(const QString& path, bool launchNow) {
    startupHandled = true;
    if (client.running()) return;
    if (!launchNow) {
        const auto error = importSession(QFileInfo(path).absoluteFilePath());
        if (!error.isEmpty()) failure(error);
        return;
    }
    if (!QFileInfo(core).isExecutable()) return failure(tr("The Frond emulator executable could not be found. Reinstall Frond or build both desktop and core targets."));
    const auto source = QFileInfo(QFileInfo(path).absoluteFilePath()).canonicalFilePath();
    if (source.isEmpty() || !QFileInfo(source).isFile()) return failure(tr("Choose an existing session file."));
    if (!QDir().mkpath(sessionFolder())) return failure(tr("Could not create the session storage directory."));
    // Compare canonical paths so an XDG data directory symlink does not clone on every resume.
    const auto managedRoot = QFileInfo(sessionFolder()).canonicalFilePath();
    const auto header = frondSessions::readHeader(source);
    auto device = frondFirmware::deviceFromId(header.device);
    if (device == frondFirmware::Device::automatic) device = frondFirmware::Device::tx;
    QString target = source;
    bool reserved = false;
    if (!source.startsWith(managedRoot + '/')) {
        // The marker holds the new name until the first save, so no other Frond takes it.
        const auto name = frondSessions::reserve(managedRoot, frondFirmware::deviceName(device), frondFirmware::deviceId(device));
        if (name.isEmpty()) return failure(tr("Could not create the session storage directory."));
        target = frondSessions::filePath(managedRoot, name);
        reserved = true;
    }
    launch(target, {source}, device);
    if (reserved && !busy) QFile::remove(frondSessions::markerPath(target));  // it didn't start
    refreshSessions();
}
void MainWindow::start() {
    if (client.running()) return;
    startupHandled = true;
    const auto path = sessionFile();
    if (path.isEmpty()) return;
    startSession(infoFor(path));
}
void MainWindow::startSession(const frondSessions::Info& info) {
    if (client.running() || info.old) return;
    startupHandled = true;
    if (!QFileInfo(core).isExecutable()) return failure(tr("The Frond emulator executable could not be found. Reinstall Frond or build both desktop and core targets."));
    const auto device = frondFirmware::deviceFromId(info.device);
    QStringList arguments;
    if (info.saved) {
        const auto source = QFileInfo(info.path).canonicalFilePath();
        if (source.isEmpty()) return failure(tr("Choose an existing session file."));
        arguments << source;
    } else {
        if (!ensureFirmware(device)) return;
        arguments << "--device" << frondFirmware::deviceId(device) << frondFirmware::storedNor(firmwareRoot(), device);
        if (!frondFirmware::storedNand(firmwareRoot(), device).isEmpty())
            arguments << "--nand" << frondFirmware::storedNand(firmwareRoot(), device);
    }
    if (!QDir().mkpath(sessionFolder())) return failure(tr("Could not create the session storage directory."));
    launch(QFileInfo(sessionFolder()).canonicalFilePath() + '/' + QFileInfo(info.path).fileName(), arguments, device);
}
// Starts the core on 'arguments' (the session file or the firmware) and saves to 'target'.
void MainWindow::launch(const QString& target, QStringList arguments, frondFirmware::Device startDevice) {
    sessionPath = target;
    if (!audio->isChecked()) arguments << "--no-sound";
    const auto caps = frondFirmware::capabilities(startDevice);
    if (fastAudio->isChecked() && caps.fastAudio) arguments << "--fast-audio";
    if (settings.contains("windowScale"))
        arguments << "--window-scale" << QString::number(windowScale->value() / 4.0);
    if (smoothLcd->isChecked()) arguments << "--smooth-lcd";
    if (caps.serial && serialArgument() != "off") arguments << "--serial" << serialArgument();
    if (caps.usb && usbArgument() != "off") arguments << "--usb" << usbArgument();
    if(caps.infrared && irdaArgument()!="off")arguments << "--irda" << irdaArgument();
    if(caps.bluetooth && bluetooth->isChecked()) arguments << "--bluetooth" << bluetoothArgument();
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
    refresh(); statusBar()->showMessage(launched ? tr("Starting the Palm…") : tr("Could not start the Palm."));
}
QString MainWindow::bluetoothArgument() const {
    return bluetooth->isChecked() ? bluetoothSocket->text() : QStringLiteral("off");
}
void MainWindow::loadBluetoothSettings() {
    bluetooth->setChecked(settings.value("bluetoothEnabled",false).toBool());
    bluetoothSocket->setText(settings.value("bluetoothSocket").toString());
}
void MainWindow::showBluetooth(const QString& state) {
    bluetoothState->setText(state.isEmpty() || state=="off" ? QString() : tr("Bluetooth: %1").arg(state));
}
QString MainWindow::irdaArgument() const {
    return irda->currentData()=="tcp" ? "tcp:"+QString::number(irdaPort->value()) : irda->currentData().toString();
}
void MainWindow::loadIrdaSettings() {
    irda->setCurrentIndex(std::max(0,irda->findData(settings.value("irdaMode","off").toString())));
    irdaPort->setValue(settings.value("irdaPort",5030).toInt());
}
void MainWindow::showIrda(const QString& state) {
    if(state.isEmpty()){irdaState->clear();return;}
    const auto parts=state.split(' ',Qt::SkipEmptyParts);
    const auto mode=parts.value(0).section(':',0,0);
    irda->setCurrentIndex(std::max(0,irda->findData(mode)));
    if(mode=="tcp"){
        const auto port=parts.value(1).section(':',-1).toInt();if(port>0)irdaPort->setValue(port);
    }
    settings.setValue("irdaMode",irda->currentData());settings.setValue("irdaPort",irdaPort->value());
    irdaState->setText(state=="off"?QString():tr("Infrared: %1").arg(state));refresh();
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
                                                    parts[3] == "running" ? tr("%1 enumerated").arg(deviceLabel()) : tr("%1 %2").arg(deviceLabel(), parts[3])));
    else usbState->setText(tr("USB: %1").arg(state));
}
void MainWindow::loadUsbSettings() {
    usb->setCurrentIndex(std::max(0, usb->findData(settings.value("usbMode", "off").toString())));
    usbPort->setValue(settings.value("usbPort", 5024).toInt());
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
    const auto path = saveAs ? outputFile(tr("Save a Copy"), tr("Frond sessions (*.frond)"), deviceLabel() + ".frond") : sessionPath;
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
// The device the controls follow: the running one, else the TX (nothing greyed).
frondFirmware::Device MainWindow::currentDevice() const {
    if (client.running()) return runningDevice;
    return frondFirmware::Device::tx;
}
QString MainWindow::deviceLabel() const {
    return frondFirmware::deviceName(runningDevice);
}
void MainWindow::showDevice(const QString& id) {
    runningDevice = frondFirmware::deviceFromId(id);
    if (runningDevice == frondFirmware::Device::automatic) runningDevice = frondFirmware::Device::tx;
    if (!frondFirmware::capabilities(runningDevice).serial) showSerial("off");
    refresh();
}
void MainWindow::failure(const QString& message) {
    QMessageBox::critical(this, tr("Frond"), message);
}
void MainWindow::closeEvent(QCloseEvent* event) {
    if (allowClose || !client.running()) { settings.setValue("windowGeometry", saveGeometry()); event->accept(); return; }
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
