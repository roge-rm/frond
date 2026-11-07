#include "MainWindow.h"
#include "FirmwareInput.h"
#include "SdExchangeDialog.h"
#include "MemoryStickFilesDialog.h"
#include "Dialogs.h"
#include "FirmwareStore.h"
#include "AudioQualification.h"
#include "ConnectionsWindow.h"
#include "LinkCommon.h"
#include "CardsWindow.h"
#include <QCoreApplication>
#include <QFileSystemWatcher>
#include <QJsonObject>
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
#include <QRadioButton>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

// frond-68k sits beside the ARM core: out/frond-68k in a build tree, frond-68k-core when installed.
static QString siblingCore68k(const QString& core) {
    if (core.isEmpty()) return {};
    const QFileInfo info(core);
    const auto name = info.fileName();
    const auto sibling = name == QLatin1String("frond-core") ? QStringLiteral("frond-68k-core") : name + QStringLiteral("-68k");
    return info.absolutePath() + '/' + sibling;
}

MainWindow::MainWindow(const QString& core, const QString& core68k)
    : core(core), core68k(core68k.isEmpty() ? siblingCore68k(core) : core68k) {
    frondCards::defaultCore() = core;
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
    settingsDialog->resume->setChecked(settings.value("resume", true).toBool());
    connect(settingsDialog->resume, &QCheckBox::toggled, this, [this](bool enabled) { settings.setValue("resume", enabled); });
    // The running page follows the Android Menu screen: Applications, SD card, Device, Session.
    auto section = [&](const QString& title, bool first = false) {
        auto* heading = new QLabel(title, root);
        auto font = heading->font(); font.setPointSizeF(font.pointSizeF() * 1.2); font.setBold(true);
        heading->setFont(font);
        if (!first) layout->addSpacing(8);
        layout->addWidget(heading);
        return heading;
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
    storageHeading = section(tr("SD card"));
    storageHeading->setObjectName("storageHeading");
    // One row: the card in the slot (opens the picker), its state when the buttons don't say it
    // (Ejected, or why there's no button) and the card's buttons. A linked card held elsewhere
    // shows where it is, and Insert once it is free.
    auto* cardRow = new QHBoxLayout;
    cardPick = button(tr("No card"), "cardPick", true);
    sdStatus = new QLabel(tr("Not known"), root);
    sdStatus->setObjectName("sdState");
    cardRow->addWidget(cardPick);
    cardRow->addWidget(sdStatus);
    sdReinsert = button(tr("Insert"), "sdInsert", true);
    sdExchange = button(tr("Files"), "sdExchange", true);
    sdEject = button(tr("Eject"), "sdEject", true);
    sdExport = button(tr("Export image"), "sdExport", true);
    sdRetry = button(tr("Try again"), "sdRetry", true);
    for (auto* b : {sdReinsert, sdExchange, sdEject, sdExport, sdRetry}) cardRow->addWidget(b);
    cardRow->addStretch();
    layout->addLayout(cardRow);
    auto* linkedRow = new QHBoxLayout;
    linkedState = new QLabel(root);
    linkedState->setObjectName("linkedState");
    linkedInsert = button(tr("Insert"), "linkedInsert", true);
    linkedRow->addWidget(linkedState); linkedRow->addWidget(linkedInsert); linkedRow->addStretch();
    layout->addLayout(linkedRow);
    connect(cardPick, &QPushButton::clicked, this, &MainWindow::pickCard);
    connect(linkedInsert, &QPushButton::clicked, this, [this] {
        const auto slot = frondCards::readSlot(runningMachineDir());
        if (!slot.link.isEmpty() && !cardPresent) changeCard(slot.link);
    });
    connect(sdEject, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Eject the card?"), tr("Close any files open on the Palm first."),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) send(cardCommand("eject"));
    });
    connect(sdReinsert, &QPushButton::clicked, this, [this] { send(cardCommand("reinsert")); });
    connect(sdExport, &QPushButton::clicked, this, [this] {
        // A Memory Stick Pro exports as a raw image; ask the core which kind is in the slot.
        if (memoryStick()) { msExportPending = true; send("ms-type"); return; }
        const auto file = outputFile(tr("Export image"), memoryStick() ? tr("Memory Stick images (*.frondms)") : tr("SD images (*.img)"),
                                     memoryStick() ? "Memory Stick.frondms" : "SD card.img");
        if (!file.isEmpty()) send(cardCommand("export"), file);
    });
    connect(sdExchange, &QPushButton::clicked, this, &MainWindow::beginExchange);
    connect(sdRetry, &QPushButton::clicked, this, [this] { send(cardCommand("state")); });

    cardReadOnly = new QCheckBox(tr("Write protected"), root);
    cardReadOnly->setObjectName("cardReadOnly");
    layout->addWidget(cardReadOnly);
    connect(cardReadOnly, &QCheckBox::clicked, this, [this](bool on) { send("ms-read-only", on ? "on" : "off"); });

    section(tr("Device"));
    auto* grid = new QGridLayout;
    layout->addLayout(grid);
    grid->setHorizontalSpacing(12);
    auto label = [&](const QString& text, int row) {
        auto* l = new QLabel(text, root);
        grid->addWidget(l, row, 0);
        return l;
    };
    // One row per device: label, control, and the port beside it. Readout rows hide while empty.
    soundToggle = new QCheckBox(tr("On"), root);
    soundToggle->setObjectName("sound");
    label(tr("Sound"), 1); grid->addWidget(soundToggle, 1, 1, 1, 2);
    microphoneToggle = new QCheckBox(tr("On"), root);
    microphoneToggle->setObjectName("microphone");
    microphoneLabel = label(tr("Microphone"), 14); grid->addWidget(microphoneToggle, 14, 1, 1, 2);
    microphoneState = new QLabel(root); grid->addWidget(microphoneState, 15, 1, 1, 2);
    connect(microphoneToggle, &QCheckBox::clicked, this, [this](bool on) {
        send("microphone-mode", on ? "on" : "off");
    });
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
    networkLabel = label(tr("Network"), 2); grid->addWidget(network, 2, 1, 1, 2);
    keyboard = new QCheckBox(tr("Attached"),root);
    keyboard->setObjectName("keyboardAttached");
    keyboard->setToolTip(tr("Uses the Palm Universal Wireless Keyboard protocol. Install and enable its Palm OS driver first. Type in the Palm window; modifiers and key releases are sent to the driver."));
    connect(keyboard,&QCheckBox::clicked,this,[this](bool attached) { send("keyboard-attach",attached?"on":"off"); });
    keyboardLabel = label(tr("Infrared keyboard"), 3); grid->addWidget(keyboard, 3, 1, 1, 2);
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
    serialLabel = label(tr("Serial port"), 4); grid->addWidget(serial, 4, 1); grid->addWidget(serialPort, 4, 2);
    serialState = new QLabel(root);
    serialState->setObjectName("serialState");
    serialState->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addWidget(serialState, 5, 1, 1, 2);
    // While running, the choice is saved only once the core has opened it.
    auto serialChanged = [this] {
        if (client.running()) {
            if (client.available()) send("serial-mode", serialArgument());
            return;
        }
        refresh();
    };
    connect(serial, qOverload<int>(&QComboBox::activated), this, serialChanged);
    connect(serialPort, &QSpinBox::editingFinished, this, [this, serialChanged] {
        if (serial->currentData() == "tcp") serialChanged();
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
    usbLabel = label(tr("USB HotSync"), 6); grid->addWidget(usb, 6, 1); grid->addWidget(usbPort, 6, 2);
    usbState = new QLabel(root);
    usbState->setObjectName("usbState");
    usbState->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addWidget(usbState, 7, 1, 1, 2);
    auto usbChanged = [this] {
        if (client.running()) {
            if (client.available()) send("usb-mode", usbArgument());
            return;
        }
        refresh();
    };
    connect(usb, qOverload<int>(&QComboBox::activated), this, usbChanged);
    connect(usbPort, &QSpinBox::editingFinished, this, [this, usbChanged] {
        if (usb->currentData() == "tcp") usbChanged();
    });
    showUsb({});
    irda=new QComboBox(root);irda->setObjectName("irdaMode");
    irda->addItem(tr("Off"),"off");
    irda->addItem(tr("Terminal (PTY)"),"pty");
    irda->addItem(tr("Wait for a peer"),"tcp");
    irda->setToolTip(tr("Pick a connection to beam to another Frond. Pair them in File > Connections."));
    irdaPort=new QSpinBox(root);irdaPort->setObjectName("irdaPort");
    irdaPort->setRange(1,65535);irdaPort->setPrefix(tr("Port "));loadIrdaSettings();
    label(tr("Infrared beaming"), 8); grid->addWidget(irda, 8, 1); grid->addWidget(irdaPort, 8, 2);
    irdaState=new QLabel(root);irdaState->setObjectName("irdaState");
    irdaState->setTextInteractionFlags(Qt::TextSelectableByMouse);grid->addWidget(irdaState, 9, 1, 1, 2);
    auto irdaChanged=[this] {
        if(client.running()){if(client.available())send("irda-mode",irdaArgument());return;}
        refresh();
    };
    connect(irda,qOverload<int>(&QComboBox::activated),this,irdaChanged);
    connect(irdaPort,&QSpinBox::editingFinished,this,[this,irdaChanged]{if(irda->currentData()=="tcp")irdaChanged();});

    bluetooth = new QCheckBox(tr("On"), root);
    bluetooth->setObjectName("bluetoothEnabled");
    bluetoothSocket = new QLineEdit(root);
    bluetoothSocket->setObjectName("bluetoothSocket");
    bluetoothSocket->setPlaceholderText(tr("Absolute controller socket path"));
    loadBluetoothSettings();
    bluetoothLabel = label(tr("Bluetooth"), 10); grid->addWidget(bluetooth, 10, 1, 1, 2); grid->addWidget(bluetoothSocket, 11, 1, 1, 2);
    bluetoothState = new QLabel(root); bluetoothState->setObjectName("bluetoothState");
    bluetoothState->setWordWrap(true); bluetoothState->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addWidget(bluetoothState, 12, 1, 1, 2);
    grid->setColumnStretch(1, 1);
    grid->setColumnMinimumWidth(2, 110);
    for (auto* c : {static_cast<QWidget*>(network), static_cast<QWidget*>(serial), static_cast<QWidget*>(usb), static_cast<QWidget*>(irda)})
        c->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    holdLabel = new QLabel(tr("Hold"), root);
    holdToggle = new QCheckBox(tr("On"), root);
    holdToggle->setObjectName("hold");
    grid->addWidget(holdLabel, 13, 0);
    grid->addWidget(holdToggle, 13, 1, 1, 2);
    connect(holdToggle, &QCheckBox::clicked, this, [this](bool on) {
        if (frondFirmware::profile(runningDevice).frontend.holdSupported && holdKnown)
            send("hold", on ? "on" : "off");
    });
    // 68k devices: LCD contrast, sent live and kept in the machine settings and the session.
    contrastLabel = label(tr("Contrast"), 0);
    contrast = new QSlider(Qt::Horizontal, root);
    contrast->setObjectName("runningContrast");
    contrast->setRange(0, 100);
    contrast->setPageStep(10);
    contrast->setTracking(false);
    grid->addWidget(contrast, 0, 1, 1, 2);
    runningButtons << contrast;
    connect(contrast, &QSlider::valueChanged, this, [this](int value) {
        if (client.available() && frondFirmware::runs68k(runningDevice) && value != runningPreferences.contrast)
            send("display", "contrast=" + QString::number(value));
    });
    auto* reset = button(tr("Reset"), "reset");
    auto* rotate = button(tr("Rotate"), "rotate");
    rotateButton = rotate;
    // The connector's HotSync button: the IIIx cradle, the T|X and the E2.
    hotSyncButton = button(tr("HotSync"), "hotsync");
    auto* deviceButtons = new QHBoxLayout;
    deviceButtons->addWidget(reset); deviceButtons->addWidget(rotate); deviceButtons->addWidget(hotSyncButton);
    deviceButtons->addStretch();
    layout->addLayout(deviceButtons);
    connect(hotSyncButton, &QPushButton::clicked, this, [this] { send("hotsync"); });
    connect(reset, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, tr("Reset"), tr("Reset the %1 now?").arg(deviceLabel())) == QMessageBox::Yes) send("reset");
    });
    connect(rotate, &QPushButton::clicked, this, [this] { send("rotate"); });
    connect(soundToggle, &QCheckBox::clicked, this, [this](bool enabled) {
        if (!frondFirmware::capabilities(runningDevice).sound) return;
        auto prefs = runningPreferences;
        prefs.audio = enabled;
        const auto error = frondSessions::writePreferences(sessionPath, prefs);
        if (!error.isEmpty()) {
            soundToggle->setChecked(runningPreferences.audio);
            return failure(error);
        }
        runningPreferences = prefs;
        send(enabled ? "audio-on" : "audio-off");
    });

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
        refresh();
    };
    connect(bluetooth, &QCheckBox::clicked, this, bluetoothChanged);
    connect(bluetoothSocket, &QLineEdit::editingFinished, this, bluetoothChanged);

    stopButton = new QPushButton(tr("Save and Stop"), sessionBar);
    stopButton->setObjectName("stop");
    forceButton = new QPushButton(tr("Force Stop…"), sessionBar);
    forceButton->setObjectName("forceStop");
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
    file->addAction(tr("C&ards…"), this, [this] { showCards(); })->setObjectName("cardsAction");
    file->addAction(tr("&Connections…"), this, &MainWindow::showConnections)->setObjectName("connectionsAction");
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
               "On the Sony CLIÉ TG50, F1 to F4 are Date Book, Address, To Do and Memo Pad, F5 is the voice button, F6 is Home, and left and right Alt are Blue and Red.\n"
               "Escape: Power (sleep and wake are experimental)\n\n"
               "Palm OS button assignments may change what the application buttons do."));
    });
    help->addAction(tr("&About Frond"), this, [this] {
        QMessageBox::about(this, tr("About Frond"), tr("Frond %1\nExperimental Palm TX, Tungsten E2, Zire 72, Sony CLIÉ TG50 and Palm IIIx emulator\nGPL-3.0-or-later").arg(QStringLiteral(FROND_VERSION)));
    });
    connect(&client, &DesktopClient::diagnostic, this, [this](const QString& text) {
        log->moveCursor(QTextCursor::End); log->insertPlainText(text);
    });
    connect(&client, &DesktopClient::error, this, [this](const QString& message) {
        // The end of the emulator log, since the log button goes away when the core stops.
        auto lines = log->toPlainText().split('\n');
        while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
        failure(message, lines.mid(qMax(0, int(lines.size()) - 40)).join('\n'));
    });
    connect(&client, &DesktopClient::stalled, this, [this] {
        forceButton->show(); statusBar()->showMessage(tr("The emulator is not responding. Waiting for it to finish…"));
    });
    connect(&client, &DesktopClient::started, this, [this] {
        microphoneArmed = false; microphoneState->clear();
        busy = false; hardResetAfterSave = false;
        frondSessions::touchWork(sessionPath);
        statusBar()->showMessage(tr("Running. Your session will be saved when you close Frond.")); refresh();
        if (stopping) save();
        else send("device-state");
    });
    connect(&client, &DesktopClient::finished, this, [this] {
        if (exchangeDialog) exchangeDialog->reject();
        else endExchange();
        if (memoryStickFiles) memoryStickFiles->reject();
        if (cardProgress) { cardProgress->hide(); cardProgress->deleteLater(); cardProgress = nullptr; }
        holdKnown = holdOn = false;
        microphoneArmed = false; microphoneState->clear();
        busy = false; stopping = false; hardResetAfterSave = false; cardPresent = cardInserted = false; runningDevice = frondFirmware::Device::tx;
        copyLock.reset(); savingPath.clear();
        frondSessions::removeStaging(copyStaging); copyStaging.clear(); pendingCopy.clear();
        // A linked card goes back to the drawer once the final save is in, under the session lock.
        if (finalSaved && sessionLock)
            if (const auto error = frondCards::releaseAtClose(sessionFolder(), frondSessions::machineDirOf(sessionPath)); !error.isEmpty())
                log->appendPlainText(error);
        finalSaved = false; linkedWatcher.reset(); linkedWatcherId.clear(); linkedHolder.clear(); cardMove = {};
        sdKnown = protectionKnown = false; showSerial({}); loadSerialSettings(); showUsb({}); loadUsbSettings(); showIrda({}); link.setSticky("ir", {}); irdaLinkText.clear(); loadIrdaSettings(); showBluetooth({}); loadBluetoothSettings(); sessionLock.reset(); forceButton->hide(); refreshSessions();
        if (closeAfterStop) { allowClose = true; close(); }
        else statusBar()->showMessage(tr("Stopped."));
    });
    connect(&client, &DesktopClient::closeRequested, this, [this] { stop(false); });
    connect(&client, &DesktopClient::microphoneState, this, [this](const QString& state) {
        microphoneArmed = state == "on";
        microphoneState->setText(state.startsWith("off ") ? state.mid(4) : QString());
        refresh();
    });
    connect(&client, &DesktopClient::serialState, this, [this](const QString& state) { showSerial(state); });
    connect(&client, &DesktopClient::usbState, this, [this](const QString& state) { showUsb(state); });
    connect(&client, &DesktopClient::bluetoothState, this, &MainWindow::showBluetooth);
    connect(&client,&DesktopClient::infraredState,this,[this](const QString& state){showIrda(state);});
    connect(&link, &LinkClient::message, this, [this](const QJsonObject& m) {
        const auto type = m.value("type").toString();
        if (type == "ir") showIrdaLink(m);
        else if (type == "state") refreshIrdaChoices();
    });
    storeWatcher = new QFileSystemWatcher(this);
    if (QDir().mkpath(frondLink::storeFolder())) storeWatcher->addPath(frondLink::storeFolder());
    connect(storeWatcher, &QFileSystemWatcher::directoryChanged, this, [this] { refreshIrdaChoices(); });
    connect(&client, &DesktopClient::cardState, this, [this](const QString& state) {
        if (state != "inserted") return;
        sdKnown = cardPresent = cardInserted = true;
        refresh();
    });
    connect(&client, &DesktopClient::sdProgress, this, [this](qint64 done, qint64 total) {
        if (cardProgress && total > 0) cardProgress->setValue(int(done * 1000 / total));
    });
    connect(&client, &DesktopClient::result, this, [this](const QString& command, bool ok, const QString& code, const QString& message) {
        if (cardProgress) { cardProgress->hide(); cardProgress->deleteLater(); cardProgress = nullptr; }
        busy = false; forceButton->hide(); refresh();
        if (memoryStickFiles && QStringList({"ms-files", "ms-get", "ms-put", "ms-mkdir", "ms-delete", "ms-rename"}).contains(command)) return;
        if (!ok && cardMove.stage != CardMove::None) { cardMoveFailed(command, message); return; }
        if (!ok && exchangeStage != ExchangeStage::None) { exchangeResult(false, message); return; }
        if (!ok) {
            if (code == "cancelled" && !stopping) { statusBar()->showMessage(tr("Cancelled.")); return; }
            if (command == "save") { copyLock.reset(); pendingCopy.clear(); frondSessions::removeStaging(copyStaging); copyStaging.clear(); }
            hardResetAfterSave = false;
            log->appendPlainText(command + ": " + message);
            if (command == "hold-state" || command == "hold") { holdKnown = false; refresh(); }
            if (command == "sd-state" || command == "ms-state") sdKnown = false;
            if (command == "ms-type") msExportPending = false;
            if (command == "ms-protection" || command == "ms-read-only") { protectionKnown = false; refresh(); }
            if (command == "microphone-mode") { microphoneArmed = false; microphoneState->setText(message); }
            if (command == "serial-mode") showSerial("off " + message);  // the core is now off
            if (command == "usb-mode") showUsb("off " + message);
            if(command=="irda-mode")showIrda("off error: "+message);
            if (command == "display") { const QSignalBlocker blocked(contrast); contrast->setValue(runningPreferences.contrast); }
            if (stopping) {
                if (command == "save" && savingPath == sessionPath) askStop(message);
                else save();
                return;
            }
            if (code == "not-ready" && (command == "export" || command == "export-db" || command == "install" || command == "launch-app"))
                failure(message + tr("\n\nWait for Palm OS to finish its current activity, then try again."));
            else failure(message);
            if (command == "hold" && !stopping) send("hold-state");
            if (command == "ms-read-only" && !stopping) send("ms-protection");
            return;
        }
        if (command == "ms-type" && msExportPending) {
            msExportPending = false;
            const bool pro = message.startsWith("pro ");
            const auto file = outputFile(tr("Export image"), pro ? tr("Memory Stick Pro images (*.img)") : tr("Memory Stick images (*.frondms)"),
                                         pro ? "Memory Stick Pro.img" : "Memory Stick.frondms");
            if (!file.isEmpty()) send("ms-export", file);
            return;
        }
        if (command == "device-state") {
            showDevice(message);
            if (stopping) save();
            else send(frondFirmware::profile(runningDevice).frontend.holdPresent ? "hold-state" :
                storageSupported() ? cardCommand("state") : "network-state");
            return;
        }
        if (command == "hold-state") {
            holdKnown = message == "on" || message == "off";
            holdOn = message == "on";
            refresh();
            if (stopping) save();
            else send(storageSupported() ? cardCommand("state") : "network-state");
            return;
        }
        if ((command == "hold" || command == "reset" || command == "hard-reset") &&
            !stopping && frondFirmware::profile(runningDevice).frontend.holdPresent) {
            send("hold-state");
            return;
        }
        if (command == "sd-state" || command == "ms-state") {
            sdKnown = message == "none" || message == "inserted" || message == "ejected";
            cardPresent = sdKnown && message != "none";
            cardInserted = message == "inserted";
        }
        else if (command == "sd-create" || command == "sd-insert" || command == "sd-reinsert" ||
                 command == "ms-create" || command == "ms-insert" || command == "ms-reinsert") cardPresent = cardInserted = true;
        else if (command == "sd-eject" || command == "ms-eject") cardInserted = false;
        else if (command == "sd-take-out" || command == "ms-take-out") cardPresent = cardInserted = false;
        else if (command == "sd-put-in" || command == "ms-put-in") cardPresent = cardInserted = true;
        if ((command.startsWith("sd-") || command.startsWith("ms-")) &&
            command != "sd-state" && command != "ms-state") sdKnown = true;
        if (command == "ms-protection" || command == "ms-read-only") {
            protectionKnown = message == "read-only" || message == "writable";
            cardReadOnly->setChecked(message == "read-only");
        }
        if (cardMove.stage != CardMove::None && continueCardMove(command, message)) return;
        if (exchangeStage != ExchangeStage::None) { exchangeResult(true, message); return; }
        if (command == "network-state" || command == "network-mode") {
            const auto index = network->findData(message);
            if (index >= 0) network->setCurrentIndex(index);
        }
        if (command == "microphone-state" || command == "microphone-mode") {
            microphoneArmed = message == "on";
            microphoneState->setText(message.startsWith("off ") ? message.mid(4) : QString());
            if (command == "microphone-mode") {
                runningPreferences.microphone = microphoneArmed;
                frondSessions::writePreferences(sessionPath, runningPreferences);
            }
        }
        if (command=="keyboard-state" || command=="keyboard-attach") keyboard->setChecked(message=="attached");
        if (command == "display") {
            runningPreferences.contrast = contrast->value();
            runningPreferences.contrastSet = true;
            if (const auto error = frondSessions::writePreferences(sessionPath, runningPreferences); !error.isEmpty()) failure(error);
        }
        if (command == "serial-mode") { runningPreferences.serial = serialArgument(); savePeripherals(); }
        if (command == "serial-state" || command == "serial-mode") showSerial(message);
        if (command == "usb-mode") { runningPreferences.usb = usbArgument(); savePeripherals(); }
        if (command == "usb-state" || command == "usb-mode") showUsb(message);
        if (command == "irda-mode") { runningPreferences.infrared = irdaChoice(); savePeripherals(); attachInfrared(); }
        if(command=="irda-state" || command=="irda-mode")showIrda(message);
        if(command=="bluetooth-state" || command=="bluetooth-mode") {
            showBluetooth(message);
            if(command=="bluetooth-mode") {
                runningPreferences.bluetooth = bluetooth->isChecked() ? "socket:" + bluetoothSocket->text() : QStringLiteral("off");
                savePeripherals();
            }
        }
        refresh();
        if (command == "network-state" && !stopping) { send("keyboard-state"); return; }
        if (command == "keyboard-state" && !stopping && frondFirmware::capabilities(runningDevice).serial && serialArgument() != "off") { send("serial-state"); return; }
        if ((command == "keyboard-state" || command == "serial-state") && !stopping &&
            frondFirmware::capabilities(runningDevice).usb && usbArgument() != "off") { send("usb-state"); return; }
        if ((command == "sd-state" || command == "ms-state") && !stopping) {
            send(command == "ms-state" && cardPresent ? "ms-protection" : "network-state"); return;
        }
        if (command == "ms-protection" && !stopping) { send("network-state"); return; }
        if ((command == "ms-create" || command == "ms-insert" || command == "ms-reinsert" || command == "ms-put-in") && !stopping) { send("ms-protection"); return; }
        statusBar()->showMessage(command == "save" ? tr("Saved.") : tr("Running. Sessions save automatically on close."));
        const bool savedWorkingCopy = command == "save" && savingPath == sessionPath;
        if (command == "save") {
            if (savedWorkingCopy) {
                frondCards::clearSwap(frondSessions::machineDirOf(sessionPath));
                if (stopping) finalSaved = true;
                settings.setValue("lastSession", sessionPath); settings.sync();
                QFile::remove(frondSessions::markerPath(sessionPath));
                QFile::remove(frondSessions::workPath(sessionPath));
                refreshSessions();
            }
            if (!savedWorkingCopy) {
                auto error = frondSessions::writePreferences(savingPath, runningPreferences);
                // Save a Copy wrote into a staging folder; it replaces the destination only now. It
                // carries slot.json as an export does.
                if (error.isEmpty() && !pendingCopy.isEmpty())
                    error = frondCards::stageSlot(runningMachineDir(), frondSessions::machineDirOf(savingPath));
                if (error.isEmpty() && !pendingCopy.isEmpty()) error = frondSessions::publishStaged(copyStaging, pendingCopy);
                if (!pendingCopy.isEmpty()) frondSessions::removeStaging(copyStaging);
                pendingCopy.clear(); copyStaging.clear();
                if (!error.isEmpty()) failure(error);
            }
            copyLock.reset();
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
    frondCards::startUp(sessionFolder());
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
    for (auto* state : {serialState, usbState, irdaState, bluetoothState, microphoneState}) state->setVisible(!state->text().isEmpty());
    setWindowTitle(running && !sessionPath.isEmpty() ? QFileInfo(sessionPath).completeBaseName() : tr("Frond"));
    const auto* current = sessionList->currentItem();
    startButton->setEnabled(!running && !busy && current && !current->data(Qt::UserRole + 1).toBool());
    addButton->setEnabled(!running && !busy);
    stopButton->setEnabled(running && !busy && !stopping);
    const auto caps = frondFirmware::capabilities(currentDevice());
    const auto reason = tr("Not available on the %1 yet").arg(frondFirmware::deviceName(currentDevice()));
    auto note = [&](QWidget* w, bool supported, const QString& tip) { w->setToolTip(supported ? tip : reason); };
    note(network, caps.network, networkTip); note(keyboard, caps.keyboard, keyboardTip);
    note(serial, caps.serial, serialTip); note(serialPort, caps.serial, {});
    note(usb, caps.usb, usbTip); note(usbPort, caps.usb, {});
    note(irda, caps.infrared, caps.keyboard ? irdaTip + " " + tr("Detach the infrared keyboard first.") : irdaTip);note(irdaPort,caps.infrared,{});
    const bool enabled = client.available() && !busy && !stopping && exchangeStage == ExchangeStage::None;
    for (auto* w : runningButtons) w->setEnabled(enabled);
    microphoneToggle->setEnabled(enabled && caps.microphone);
    microphoneToggle->setChecked(running && microphoneArmed);
    soundToggle->setEnabled(enabled && caps.sound);
    soundToggle->setChecked(caps.sound && runningPreferences.audio);
    note(soundToggle, caps.sound, {});
    // A 68k device's missing hardware isn't shown at all; on the others it's greyed.
    const bool m68k = frondFirmware::runs68k(currentDevice());
    auto shown = [&](bool present, std::initializer_list<QWidget*> widgets) {
        for (auto* w : widgets) w->setVisible(present);
    };
    shown(!frondFirmware::serialAbsent(currentDevice()), {serialLabel, serial, serialPort});
    if (frondFirmware::serialAbsent(currentDevice())) serialState->setVisible(false);
    shown(!m68k || caps.network, {networkLabel, network});
    shown(!m68k || caps.keyboard, {keyboardLabel, keyboard});
    shown(!m68k || caps.usb, {usbLabel, usb, usbPort});
    shown(!m68k || caps.bluetooth, {bluetoothLabel, bluetooth, bluetoothSocket});
    shown(!m68k || caps.microphone, {microphoneLabel, microphoneToggle});
    if (m68k) for (auto* state : {usbState, bluetoothState}) state->setVisible(false);
    shown(m68k, {contrastLabel, contrast});
    shown(frondFirmware::hasHotSyncButton(currentDevice()), {hotSyncButton});
    shown(!m68k, {rotateButton});
    contrast->setEnabled(enabled);
    const auto& frontend = frondFirmware::profile(currentDevice()).frontend;
    holdLabel->setVisible(frontend.holdPresent);
    holdToggle->setVisible(frontend.holdPresent);
    holdLabel->setEnabled(frontend.holdSupported);
    holdToggle->setEnabled(enabled && frontend.holdSupported && holdKnown);
    holdToggle->setChecked(holdKnown && holdOn);
    note(holdToggle, frontend.holdSupported, {});
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
    irdaPort->setVisible(!irda->currentData().toString().startsWith("link:"));
    bluetooth->setEnabled(editable && caps.bluetooth);
    bluetoothSocket->setEnabled(editable && caps.bluetooth && bluetooth->isChecked());
    bluetooth->setToolTip(caps.bluetooth ? tr("See doc/using-frond.md (Bluetooth) for controller setup.") : reason);
    // Which card buttons show follows the card's state, as on Android.
    const bool memoryStick = this->memoryStick();
    const bool supported = storageSupported();
    storageHeading->setText(memoryStick ? tr("Memory Stick") : tr("SD card"));
    storageHeading->setVisible(!m68k);
    storageHeading->setEnabled(supported);
    sdStatus->setEnabled(supported);
    const bool known = sdKnown && running && supported;
    sdStatus->setText(!supported ? tr("Not available yet") : !known ? tr("Not known") : !cardPresent ? tr("No card") : cardInserted ? tr("Inserted") : tr("Ejected"));
    // The picker button already says "No card" or the card's name, and Eject shows it's in.
    sdStatus->setVisible(!m68k && (!known || (cardPresent && !cardInserted)));
    sdReinsert->setVisible(known && cardPresent && !cardInserted);
    sdExchange->setVisible(known && cardPresent && (!memoryStick || cardInserted));
    sdEject->setVisible(known && cardInserted);
    sdExport->setVisible(known && cardPresent && !cardInserted);
    sdRetry->setVisible(!known || (memoryStick && cardPresent && !protectionKnown));
    cardPick->setVisible(known);
    cardPick->setText(known ? runningCardName() : tr("No card"));
    // A linked card held by another machine, or free to go in.
    const auto slot = running ? frondCards::readSlot(runningMachineDir()) : frondCards::Slot{};
    QString linkedText;
    bool linkedFree = false;
    if (known && !cardPresent && !slot.link.isEmpty()) {
        frondCards::Card card;
        const auto library = frondCards::libraryFor(sessionFolder());
        if (frondCards::readCard(frondCards::entryDir(library, slot.link), &card)) {
            linkedFree = QFileInfo(frondCards::entryDir(library, slot.link) + '/' + frondCards::fileOfKind(card.kind)).isFile();
            linkedText = linkedFree ? tr("%1 is free").arg(card.name)
                                    : card.machine.isEmpty() ? tr("%1 is in another machine").arg(card.name) : tr("%1 is in %2").arg(card.name, card.machine);
        }
    }
    if (running && !slot.link.isEmpty() && linkedWatcherId != slot.link) {
        linkedWatcherId = slot.link;
        linkedWatcher = std::make_unique<frondCards::FreeWatcher>(frondCards::libraryFor(sessionFolder()), slot.link, [this](bool) { refresh(); });
    }
    linkedState->setText(linkedText);
    linkedState->setVisible(!linkedText.isEmpty() && supported);
    linkedInsert->setVisible(linkedFree && supported);
    for (auto* b : {cardPick, sdReinsert, sdExchange, sdEject, sdExport, sdRetry, linkedInsert}) {
        if (!supported) b->setVisible(false);
        b->setEnabled(enabled && supported);
    }
    cardReadOnly->setVisible(memoryStick && supported && known && cardPresent);
    cardReadOnly->setEnabled(enabled && protectionKnown && !cardInserted);
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
        if (!info.old && !QFileInfo::exists(frondSessions::preferencesPath(info.path))) {
            QLockFile lock(info.path + ".lock"); lock.setStaleLockTime(0);
            if (lock.tryLock()) {
                frondSessions::Preferences prefs;
                loadPreferences(info.path, frondFirmware::deviceFromId(info.device), &prefs);
            }
        }
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
    auto* edit = menu->addAction(tr("Edit settings…"), this, [this, path] { QTimer::singleShot(0, this, [this, path] { editSession(path); }); });
    edit->setObjectName("editSession");
    edit->setEnabled(!info.old && !client.running() && !busy && !frondSessions::inUse(path));
    auto* rename = menu->addAction(tr("Rename…"), this, [this, path] { QTimer::singleShot(0, this, [this, path] { renameSession(path); }); });
    rename->setObjectName("renameSession"); rename->setEnabled(!info.old);
    auto* duplicate = menu->addAction(tr("Duplicate"), this, [this, path] { QTimer::singleShot(0, this, [this, path] { duplicateSession(path); }); });
    duplicate->setObjectName("duplicateSession");
    duplicate->setEnabled(!info.old && !client.running() && !busy && !frondSessions::inUse(path));
    auto* exportAction = menu->addAction(tr("Export…"), this, [this, path] { QTimer::singleShot(0, this, [this, path] { exportSession(path); }); });
    exportAction->setObjectName("exportSession"); exportAction->setEnabled(info.saved && !info.old);
    auto* remove = menu->addAction(tr("Delete"), this, [this, path] { QTimer::singleShot(0, this, [this, path] { deleteSession(path); }); });
    remove->setObjectName("deleteSession");
    return menu;
}
frondSessions::Preferences MainWindow::defaultPreferences(frondFirmware::Device device) const {
    const frondSessions::Preferences fresh;
    const auto scale = fittingScaleQuarters(device, fresh.showKeyboard, fresh.jogControls);
    frondSessions::Preferences prefs{std::clamp(settings.value("windowScale", scale).toInt(), 4, 8),
            settings.value("smoothLcd", false).toBool(), settings.value("audio", true).toBool(),
            settings.value("fastAudio", false).toBool()};
    // A new machine starts with its peripherals off.
    prefs.peripheralsSet = true;
    return prefs;
}
void MainWindow::migratePeripherals(frondSessions::Preferences* prefs) {
    if (prefs->peripheralsSet) return;
    auto mode = [this](const char* key, const char* portKey, int port) {
        const auto value = settings.value(key, "off").toString();
        return value == "tcp" ? "tcp:" + QString::number(settings.value(portKey, port).toInt()) : value == "pty" ? value : QStringLiteral("off");
    };
    prefs->serial = mode("serialMode", "serialPort", 5023);
    prefs->usb = mode("usbMode", "usbPort", 5024);
    prefs->infrared = mode("irdaMode", "irdaPort", 5030);
    const auto socket = settings.value("bluetoothSocket").toString();
    prefs->bluetooth = settings.value("bluetoothEnabled", false).toBool() && !socket.isEmpty() ? "socket:" + socket : QStringLiteral("off");
    prefs->peripheralsSet = true;
}
void MainWindow::savePeripherals() {
    if (sessionPath.isEmpty()) return;
    runningPreferences.peripheralsSet = true;
    if (const auto error = frondSessions::writePreferences(sessionPath, runningPreferences); !error.isEmpty()) failure(error);
}
QString MainWindow::loadPreferences(const QString& path, frondFirmware::Device device, frondSessions::Preferences* prefs) {
    if (frondSessions::readPreferences(path, prefs)) {
        if (prefs->peripheralsSet) return {};
        migratePeripherals(prefs);
        return frondSessions::writePreferences(path, *prefs);
    }
    if (QFileInfo::exists(frondSessions::preferencesPath(path))) return tr("Couldn't read the machine settings.");
    *prefs = defaultPreferences(device);
    // New machines write their settings when added, so a missing file is a machine from before the
    // choice: it keeps the controls left of the screen and takes the old global peripherals.
    prefs->jogControls = frondSessions::JogControls::screen;
    prefs->peripheralsSet = false;
    migratePeripherals(prefs);
    return frondSessions::writePreferences(path, *prefs);
}
void MainWindow::editSession(const QString& path) {
    if (client.running() || busy) return;
    const auto info = infoFor(path);
    if (info.old) return;
    QLockFile lock(path + ".lock"); lock.setStaleLockTime(0);
    if (!lock.tryLock()) return failure(tr("This session is open in another Frond."));
    frondSessions::Preferences prefs;
    const auto device = frondFirmware::deviceFromId(info.device);
    if (const auto error = loadPreferences(path, device, &prefs); !error.isEmpty()) return failure(error);
    MachineSettingsDialog dialog(device, prefs, this);
    if (dialog.exec() != QDialog::Accepted) return;
    if (const auto error = frondSessions::writePreferences(path, dialog.controls->values()); !error.isEmpty()) failure(error);
}
void MainWindow::renameSession(const QString& path) {
    const auto info = infoFor(path);
    bool ok;
    const auto name = QInputDialog::getText(this, tr("Rename"), tr("Name"), QLineEdit::Normal, info.name, &ok);
    if (!ok) return;
    // Resolved before the rename, while the session's folder still exists.
    const auto before = canonicalSession(path);
    const auto error = frondSessions::rename(sessionFolder(), info, name);
    if (!error.isEmpty()) return failure(error);
    const auto renamed = frondSessions::filePath(sessionFolder(), frondSessions::cleanName(name));
    if (settings.value("lastSession").toString() == before)
        settings.setValue("lastSession", canonicalSession(renamed));
    refreshSessions(renamed);
}
// A running or open machine can't be duplicated; stop it first. A machine with a card asks once
// what the copy gets: the same card linked (first), a copy of it, or no card.
void MainWindow::duplicateSession(const QString& path) {
    if (client.running() || busy) return;
    const auto info = infoFor(path);
    auto choice = frondSessions::CardChoice::copy;
    if (info.saved && !info.old) {
        const auto dir = frondSessions::machineDirOf(info.path);
        frondCards::reconcile(sessionFolder());
        const auto slot = frondCards::readSlot(dir);
        const auto cardName = frondCards::cardNameOf(sessionFolder(), dir);
        const bool holds = !frondCards::cardFileIn(dir).isEmpty() && !slot.card.isEmpty();
        if (!cardName.isEmpty() && (holds || !slot.link.isEmpty())) {
            QDialog dialog(this);
            dialog.setObjectName("duplicateCard");
            dialog.setWindowTitle(tr("Duplicate %1").arg(info.name));
            auto* layout = new QVBoxLayout(&dialog);
            auto* link = new QRadioButton(tr("Link %1").arg(cardName), &dialog);
            link->setObjectName("duplicateLink");
            auto* copy = new QRadioButton(tr("Copy %1").arg(cardName), &dialog);
            copy->setObjectName("duplicateCopy");
            auto* none = new QRadioButton(tr("No card"), &dialog);
            none->setObjectName("duplicateNone");
            auto* clause = new QLabel(&dialog);
            clause->setObjectName("duplicateClause");
            clause->setWordWrap(true);
            for (auto* r : {link, copy, none}) layout->addWidget(r);
            layout->addWidget(clause);
            copy->setVisible(holds);
            link->setChecked(true);
            const auto copyName = frondCards::freeCardName(frondCards::libraryFor(sessionFolder()), cardName);
            auto update = [&] {
                clause->setText(link->isChecked() ? tr("The copy shares %1 with %2.").arg(cardName, info.name)
                                : copy->isChecked() ? tr("The copy gets its own card, %1.").arg(copyName)
                                                    : tr("The copy has no card."));
            };
            for (auto* r : {link, copy, none}) connect(r, &QRadioButton::toggled, &dialog, update);
            update();
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            buttons->button(QDialogButtonBox::Ok)->setText(tr("Duplicate"));
            layout->addWidget(buttons);
            connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            if (dialog.exec() != QDialog::Accepted) return;
            choice = link->isChecked() ? frondSessions::CardChoice::link
                   : copy->isChecked() ? frondSessions::CardChoice::copy : frondSessions::CardChoice::none;
        }
    }
    QProgressDialog dialog(tr("Duplicating %1…").arg(info.name), tr("Cancel"), 0, 1000, this);
    dialog.setObjectName("duplicateProgress");
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setMinimumDuration(500);
    dialog.setValue(0);
    QString name;
    const auto error = frondSessions::duplicate(sessionFolder(), info, &name, [&](qint64 done, qint64 total) {
        dialog.setValue(total > 0 ? int(done * 1000 / total) : 0);
        QCoreApplication::processEvents();
        return !dialog.wasCanceled();
    }, choice);
    const bool cancelled = dialog.wasCanceled();
    dialog.close();
    if (!error.isEmpty() && !cancelled) failure(error);
    refreshSessions(error.isEmpty() ? frondSessions::filePath(sessionFolder(), name) : QString());
    if (cardsWindow) cardsWindow->refresh();
}
void MainWindow::exportSession(const QString& path) {
    const auto info = infoFor(path);
    if (!info.saved) return;
    const auto target = outputFile(tr("Export"), tr("Frond sessions (*.frond)"), info.name + ".frond");
    if (target.isEmpty()) return;
    if (!confirmReplace(frondSessions::exportDestDir(target))) return;
    const auto error = frondSessions::exportFile(info, target);
    if (!error.isEmpty()) failure(error);
    else statusBar()->showMessage(tr("Session exported to %1.").arg(frondSessions::exportDestDir(target)));
}
void MainWindow::deleteSession(const QString& path) {
    const auto info = infoFor(path);
    // A card in the machine goes back to Cards; the dialog says so.
    QString question = tr("Delete %1?").arg(info.name);
    if (info.saved && !info.old) {
        const auto dir = frondSessions::machineDirOf(info.path);
        frondCards::reconcile(sessionFolder());
        const auto slot = frondCards::readSlot(dir);
        quint32 version = 0;
        frondCards::descriptorsOf(info.path, &version);
        if (!frondCards::cardFileIn(dir).isEmpty() && !slot.card.isEmpty())
            question += ' ' + tr("Its card %1 stays in Cards.").arg(frondCards::cardNameOf(sessionFolder(), dir));
        else if (version == 6 && frondCards::v6HasCard(info.path))
            question += ' ' + tr("Its card stays in Cards.");
    }
    if (QMessageBox::question(this, tr("Delete"), question,
                              QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    const auto last = canonicalSession(path);
    const auto error = runCardJob(this, tr("Deleting %1…").arg(info.name), [&](const frondSessions::CopyProgress& progress) {
        return frondSessions::remove(info, progress);
    });
    if (cardsWindow) cardsWindow->refresh();
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
    const auto selected = sessionFile().isEmpty() ? frondFirmware::Device::automatic
        : frondFirmware::deviceFromId(infoFor(sessionFile()).device);
    const auto prefs = defaultPreferences(selected);
    FirmwareDialog dialog(firmwareRoot(), selected, this, &prefs, !settings.contains("windowScale"));
    if (dialog.exec() != QDialog::Accepted) return;
    const auto device = dialog.imported();
    const auto name = frondSessions::reserve(sessionFolder(), frondFirmware::deviceName(device), frondFirmware::deviceId(device));
    if (name.isEmpty())
        return failure(tr("Could not create the session storage directory."));
    const auto path = frondSessions::filePath(sessionFolder(), name);
    QLockFile lock(path + ".lock"); lock.setStaleLockTime(0);
    if (!lock.tryLock()) return failure(tr("This session is open in another Frond."));
    if (const auto error = frondSessions::writePreferences(path, dialog.machineSettings->values()); !error.isEmpty()) {
        QFile::remove(frondSessions::markerPath(path));
        return failure(error);
    }
    lock.unlock();
    refreshSessions(path);
    startSession(infoFor(path));
}
// The command line and resume: a session file anywhere. One outside the managed folder runs
// from a new folder in it and is saved there.
void MainWindow::openSession(const QString& path, bool launchNow) {
    startupHandled = true;
    if (client.running()) return;
    if (!launchNow) {
        const auto error = importSession(QFileInfo(path).absoluteFilePath());
        if (!error.isEmpty()) failure(error);
        return;
    }
    if (!QDir().mkpath(sessionFolder())) return failure(tr("Could not create the session storage directory."));
    const auto resolved = frondSessions::locate(sessionFolder(), QFileInfo(path).absoluteFilePath());
    const auto source = QFileInfo(resolved).canonicalFilePath();
    if (source.isEmpty() || !(QFileInfo(source).isFile() || QFileInfo(source).isDir()))
        return failure(tr("Choose an existing session file."));
    // Compare canonical paths so an XDG data directory symlink does not clone on every resume.
    const auto managedRoot = QFileInfo(sessionFolder()).canonicalFilePath();
    const auto load = QFileInfo(source).isDir() ? frondSessions::sessionFrondIn(source) : source;
    const auto header = frondSessions::readHeader(load);
    auto device = frondFirmware::deviceFromId(header.device);
    if (device == frondFirmware::Device::automatic) device = frondFirmware::Device::tx;
    if (!QFileInfo(coreFor(device)).isExecutable()) return failure(tr("The Frond emulator executable could not be found. Reinstall Frond or build both desktop and core targets."));
    QString target = load;
    QStringList arguments{load};
    bool reserved = false;
    if (!source.startsWith(managedRoot + '/')) {
        // The marker holds the new name until the first save, so no other Frond takes it. The core
        // copies or extracts the cards into that folder and runs from there, so the original
        // and the folder around it stay as they were.
        const auto name = frondSessions::reserve(managedRoot, frondFirmware::deviceName(device), frondFirmware::deviceId(device));
        if (name.isEmpty()) return failure(tr("Could not create the session storage directory."));
        target = frondSessions::filePath(managedRoot, name);
        arguments << "--work-folder" << frondSessions::machineDir(managedRoot, name);
        QFile work(frondSessions::workPath(target));
        if (!work.open(QIODevice::WriteOnly)) {
            QFile::remove(frondSessions::markerPath(target));
            frondSessions::removeMachineFolder(frondSessions::machineDirOf(target));
            return failure(tr("Could not create the session storage directory."));
        }
        reserved = true;
    }
    launch(target, arguments, device);
    if (reserved && !busy) {
        QFile::remove(frondSessions::markerPath(target));
        QFile::remove(frondSessions::preferencesPath(target));
        frondSessions::removeMachineFolder(frondSessions::machineDirOf(target));
    }
    refreshSessions();
}
QString MainWindow::coreFor(frondFirmware::Device device) const {
    return frondFirmware::runs68k(device) ? core68k : core;
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
    const auto device = frondFirmware::deviceFromId(info.device);
    if (!QFileInfo(coreFor(device)).isExecutable()) return failure(tr("The Frond emulator executable could not be found. Reinstall Frond or build both desktop and core targets."));
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
    // A new machine runs in its own folder, where cards above 128 MB live until the first save.
    if (!info.saved) {
        const auto folder = frondSessions::machineDirOf(canonicalSession(info.path));
        if (!QDir().mkpath(folder)) return failure(tr("Could not create the session storage directory."));
        if (QFileInfo(folder).canonicalFilePath() != QFileInfo(sessionFolder()).canonicalFilePath())
            arguments << "--work-folder" << folder;
    }
    launch(canonicalSession(info.path), arguments, device);
}
// Starts the core on 'arguments' (the session file or the firmware) and saves to 'target'.
void MainWindow::launch(const QString& target, QStringList arguments, frondFirmware::Device startDevice) {
    sessionPath = target;
    holdKnown = holdOn = false;
    const auto caps = frondFirmware::capabilities(startDevice);
    const bool m68k = frondFirmware::runs68k(startDevice);
    if (m68k) {
        // frond-68k runs from the one session file and has no NAND or card folder.
        for (const auto* option : {"--nand", "--work-folder"})
            for (int i = arguments.indexOf(option); i >= 0; i = arguments.indexOf(option))
                arguments.remove(i, std::min<qsizetype>(2, arguments.size() - i));
    }
    sessionLock = std::make_unique<QLockFile>(sessionPath + ".lock");
    sessionLock->setStaleLockTime(0);
    if (!sessionLock->tryLock()) {
        sessionLock.reset();
        return failure(tr("This working session is already open, or its lock file could not be created."));
    }
    if (!QFileInfo::exists(frondSessions::preferencesPath(target)) && !arguments.isEmpty() &&
        frondSessions::readPreferences(arguments.first(), &runningPreferences)) {
        if (const auto error = frondSessions::writePreferences(target, runningPreferences); !error.isEmpty()) {
            sessionLock.reset(); return failure(error);
        }
    }
    if (const auto error = loadPreferences(target, startDevice, &runningPreferences); !error.isEmpty()) {
        sessionLock.reset(); return failure(error);
    }
    runningDevice = startDevice == frondFirmware::Device::automatic ? frondFirmware::Device::tx : startDevice;
    loadSerialSettings(); loadUsbSettings(); loadIrdaSettings(); loadBluetoothSettings();
    if (caps.serial && serialArgument() != "off") arguments << "--serial" << serialArgument();
    if (caps.usb && usbArgument() != "off") arguments << "--usb" << usbArgument();
    if(caps.infrared && irdaArgument()!="off")arguments << "--irda" << irdaArgument();
    if(caps.bluetooth && bluetooth->isChecked()) arguments << "--bluetooth" << bluetoothArgument();
    irdaLinkText = caps.infrared && irdaChoice().startsWith("link:") && !frondLink::knownLink(irdaChoice())
        ? tr("Infrared: Unknown connection") : QString();
    soundToggle->setChecked(caps.sound && runningPreferences.audio);
    if (!caps.sound || !runningPreferences.audio) arguments << "--no-sound";
    const auto sourceSession = !arguments.isEmpty() && !arguments.first().startsWith("--") ? arguments.first() : QString();
    if (!m68k && runningPreferences.fastAudio) {
        const auto option = frondAudio::lowLatencyOption(startDevice, sourceSession,
            frondFirmware::storedNor(firmwareRoot(), startDevice));
        if (!option.isEmpty()) arguments << option;
    }
    // frond-68k's scale is screen pixels per Palm pixel; its 160-pixel LCD takes twice the ARM value.
    arguments << "--window-scale" << QString::number(runningPreferences.scale / (m68k ? 2.0 : 4.0));
    if (runningPreferences.smooth) arguments << "--smooth-lcd";
    if (m68k) {
        if (runningPreferences.contrastSet) arguments << "--contrast" << QString::number(runningPreferences.contrast);
        if (runningPreferences.uncapped) arguments << "--speed" << "uncapped";
    } else if (runningPreferences.fastEngine) arguments << "--engine" << "fast";
    {
        const QSignalBlocker blocked(contrast);
        contrast->setValue(runningPreferences.contrast);
    }
    if (startDevice == frondFirmware::deviceFromId("sony-clie-tg50")) {
        if (!runningPreferences.showKeyboard) arguments << "--hide-keyboard";
        arguments << "--jog-controls" << frondSessions::jogControlsName(runningPreferences.jogControls);
    }
    // A saved machine in the managed folder: its slot (a linked card it takes now) gives the card
    // flags. frond-68k has no cards.
    finalSaved = false; linkedWatcher.reset(); linkedWatcherId.clear(); linkedHolder.clear(); cardMove = {};
    const auto machine = frondSessions::machineDirOf(target);
    if (!m68k && !sourceSession.isEmpty() && QFileInfo(sourceSession).canonicalFilePath() == QFileInfo(target).canonicalFilePath() &&
        QFileInfo(frondCards::sessionsOf(machine)).canonicalFilePath() == QFileInfo(sessionFolder()).canonicalFilePath()) {
        arguments << frondCards::takeAtStart(sessionFolder(), machine, &linkedHolder);
        const auto slot = frondCards::readSlot(machine);
        if (!slot.link.isEmpty() && frondCards::cardFileIn(machine).isEmpty()) {
            linkedWatcherId = slot.link;
            linkedWatcher = std::make_unique<frondCards::FreeWatcher>(frondCards::libraryFor(sessionFolder()), slot.link,
                                                                      [this](bool) { refresh(); });
        }
    }
    stopping = closeAfterStop = allowClose = false;
    log->clear(); busy = true;
    const bool launched = client.start(coreFor(startDevice), arguments);
    if (!launched) { busy = false; sessionLock.reset(); }
    else attachInfrared();
    refresh(); statusBar()->showMessage(launched ? tr("Starting the Palm…") : tr("Could not start the Palm."));
}
QString MainWindow::bluetoothArgument() const {
    return bluetooth->isChecked() ? bluetoothSocket->text() : QStringLiteral("off");
}
void MainWindow::loadBluetoothSettings() {
    const auto& value = runningPreferences.bluetooth;
    bluetooth->setChecked(value.startsWith("socket:"));
    if (value.startsWith("socket:")) bluetoothSocket->setText(value.mid(7));
}
void MainWindow::showBluetooth(const QString& state) {
    bluetoothState->setText(state.isEmpty() || state=="off" ? QString() : tr("Bluetooth: %1").arg(state));
}
QString MainWindow::irdaChoice() const {
    return irda->currentData()=="tcp" ? "tcp:"+QString::number(irdaPort->value()) : irda->currentData().toString();
}
// A connection runs the core's IR on a Unix socket the hub joins to the link; one per window.
QString MainWindow::irdaArgument() const {
    const auto choice = irdaChoice();
    if (!choice.startsWith("link:")) return choice;
    if (!frondLink::knownLink(choice)) return QStringLiteral("off");
    const auto path = frondLink::coreSocketPath("ir", QString::number(QCoreApplication::applicationPid()));
    return path.isEmpty() ? QStringLiteral("off") : "unix:" + path;
}
void MainWindow::loadIrdaSettings() {
    const auto& value = runningPreferences.infrared;
    const QSignalBlocker blocked(irda);
    frondLink::addConnectionChoices(irda, value);
    irda->setCurrentIndex(std::max(0, irda->findData(value.startsWith("tcp:") ? QString("tcp") : value)));
    irdaPort->setValue(value.startsWith("tcp:") ? value.mid(4).toInt() : 5030);
}
void MainWindow::refreshIrdaChoices() {
    const auto current = irda->currentData().toString();
    const QSignalBlocker blocked(irda);
    frondLink::addConnectionChoices(irda, current);
    irda->setCurrentIndex(std::max(0, irda->findData(current)));
}
void MainWindow::showIrda(const QString& state) {
    if(state.isEmpty()){irdaState->clear();return;}
    const auto parts=state.split(' ',Qt::SkipEmptyParts);
    const auto mode=parts.value(0).section(':',0,0);
    // On a connection the hub's status shows instead of the socket.
    if (mode == "unix" && irda->currentData().toString().startsWith("link:")) { irdaState->setText(irdaLinkText); refresh(); return; }
    irda->setCurrentIndex(std::max(0,irda->findData(mode)));
    if(mode=="tcp"){
        const auto port=parts.value(1).section(':',-1).toInt();if(port>0)irdaPort->setValue(port);
    }
    irdaState->setText(state=="off"?QString():tr("Infrared: %1").arg(state));refresh();
}
void MainWindow::attachInfrared() {
    const auto choice = irdaChoice();
    const bool wanted = client.running() && frondFirmware::capabilities(runningDevice).infrared &&
                        frondLink::knownLink(choice) && irdaArgument().startsWith("unix:");
    if (!wanted) { link.setSticky("ir", {}); if (!choice.startsWith("link:")) irdaLinkText.clear(); return; }
    irdaLinkText = tr("Infrared: connecting to %1").arg(frondLink::connectionName(choice.mid(5)));
    link.setSticky("ir", {{"op", "ir-attach"}, {"machine", QFileInfo(sessionPath).completeBaseName()},
                          {"device", frondFirmware::deviceName(runningDevice)}, {"link", choice.mid(5)},
                          {"socket", irdaArgument().mid(5)}});
    irdaState->setText(irdaLinkText);
    refresh();
}
void MainWindow::showIrdaLink(const QJsonObject& s) {
    const auto peer = s.value("peer").toString(), state = s.value("state").toString(), far = s.value("far").toString();
    const auto machine = s.value("machine").toString().isEmpty() ? s.value("device").toString() : s.value("machine").toString();
    QString text;
    if (state == "joined") text = far == "asleep" ? tr("Infrared: %1, Palm asleep").arg(peer) :
                                  far == "paused" ? tr("Infrared: %1, Palm paused").arg(peer) :
                                  tr("Infrared: %1, joined to %2").arg(peer, machine);
    else if (state == "waiting") text = tr("Infrared: waiting for a machine on %1").arg(peer);
    else if (state == "busy") text = tr("Infrared on %1 is in use by %2").arg(peer, s.value("busyBy").toString());
    else if (state == "notfound") text = tr("Infrared: %1 not found").arg(peer);
    else if (state == "keychanged") text = tr("Infrared: %1 key changed, pair again").arg(peer);
    else if (state == "forgotten") text = tr("Infrared: pair again with %1").arg(peer);
    else if (state == "unknown") text = tr("Infrared: Unknown connection");
    else text = tr("Infrared: connecting to %1").arg(peer);
    irdaLinkText = text;
    if (client.running() && irda->currentData().toString().startsWith("link:")) { irdaState->setText(text); refresh(); }
}
void MainWindow::showConnections() {
    if (!connectionsWindow) connectionsWindow = new ConnectionsWindow(&link, this);
    connectionsWindow->show();
    connectionsWindow->raise();
    connectionsWindow->activateWindow();
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
    const auto& value = runningPreferences.serial;
    serial->setCurrentIndex(std::max(0, serial->findData(value.startsWith("tcp:") ? QString("tcp") : value)));
    serialPort->setValue(value.startsWith("tcp:") ? value.mid(4).toInt() : 5023);
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
    const auto& value = runningPreferences.usb;
    usb->setCurrentIndex(std::max(0, usb->findData(value.startsWith("tcp:") ? QString("tcp") : value)));
    usbPort->setValue(value.startsWith("tcp:") ? value.mid(4).toInt() : 5024);
}
QString MainWindow::outputFile(const QString& title, const QString& filter, const QString& defaultName) {
    QFileDialog dialog(this, title, QDir::homePath() + '/' + defaultName, filter);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(filter.contains("*.frondms") ? "frondms" : filter.contains("*.frond") ? "frond" : filter.contains("*.img") ? "img" : "zip");
    if (dialog.exec() != QDialog::Accepted || dialog.selectedFiles().isEmpty()) return {};
    return QFileInfo(dialog.selectedFiles().first()).absoluteFilePath();
}
void MainWindow::save(bool saveAs) {
    if (!client.available()) return;
    if (!saveAs) {
        savingPath = sessionPath; send("save", sessionPath);
        return;
    }
    const auto path = outputFile(tr("Save a Copy"), tr("Frond sessions (*.frond)"), deviceLabel() + ".frond");
    if (path.isEmpty()) return;
    saveCopyTo(path);
}
void MainWindow::saveCopyTo(const QString& path) {
    if (!client.available()) return;
    const auto dest = frondSessions::exportDestDir(path);
    if (QFileInfo(dest).canonicalFilePath() == QFileInfo(frondSessions::machineDirOf(sessionPath)).canonicalFilePath() &&
        QFileInfo(dest).exists())
        return failure(tr("That is the session itself."));
    if (!confirmReplace(dest)) return;
    const auto name = QFileInfo(dest).fileName();
    const auto destFrond = dest + '/' + name + ".frond";
    copyLock = std::make_unique<QLockFile>(destFrond + ".lock"); copyLock->setStaleLockTime(0);
    if (frondSessions::isMachineFolder(dest) && !copyLock->tryLock()) { copyLock.reset(); return failure(tr("This session is open in another Frond.")); }
    // The core saves the copy into a staging folder; the working session's files stay as they are.
    copyStaging = frondSessions::makeStaging(dest);
    if (copyStaging.isEmpty() || !QDir().mkdir(copyStaging + '/' + name)) {
        frondSessions::removeStaging(copyStaging); copyStaging.clear(); copyLock.reset();
        return failure(tr("Couldn't save the session."));
    }
    pendingCopy = dest;
    savingPath = copyStaging + '/' + name + '/' + name + ".frond";
    if (!send("save", savingPath)) {
        frondSessions::removeStaging(copyStaging); copyStaging.clear(); pendingCopy.clear(); savingPath.clear(); copyLock.reset();
    }
}
// False when `dest` can't be written, or is a session the user doesn't want replaced.
bool MainWindow::confirmReplace(const QString& dest) {
    bool existing = false;
    if (const auto error = frondSessions::exportTargetError(dest, &existing); !error.isEmpty()) {
        failure(error);
        return false;
    }
    if (!existing) return true;
    return QMessageBox::question(this, tr("Replace"), tr("Replace the session %1?").arg(QFileInfo(dest).fileName()),
                                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
}
bool MainWindow::send(const QString& command, const QString& argument) {
    if (!client.request(command, argument)) return false;
    busy = true; refresh(); statusBar()->showMessage(tr("Working…"));
    const bool msCard = command == "ms-create" || command == "ms-insert" || command == "ms-export";
    if (command == "sd-create" || command == "sd-insert" || command == "sd-export" || msCard) {
        // Large cards take a while; the core reports each percent and stops on cancel.
        delete cardProgress;
        cardProgress = new QProgressDialog(command.endsWith("-create") ? tr("Making the card…") :
                                           command.endsWith("-insert") ? tr("Inserting the image…") : tr("Exporting the image…"),
                                           tr("Cancel"), 0, 1000, this);
        cardProgress->setObjectName("cardProgress");
        cardProgress->setWindowTitle(msCard ? tr("Memory Stick") : tr("SD card"));
        cardProgress->setWindowModality(Qt::WindowModal);
        cardProgress->setAutoClose(false); cardProgress->setAutoReset(false);
        cardProgress->setMinimumDuration(500);
        cardProgress->setValue(0);
        connect(cardProgress, &QProgressDialog::canceled, this, [this] { client.cancelFiles(); });
    }
    return true;
}
void MainWindow::stop(bool closeApp) {
    closeAfterStop = closeAfterStop || closeApp;
    if (!client.running()) { if (closeApp) { allowClose = true; close(); } return; }
    if (stopping) return;
    stopping = true; client.cancelFiles(); refresh();
    if (exchangeDialog) exchangeDialog->reject();
    else if (memoryStickFiles) memoryStickFiles->reject();
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
// Shown without a nested event loop, so it can be called from the client's process and socket
// signals: an exec() there let the client delete its socket while the socket was still in use.
void MainWindow::failure(const QString& message, const QString& details) {
    if (failureBox && failureBox->isVisible()) {
        if (!failureBox->text().contains(message)) failureBox->setText(failureBox->text() + "\n\n" + message);
        if (!details.isEmpty() && failureBox->detailedText().isEmpty()) failureBox->setDetailedText(details);
        return;
    }
    auto* box = new QMessageBox(QMessageBox::Critical, tr("Frond"), message, QMessageBox::Ok, this);
    if (!details.isEmpty()) box->setDetailedText(details);
    connect(box, &QDialog::finished, box, &QObject::deleteLater);
    failureBox = box;
    box->open();
}
void MainWindow::closeEvent(QCloseEvent* event) {
    if (allowClose || !client.running()) { settings.setValue("windowGeometry", saveGeometry()); event->accept(); return; }
    event->ignore(); stop(true);
}

bool MainWindow::memoryStick() const {
    return deviceProfileStorageSlot5(frondFirmware::profile(currentDevice())) == DeviceStorageSlot5::memoryStick;
}
bool MainWindow::storageSupported() const {
    const auto& frontend = frondFirmware::profile(currentDevice()).frontend;
    return memoryStick() ? frontend.memoryStick : frontend.sdSlot;
}
QString MainWindow::cardCommand(const char* operation) const {
    return (memoryStick() ? QStringLiteral("ms-") : QStringLiteral("sd-")) + QLatin1String(operation);
}
void MainWindow::beginExchange() {
    if (memoryStick()) {
        if (!client.available() || stopping || !cardInserted || memoryStickFiles) return;
        if (QMessageBox::question(this, tr("Open Memory Stick files?"), tr("Close files on the Palm first; changes are saved immediately."),
                                  QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
        if (!client.available() || stopping || !cardInserted) return;
        MemoryStickFilesDialog dialog(client, [this](const QString& command, const QString& argument) { return send(command, argument); },
                                      cardReadOnly->isChecked(), this);
        memoryStickFiles = &dialog; dialog.exec(); memoryStickFiles = nullptr;
        if (stopping && client.running() && client.available()) save();
        return;
    }
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
    exchangeBackup = exchangeRecovery->path() + "/before-exchange";
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
        const auto maxCard = qint64(std::min<quint64>(frondFirmware::profile(runningDevice).sdMax, quint64(2048) << 20));
        if (!exchange->open(exchangeFiles->path() + "/original.img", error, maxCard)) {
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

// ---- Cards on the running machine ----

QString MainWindow::runningMachineDir() const {
    return client.running() && !sessionPath.isEmpty() ? frondSessions::machineDirOf(sessionPath) : QString();
}
// The card in the slot by its Cards name; one that isn't in Cards yet by its type.
QString MainWindow::runningCardName() const {
    if (!cardPresent) return tr("No card");
    const auto dir = runningMachineDir();
    const auto slot = frondCards::readSlot(dir);
    frondCards::Card card;
    if (!slot.card.isEmpty() && frondCards::readCard(frondCards::entryDir(frondCards::libraryFor(sessionFolder()), slot.card), &card))
        return card.name;
    const auto file = frondCards::cardFileIn(dir);
    return frondCards::kindName(file.isEmpty() ? (memoryStick() ? QStringLiteral("ms") : QStringLiteral("sd")) : frondCards::kindOfFile(file));
}
CardsWindow* MainWindow::showCards() {
    if (!cardsWindow) {
        auto* w = new CardsWindow(sessionFolder(), this);
        w->setAttribute(Qt::WA_DeleteOnClose);
        w->runningMachine = [this] { return runningMachineDir(); };
        w->exchangeOpen = [this] { return exchangeStage != ExchangeStage::None || memoryStickFiles != nullptr || cardMove.stage != CardMove::None; };
        w->takeOutRunning = [this](const QString&) { if (client.available() && !busy && cardPresent) changeCard({}); };
        w->changed = [this] { refreshSessions(); refresh(); };
        cardsWindow = w;
        // The rows were made before the window knew its running machine.
        w->refresh();
    }
    cardsWindow->show();
    cardsWindow->raise();
    cardsWindow->activateWindow();
    return cardsWindow;
}
void MainWindow::pickCard() {
    if (!client.available() || busy || stopping || exchangeStage != ExchangeStage::None || cardMove.stage != CardMove::None) return;
    const auto dir = runningMachineDir();
    const auto slot = frondCards::readSlot(dir);
    const auto current = cardPresent ? slot.card : QString();
    CardPickerDialog dialog(sessionFolder(), dir, runningDevice, current, this);
    if (dialog.exec() != QDialog::Accepted) return;
    if (cardsWindow) cardsWindow->refresh();
    if (!client.available() || busy || stopping) return;
    const auto chosen = dialog.chosen();
    if (chosen == current && (cardPresent || chosen.isEmpty())) return refresh();
    changeCard(chosen);
}
// Each step goes through the core so the card's writes reach its file before it moves: eject (with
// the usual confirmation), take out, then put the new card in.
void MainWindow::changeCard(const QString& in) {
    cardMove = {CardMove::None, in};
    if (cardPresent && cardInserted) {
        if (QMessageBox::question(this, tr("Eject the card?"), tr("Close any files open on the Palm first."),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) {
            cardMove = {};
            return refresh();
        }
        if (!client.available() || busy) { cardMove = {}; return; }
        cardMove.stage = CardMove::Eject;
        if (!send(cardCommand("eject"))) cardMove = {};
        return;
    }
    if (cardPresent) {
        cardMove.stage = CardMove::TakeOut;
        if (!send(cardCommand("take-out"))) cardMove = {};
        return;
    }
    if (!in.isEmpty()) putInCard(in);
    else cardMove = {};
}
void MainWindow::putInCard(const QString& in) {
    const auto library = frondCards::libraryFor(sessionFolder());
    QString error;
    frondCards::Card card;
    {
        frondCards::LibraryLock lock(library);
        error = !lock.ok ? frondCards::busyMessage() : frondCards::toMachine(library, in, runningMachineDir());
        if (error.isEmpty() && !frondCards::readCard(frondCards::entryDir(library, in), &card)) error = tr("Couldn't find that card.");
    }
    if (!error.isEmpty()) {
        cardMove = {};
        refresh();
        return failure(error);
    }
    cardMove = {CardMove::PutIn, in};
    const auto argument = QString::fromLatin1(card.identity.leftJustified(32, '\0', true).toHex()) +
                          (card.writeProtected ? QStringLiteral(" protected") : QStringLiteral(" writable"));
    if (!send(cardCommand("put-in"), argument)) cardMoveFailed(cardCommand("put-in"), tr("Couldn't reach the Palm."));
    if (cardsWindow) cardsWindow->refresh();
}
// True when the result was a step of a card move and has been dealt with.
bool MainWindow::continueCardMove(const QString& command, const QString& message) {
    if (cardMove.stage == CardMove::Eject && command == cardCommand("eject")) {
        cardMove.stage = CardMove::TakeOut;
        if (!send(cardCommand("take-out"))) cardMove = {};
        return true;
    }
    if (cardMove.stage == CardMove::TakeOut && command == cardCommand("take-out")) {
        // The core settled the card in the folder's file: it goes to the drawer with the serial and
        // protection the core reports. A card that isn't in Cards yet gets its entry first.
        const auto dir = runningMachineDir();
        const auto library = frondCards::libraryFor(sessionFolder());
        const auto parts = message.split(' ');
        QString error;
        {
            frondCards::LibraryLock lock(library);
            if (!lock.ok) error = frondCards::busyMessage();
            else {
                auto slot = frondCards::readSlot(dir);
                frondCards::Card card;
                const auto file = frondCards::cardFileIn(dir);
                if (file.isEmpty()) error = tr("Couldn't find the card's file.");
                else if (slot.card.isEmpty() || !frondCards::readCard(frondCards::entryDir(library, slot.card), &card)) {
                    slot.present = true;
                    if (!frondCards::moveIn(library, dir, file, slot, {}, &card)) error = tr("Couldn't move the card into Cards.");
                }
                if (error.isEmpty()) {
                    // The core's serial, unless another card has it (then the card keeps the new one it got).
                    if (parts.size() == 2 && parts[0].size() == 64) {
                        const auto reported = QByteArray::fromHex(parts[0].toLatin1());
                        if (!frondCards::identityUsed(library, reported, card.id)) card.identity = reported;
                    }
                    card.writeProtected = parts.value(1) == "protected";
                    frondCards::writeCard(frondCards::entryDir(library, card.id), card);
                    error = frondCards::toDrawer(library, dir, card.id);
                }
            }
        }
        const auto in = cardMove.in;
        cardMove = {};
        if (!error.isEmpty()) {
            refresh();
            failure(error);
            return true;
        }
        if (cardsWindow) cardsWindow->refresh();
        if (!in.isEmpty()) putInCard(in);
        else {
            refresh();
            statusBar()->showMessage(tr("Card taken out."));
        }
        return true;
    }
    if (cardMove.stage == CardMove::PutIn && command == cardCommand("put-in")) {
        cardMove = {};
        if (cardsWindow) cardsWindow->refresh();
        return false;  // the usual follow-ups (protection for a stick) run
    }
    return false;
}
void MainWindow::cardMoveFailed(const QString& command, const QString& message) {
    // A card moved in that the core couldn't open goes back to the drawer.
    if (cardMove.stage == CardMove::PutIn && command == cardCommand("put-in")) {
        const auto library = frondCards::libraryFor(sessionFolder());
        frondCards::LibraryLock lock(library);
        if (lock.ok) frondCards::toDrawer(library, runningMachineDir(), cardMove.in);
    }
    cardMove = {};
    refresh();
    if (cardsWindow) cardsWindow->refresh();
    failure(message);
}
