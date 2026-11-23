#include "DanaViewControls.h"
#include <algorithm>
#include <cmath>
#include "Dialogs.h"
#include "FirmwareStore.h"
#include "CardStore.h"
#include "AudioQualification.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QSlider>
#include <QVBoxLayout>
#include <QGuiApplication>
#include <QScreen>
#include <QSignalBlocker>
#include "WindowScale.h"
#include <QSpinBox>
#include "LinkCommon.h"
#include "ControlsEditor.h"

using frondFirmware::Device;

FirmwareDialog::FirmwareDialog(const QString& root, Device preselect, QWidget* parent,
                               const frondSessions::Preferences* initialSettings, bool automaticScale) : QDialog(parent), root(root) {
    setWindowTitle(tr("Import firmware"));
    setObjectName("firmwareDialog");
    form = new QFormLayout;
    device = new QComboBox(this);
    device->setObjectName("device");
    // Items hold the device's fixed name; empty detects.
    device->addItem(tr("Detect from the NOR image"), QString());
    // Sorted by name here; the device list itself keeps its order.
    QList<Device> sorted(std::begin(frondFirmware::devices), std::end(frondFirmware::devices));
    std::stable_sort(sorted.begin(), sorted.end(), [](Device a, Device b) {
        return QString::localeAwareCompare(frondFirmware::deviceName(a), frondFirmware::deviceName(b)) < 0;
    });
    for (auto d : sorted) {
        const auto id = frondFirmware::deviceId(d);
        const auto name = frondFirmware::deviceName(d);
        device->addItem(name, id);
    }
    device->setCurrentIndex(std::max(0, device->findData(frondFirmware::deviceId(preselect))));
    form->addRow(tr("Device"), device);
    auto field = [&](const QString& label, const QString& key, QLabel** labelWidget = nullptr) {
        auto* row = new QHBoxLayout;
        auto* edit = new QLineEdit(this);
        edit->setObjectName(key);
        edit->setMinimumWidth(280);
        auto* choose = new QPushButton(tr("Choose…"), this);
        choose->setObjectName(key + "Choose");
        row->addWidget(edit, 1); row->addWidget(choose);
        connect(choose, &QPushButton::clicked, this, [this, edit, label] {
            const auto file = QFileDialog::getOpenFileName(this, label, edit->text(), tr("Firmware images (*.bin *.rom *.zip *.prc *.nbf);;All files (*)"));
            if (!file.isEmpty()) edit->setText(file);
        });
        auto* text = new QLabel(label, this);
        form->addRow(text, row);
        if (labelWidget) *labelWidget = text;
        return edit;
    };
    nor = field(tr("NOR image"), "nor", &norLabel);
    nand = field(tr("NAND image (optional for the E2)"), "nand", &nandLabel);
    // The Zire 72 has no NAND, and a 68k device takes one ROM file.
    auto nandShown = [this] {
        const auto selected = frondFirmware::deviceFromId(device->currentData().toString());
        const bool none = frondFirmware::profile(selected).nandSize == 0;
        nand->setEnabled(!none);
        if (auto* choose = findChild<QPushButton*>("nandChoose")) choose->setEnabled(!none);
        norLabel->setText(frondFirmware::runs68k(selected) ? tr("ROM image") : tr("NOR image"));
        const bool rx1950 = frondFirmware::profile(selected).board == DeviceBoard5::s3c2442Rx1950;
        nandLabel->setText(rx1950 ? tr("OS image (CEOS.nbf)") : tr("NAND image (optional for the E2)"));
    };
    connect(device, &QComboBox::currentIndexChanged, this, nandShown);
    nandShown();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto* import = buttons->addButton(tr("Import"), QDialogButtonBox::AcceptRole);
    import->setObjectName("import");
    import->setDefault(true);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(import, &QPushButton::clicked, this, [this] {
        const auto chosen = chosenCards();
        if (chosen.size() == 2 && !chosen[0].isEmpty() && chosen[0] == chosen[1])
            return void(QMessageBox::critical(this, tr("Frond"), tr("Pick a different card for each slot.")));
        const auto error = importImages();
        if (error.isEmpty()) {
            if (const auto warning = frondFirmware::importWarning(stored, nand->text()); !warning.isEmpty())
                QMessageBox::warning(this, tr("Frond"), warning);
            accept();
        } else QMessageBox::critical(this, tr("Frond"), error);
    });
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    if (initialSettings) {
        machineSettings = new MachineSettings(*initialSettings, automaticScale, this);
        machineSettings->setNewMachine();
        auto update = [this] {
            auto selected = frondFirmware::deviceFromId(device->currentData().toString());
            if (selected == Device::automatic) selected = frondFirmware::detectPicked(nor->text());
            machineSettings->setDevice(selected);
        };
        connect(device, &QComboBox::currentIndexChanged, this, update);
        connect(nor, &QLineEdit::textChanged, this, update);
        update();
        layout->addWidget(machineSettings);
    }
    layout->addWidget(buttons);
}

void FirmwareDialog::offerCards(const QString& folder) {
    sessions = folder;
    for (int n = 0; n < 2; ++n) {
        cards[n] = new QComboBox(this);
        cards[n]->setObjectName(n ? "card2Choice" : "card1Choice");
        form->addRow(n ? tr("Card 2") : tr("Card 1"), cards[n]);
    }
    connect(device, &QComboBox::currentIndexChanged, this, &FirmwareDialog::fillCards);
    connect(nor, &QLineEdit::textChanged, this, &FirmwareDialog::fillCards);
    connect(cards[0], &QComboBox::currentIndexChanged, this, &FirmwareDialog::fillCards);
    fillCards();
}
// No card, then the drawer cards slot n takes, by name; shown only for a two-slot machine. Card 2
// leaves out the card picked for card 1.
void FirmwareDialog::fillCards() {
    if (!cards[0]) return;
    auto selected = frondFirmware::deviceFromId(device->currentData().toString());
    if (selected == Device::automatic) selected = frondFirmware::detectPicked(nor->text());
    const bool two = frondCards::secondSlot(selected);
    QList<frondCards::Entry> drawer;
    if (two)
        for (const auto& e : frondCards::entries(frondCards::libraryFor(sessions)))
            if (e.inDrawer) drawer << e;
    std::sort(drawer.begin(), drawer.end(), [](const auto& a, const auto& b) { return a.card.name.localeAwareCompare(b.card.name) < 0; });
    for (int n = 0; n < 2; ++n) {
        const QSignalBlocker blocked(cards[n]);
        const auto keep = cards[n]->currentData().toString();
        const auto first = n ? cards[0]->currentData().toString() : QString();
        cards[n]->clear();
        cards[n]->addItem(tr("No card"), QString());
        for (const auto& e : drawer)
            if ((first.isEmpty() || e.card.id != first) &&
                frondCards::deviceTakes(selected, frondCards::takeKind(e.card), frondCards::sizeOf(e.card.kind, e.card.bytes), nullptr, n))
                cards[n]->addItem(e.card.name + QStringLiteral(" · ") + frondCards::cardSizeText(e.card), e.card.id);
        cards[n]->setCurrentIndex(std::max(0, cards[n]->findData(keep)));
        form->setRowVisible(cards[n], two);
    }
}
QStringList FirmwareDialog::chosenCards() const {
    if (!cards[0] || !cards[0]->isVisibleTo(this)) return {};
    return {cards[0]->currentData().toString(), cards[1]->currentData().toString()};
}
QString FirmwareDialog::importImages() {
    auto chosen = frondFirmware::deviceFromId(device->currentData().toString());
    const auto error = frondFirmware::importImages(root, chosen, nor->text(), nand->isEnabled() ? nand->text() : QString(), &chosen);
    if (error.isEmpty()) stored = chosen;
    return error;
}

namespace {
QString midiCore;
}
void setMidiPortsCore(const QString& core) { midiCore = core; }
void listMidiOutputPorts(QObject* context, std::function<void(const QStringList&)> done) {
    if (midiCore.isEmpty()) {
        done({});
        return;
    }
    auto* core = new QProcess(context);
    auto* timeout = new QTimer(core);
    timeout->setSingleShot(true);
    QObject::connect(timeout, &QTimer::timeout, core, [core] { core->kill(); });
    QObject::connect(core, &QProcess::finished, context, [core, done](int, QProcess::ExitStatus status) {
        QStringList ports;
        // "N: name" a line.
        if (status == QProcess::NormalExit)
            for (const auto& line : QString::fromUtf8(core->readAllStandardOutput()).split('\n')) {
                const int colon = line.indexOf(QStringLiteral(": "));
                bool number = false;
                if (colon > 0) line.left(colon).toUInt(&number);
                if (number) ports << line.mid(colon + 2).trimmed();
            }
        core->deleteLater();
        done(ports);
    });
    QObject::connect(core, &QProcess::errorOccurred, context, [core, done](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        core->deleteLater();
        done({});
    });
    core->start(midiCore, {"--music-midi-ports"});
    timeout->start(5000);
}

QString midiPortKey(const QString& name) {
    static const QRegularExpression clientPort(QStringLiteral("\\s+\\d+:\\d+$"));
    static const QRegularExpression bracketed(QStringLiteral("\\s*\\(\\d+(:\\d+)?\\)"));
    QString key = name;
    key.remove(clientPort);
    key.remove(bracketed);
    return key.trimmed().toLower();
}

MachineSettings::MachineSettings(const frondSessions::Preferences& prefs, bool automaticScale, QWidget* parent)
    : QWidget(parent), automaticScale(automaticScale), originalPreferences(prefs), controllerPrefs(prefs) {
    auto* layout = new QVBoxLayout(this);
    auto* display = new QGroupBox(tr("Display"), this);
    auto* displayLayout = new QVBoxLayout(display);
    scaleLabel = new QLabel(display);
    windowScale = new QSlider(Qt::Horizontal, display);
    windowScale->setObjectName("windowScale");
    windowScale->setRange(4, 8);
    windowScale->setTickInterval(1);
    windowScale->setTickPosition(QSlider::TicksBelow);
    windowScale->setToolTip(tr("1× to 2× in 0.25 steps."));
    smoothLcd = new QCheckBox(tr("Smooth pixels"), display);
    smoothLcd->setObjectName("smoothLcd");
    showMips = new QCheckBox(tr("Show speed"), display);
    showMips->setObjectName("showMips");
    contrastLabel = new QLabel(display);
    contrast = new QSlider(Qt::Horizontal, display);
    contrast->setObjectName("contrast");
    contrast->setRange(0, 100);
    contrast->setPageStep(10);
    contrastLabel->setBuddy(contrast);

    showKeyboard = new QCheckBox(tr("Show keyboard"), display);
    showKeyboard->setObjectName("showKeyboard");
    jogControlsLabel = new QLabel(tr("Jog controls"), display);
    jogControls = new QComboBox(display);
    jogControls->setObjectName("jogControls");
    for (auto jog : {frondSessions::JogControls::hidden, frondSessions::JogControls::screen, frondSessions::JogControls::keyboard})
        jogControls->addItem(frondSessions::jogControlsLabel(jog, false), int(jog));
    jogControlsLabel->setBuddy(jogControls);
    showKeyboard->setChecked(prefs.showKeyboard);
    jogControls->setCurrentIndex(jogControls->findData(int(prefs.jogControls)));
    jogLandscapeLabel = new QLabel(tr("Jog in landscape"), display);
    jogLandscape = new QComboBox(display);
    jogLandscape->setObjectName("jogLandscape");
    for (auto jog : {frondSessions::JogLandscape::hidden, frondSessions::JogLandscape::beside,
                     frondSessions::JogLandscape::under, frondSessions::JogLandscape::over})
        jogLandscape->addItem(frondSessions::jogLandscapeLabel(jog), int(jog));
    jogLandscapeLabel->setBuddy(jogLandscape);
    jogLandscape->setCurrentIndex(jogLandscape->findData(int(prefs.jogLandscape)));
    danaView = new DanaViewControls(display);
    danaView->setValues(prefs.danaView);
    displayLayout->addWidget(danaView);
    displayLayout->addWidget(scaleLabel);
    displayLayout->addWidget(windowScale);
    displayLayout->addWidget(smoothLcd);
    displayLayout->addWidget(showMips);
    displayLayout->addWidget(contrastLabel);
    displayLayout->addWidget(contrast);
    displayLayout->addWidget(showKeyboard);
    auto* jogRow = new QHBoxLayout;
    jogRow->addWidget(jogControlsLabel);
    jogRow->addWidget(jogControls, 1);
    displayLayout->addLayout(jogRow);
    auto* jogLandscapeRow = new QHBoxLayout;
    jogLandscapeRow->addWidget(jogLandscapeLabel);
    jogLandscapeRow->addWidget(jogLandscape, 1);
    displayLayout->addLayout(jogLandscapeRow);
    layout->addWidget(display);
    auto* sound = new QGroupBox(tr("Audio"), this);
    auto* soundLayout = new QVBoxLayout(sound);
    audio = new QCheckBox(tr("Sound"), sound);
    audio->setObjectName("audio");
    fastAudio = new QCheckBox(tr("Low-latency audio"), sound);
    fastAudio->setObjectName("fastAudio");
    soundLayout->addWidget(audio);
    soundLayout->addWidget(fastAudio);
    rumble = new QCheckBox(tr("Rumble"), sound);
    rumble->setObjectName("rumble");
    soundLayout->addWidget(rumble);
    musicOutputLabel = new QLabel(tr("Music output"), sound);
    musicOutput = new QComboBox(sound);
    musicOutput->setObjectName("musicOutput");
    musicOutput->addItem(tr("Built-in FM Synth"), "fm");
    musicOutput->addItem(tr("SoundFont"), "soundfont");
    musicOutput->addItem(tr("MIDI device"), "midi");
    musicOutput->setCurrentIndex(std::max(0, musicOutput->findData(prefs.musicOutput)));
    musicOutputLabel->setBuddy(musicOutput);
    auto* musicRow = new QHBoxLayout;
    musicRow->addWidget(musicOutputLabel);
    musicRow->addWidget(musicOutput, 1);
    soundLayout->addLayout(musicRow);
    soundFontRow = new QWidget(sound);
    auto* soundFontLayout = new QHBoxLayout(soundFontRow);
    soundFontLayout->setContentsMargins(0, 0, 0, 0);
    soundFont = new QLineEdit(prefs.soundFont, soundFontRow);
    soundFont->setObjectName("soundFont");
    soundFont->setPlaceholderText(tr("SoundFont file (.sf2)"));
    soundFontChoose = new QPushButton(tr("Choose…"), soundFontRow);
    soundFontChoose->setObjectName("soundFontChoose");
    soundFontLayout->addWidget(soundFont, 1);
    soundFontLayout->addWidget(soundFontChoose);
    soundLayout->addWidget(soundFontRow);
    connect(soundFontChoose, &QPushButton::clicked, this, [this] {
        const auto file = QFileDialog::getOpenFileName(this, tr("Choose a SoundFont"), QFileInfo(soundFont->text()).absolutePath(),
                                                       tr("SoundFont (*.sf2);;All files (*)"));
        if (!file.isEmpty()) soundFont->setText(file);
    });
    midiPortRow = new QWidget(sound);
    auto* midiLayout = new QHBoxLayout(midiPortRow);
    midiLayout->setContentsMargins(0, 0, 0, 0);
    midiPort = new QComboBox(midiPortRow);
    midiPort->setObjectName("midiPort");
    // Filled from the core when MIDI device is first shown; the saved port stays chosen meanwhile.
    if (!prefs.midiOutPort.isEmpty()) midiPort->addItem(prefs.midiOutPort == "virtual" ? tr("Frond's own port") : prefs.midiOutPort, prefs.midiOutPort);
    midiLayout->addWidget(midiPort, 1);
    soundLayout->addWidget(midiPortRow);
    connect(musicOutput, &QComboBox::currentIndexChanged, this, [this] { showMusic(); });
    showMusic();
    layout->addWidget(sound);
    controllerGroup = new QGroupBox(tr("Game controller"), this);
    auto* controllerLayout = new QHBoxLayout(controllerGroup);
    auto* controls = new QPushButton(tr("Controls…"), controllerGroup);
    controls->setObjectName("machineControls");
    controllerLayout->addWidget(controls);
    controllerLayout->addStretch();
    connect(controls, &QPushButton::clicked, this, [this] {
        if (device == Device::automatic) return;
        const auto& profile = frondFirmware::profile(device);
        ControlsDialog dialog(profile, controllerMapFor(controllerPrefs, profile), this);
        connect(&dialog, &ControlsDialog::changed, this, [this](const ControllerMap& map, bool defaults) {
            controllerPrefs.controllerMapSet = !defaults;
            controllerPrefs.controllerMap = defaults ? QString() : QString::fromStdString(map.encode());
        });
        dialog.exec();
    });
    layout->addWidget(controllerGroup);
    auto* batteryGroup = new QGroupBox(tr("Battery"), this);
    auto* batteryLayout = new QVBoxLayout(batteryGroup);
    batteryMode = new QComboBox(batteryGroup);
    batteryMode->setObjectName("batteryMode");
    batteryMode->addItem(tr("Maximum"), "max");
    batteryMode->addItem(tr("Set level"), "level");
    batteryMode->addItem(tr("Host battery"), "host");
    batteryMode->setCurrentIndex(std::max(0, batteryMode->findData(prefs.batteryMode)));
    batteryLevelLabel = new QLabel(batteryGroup);
    batteryLevel = new QSlider(Qt::Horizontal, batteryGroup);
    batteryLevel->setObjectName("batteryLevel");
    batteryLevel->setRange(0, 100);
    batteryLevel->setPageStep(10);
    batteryLevel->setValue(prefs.batteryLevel);
    batteryCharging = new QCheckBox(tr("Charging"), batteryGroup);
    batteryCharging->setObjectName("batteryCharging");
    batteryCharging->setChecked(prefs.batteryCharging);
    batteryLayout->addWidget(batteryMode);
    batteryLayout->addWidget(batteryLevelLabel);
    batteryLayout->addWidget(batteryLevel);
    batteryLayout->addWidget(batteryCharging);
    auto batteryShow = [this] {
        const auto mode = batteryMode->currentData().toString();
        batteryLevelLabel->setText(tr("Level: %1").arg(batteryLevel->value()));
        batteryLevelLabel->setVisible(mode == "level");
        batteryLevel->setVisible(mode == "level");
        batteryCharging->setVisible(mode != "host");
    };
    connect(batteryMode, &QComboBox::currentIndexChanged, this, batteryShow);
    connect(batteryLevel, &QSlider::valueChanged, this, batteryShow);
    batteryShow();
    layout->addWidget(batteryGroup);
    auto* speedBox = new QGroupBox(tr("Speed"), this);
    speedGroup = speedBox;
    auto* speedLayout = new QVBoxLayout(speedBox);
    auto* speedRow = new QHBoxLayout;
    speedLabel = new QLabel(tr("Speed"), speedBox);
    speed = new QComboBox(speedBox);
    speed->setObjectName("speed");
    chosenSpeed = frondSessions::speedName(prefs);
    fillSpeedBox(speed, false, chosenSpeed);
    connect(speed, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { chosenSpeed = speed->currentData().toString(); });
    speedLabel->setBuddy(speed);
    speedRow->addWidget(speedLabel);
    speedRow->addWidget(speed, 1);
    speedLayout->addLayout(speedRow);
    auto* engineRow = new QHBoxLayout;
    engineLabel = new QLabel(tr("Engine"), speedBox);
    engine = new QComboBox(speedBox);
    engine->setObjectName("engine");
    engine->addItem(tr("Exact"), false);
    engine->addItem(tr("Fast"), true);
    engine->setCurrentIndex(prefs.fastEngine ? 1 : 0);
    engineLabel->setBuddy(engine);
    engineRow->addWidget(engineLabel);
    engineRow->addWidget(engine, 1);
    speedLayout->addLayout(engineRow);
    layout->addWidget(speedBox);
    peripherals = new QGroupBox(tr("Connections"), this);
    auto* peripheralsLayout = new QGridLayout(peripherals);
    // One row per peripheral: the local choices, then the connections, and a port for Wait for a peer.
    auto row = [&](int at, const QString& title, const QString& name, const QString& value, int port,
                   QLabel*& label, QComboBox*& combo, QSpinBox*& spin) {
        label = new QLabel(title, peripherals);
        combo = new QComboBox(peripherals);
        combo->setObjectName(name);
        combo->addItem(tr("Off"), "off");
        combo->addItem(tr("Terminal (PTY)"), "pty");
        combo->addItem(tr("Wait for a peer"), "tcp");
        frondLink::addConnectionChoices(combo, value);
        spin = new QSpinBox(peripherals);
        spin->setObjectName(name + "Port");
        spin->setRange(1, 65535);
        spin->setPrefix(tr("Port "));
        spin->setValue(value.startsWith("tcp:") ? value.mid(4).toInt() : port);
        combo->setCurrentIndex(std::max(0, combo->findData(value.startsWith("tcp:") ? QString("tcp") : value)));
        label->setBuddy(combo);
        peripheralsLayout->addWidget(label, at, 0);
        peripheralsLayout->addWidget(combo, at, 1);
        peripheralsLayout->addWidget(spin, at, 2);
        auto* box = combo; auto* field = spin;
        connect(combo, &QComboBox::currentIndexChanged, this, [box, field] { field->setVisible(box->currentData() == "tcp"); });
        spin->setVisible(combo->currentData() == "tcp");
    };
    row(0, tr("Infrared"), "machineInfrared", prefs.infrared, 5030, infraredLabel, infrared, infraredPort);
    row(1, tr("Serial"), "machineSerial", prefs.serial, 5023, serialLabel, serial, serialPort);
    infrared->insertItem(infrared->findData("tcp") + 1, tr("Printer"), "printer");
    infrared->setCurrentIndex(std::max(0, infrared->findData(prefs.infrared.startsWith("tcp:") ? QString("tcp") : prefs.infrared)));
    if (prefs.infrared == "phone") { frondLink::setPhoneChoice(infrared, true); infrared->setCurrentIndex(infrared->findData("phone")); }
    if (prefs.serial == "modem") { frondLink::setModemChoice(serial, true); serial->setCurrentIndex(serial->findData("modem")); }
    bluetoothLabel = new QLabel(tr("Bluetooth"), peripherals);
    bluetooth = new QComboBox(peripherals);
    bluetooth->setObjectName("machineBluetooth");
    bluetooth->addItem(tr("Off"), "off");
    bluetooth->addItem(tr("This Frond"), "radio");
    bluetooth->addItem(tr("Mobile phone"), "phone");
    bluetooth->addItem(tr("Controller socket"), "socket");
    frondLink::addConnectionChoices(bluetooth, prefs.bluetooth, false);
    bluetooth->setCurrentIndex(std::max(0, bluetooth->findData(prefs.bluetooth.startsWith("socket:") ? QString("socket") : prefs.bluetooth)));
    bluetoothSocket = new QLineEdit(prefs.bluetooth.startsWith("socket:") ? prefs.bluetooth.mid(7) : QString(), peripherals);
    bluetoothSocket->setObjectName("machineBluetoothSocket");
    bluetoothSocket->setPlaceholderText(tr("Absolute controller socket path"));
    bluetoothLabel->setBuddy(bluetooth);
    peripheralsLayout->addWidget(bluetoothLabel, 2, 0);
    peripheralsLayout->addWidget(bluetooth, 2, 1);
    peripheralsLayout->addWidget(bluetoothSocket, 3, 1, 1, 2);
    connect(bluetooth, &QComboBox::currentIndexChanged, this, [this] { bluetoothSocket->setVisible(bluetooth->currentData() == "socket"); });
    bluetoothSocket->setVisible(bluetooth->currentData() == "socket");
    peripheralsLayout->setColumnStretch(1, 1);
    layout->addWidget(peripherals);
    peripherals->setVisible(false);
    contrast->setValue(prefs.contrast);
    contrastLabel->setText(tr("Contrast: %1").arg(prefs.contrast));
    connect(contrast, &QSlider::valueChanged, this, [this](int value) { contrastLabel->setText(tr("Contrast: %1").arg(value)); });
    // setDevice shows the rows for the chosen device.
    for (QWidget* w : {static_cast<QWidget*>(contrastLabel), static_cast<QWidget*>(contrast),
                       static_cast<QWidget*>(speedLabel), static_cast<QWidget*>(speed)}) w->setVisible(false);
    speedGroup->setVisible(true);
    windowScale->setValue(prefs.scale);
    smoothLcd->setChecked(prefs.smooth);
    showMips->setChecked(prefs.showMips);
    audio->setChecked(prefs.audio);
    fastAudio->setChecked(prefs.fastAudio);
    rumble->setChecked(prefs.rumble);
    connect(windowScale, &QSlider::valueChanged, this, [this](int value) {
        this->automaticScale = false;
        scaleLabel->setText(tr("Size: %1×").arg(value / 4.0));
    });
    scaleLabel->setText(tr("Size: %1×").arg(windowScale->value() / 4.0));
}

// Music output and its file or port, on the Zodiac only. The ports are listed the first time MIDI device shows.
void MachineSettings::showMusic() {
    const bool zodiac = device != Device::automatic && frondFirmware::profile(device).board == DeviceBoard5::imx1Zodiac;
    const auto choice = musicOutput->currentData().toString();
    musicOutputLabel->setVisible(zodiac);
    musicOutput->setVisible(zodiac);
    soundFontRow->setVisible(zodiac && choice == "soundfont");
    midiPortRow->setVisible(zodiac && choice == "midi");
    if (!zodiac || choice != "midi" || midiPortsListed) return;
    midiPortsListed = true;
    listMidiOutputPorts(this, [this](const QStringList& ports) {
        const auto keep = midiPort->currentData().toString();
        const QSignalBlocker blocked(midiPort);
        midiPort->clear();
#ifndef Q_OS_WIN
        midiPort->addItem(tr("Frond's own port"), "virtual");
#endif
        for (const auto& port : ports) midiPort->addItem(port, port);
        int index = keep.isEmpty() ? 0 : midiPort->findData(keep);
        // The same port under new client numbers.
        if (index < 0)
            for (int i = 0; i < midiPort->count() && index < 0; ++i)
                if (midiPortKey(midiPort->itemData(i).toString()) == midiPortKey(keep)) index = i;
        // A saved port that isn't there now stays, so the choice isn't lost while the device is unplugged.
        if (index < 0) {
            midiPort->addItem(tr("%1 (not found)").arg(keep), keep);
            index = midiPort->count() - 1;
        }
        midiPort->setCurrentIndex(index);
    });
}

void fillSpeedBox(QComboBox* box, bool m68k, const QString& name) {
    const QSignalBlocker blocked(box);
    box->clear();
    for (const auto& choice : frondSessions::speedChoices(m68k)) box->addItem(frondSessions::speedLabel(choice), choice);
    box->setCurrentIndex(std::max(0, box->findData(m68k && name == QLatin1String("8x") ? QStringLiteral("uncapped") : name)));
}

int fittingScaleQuarters(Device device, bool keyboard, frondSessions::JogControls jog) {
    const auto& display = frondFirmware::profile(device).frontend;
    int width = display.width + display.sideTabWidth, height = display.height + display.silkscreenHeight + display.stripHeight;
    if (device == frondFirmware::deviceFromId("sony-clie-tg50")) {
        // The TG50 sizes the core lays out: the jog tab adds 38 across unless it is hidden or beside the keyboard.
        const bool hidden = jog == frondSessions::JogControls::hidden, beside = keyboard && jog == frondSessions::JogControls::keyboard;
        width = hidden || beside ? 320 : 358;
        height = keyboard ? (beside ? 518 : hidden ? 544 : 571) : (hidden ? 368 : 374);
    } else if (frondSessions::jogBesideButtons(display)) {
        // Hidden takes the tab off; beside the buttons the LCD spans the width and the silkscreen
        // and strip shrink beside the tab, rounded as the core does: the silkscreen to whole rows,
        // the strip up.
        if (jog == frondSessions::JogControls::hidden) width = display.width;
        if (jog == frondSessions::JogControls::keyboard) {
            const double scale = double(display.width - display.sideTabWidth) / display.width;
            width = display.width;
            height = display.height + int(std::lround(display.silkscreenHeight * scale)) + int(std::ceil(display.stripHeight * scale));
        }
    }
    // A 160x160 68k device is drawn at twice its size at 1x, the same width as a 320-pixel ARM screen; a 320-wide one is not.
    if (frondFirmware::runs68k(device) && display.width == 160) width *= 2, height *= 2;
    const auto* screen = QGuiApplication::primaryScreen();
    const auto available = screen ? screen->availableGeometry().size() : QSize();
    return int(4 * fittingWindowScale(width, height, available.width(), available.height()));
}

frondSessions::Preferences MachineSettings::values() const {
    auto prefs = originalPreferences;
    prefs.danaView = danaView->values();
    prefs.scale = windowScale->value();
    prefs.smooth = smoothLcd->isChecked();
    prefs.showMips = showMips->isChecked();
    prefs.batteryMode = batteryMode->currentData().toString();
    prefs.batteryLevel = batteryLevel->value();
    prefs.batteryCharging = batteryCharging->isChecked();
    prefs.audio = audio->isChecked();
    prefs.fastAudio = fastAudio->isChecked();
    prefs.rumble = rumble->isChecked();
    prefs.controllerMapSet = controllerPrefs.controllerMapSet;
    prefs.controllerMap = controllerPrefs.controllerMap;
    if (device != Device::automatic && frondFirmware::profile(device).board == DeviceBoard5::imx1Zodiac) {
        prefs.musicOutput = musicOutput->currentData().toString();
        prefs.soundFont = soundFont->text().trimmed();
        prefs.midiOutPort = midiPort->currentData().toString();
    }
    prefs.showKeyboard = showKeyboard->isChecked();
    prefs.jogControls = frondSessions::JogControls(jogControls->currentData().toInt());
    prefs.jogLandscape = frondSessions::JogLandscape(jogLandscape->currentData().toInt());
    prefs.fastEngine = engine->currentData().toBool();
    if (contrast->value() != prefs.contrast) prefs.contrastSet = true;
    prefs.contrast = contrast->value();
    frondSessions::setSpeed(&prefs, speed->currentData().toString());
    const auto choice = [](QComboBox* combo, QSpinBox* port) {
        const auto mode = combo->currentData().toString();
        return mode == "tcp" ? "tcp:" + QString::number(port->value()) : mode;
    };
    if (peripheralsShown && device != Device::automatic && frondFirmware::capabilities(device).infrared) {
        prefs.infrared = choice(infrared, infraredPort);
        prefs.peripheralsSet = true;
    }
    if (peripheralsShown && device != Device::automatic && frondFirmware::capabilities(device).serial &&
        !frondFirmware::serialAbsent(device)) {
        prefs.serial = choice(serial, serialPort);
        prefs.peripheralsSet = true;
    }
    if (peripheralsShown && device != Device::automatic && frondFirmware::capabilities(device).bluetooth) {
        const auto mode = bluetooth->currentData().toString();
        const auto path = bluetoothSocket->text().trimmed();
        prefs.bluetooth = mode == "socket" ? (path.isEmpty() ? QStringLiteral("off") : "socket:" + path) : mode;
        prefs.peripheralsSet = true;
    }
    return prefs;
}
void MachineSettings::setNewMachine() {
    automaticEngine = true;
    connect(engine, &QComboBox::activated, this, [this] { automaticEngine = false; });
    setDevice(device);
}
void MachineSettings::showPeripherals(bool shown) {
    peripheralsShown = shown;
    setDevice(device);
}
void MachineSettings::setDevice(Device device) {
    // A map made for another device starts again from the new one's defaults.
    if (device != this->device && this->device != Device::automatic) {
        controllerPrefs.controllerMapSet = false;
        controllerPrefs.controllerMap.clear();
    }
    this->device = device;
    controllerGroup->setVisible(device != Device::automatic && controlsShown(frondFirmware::profile(device)));
    const bool known = peripheralsShown && device != Device::automatic;
    const bool hasInfrared = known && frondFirmware::capabilities(device).infrared;
    const bool hasSerial = known && frondFirmware::capabilities(device).serial && !frondFirmware::serialAbsent(device);
    const bool hasBluetooth = known && frondFirmware::capabilities(device).bluetooth;
    peripherals->setVisible(hasInfrared || hasSerial || hasBluetooth);
    for (QWidget* w : {static_cast<QWidget*>(bluetoothLabel), static_cast<QWidget*>(bluetooth)}) w->setVisible(hasBluetooth);
    bluetoothSocket->setVisible(hasBluetooth && bluetooth->currentData() == "socket");
    for (QWidget* w : {static_cast<QWidget*>(infraredLabel), static_cast<QWidget*>(infrared)}) w->setVisible(hasInfrared);
    infraredPort->setVisible(hasInfrared && infrared->currentData() == "tcp");
    {
        const QSignalBlocker blocked(infrared);
        const auto mode = infrared->currentData();
        frondLink::setPhoneChoice(infrared, known && frondFirmware::capabilities(device).irPhone);
        if (const int at = infrared->findData(mode); at >= 0) infrared->setCurrentIndex(at);
    }
    {
        const QSignalBlocker blocked(serial);
        const auto mode = serial->currentData();
        frondLink::setModemChoice(serial, known && frondFirmware::capabilities(device).modem);
        if (const int at = serial->findData(mode); at >= 0) serial->setCurrentIndex(at);
    }
    for (QWidget* w : {static_cast<QWidget*>(serialLabel), static_cast<QWidget*>(serial)}) w->setVisible(hasSerial);
    serialPort->setVisible(hasSerial && serial->currentData() == "tcp");
    const bool dana = frondFirmware::isDana(device);
    danaView->setVisible(dana);
    windowScale->setVisible(!dana); scaleLabel->setVisible(!dana);
    const bool tg50 = device == frondFirmware::deviceFromId("sony-clie-tg50");
    const auto& frontend = frondFirmware::profile(device).frontend;
    showKeyboard->setVisible(tg50);
    jogControlsLabel->setVisible(frondSessions::jogControlsShown(frontend));
    jogControls->setVisible(frondSessions::jogControlsShown(frontend));
    jogLandscapeLabel->setVisible(frondSessions::jogLandscapeShown(frontend));
    jogLandscape->setVisible(frondSessions::jogLandscapeShown(frontend));
    jogControls->setItemText(jogControls->findData(int(frondSessions::JogControls::keyboard)),
        frondSessions::jogControlsLabel(frondSessions::JogControls::keyboard, frondSessions::jogBesideButtons(frontend)));
    const bool m68k = frondFirmware::runs68k(device);
    fastAudio->setVisible(!m68k);
    rumble->setText(frondFirmware::profile(device).board == DeviceBoard5::imx1Zodiac ? tr("Rumble") : tr("Vibrate"));
    rumble->setVisible(known && frondFirmware::capabilities(device).vibrate);
    showMusic();
    showMips->setVisible(!m68k);
    contrastLabel->setVisible(frondFirmware::lcdContrast(device));
    contrast->setVisible(frondFirmware::lcdContrast(device));
    // Speed for every device, without 8x on a 68k; the Engine choice has no JIT to pick on a 68k.
    const bool engineShown = !m68k;
    fillSpeedBox(speed, m68k, chosenSpeed);
    speedLabel->setVisible(true);
    speed->setVisible(true);
    engineLabel->setVisible(engineShown);
    engine->setVisible(engineShown);
    if (automaticEngine && device != Device::automatic) {
        const QSignalBlocker blocked(engine);
        engine->setCurrentIndex(frondFirmware::fastByDefault(device) ? 1 : 0);
    }
    speedGroup->setVisible(true);
    const bool soundSupported = device != Device::automatic && frondFirmware::capabilities(device).sound;
    audio->setEnabled(soundSupported);
    audio->setToolTip(soundSupported ? QString() : device == Device::automatic ? tr("Choose firmware first") :
        tr("Not available on the %1 yet").arg(frondFirmware::deviceName(device)));
    const bool supported = frondAudio::lowLatencyAvailable(device);
    fastAudio->setEnabled(supported);
    fastAudio->setToolTip(supported ? QString() : device == Device::automatic ? tr("Choose firmware first") :
        tr("Not available on the %1 yet").arg(frondFirmware::deviceName(device)));
    if (automaticScale && device != Device::automatic) {
        const QSignalBlocker blocked(windowScale);
        windowScale->setValue(fittingScaleQuarters(device, showKeyboard->isChecked(),
            frondSessions::JogControls(jogControls->currentData().toInt())));
        scaleLabel->setText(tr("Size: %1×").arg(windowScale->value() / 4.0));
    }
}

MachineSettingsDialog::MachineSettingsDialog(Device device, const frondSessions::Preferences& prefs, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Edit settings"));
    setObjectName("machineSettingsDialog");
    auto* layout = new QVBoxLayout(this);
    controls = new MachineSettings(prefs, false, this);
    controls->setDevice(device);
    controls->showPeripherals(true);
    layout->addWidget(controls);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Settings"));
    setObjectName("settingsDialog");
    auto* layout = new QVBoxLayout(this);
    resume = new QCheckBox(tr("Resume last session on startup"), this);
    resume->setObjectName("resume");
    layout->addWidget(resume);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);
}
