#include "MainWindow.h"
#include "LicencesDialog.h"
#include "FirmwareInput.h"
#include "CardFilesWindow.h"
#include "Dialogs.h"
#include "FirmwareStore.h"
#include "AudioQualification.h"
#include "ConnectionsWindow.h"
#include "LinkCommon.h"
#include "CardsWindow.h"
#include "RunningRules.h"
#include "RunningPanels.h"
#include <QCursor>
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
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QApplication>
#include <QShortcut>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
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
    // The running controls sit in four panels under the bar. The ones the bar or the Machine menu
    // already offers are made here, hidden, so each has one handler.
    // Running: a strip for the hidden bar over the Palm and its panel side by side.
    runArea = new QWidget(outer);
    runArea->setObjectName("runArea");
    auto* runLayout = new QVBoxLayout(runArea);
    runLayout->setContentsMargins(0, 0, 0, 0); runLayout->setSpacing(0);
    barHandle = new BarHandle(runArea);
    runLayout->addWidget(barHandle);
    auto* body = bodyLayout = new QHBoxLayout;
    body->setSpacing(0);
    palmHolder = new PalmHolder(runArea);
    palmHolder->hide();
    panels = new PanelHost(runArea);
    body->addWidget(palmHolder, 1);
    body->addWidget(panels);
    runLayout->addLayout(body, 1);
    outerLayout->addWidget(runArea, 1);
    runArea->hide();
    barHandle->hide();
    connect(barHandle, &BarHandle::clicked, this, [this] { setBarShown(true); });
    runArea->installEventFilter(this);
    overlayTimer = new QTimer(this);
    overlayTimer->setSingleShot(true);
    overlayTimer->setInterval(runningRules::fullScreenHideMs);
    connect(overlayTimer, &QTimer::timeout, this, [this] {
        if (!fullScreenOn) return;
        if (runningRules::barStaysInFullScreen(runningBar->underMouse(), QApplication::activePopupWidget() != nullptr, panels->current() >= 0))
            overlayTimer->start();
        else showOverlayBar(false);
    });
    // The pointer over the Palm only reaches the core, so the top edge is watched here.
    edgeTimer = new QTimer(this);
    edgeTimer->setInterval(15);
    connect(edgeTimer, &QTimer::timeout, this, [this] {
        if (!fullScreenOn || overlayBarShown) { edgeTimer->stop(); return; }
        if (runningRules::revealBar(QCursor::pos(), QRect(runArea->mapToGlobal(QPoint(0, 0)), runArea->size()))) {
            showOverlayBar(true);
            overlayTimer->start();
        }
    });
    drivers = new QWidget(this);
    drivers->hide();
    auto* root = drivers;
    auto* appsLayout = panels->addPage(tr("Applications"));
    auto* cardLayout = panels->addPage(tr("Storage"));
    auto* displayLayout = panels->addPage(tr("Display"));
    auto* connectionsLayout = panels->addPage(tr("Connections"));
    settingsDialog = new SettingsDialog(this);
    settingsDialog->resume->setChecked(settings.value("resume", true).toBool());
    connect(settingsDialog->resume, &QCheckBox::toggled, this, [this](bool enabled) { settings.setValue("resume", enabled); });
    auto button = [&](const QString& text, const char* name, bool plain = false) {
        auto* b = new QPushButton(text, root);
        b->setObjectName(name);
        if (!plain) runningButtons << b;
        return b;
    };
    appsPanel = new AppsPanel(root);
    appsLayout->addWidget(appsPanel, 1);
    connect(appsPanel, &AppsPanel::installRequested, this, [this] {
        const auto paths = QFileDialog::getOpenFileNames(this, tr("Install application"), {}, tr("Palm databases (*.prc *.pdb);;All files (*)"));
        installFiles(paths);
    });
    connect(appsPanel, &AppsPanel::otherRequested, this, &MainWindow::otherDatabase);
    connect(appsPanel, &AppsPanel::launchRequested, this, [this](const QString& name) { send("launch-app", name); });
    connect(appsPanel, &AppsPanel::exportAllRequested, this, [this] {
        const auto path = outputFile(tr("Export RAM databases"), tr("ZIP archives (*.zip)"), "Palm databases.zip");
        if (!path.isEmpty()) send("export", path);
    });
    connect(appsPanel, &AppsPanel::exportRequested, this, [this](const QString& name) { exportDatabase(name); });
    connect(appsPanel, &AppsPanel::deleteRequested, this, &MainWindow::deleteApplication);

    // The buttons shown depend on the card's state; refresh() picks them.
    storageHeading = new QLabel(tr("SD card"), root);
    storageHeading->setObjectName("storageHeading");
    cardLayout->addWidget(storageHeading);
    // One row: the card in the slot (opens the picker), its state when the buttons don't say it
    // (Ejected, or why there's no button) and the card's buttons. A linked card held elsewhere
    // shows where it is, and Insert once it is free.
    auto* cardRow = new QVBoxLayout;
    cardPick = button(tr("No card"), "cardPick", true);
    sdStatus = new QLabel(tr("Not known"), root);
    sdStatus->setObjectName("sdState");
    cardRow->addWidget(cardPick);
    cardRow->addWidget(sdStatus);
    sdReinsert = button(tr("Insert"), "sdInsert", true);
    sdExchange = button(tr("Files…"), "sdExchange", true);
    sdEject = button(tr("Eject"), "sdEject", true);
    sdExport = button(tr("Export image"), "sdExport", true);
    sdRetry = button(tr("Try again"), "sdRetry", true);
    for (auto* b : {sdReinsert, sdExchange, sdEject, sdExport, sdRetry}) cardRow->addWidget(b);
    cardLayout->addLayout(cardRow);
    auto* linkedRow = new QHBoxLayout;
    linkedState = new QLabel(root);
    linkedState->setObjectName("linkedState");
    linkedInsert = button(tr("Insert"), "linkedInsert", true);
    linkedRow->addWidget(linkedState); linkedRow->addWidget(linkedInsert); linkedRow->addStretch();
    cardLayout->addLayout(linkedRow);
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
    cardLayout->addWidget(cardReadOnly);
    auto* cardsButton = new QPushButton(tr("Cards…"), root);
    cardsButton->setObjectName("openCards");
    cardLayout->addWidget(cardsButton);
    // Internal: the RAM databases of every running machine, in the same Files… as a card.
    internalHeading = new QLabel(tr("Internal"), root);
    internalHeading->setObjectName("internalHeading");
    cardLayout->addSpacing(12);
    cardLayout->addWidget(internalHeading);
    internalButton = button(tr("Files…"), "internalFiles", true);
    cardLayout->addWidget(internalButton);
    cardLayout->addStretch();
    connect(internalButton, &QPushButton::clicked, this, &MainWindow::openInternal);
    connect(cardsButton, &QPushButton::clicked, this, [this] { showCards(); });
    connect(cardReadOnly, &QCheckBox::clicked, this, [this](bool on) { send("ms-read-only", on ? "on" : "off"); });

    auto* displayGrid = new QGridLayout;
    displayLayout->addLayout(displayGrid);
    auto* grid = new QGridLayout;  // the Connections panel
    connectionsLayout->addLayout(grid);
    for (auto* g : {displayGrid, grid}) g->setHorizontalSpacing(12);
    auto label = [&](const QString& text, int row, QGridLayout* into = nullptr) {
        auto* l = new QLabel(text, root);
        if (into) into->addWidget(l, row, 0);
        return l;
    };
    // The control with its port field below it, so both fit the panel's width.
    auto stack = [](QWidget* control, QWidget* port) {
        auto* v = new QVBoxLayout;
        v->setContentsMargins(0, 0, 0, 0); v->setSpacing(4);
        v->addWidget(control); v->addWidget(port);
        return v;
    };
    // One row per device: label, control, and the port beside it. Readout rows hide while empty.
    soundToggle = new QCheckBox(tr("On"), root);
    soundToggle->setObjectName("sound");
    label(tr("Sound"), 1, displayGrid); displayGrid->addWidget(soundToggle, 1, 1, 1, 2);
    microphoneToggle = new QCheckBox(tr("On"), root);
    microphoneToggle->setObjectName("microphone");
    microphoneLabel = label(tr("Microphone"), 2, displayGrid); displayGrid->addWidget(microphoneToggle, 2, 1, 1, 2);
    microphoneState = new QLabel(root); displayGrid->addWidget(microphoneState, 3, 1, 1, 2);
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
    networkLabel = label(tr("Network"), 2);
    keyboard = new QCheckBox(tr("Attached"),root);
    keyboard->setObjectName("keyboardAttached");
    keyboard->setToolTip(tr("Uses the Palm Universal Wireless Keyboard protocol. Install and enable its Palm OS driver first. Type in the Palm window; modifiers and key releases are sent to the driver."));
    connect(keyboard,&QCheckBox::clicked,this,[this](bool attached) { send("keyboard-attach",attached?"on":"off"); });
    keyboardLabel = label(tr("Infrared keyboard"), 3);
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
    serialLabel = label(tr("Serial port"), 4, grid); grid->addLayout(stack(serial, serialPort), 4, 1, 1, 2);
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
    usbLabel = label(tr("USB HotSync"), 6, grid); grid->addLayout(stack(usb, usbPort), 6, 1, 1, 2);
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
    irda->setToolTip(tr("Pick a connection to beam to another Frond. Pair them in File > Pairing."));
    irdaPort=new QSpinBox(root);irdaPort->setObjectName("irdaPort");
    irdaPort->setRange(1,65535);irdaPort->setPrefix(tr("Port "));loadIrdaSettings();
    label(tr("Infrared beaming"), 8, grid); grid->addLayout(stack(irda, irdaPort), 8, 1, 1, 2);
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
    bluetoothLabel = label(tr("Bluetooth"), 10, grid); grid->addWidget(bluetooth, 10, 1, 1, 2); grid->addWidget(bluetoothSocket, 11, 0, 1, 3);
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
    connect(holdToggle, &QCheckBox::clicked, this, [this](bool on) {
        if (frondFirmware::profile(runningDevice).frontend.holdSupported && holdKnown)
            send("hold", on ? "on" : "off");
    });
    // 68k devices: LCD contrast, sent live and kept in the machine settings and the session.
    contrastLabel = label(tr("Contrast"), 0, displayGrid);
    contrast = new QSlider(Qt::Horizontal, root);
    contrast->setObjectName("runningContrast");
    contrast->setRange(0, 100);
    contrast->setPageStep(10);
    contrast->setTracking(false);
    displayGrid->addWidget(contrast, 0, 1, 1, 2);
    runningButtons << contrast;
    connect(contrast, &QSlider::valueChanged, this, [this](int value) {
        if (client.available() && frondFirmware::runs68k(runningDevice) && value != runningPreferences.contrast)
            { contrastPending = true; send("display", "contrast=" + QString::number(value)); }
    });
    // Settings the core changes while it runs. Each is kept in the machine settings first.
    sizeLabel = label(tr("Size"), 3, displayGrid);
    sizeSlider = new QSlider(Qt::Horizontal, root);
    sizeSlider->setObjectName("runningSize");
    sizeSlider->setRange(4, 8);
    sizeSlider->setPageStep(1);
    sizeSlider->setTracking(false);
    displayGrid->addWidget(sizeSlider, 3, 1, 1, 2);
    smoothToggle = new QCheckBox(tr("Smooth pixels"), root);
    smoothToggle->setObjectName("runningSmooth");
    displayGrid->addWidget(smoothToggle, 4, 0, 1, 3);
    keyboardShow = new QCheckBox(tr("Show keyboard"), root);
    keyboardShow->setObjectName("runningShowKeyboard");
    displayGrid->addWidget(keyboardShow, 5, 0, 1, 3);
    jogLabel = label(tr("Jog controls"), 6, displayGrid);
    jogBox = new QComboBox(root);
    jogBox->setObjectName("runningJogControls");
    jogBox->addItem(tr("Hidden"), int(frondSessions::JogControls::hidden));
    jogBox->addItem(tr("Left of screen"), int(frondSessions::JogControls::screen));
    jogBox->addItem(tr("Left of keyboard"), int(frondSessions::JogControls::keyboard));
    displayGrid->addWidget(jogBox, 6, 1, 1, 2);
    speedLabel = label(tr("Speed"), 7, displayGrid);
    speedBox = new QComboBox(root);
    speedBox->setObjectName("runningSpeed");
    speedBox->addItem(tr("Original"), false);
    speedBox->addItem(tr("Uncapped"), true);
    displayGrid->addWidget(speedBox, 7, 1, 1, 2);
    for (auto* w : std::initializer_list<QWidget*>{sizeSlider, smoothToggle, keyboardShow, jogBox, speedBox}) runningButtons << w;
    connect(sizeSlider, &QSlider::valueChanged, this, [this](int value) { liveDisplay("size"); (void)value; });
    connect(smoothToggle, &QCheckBox::clicked, this, [this] { liveDisplay("smooth"); });
    connect(keyboardShow, &QCheckBox::clicked, this, [this] { liveDisplay("keyboard"); });
    connect(jogBox, qOverload<int>(&QComboBox::activated), this, [this] { liveDisplay("jog"); });
    connect(speedBox, qOverload<int>(&QComboBox::activated), this, [this] { liveDisplay("speed"); });
    auto* reset = button(tr("Reset"), "reset");
    resetButton = reset;
    auto* rotate = button(tr("Rotate"), "rotate");
    rotateButton = rotate;
    // The connector's HotSync button: the IIIx cradle, the T|X and the E2.
    hotSyncButton = button(tr("HotSync"), "hotsync");
    displayLayout->addWidget(rotate, 0, Qt::AlignLeft);
    displayLayout->addStretch();
    connectionsLayout->addWidget(hotSyncButton, 0, Qt::AlignLeft);
    auto* pair = new QPushButton(tr("Pair with another Frond…"), root);
    pair->setObjectName("pairWith");
    connectionsLayout->addWidget(pair, 0, Qt::AlignLeft);
    connectionsLayout->addStretch();
    connect(pair, &QPushButton::clicked, this, &MainWindow::showConnections);
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

    auto* saveButton = button(tr("Save"), "save");
    auto* copyButton = button(tr("Save a copy…"), "saveCopy");
    auto* logButton = button(tr("Emulator log"), "emulatorLog", true);
    auto* hardReset = button(tr("Hard reset"), "hardReset");
    this->saveButton = saveButton; saveCopyButton = copyButton; this->logButton = logButton; hardResetButton = hardReset;
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
    auto* logDialog = new QDialog(this);
    logDialog->setWindowTitle(tr("Emulator log")); logDialog->resize(700, 400);
    auto* logLayout = new QVBoxLayout(logDialog);
    log = new QPlainTextEdit(logDialog); log->setObjectName("logText"); log->setReadOnly(true); log->setMaximumBlockCount(1000);
    logLayout->addWidget(log);
    connect(logButton, &QPushButton::clicked, logDialog, &QWidget::show);
    auto bluetoothChanged = [this] {
        if (client.running()) { if (client.available()) send("bluetooth-mode", bluetoothArgument()); return; }
        refresh();
    };
    connect(bluetooth, &QCheckBox::clicked, this, bluetoothChanged);
    connect(bluetoothSocket, &QLineEdit::editingFinished, this, bluetoothChanged);

    stopButton = new QPushButton(tr("Save and Stop"), root);
    stopButton->setObjectName("stop");
    forceButton = new QPushButton(tr("Force Stop…"), root);
    forceButton->setObjectName("forceStop");
    forceButton->hide();
    setCentralWidget(outer);
    setAcceptDrops(true);
    connect(panels, &PanelHost::aboutToChange, this, [this](int page) {
        if (embedded && page >= 0 && !panelWasOpen && !isMaximized() && !isFullScreen()) baseWidth = width();
    });
    connect(panels, &PanelHost::changed, this, &MainWindow::panelChanged);
    // The same keys the Palm window sends back as shortcut events; Quit is the File menu's.
    for (const auto& key : runningRules::shortcutKeys()) {
        if (key.what == runningRules::Shortcut::Quit) continue;
        auto* shortcut = new QShortcut(QKeySequence(QString::fromLatin1(key.keys)), this);
        connect(shortcut, &QShortcut::activated, this, [this, what = key.what] { runShortcut(what); });
    }
    connect(&client, &DesktopClient::shortcut, this, [this](const QString& name) {
        // A dialog in front owns the keys.
        if (const auto what = runningRules::shortcutFor(name); what && !QApplication::activeModalWidget()) runShortcut(*what);
    });
    connect(stopButton, &QPushButton::clicked, this, [this] { stop(false); });
    connect(forceButton, &QPushButton::clicked, this, [this] {
        if (QMessageBox::warning(this, tr("Force Stop"), tr("Stop without saving? Changes since the last save will be lost."),
                                 QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Discard) {
            allowClose = closeAfterStop; client.forceStop();
        }
    });
    runningBar = new RunningBar(this);
    addToolBar(Qt::TopToolBarArea, runningBar);
    runningBar->installEventFilter(this);
    connect(runningBar, &RunningBar::chosen, this, &MainWindow::barChosen);
    connect(runningBar, &RunningBar::pressed, this, &MainWindow::barPressed);
    connect(runningBar, &RunningBar::networkChosen, this, [this](const QString& mode) {
        if (const auto index = network->findData(mode); index >= 0 && network->isEnabled()) { network->setCurrentIndex(index); emit network->activated(index); }
    });
    auto* file = menuBar()->addMenu(tr("&File"));
    importAction = file->addAction(tr("&Import session…"), this, &MainWindow::importSessionDialog);
    importAction->setShortcut(QKeySequence::Open);
    file->addAction(tr("C&ards…"), this, [this] { showCards(); })->setObjectName("cardsAction");
    file->addAction(tr("&Pairing…"), this, &MainWindow::showConnections)->setObjectName("connectionsAction");
    file->addAction(tr("&Firmware…"), this, [this] { showFirmware(); });
    file->addAction(tr("&Settings…"), this, [this] { settingsDialog->exec(); });
    file->addSeparator();
    file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);
    auto* help = menuBar()->addMenu(tr("&Help"));
    helpMenu = help;
    help->addAction(tr("Hardware &Keyboard Controls"), this, [this] {
        QMessageBox::information(this, tr("Hardware Keyboard Controls"),
            tr("With the Palm window focused and the infrared keyboard detached:\n\n"
               "Arrow keys: five-way navigation\nEnter: centre select\n"
               "F1: Home\nF2: Calendar\nF3: Contacts\nF4: Web\n"
               "On the Tungsten E2 and E3, F1 to F4 are Calendar, Contacts, Tasks and Note Pad.\n"
               "On the Zire 72, F1 to F4 are Calendar, Contacts, Music and Camera, and F5 is the side voice button.\n"
               "On the Sony CLIÉ TG50, F1 to F4 are Date Book, Address, To Do and Memo Pad, F5 is the voice button, F6 is Home, and left and right Alt are Blue and Red.\n"
               "Escape: Power, on devices with a power key in the button bar, except the de-NVFS E2\n\n"
               "Shortcuts, in the Palm window and in this one:\n"
               "Ctrl+S: Save. Ctrl+Shift+S: Save a copy. Ctrl+W: Save and Stop. Ctrl+I: Install.\n"
               "Ctrl+1 to Ctrl+4: Applications, Card, Display and Connections panels.\n"
               "Ctrl+M: Sound on or off. Ctrl+B: Hide or show the bar. Ctrl+L: Emulator log. Ctrl+R: Reset.\n"
               "F12: Screenshot. Ctrl+Q: Quit.\n"
               "With the infrared keyboard attached, the Ctrl keys go to the Palm and only F12 is a shortcut.\n\n"
               "Palm OS button assignments may change what the application buttons do."));
    });
    help->addAction(tr("&About Frond"), this, [this] {
        QMessageBox box(QMessageBox::NoIcon, tr("About Frond"),
            tr("<p><b>Frond %1</b><br>A Palm OS emulator.</p>"
               "<p>Copyright Frond, CloudpilotEmu and uARM authors.<br>"
               "GPL-3.0-or-later, with no warranty.</p>"
               "<p><a href=\"https://github.com/roge-rm/frond\">github.com/roge-rm/frond</a></p>")
                .arg(QStringLiteral(FROND_VERSION)), QMessageBox::Close, this);
        box.setTextFormat(Qt::RichText);
        box.setTextInteractionFlags(Qt::TextBrowserInteraction);
        auto* licences = box.addButton(tr("Licences"), QMessageBox::ActionRole);
        // Any button closes the box, so the licences open from here and the box comes back.
        do box.exec(); while (box.clickedButton() == licences && (showLicences(this), true));
    });
    connect(&client, &DesktopClient::diagnostic, this, [this](const QString& text) {
        log->moveCursor(QTextCursor::End); log->insertPlainText(text);
    });
    connect(&client, &DesktopClient::error, this, [this](const QString& message) {
        installQueue.clear(); installTotal = 0; installFailed.clear(); installHint = 0;
        // The end of the emulator log, since the log button goes away when the core stops.
        auto lines = log->toPlainText().split('\n');
        while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
        failure(message, lines.mid(qMax(0, int(lines.size()) - 40)).join('\n'));
    });
    connect(&client, &DesktopClient::stalled, this, [this] {
        forceButton->show(); runningBar->setForceStop(true); notify(tr("The emulator is not responding. Waiting for it to finish…"), 0);
    });
    connect(&client, &DesktopClient::started, this, [this] {
        microphoneArmed = false; microphoneState->clear();
        busy = false; hardResetAfterSave = false;
        frondSessions::touchWork(sessionPath);
        notify(tr("Running. Your session will be saved when you close Frond.")); refresh();
        if (stopping) save();
        else send("device-state");
    });
    connect(&client, &DesktopClient::finished, this, [this] {
        if (exchangeDialog) exchangeDialog->reject();
        else endExchange();
        if (cardProgress) { cardProgress->hide(); cardProgress->deleteLater(); cardProgress = nullptr; }
        holdKnown = holdOn = false;
        palmHolder->release(); embedded = embedWanted = embedTried = panelWasOpen = fitOffPending = false;
        appsStale = false; deleteApp = {}; knownDatabases.clear(); appsPanel->setDatabases({});
        if (internalFiles) internalFiles->close();
        internalJob = {}; internalListWanted = false;
        microphoneArmed = false; microphoneState->clear();
        installQueue.clear(); installTotal = 0; installFailed.clear(); installHint = 0;
        busy = false; stopping = false; hardResetAfterSave = false; cardPresent = cardInserted = false; runningDevice = frondFirmware::Device::tx;
        copyLock.reset(); savingPath.clear();
        frondSessions::removeStaging(copyStaging); copyStaging.clear(); pendingCopy.clear();
        // A linked card goes back to the drawer once the final save is in, under the session lock.
        if (finalSaved && sessionLock)
            if (const auto error = frondCards::releaseAtClose(sessionFolder(), frondSessions::machineDirOf(sessionPath)); !error.isEmpty())
                log->appendPlainText(error);
        finalSaved = false; linkedWatcher.reset(); linkedWatcherId.clear(); linkedHolder.clear(); cardMove = {};
        sdKnown = protectionKnown = false; showSerial({}); link.setSticky("serial", {}); serialLinkText.clear(); loadSerialSettings(); showUsb({}); loadUsbSettings(); showIrda({}); link.setSticky("ir", {}); irdaLinkText.clear(); loadIrdaSettings(); showBluetooth({}); loadBluetoothSettings(); sessionLock.reset(); forceButton->hide(); runningBar->setForceStop(false); refreshSessions();
        refresh();
        if (closeAfterStop) { allowClose = true; close(); }
        else notify(tr("Stopped."));
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
        else if (type == "serial") showSerialLink(m);
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
        busy = false; forceButton->hide(); runningBar->setForceStop(false); refresh();
        QTimer::singleShot(0, this, [this] {
            if (internalListWanted) listInternal();
            if (appsStale) listApps();
        });
        if (command == "db-list") { databasesReply(ok, message); return; }
        if (command == "window-id") {
            embedReply(ok, message);
            // The core started fitted for the embedding; on its own it goes back to following the
            // Palm's size (Rotate, keyboard, jog). frond-68k can't change that live and has no rotation.
            if (!embedded && !stopping && !frondFirmware::runs68k(runningDevice) && send("display", "fit=none")) {
                fitOffPending = true;
                return;
            }
            if (stopping) save();
            else afterDevice();
            return;
        }
        if (command == "display" && fitOffPending) {
            fitOffPending = false;
            if (!ok) log->appendPlainText(command + ": " + message);
            if (stopping) save();
            else afterDevice();
            return;
        }
        if (deleteApp.active && !stopping && command == "delete-db") { deleteReply(ok, message); return; }
        if (command == "input-replay") {
            replayFiles.reset();
            const bool rearm = ok && replayMicrophone;
            replayMicrophone = false;
            if (ok) {
                if (replayKind == "screenshot") notify(tr("Screenshot saved: %1").arg(replayShot));
                if (rearm) send("microphone-mode", "on");
                return;
            }
        }
        if (command == "install") appsStale = true;
        if (internalJob.active && !stopping && (command == "install" || command == "export-db" || command == "delete-db")) {
            internalReply(ok, message);
            return;
        }
        if (command == "install" && installTotal > 0 && installResult(ok, code, message)) return;
        if (!ok && cardMove.stage != CardMove::None) { cardMoveFailed(command, message); return; }
        if (!ok && exchangeStage != ExchangeStage::None) { exchangeResult(false, message); return; }
        if (!ok) {
            if (code == "cancelled" && !stopping) { notify(tr("Cancelled.")); return; }
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
            if (command == "display" && contrastPending) { const QSignalBlocker blocked(contrast); contrast->setValue(runningPreferences.contrast); }
            if (command == "display") { contrastPending = false; liveDisplay({}); }
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
            else if (embedWanted && !embedTried) { embedTried = true; send("window-id"); }
            else afterDevice();
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
        if (command == "display" && contrastPending) {
            contrastPending = false;
            runningPreferences.contrast = contrast->value();
            runningPreferences.contrastSet = true;
            if (const auto error = frondSessions::writePreferences(sessionPath, runningPreferences); !error.isEmpty()) failure(error);
        }
        if (command == "serial-mode") { runningPreferences.serial = serialChoice(); savePeripherals(); attachSerial(); }
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
        notify(command == "save" ? tr("Saved.") : tr("Running. Sessions save automatically on close."));
        const bool savedWorkingCopy = command == "save" && savingPath == sessionPath;
        if (command == "save") {
            if (savedWorkingCopy) {
                // While Files has the card out, the slot keeps swap until it's back in.
                if (exchangeStage == ExchangeStage::None) frondCards::clearSwap(frondSessions::machineDirOf(sessionPath));
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
    notify(tr("Ready. Sessions are saved automatically when you close Frond."));
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
    runArea->setVisible(running);
    palmHolder->setVisible(running && embedded);
    barHandle->setVisible(running && !runningBar->barShown() && !fullScreenOn);
    statusBar()->setVisible(!running);
    if (running != machineMenuShown) {
        machineMenuShown = running;
        centralWidget()->layout()->setContentsMargins(running ? QMargins() : QMargins(9, 9, 9, 9));
        if (running) {
            menuBar()->insertMenu(helpMenu->menuAction(), runningBar->menu());
            listGeometry = saveGeometry();
            appsStale = true;
        } else {
            leaveFullScreen();
            menuBar()->removeAction(runningBar->menu()->menuAction());
            runningBar->setBarShown(true);
            panels->open(-1);
            // The panel's and the Palm's minimum sizes go with them once the layout settles.
            panels->setMinimumWidth(0); panels->setMaximumWidth(QWIDGETSIZE_MAX);
            if (layout()) layout()->activate();
            centralWidget()->layout()->activate();
            if (!listGeometry.isEmpty()) {
                restoreGeometry(listGeometry);
                QTimer::singleShot(0, this, [this, geometry = listGeometry] { if (!client.running()) restoreGeometry(geometry); });
            }
            listGeometry.clear();
        }
        if (running) fitWindow();
    }
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
    shown(embedded, {sizeLabel, sizeSlider});
    shown(currentDevice() == frondFirmware::deviceFromId("sony-clie-tg50"), {keyboardShow, jogLabel, jogBox});
    shown(m68k, {speedLabel, speedBox});
    liveDisplay({});
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
    serialPort->setEnabled(caps.serial && serial->currentData() == "tcp"); serialPort->setReadOnly(!editable);
    if (!frondFirmware::serialAbsent(currentDevice())) serialPort->setVisible(!serial->currentData().toString().startsWith("link:"));
    usb->setEnabled(editable && caps.usb);
    usbPort->setEnabled(caps.usb && usb->currentData() == "tcp"); usbPort->setReadOnly(!editable);
    irda->setEnabled(editable && caps.infrared && !keyboard->isChecked());
    irdaPort->setEnabled(caps.infrared && !keyboard->isChecked() && irda->currentData()=="tcp"); irdaPort->setReadOnly(!editable);
    irdaPort->setVisible(!irda->currentData().toString().startsWith("link:"));
    bluetooth->setEnabled(editable && caps.bluetooth);
    bluetoothSocket->setEnabled(caps.bluetooth && bluetooth->isChecked()); bluetoothSocket->setReadOnly(!editable);
    bluetooth->setToolTip(caps.bluetooth ? tr("See doc/using-frond.md (Bluetooth) for controller setup.") : reason);
    // Which card buttons show follows the card's state, as on Android.
    const bool memoryStick = this->memoryStick();
    const bool supported = storageSupported();
    storageHeading->setText(memoryStick ? tr("Memory Stick") : tr("SD card"));
    panels->setTitle(1, tr("Storage"));
    internalButton->setEnabled(enabled && !internalFiles);
    appsPanel->setEnabled(enabled);
    storageHeading->setVisible(!m68k);
    storageHeading->setEnabled(supported);
    sdStatus->setEnabled(supported);
    const bool known = sdKnown && running && supported;
    sdStatus->setText(!supported ? tr("Not available yet") : !known ? tr("Not known") : !cardPresent ? tr("No card") : cardInserted ? tr("Inserted") : tr("Ejected"));
    // The picker button already says "No card" or the card's name, and Eject shows it's in.
    sdStatus->setVisible(!m68k && (!known || (cardPresent && !cardInserted)));
    sdReinsert->setVisible(known && cardPresent && !cardInserted);
    sdExchange->setVisible(known && cardPresent);
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
    runningRules::State state;
    state.ready = enabled;
    state.running = running;
    if (holdKnown) state.hold = holdOn;
    state.keyboardAttached = keyboard->isChecked();
    state.beamOff = irda->currentData() == "off";
    state.microphone = microphoneArmed ? "on" : microphoneState->text().isEmpty() ? "off" : "off " + microphoneState->text();
    state.network = network->currentData().toString();
    state.barShown = runningBar->barShown();
    state.embedded = embedded;
    state.fullScreen = fullScreenOn;
    QList<QPair<QString, QString>> modes;
    for (int i = 0; i < network->count(); i++) modes << qMakePair(network->itemData(i).toString(), network->itemText(i));
    const auto traits = runningRules::traitsFor(currentDevice());
    runningBar->setMachine(running && !sessionPath.isEmpty() ? QFileInfo(sessionPath).completeBaseName() : QString(),
                           runningRules::subtitle(traits, runningPreferences.fastEngine, frondFirmware::deviceName(currentDevice())));
    runningBar->apply(traits, state, runningRules::cardIcon(supported, known, cardPresent, cardInserted),
                      caps.sound && runningPreferences.audio, modes);
    if (fullScreenOn) showOverlayBar(overlayBarShown);
    else runningBar->setVisible(running && runningBar->barShown());
}
void MainWindow::notify(const QString& text, int ms) {
    if (client.running()) runningBar->say(text, ms);
    else statusBar()->showMessage(text);
}
void MainWindow::togglePanel(int page) { panels->open(panels->current() == page ? -1 : page); }
void MainWindow::panelChanged(int page) {
    runningBar->setOpenPanel(page);
    if (fullScreenOn) {
        placeOverlays();
        if (page < 0 && overlayBarShown) overlayTimer->start();
    }
    if (page == 0) { appsStale = true; listApps(); }
    setPanelOpen(page >= 0);
}
// Embedded, the panel opens at the right of the Palm and the window grows by its width. Without
// the embedding the window is as wide as a panel needs and as tall as the bar and the open panel.
void MainWindow::setPanelOpen(bool open) {
    const bool changed = open != panelWasOpen;
    panelWasOpen = open;
    if (embedded) {
        panels->setFixedWidth(runningRules::panelWidth);
        // Showing the panel can already stretch the window to the layout's minimum, so opening
        // starts from the width the window had without it.
        if (changed && client.running() && !isMaximized() && !isFullScreen()) {
            // The layout's minimum width drops with the panel; settle it before shrinking.
            runArea->layout()->invalidate();
            centralWidget()->layout()->invalidate();
            if (layout()) { layout()->invalidate(); layout()->activate(); }
            if (open) resize(runningRules::windowWidth(baseWidth, true), height());
            else {
                // The layout's minimum width settles after the panel is gone.
                const int target = std::max(1, width() - runningRules::panelWidth);
                QTimer::singleShot(0, this, [this, target] {
                    if (client.running() && embedded && !panelWasOpen && !isMaximized() && !isFullScreen()) resize(target, height());
                });
            }
        }
        return;
    }
    fitWindow();
}
void MainWindow::fitWindow() {
    if (!client.running() || isMaximized() || isFullScreen() || embedded) return;
    panels->setMinimumHeight(panels->isVisibleTo(this) ? 420 : 0);
    if (layout()) layout()->activate();
    centralWidget()->layout()->activate();
    resize(480, 1);
    // The layout's minimum height settles after the panel is gone.
    QTimer::singleShot(0, this, [this] { if (client.running() && !isMaximized() && !isFullScreen() && !embedded) resize(480, 1); });
}
// Full screen is only for the embedded Palm. The menu bar goes, the bar and an open panel float
// over the Palm (native windows of the main window, so they stack above the embedded one) and the Palm fits the screen.
void MainWindow::toggleFullScreen() {
    if (fullScreenOn) leaveFullScreen();
    else enterFullScreen();
}
void MainWindow::enterFullScreen() {
    if (fullScreenOn || !client.running() || !embedded) return;
    fullScreenOn = true;
    fullScreenWasMaximized = isMaximized();
    fullScreenReturnSize = QSize(panelWasOpen ? baseWidth : width(), height());
    menuBar()->hide();
    removeToolBar(runningBar);
    for (QWidget* w : {static_cast<QWidget*>(runningBar), static_cast<QWidget*>(panels)}) {
        w->setParent(this);
        w->setAttribute(Qt::WA_NativeWindow);
        w->setAutoFillBackground(true);
    }
    bodyLayout->removeWidget(panels);
    barHandle->hide();
    showFullScreen();
    overlayBarShown = true;
    panels->setVisible(panels->current() >= 0);
    refresh();
    placeOverlays();
    overlayTimer->start();
}
void MainWindow::leaveFullScreen() {
    if (!fullScreenOn) return;
    // A panel left open would squeeze the Palm in the smaller window.
    panels->open(-1);
    fullScreenOn = false;
    overlayTimer->stop();
    edgeTimer->stop();
    panels->setParent(runArea);
    bodyLayout->addWidget(panels);
    panels->setVisible(panels->current() >= 0);
    addToolBar(Qt::TopToolBarArea, runningBar);
    menuBar()->show();
    if (fullScreenWasMaximized) showMaximized();
    else {
        // The window manager brings back the size from before full screen, which is wider by a
        // panel when one was open then.
        showNormal();
        if (fullScreenReturnSize.width() > 0) resize(fullScreenReturnSize);
    }
    refresh();
}
void MainWindow::showOverlayBar(bool on) {
    overlayBarShown = on;
    runningBar->setVisible(on);
    if (on) edgeTimer->stop();
    else if (fullScreenOn) edgeTimer->start();
    placeOverlays();
}
void MainWindow::placeOverlays() {
    if (!fullScreenOn) return;
    // Each is its own X window, so the raise goes to the window; the Palm's goes first.
    const auto raise = [](QWidget* w) { if (w->windowHandle()) w->windowHandle()->raise(); else w->raise(); };
    palmHolder->raisePalm();
    const auto area = runArea->size();
    const auto origin = runArea->mapTo(this, QPoint(0, 0));
    runningBar->setGeometry(origin.x(), origin.y(), area.width(), runningBar->height());
    if (panels->current() >= 0) {
        panels->setGeometry(runningRules::overlayPanel(area, overlayBarShown ? runningBar->height() : 0).translated(origin));
        raise(panels);
    }
    if (overlayBarShown) raise(runningBar);
}
bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (fullScreenOn) {
        if (watched == runningBar && event->type() == QEvent::Enter) overlayTimer->stop();
        else if (watched == runningBar && event->type() == QEvent::Leave) overlayTimer->start();
        else if (watched == runArea && event->type() == QEvent::Resize) QTimer::singleShot(0, this, &MainWindow::placeOverlays);
    }
    return QMainWindow::eventFilter(watched, event);
}
// Shows or hides the bar, keeps the choice in the machine's settings and keeps the Palm's size.
void MainWindow::setBarShown(bool shown) {
    if (shown == runningBar->barShown()) return;
    runningBar->setBarShown(shown);
    barHandle->setVisible(client.running() && !shown);
    runningPreferences.barShown = shown;
    if (!sessionPath.isEmpty() && client.running())
        if (const auto error = frondSessions::writePreferences(sessionPath, runningPreferences); !error.isEmpty()) failure(error);
    if (embedded && !isMaximized() && !isFullScreen()) {
        const int change = runningBar->height() - runningRules::handleHeight;
        resize(width(), height() + (shown ? change : -change));
    }
    refresh();
}
// The window-id reply. An X11 id is wrapped and shown under the bar; anything else leaves the
// core's own window and the bar-only window as they are.
void MainWindow::embedReply(bool ok, const QString& message) {
    const auto id = ok ? runningRules::parseWindowId(message) : 0;
    embedded = id && palmHolder->embed(id);
    if (!embedded) {
        log->appendPlainText(!ok || !id ? tr("The Palm stays in its own window: %1").arg(message)
                                        : tr("The Palm stays in its own window: window %1 couldn't be embedded.").arg(id));
        // A hidden bar couldn't be brought back there; the saved choice stays for the next embedding.
        runningBar->setBarShown(true);
        refresh();
        return;
    }
    panels->setMinimumHeight(0);
    panels->setFixedWidth(runningRules::panelWidth);
    palmHolder->show();
    if (!isMaximized() && !isFullScreen()) {
        if (layout()) layout()->activate();
        centralWidget()->layout()->activate();
        auto size = sizeHint().expandedTo(minimumSizeHint());
        const auto* screen = windowHandle() ? windowHandle()->screen() : QGuiApplication::primaryScreen();
        if (screen && !screen->availableGeometry().isEmpty()) size = size.boundedTo(screen->availableGeometry().size());
        resize(size);
    }
    refresh();
}
// What follows the device name: the first state questions, in the order the window needs them.
// A display setting changed in the panel: kept in the machine settings, then sent to the core. With
// nothing named it only shows the settings in the controls.
void MainWindow::liveDisplay(const QString& what) {
    if (what.isEmpty() || !client.available()) {
        const QSignalBlocker a(sizeSlider), b(smoothToggle), c(keyboardShow), d(jogBox), e(speedBox);
        if (!sizeSlider->isSliderDown()) sizeSlider->setValue(runningPreferences.scale);
        sizeLabel->setText(tr("Size: %1×").arg(sizeSlider->value() / 4.0));
        smoothToggle->setChecked(runningPreferences.smooth);
        keyboardShow->setChecked(runningPreferences.showKeyboard);
        jogBox->setCurrentIndex(std::max(0, jogBox->findData(int(runningPreferences.jogControls))));
        speedBox->setCurrentIndex(runningPreferences.uncapped ? 1 : 0);
        return;
    }
    auto prefs = runningPreferences;
    QString command = "display", argument;
    const auto onOff = [](bool on) { return on ? QStringLiteral("on") : QStringLiteral("off"); };
    if (what == "size") prefs.scale = sizeSlider->value();
    else if (what == "smooth") { prefs.smooth = smoothToggle->isChecked(); argument = "smooth=" + onOff(prefs.smooth); }
    else if (what == "keyboard") { prefs.showKeyboard = keyboardShow->isChecked(); argument = "keyboard=" + onOff(prefs.showKeyboard); }
    else if (what == "jog") {
        prefs.jogControls = frondSessions::JogControls(jogBox->currentData().toInt());
        argument = "jog=" + frondSessions::jogControlsName(prefs.jogControls);
    } else if (what == "speed") {
        prefs.uncapped = speedBox->currentData().toBool();
        command = "speed"; argument = prefs.uncapped ? "uncapped" : "original";
    }
    if (const auto error = frondSessions::writePreferences(sessionPath, prefs); !error.isEmpty()) { liveDisplay({}); return failure(error); }
    const int oldScale = runningPreferences.scale;
    runningPreferences = prefs;
    if (what == "size") {
        // The core follows the window, so the window is what changes size.
        if (embedded && !isMaximized() && !isFullScreen() && oldScale != prefs.scale) {
            const auto now = palmHolder->size(), next = runningRules::scaledPalm(now, oldScale, prefs.scale);
            resize(width() + next.width() - now.width(), height() + next.height() - now.height());
        }
        sizeLabel->setText(tr("Size: %1×").arg(prefs.scale / 4.0));
        return;
    }
    send(command, argument);
}
void MainWindow::afterDevice() {
    send(frondFirmware::profile(runningDevice).frontend.holdPresent ? "hold-state" :
         storageSupported() ? cardCommand("state") : "network-state");
}
void MainWindow::barChosen(runningRules::Id id) {
    using runningRules::Id;
    // Each row does what its control does, so the menu and the panels always agree.
    switch (id) {
        case Id::Hold: holdToggle->click(); break;
        case Id::Keyboard: keyboard->click(); break;
        case Id::Microphone: microphoneToggle->click(); break;
        case Id::Display: panels->open(2); break;
        case Id::Connections: panels->open(3); break;
        case Id::Screenshot: screenshot(); break;
        case Id::HideBar: setBarShown(!runningBar->barShown()); break;
        case Id::Save: saveButton->click(); break;
        case Id::SaveCopy: saveCopyButton->click(); break;
        case Id::Log: logButton->click(); break;
        case Id::Reset: resetButton->click(); break;
        case Id::HardReset: hardResetButton->click(); break;
        case Id::SaveStop: stopButton->click(); break;
        case Id::FullScreen: toggleFullScreen(); break;
        case Id::Network: break;
    }
}
// Each key does what its bar button or menu row does; a control that is greyed out does nothing.
void MainWindow::runShortcut(runningRules::Shortcut what) {
    using runningRules::Shortcut;
    if (what == Shortcut::Quit) { close(); return; }
    if (!client.running()) return;
    const bool idle = client.available() && !busy && !stopping && exchangeStage == ExchangeStage::None;
    switch (what) {
        case Shortcut::Save: if (idle) barChosen(runningRules::Id::Save); break;
        case Shortcut::SaveCopy: if (idle) barChosen(runningRules::Id::SaveCopy); break;
        case Shortcut::SaveStop: stopButton->click(); break;
        case Shortcut::Install: if (idle) emit appsPanel->installRequested(); break;
        case Shortcut::Applications: togglePanel(0); break;
        case Shortcut::Card: togglePanel(1); break;
        case Shortcut::Display: togglePanel(2); break;
        case Shortcut::Connections: togglePanel(3); break;
        case Shortcut::Sound: soundToggle->click(); break;
        case Shortcut::HideBar: if (embedded) barChosen(runningRules::Id::HideBar); break;
        case Shortcut::Log: barChosen(runningRules::Id::Log); break;
        case Shortcut::Reset: barChosen(runningRules::Id::Reset); break;
        case Shortcut::Screenshot: screenshot(); break;
        case Shortcut::FullScreen: toggleFullScreen(); break;
        case Shortcut::Quit: break;
    }
}
void MainWindow::barPressed(RunningBar::Button button) {
    switch (button) {
        case RunningBar::Button::Stop: stopButton->click(); break;
        case RunningBar::Button::Applications: togglePanel(0); break;
        case RunningBar::Button::Card: togglePanel(1); break;
        case RunningBar::Button::Sound: soundToggle->click(); break;
        case RunningBar::Button::Power: replay(runningRules::powerReplay(), "power"); break;
        case RunningBar::Button::ForceStop: forceButton->click(); break;
    }
}
// Writes the replay file and hands it to the core. The core drops the microphone for a replay,
// so it is turned back on when the replay is loaded.
bool MainWindow::replay(const QString& lines, const QString& kind) {
    if (!client.available() || busy || stopping) return false;
    replayFiles = std::make_unique<QTemporaryDir>();
    QFile file(replayFiles->filePath("input.replay"));
    if (!replayFiles->isValid() || !file.open(QIODevice::WriteOnly) || file.write(lines.toUtf8()) < 0) {
        replayFiles.reset();
        failure(tr("Couldn't write the input file."));
        return false;
    }
    file.close();
    replayMicrophone = microphoneArmed; replayKind = kind;
    if (!send("input-replay", file.fileName())) { replayFiles.reset(); replayMicrophone = false; return false; }
    return true;
}
void MainWindow::screenshot() {
    const auto folder = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + "/Frond";
    if (!QDir().mkpath(folder)) return failure(tr("Couldn't make the folder %1.").arg(folder));
    const auto name = runningRules::screenshotName(QFileInfo(sessionPath).completeBaseName(),
                                                   QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
    replayShot = name;
    replay(runningRules::snapshotReplay(folder + "/" + name), "screenshot");
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
            if (frondSessions::tryLockSession(lock, info.path)) {
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
    if (!frondSessions::tryLockSession(lock, path)) return failure(tr("This session is open in another Frond."));
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
    // Copying and packing a T|X takes a couple of seconds, so it runs on a worker behind a progress
    // dialog. The session's lock is held for it, so Cards greys its media as for a running machine;
    // one locked already is running, and Cards greys it anyway.
    QLockFile hold(info.path + ".lock");
    hold.setStaleLockTime(0);
    const bool held = frondSessions::tryLockSession(hold, info.path);
    if (held && cardsWindow) cardsWindow->refresh();
    const auto error = runCardJob(this, tr("Exporting %1…").arg(info.name), [&](const frondSessions::CopyProgress& progress) {
        if (exportHook) exportHook();
        return frondSessions::exportFile(info, target, progress);
    });
    if (held) hold.unlock();
    if (cardsWindow) cardsWindow->refresh();
    if (error == frondSessions::cancelledText()) return;
    if (!error.isEmpty()) failure(error);
    else notify(tr("Session exported to %1.").arg(frondSessions::exportDestDir(target)));
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
    const auto folder = sessionFolder();
    // A packed NAND is unpacked and checked on a worker behind a progress dialog.
    const auto error = runCardJob(this, tr("Importing the session…"), [&](const frondSessions::CopyProgress& progress) {
        return frondSessions::importFile(folder, path, &name, progress);
    });
    if (error.isEmpty()) refreshSessions(frondSessions::filePath(sessionFolder(), name));
    return error;
}
void MainWindow::importSessionDialog() {
    if (client.running()) return;
    const auto path = QFileDialog::getOpenFileName(this, tr("Import session"), {}, tr("Frond sessions (*.frond *.session);;All files (*)"));
    if (path.isEmpty()) return;
    const auto error = importSession(path);
    if (error == frondSessions::cancelledText()) return;
    if (!error.isEmpty()) failure(error);
    else notify(tr("Session imported."));
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
    if (!frondSessions::tryLockSession(lock, path)) return failure(tr("This session is open in another Frond."));
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
        if (!error.isEmpty() && error != frondSessions::cancelledText()) failure(error);
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
    // A core left running by a Frond that stopped still holds the session while it saves.
    if (frondSessions::coreRunning(sessionPath)) {
        sessionLock.reset();
        return failure(tr("The Palm that last ran this session is still saving it. Try again in a moment."));
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
    serialLinkText = caps.serial && serialChoice().startsWith("link:") && !frondLink::knownLink(serialChoice())
        ? tr("Serial: Unknown connection") : QString();
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
    // The Palm's window follows Frond's window when it is embedded.
    embedWanted = runningRules::embedPossible(QGuiApplication::platformName(), qEnvironmentVariableIsSet("FROND_TRY_EMBED"));
    embedTried = embedded = false;
    if (embedWanted) arguments << "--window-fit" << "whole";
    runningBar->setBarShown(runningPreferences.barShown);
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
    // If Frond goes without stopping the Palm, the core saves the working session itself.
    arguments << "--save-on-disconnect" << QFileInfo(target).absoluteFilePath();
    stopping = closeAfterStop = allowClose = false;
    log->clear(); busy = true;
    const bool launched = client.start(coreFor(startDevice), arguments);
    if (!launched) { busy = false; sessionLock.reset(); }
    else { attachInfrared(); attachSerial(); }
    refresh(); notify(launched ? tr("Starting the Palm…") : tr("Could not start the Palm."));
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
    for (auto* combo : {irda, serial}) {
        const auto current = combo->currentData().toString();
        const QSignalBlocker blocked(combo);
        frondLink::addConnectionChoices(combo, current);
        combo->setCurrentIndex(std::max(0, combo->findData(current)));
    }
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

QString MainWindow::serialChoice() const {
    const auto mode = serial->currentData().toString();
    return mode == "tcp" ? "tcp:" + QString::number(serialPort->value()) : mode;
}
// A connection runs the core's serial port on a Unix socket the hub opens once the channel joins.
QString MainWindow::serialArgument() const {
    const auto choice = serialChoice();
    if (!choice.startsWith("link:")) return choice;
    if (!frondLink::knownLink(choice)) return QStringLiteral("off");
    const auto path = frondLink::coreSocketPath("serial", QString::number(QCoreApplication::applicationPid()));
    return path.isEmpty() ? QStringLiteral("off") : "unix:" + path;
}
void MainWindow::attachSerial() {
    const auto choice = serialChoice();
    const bool wanted = client.running() && frondFirmware::capabilities(runningDevice).serial &&
                        frondLink::knownLink(choice) && serialArgument().startsWith("unix:");
    if (!wanted) { link.setSticky("serial", {}); if (!choice.startsWith("link:")) serialLinkText.clear(); return; }
    serialLinkText = tr("Serial: connecting to %1").arg(frondLink::connectionName(choice.mid(5)));
    link.setSticky("serial", {{"op", "serial-attach"}, {"machine", QFileInfo(sessionPath).completeBaseName()},
                              {"device", frondFirmware::deviceName(runningDevice)}, {"link", choice.mid(5)},
                              {"socket", serialArgument().mid(5)}});
    serialState->setText(serialLinkText);
    refresh();
}
void MainWindow::showSerialLink(const QJsonObject& s) {
    const auto peer = s.value("peer").toString(), state = s.value("state").toString(), far = s.value("far").toString();
    const auto machine = s.value("machine").toString().isEmpty() ? s.value("device").toString() : s.value("machine").toString();
    QString text;
    if (state == "joined") text = far == "asleep" ? tr("Serial: %1, port closed").arg(peer) :
                                  far == "paused" ? tr("Serial: %1, Palm paused").arg(peer) :
                                  tr("Serial: %1, joined to %2").arg(peer, machine);
    else if (state == "waiting") text = tr("Serial: waiting for a machine on %1").arg(peer);
    else if (state == "busy") text = tr("Serial on %1 is in use by %2").arg(peer, s.value("busyBy").toString());
    else if (state == "notfound") text = tr("Serial: %1 not found").arg(peer);
    else if (state == "keychanged") text = tr("Serial: %1 key changed, pair again").arg(peer);
    else if (state == "forgotten") text = tr("Serial: pair again with %1").arg(peer);
    else if (state == "unknown") text = tr("Serial: Unknown connection");
    else text = tr("Serial: connecting to %1").arg(peer);
    serialLinkText = text;
    if (client.running() && serial->currentData().toString().startsWith("link:")) { serialState->setText(text); refresh(); }
}
// Live readout of the core's serial-state reply: "off", "off <error>" or
// "<pty|tcp> <endpoint> <connected|waiting>". Empty while no core is running.
// While running, the mode and port follow what the core actually has open.
void MainWindow::showSerial(const QString& state) {
    const auto parts = state.split(' ');
    // On a connection the hub's status shows instead of the socket.
    if (parts[0] == "unix" && serial->currentData().toString().startsWith("link:")) {
        serialState->setText(serialLinkText); refresh(); return;
    }
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
    const QSignalBlocker blocked(serial);
    frondLink::addConnectionChoices(serial, value);
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
    if (frondSessions::isMachineFolder(dest) && !frondSessions::tryLockSession(*copyLock, destFrond)) { copyLock.reset(); return failure(tr("This session is open in another Frond.")); }
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
// Asks the core for the RAM databases when the Applications panel is open and the core is free.
void MainWindow::listApps() {
    if (!appsStale || panels->current() != 0 || !client.available() || busy || stopping || exchangeStage != ExchangeStage::None) return;
    appsStale = false;
    if (client.request("db-list")) { busy = true; refresh(); }
}
void MainWindow::databasesReply(bool ok, const QString& message) {
    const auto all = ok ? runningRules::parseDatabases(message) : std::vector<runningRules::Database>{};
    if (deleteApp.active && deleteApp.lookup) { deleteLookup(ok, message, all); return; }
    if (internalFiles) {
        // A failed list keeps the last one and says why; it isn't asked for again until the next change.
        internalFiles->setDatabases(ok ? all : knownDatabases);
        if (!ok) internalFiles->note->setText(message);
    }
    // A list that can't be had (Palm OS still starting) is left as it was.
    if (!ok) { log->appendPlainText("db-list: " + message); return; }
    knownDatabases = all;
    appsPanel->setDatabases(all);
}
void MainWindow::openInternal() {
    if (!client.running()) return;
    if (internalFiles) { internalFiles->raise(); internalFiles->activateWindow(); return; }
    CardFilesWindow::Databases d;
    d.list = [this] { internalListWanted = true; listInternal(); };
    d.install = [this](const QStringList& files) {
        QList<QPair<QString, QString>> commands;
        for (const auto& f : files) commands << qMakePair(QStringLiteral("install"), f);
        startInternal("install", commands);
    };
    d.exportTo = [this](const QStringList& names, const QString& folder) {
        internalJob.temp = std::make_shared<QTemporaryDir>();
        QList<QPair<QString, QString>> commands;
        for (int i = 0; i < names.size(); ++i)
            commands << qMakePair(QStringLiteral("export-db"), internalJob.temp->filePath(QString::number(i) + ".zip") + "\n" + names[i]);
        startInternal("export", commands, folder);
    };
    d.remove = [this](const QStringList& names) {
        QList<QPair<QString, QString>> commands;
        for (const auto& n : names) commands << qMakePair(QStringLiteral("delete-db"), n);
        startInternal("delete", commands);
    };
    internalFiles = new CardFilesWindow(std::move(d), tr("Internal"), this);
    internalFiles->setObjectName("internalFilesWindow");
    internalFiles->deleteOnClose = true;
    connect(internalFiles, &QDialog::finished, this, [this] { refresh(); });
    internalFiles->show();
    refresh();
}
// Asks for db-list for the Internal window once the core is free.
void MainWindow::listInternal() {
    if (!internalListWanted || !internalFiles || !client.available() || busy || stopping || internalJob.active ||
        exchangeStage != ExchangeStage::None || deleteApp.active || installTotal > 0)
        return;
    internalListWanted = false;
    if (!send("db-list")) internalListWanted = true;
}
void MainWindow::startInternal(const QString& kind, const QList<QPair<QString, QString>>& commands, const QString& folder) {
    if (!client.available() || busy || stopping || internalJob.active || exchangeStage != ExchangeStage::None || deleteApp.active ||
        installTotal > 0) {
        if (internalFiles) internalFiles->databasesDone(tr("Frond is busy. Try again in a moment."));
        return;
    }
    const auto temp = internalJob.temp;
    internalJob = {};
    internalJob.active = true;
    internalJob.kind = kind;
    internalJob.folder = folder;
    internalJob.queue = commands;
    internalJob.temp = temp;
    internalNext();
}
void MainWindow::internalNext() {
    while (!internalJob.queue.isEmpty()) {
        const auto [command, argument] = internalJob.queue.takeFirst();
        internalJob.current = command == "install" ? QFileInfo(argument).fileName() : argument.section('\n', -1);
        if (command == "export-db") internalJob.zips << argument.section('\n', 0, 0);
        if (send(command, argument)) return;
        if (command == "export-db") internalJob.zips.removeLast();
        internalJob.failed << internalJob.current + ": " + tr("Couldn't send it to the emulator.");
    }
    internalDone();
}
void MainWindow::internalReply(bool ok, const QString& message) {
    if (ok) internalJob.done++;
    else {
        internalJob.failed << internalJob.current + ": " + message;
        if (internalJob.kind == "export") internalJob.zips.removeLast();
    }
    internalNext();
}
void MainWindow::internalDone() {
    auto job = internalJob;
    internalJob = {};
    appsStale = true;
    QString message;
    if (job.kind == "export") {
        int files = 0;
        for (const auto& zip : job.zips) {
            QString error;
            const int n = frondCardFiles::unzipInto(zip, job.folder, &error);
            if (n < 0) job.failed << error;
            else files += n;
        }
        message = files == 1 ? tr("1 file copied") : tr("%1 files copied").arg(files);
    } else if (job.kind == "install") {
        message = tr("%1 added").arg(job.done);
    } else {
        message = job.done == 1 ? tr("1 deleted") : tr("%1 deleted").arg(job.done);
    }
    job.temp.reset();
    if (!job.failed.isEmpty())
        QMessageBox::warning(internalFiles ? static_cast<QWidget*>(internalFiles) : this,
                             job.kind == "install" ? tr("Couldn't add the files") : job.kind == "export" ? tr("Couldn't copy to the folder")
                                                                                                      : tr("Couldn't delete"),
                             job.failed.join('\n'));
    if (internalFiles) internalFiles->databasesDone(message);
}
// Deletes an application and the RAM databases with its creator. The application goes first: if
// the core refuses it (ROM, running, open) nothing else is touched.
void MainWindow::deleteApplication(const QString& name) {
    if (!client.available() || busy || stopping || deleteApp.active) return;
    if (QMessageBox::question(this, tr("Delete"), tr("Delete %1 and its data?").arg(name),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    deleteApp = {};
    deleteApp.active = deleteApp.lookup = true;
    deleteApp.name = name;
    if (!send("db-list")) deleteApp = {};
}
void MainWindow::deleteLookup(bool ok, const QString& message, const std::vector<runningRules::Database>& all) {
    if (!ok) { deleteApp = {}; failure(message); return; }
    const runningRules::Database* app = nullptr;
    for (const auto& d : all) if (d.name == deleteApp.name && d.application()) app = &d;
    if (!app) { deleteApp = {}; appsStale = true; failure(tr("There is no application named \"%1\".").arg(deleteApp.name)); return; }
    deleteApp.lookup = false;
    deleteApp.queue << app->name;
    for (const auto& d : runningRules::dataOf(all, *app)) deleteApp.queue << d.name;
    deleteApp.first = true;
    deleteNext();
}
void MainWindow::deleteNext() {
    if (deleteApp.queue.isEmpty()) {
        const auto name = deleteApp.name;
        const auto failed = deleteApp.failed;
        deleteApp = {};
        appsStale = true;
        if (failed.isEmpty()) notify(tr("Deleted %1.").arg(name));
        else failure(tr("%1 was deleted, but its data was not all: %2").arg(name, failed.join("; ")));
        return;
    }
    if (!send("delete-db", deleteApp.queue.takeFirst())) { deleteApp = {}; appsStale = true; }
}
void MainWindow::deleteReply(bool ok, const QString& message) {
    if (!ok && deleteApp.first) {
        deleteApp = {};
        appsStale = true;
        failure(message);
        return;
    }
    if (!ok) deleteApp.failed << message;
    deleteApp.first = false;
    deleteNext();
}
void MainWindow::exportDatabase(const QString& name) {
    const auto path = outputFile(tr("Export database"), tr("ZIP archives (*.zip)"), name + ".zip");
    if (!path.isEmpty()) send("export-db", path + "\n" + name);
}
// Any RAM database by name: launch it or export it.
void MainWindow::otherDatabase() {
    QDialog dialog(this);
    dialog.setObjectName("otherDialog");
    dialog.setWindowTitle(tr("Other database"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(tr("Palm database name"), &dialog));
    auto* name = new QComboBox(&dialog);
    name->setObjectName("otherName");
    name->setEditable(true);
    for (const auto& d : knownDatabases) name->addItem(d.name);
    name->setCurrentIndex(-1);
    layout->addWidget(name);
    auto* buttons = new QDialogButtonBox(&dialog);
    auto* launchButton = buttons->addButton(tr("Launch"), QDialogButtonBox::AcceptRole);
    auto* exportButton = buttons->addButton(tr("Export…"), QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    launchButton->setObjectName("otherLaunch");
    exportButton->setObjectName("otherExport");
    QAbstractButton* chosen = nullptr;
    connect(buttons, &QDialogButtonBox::clicked, &dialog, [&](QAbstractButton* b) { chosen = b; if (b != buttons->button(QDialogButtonBox::Cancel)) dialog.accept(); else dialog.reject(); });
    if (dialog.exec() != QDialog::Accepted || name->currentText().trimmed().isEmpty()) return;
    const auto value = name->currentText().trimmed();
    if (chosen == launchButton) send("launch-app", value);
    else exportDatabase(value);
}
void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (client.running() && !installPaths(event->mimeData()).isEmpty()) event->acceptProposedAction();
}
void MainWindow::dropEvent(QDropEvent* event) {
    const auto paths = installPaths(event->mimeData());
    if (!client.running() || paths.isEmpty()) return;
    event->acceptProposedAction();
    installFiles(paths);
}
QStringList MainWindow::installPaths(const QMimeData* data) const {
    QStringList out;
    for (const auto& url : data->urls()) {
        const auto path = url.toLocalFile();
        const auto suffix = QFileInfo(path).suffix().toLower();
        if (suffix == "prc" || suffix == "pdb" || suffix == "pqa") out << path;
    }
    return out;
}
void MainWindow::installFiles(const QStringList& paths) {
    if (paths.isEmpty() || busy || installTotal > 0) return;
    if (paths.size() == 1) { send("install", QFileInfo(paths.first()).absoluteFilePath()); return; }
    installQueue.clear(); installFailed.clear(); installHint = 0;
    for (const auto& path : paths) installQueue << QFileInfo(path).absoluteFilePath();
    installTotal = installQueue.size();
    while (installTotal > 0 && !installNext()) {}
}
// Sends the next file; false when it couldn't be sent (counted as failed), true once one is in
// flight or the batch has ended.
bool MainWindow::installNext() {
    if (installQueue.isEmpty()) return true;
    installCurrent = installQueue.takeFirst();
    const int number = installTotal - installQueue.size();
    if (send("install", installCurrent)) {
        notify(tr("Installing %1 of %2…").arg(number).arg(installTotal));
        return true;
    }
    installFailed << QFileInfo(installCurrent).fileName() + ": " + tr("Couldn't send it to the emulator.");
    if (!installQueue.isEmpty()) return false;
    installTotal = 0;
    failure(installFailed.join('\n'));
    installFailed.clear();
    return true;
}
// Takes a batch install's reply. True when it is dealt with: more files follow, or the summary
// was shown. False lets the usual handling run (cancelled, or the window is closing).
bool MainWindow::installResult(bool ok, const QString& code, const QString& message) {
    if (!ok && (code == "cancelled" || stopping)) { installQueue.clear(); installTotal = 0; return false; }
    if (!ok) {
        installFailed << QFileInfo(installCurrent).fileName() + ": " + message;
        if (code == "not-ready") ++installHint;
    }
    while (!installQueue.isEmpty()) {
        if (installNext()) return true;
    }
    if (installTotal == 0) return true;
    const auto failed = installFailed;
    const int hint = installHint, total = installTotal;
    installTotal = 0; installFailed.clear(); installHint = 0;
    if (failed.isEmpty()) return false;
    notify(tr("Installed %1 of %2.").arg(total - failed.size()).arg(total));
    failure(failed.join('\n') + (hint ? tr("\n\nWait for Palm OS to finish its current activity, then try again.") : QString()));
    return true;
}
bool MainWindow::send(const QString& command, const QString& argument) {
    if (!client.request(command, argument)) return false;
    busy = true; refresh(); notify(tr("Working…"));
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
    else if (client.available() && exchangeStage == ExchangeStage::None) save();
    else notify(tr("Waiting to save the session…"));
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
    if (allowClose || !client.running()) {
        settings.setValue("windowGeometry", listGeometry.isEmpty() ? saveGeometry() : listGeometry);
        event->accept();
        return;
    }
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
    if (!client.available() || stopping || !cardPresent || exchangeStage != ExchangeStage::None || cardMove.stage != CardMove::None) return;
    if (cardInserted && QMessageBox::question(this, tr("Open the card's files?"),
            tr("Close any files open on the Palm first. The card is ejected while Files is open and goes back in when you close it."),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
    // The Palm window can request shutdown while the confirmation is open.
    if (!client.available() || stopping || !cardPresent || exchangeStage != ExchangeStage::None) return;
    exchangeWasInserted = cardInserted;
    exchangeIdentity.clear();
    if (cardInserted) { exchangeStage = ExchangeStage::Eject; send(cardCommand("eject")); }
    else { exchangeStage = ExchangeStage::Save; savingPath = sessionPath; send("save", sessionPath); }
}
void MainWindow::endExchange() {
    if (exchangeDialog) {
        // Closing it here must not start a put-in.
        auto* dialog = exchangeDialog.data();
        exchangeDialog = nullptr;
        dialog->closeCard();
        dialog->disconnect(this);
        dialog->close();
    }
    exchangeStage = ExchangeStage::None; exchangeIdentity.clear(); busy = false; refresh();
    if (cardsWindow) cardsWindow->refresh();
}
void MainWindow::exchangeResult(bool ok, const QString& message) {
    const auto stage = exchangeStage;
    if (ok && stage == ExchangeStage::TakeOut) {
        const auto parts = message.split(' ');
        exchangeIdentity = parts.value(0).size() == 64 ? parts.value(0) : QString(64, '0');
        exchangeProtected = parts.value(1) == "protected";
    }
    // Stopping with the card out: it goes back in before the final save.
    if (ok && stopping && stage == ExchangeStage::TakeOut) {
        exchangeStage = ExchangeStage::PutIn;
        if (!send(cardCommand("put-in"), exchangeIdentity + (exchangeProtected ? " protected" : " writable"))) { endExchange(); save(); }
        return;
    }
    if (!ok || stopping) {
        const bool reinsert = !ok && !stopping && exchangeWasInserted && (stage == ExchangeStage::Save || stage == ExchangeStage::TakeOut);
        endExchange();
        if (!ok && stage == ExchangeStage::PutIn)
            failure(tr("Couldn't put the card back in. It's kept in the machine and goes in at the next start. %1").arg(message));
        else if (!ok) failure(message);
        if (reinsert && client.available()) send(cardCommand("reinsert"));
        if (stopping) save();
        return;
    }
    if (stage == ExchangeStage::Eject) {
        exchangeStage = ExchangeStage::Save; savingPath = sessionPath; send("save", sessionPath);
    } else if (stage == ExchangeStage::Save) {
        // The working session is saved; as for any save, except that swap stays set from here
        // until the card is back in, so a machine that dies meanwhile loads it from the folder.
        savingPath.clear();
        settings.setValue("lastSession", sessionPath); settings.sync();
        QFile::remove(frondSessions::markerPath(sessionPath));
        QFile::remove(frondSessions::workPath(sessionPath));
        const auto dir = runningMachineDir();
        auto slot = frondCards::readSlot(dir);
        if (!slot.swap) {
            slot.present = true;
            slot.swap = true;
            if (!frondCards::writeSlot(dir, slot)) { exchangeResult(false, tr("Couldn't open the card.")); return; }
        }
        exchangeStage = ExchangeStage::TakeOut; send(cardCommand("take-out"));
    } else if (stage == ExchangeStage::TakeOut) {
        exchangeStage = ExchangeStage::Open;
        openExchangeCard();
    } else if (stage == ExchangeStage::PutIn) {
        if (!exchangeWasInserted) { exchangeStage = ExchangeStage::EjectAgain; send(cardCommand("eject")); return; }
        endExchange();
        notify(tr("Card put back in."));
    } else if (stage == ExchangeStage::EjectAgain) {
        endExchange();
        notify(tr("Card put back in, ejected."));
    }
}
// The card is out of the core in the machine folder's card file (sd.frondsd, ms.frondms or
// ms.frondmsp): opens it in Files.
void MainWindow::openExchangeCard() {
    const auto dir = runningMachineDir();
    const auto path = !memoryStick() ? dir + "/sd.frondsd" : QFileInfo::exists(dir + "/ms.frondms") ? dir + "/ms.frondms" : dir + "/ms.frondmsp";
    std::unique_ptr<frondCardFile::CardFile> card;
    QString error;
    try {
        card = frondCardFile::CardFile::open(path, !exchangeProtected);
        frondCardFile::FatImage check(*card);
    } catch (const frondCardFile::Error& e) {
        error = e.message;
        card.reset();
    }
    if (!error.isEmpty()) {
        // The card goes back in as it was.
        failure(error);
        exchangeClosed();
        return;
    }
    auto* dialog = new CardFilesWindow(std::move(card), runningCardName(), this);
    dialog->deleteOnClose = true;
    dialog->setWindowModality(Qt::WindowModal);
    exchangeDialog = dialog;
    connect(dialog, &QDialog::finished, this, &MainWindow::exchangeClosed);
    refresh();
    dialog->QDialog::open();
}
// Files closed: the card file is closed and goes back in the Palm.
void MainWindow::exchangeClosed() {
    if (exchangeDialog) exchangeDialog->closeCard();
    exchangeDialog = nullptr;
    if (exchangeStage != ExchangeStage::Open) return;
    if (!client.available()) {
        endExchange();
        failure(tr("The emulator stopped. The card is kept in the machine and goes in at the next start."));
        return;
    }
    exchangeStage = ExchangeStage::PutIn;
    if (!send(cardCommand("put-in"), exchangeIdentity + (exchangeProtected ? " protected" : " writable"))) exchangeResult(false, tr("Couldn't reach the Palm."));
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
        w->exchangeOpen = [this] { return exchangeStage != ExchangeStage::None || cardMove.stage != CardMove::None; };
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
            notify(tr("Card taken out."));
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
