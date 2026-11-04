#include "Dialogs.h"
#include "FirmwareStore.h"
#include "AudioQualification.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <QGuiApplication>
#include <QScreen>
#include <QSignalBlocker>
#include "WindowScale.h"

using frondFirmware::Device;

FirmwareDialog::FirmwareDialog(const QString& root, Device preselect, QWidget* parent,
                               const frondSessions::Preferences* initialSettings, bool automaticScale) : QDialog(parent), root(root) {
    setWindowTitle(tr("Import firmware"));
    setObjectName("firmwareDialog");
    auto* form = new QFormLayout;
    device = new QComboBox(this);
    device->setObjectName("device");
    // Items hold the device's fixed name; empty detects.
    device->addItem(tr("Detect from the NOR image"), QString());
    for (auto d : frondFirmware::devices) device->addItem(frondFirmware::deviceName(d), frondFirmware::deviceId(d));
    device->setCurrentIndex(std::max(0, device->findData(frondFirmware::deviceId(preselect))));
    form->addRow(tr("Device"), device);
    auto field = [&](const QString& label, const QString& key) {
        auto* row = new QHBoxLayout;
        auto* edit = new QLineEdit(this);
        edit->setObjectName(key);
        edit->setMinimumWidth(280);
        auto* choose = new QPushButton(tr("Choose…"), this);
        choose->setObjectName(key + "Choose");
        row->addWidget(edit, 1); row->addWidget(choose);
        connect(choose, &QPushButton::clicked, this, [this, edit, label] {
            const auto file = QFileDialog::getOpenFileName(this, label, edit->text(), tr("Firmware images (*.bin *.rom);;All files (*)"));
            if (!file.isEmpty()) edit->setText(file);
        });
        form->addRow(label, row);
        return edit;
    };
    nor = field(tr("NOR image"), "nor");
    nand = field(tr("NAND image (optional for the E2)"), "nand");
    // The Zire 72 has no NAND.
    auto nandShown = [this] {
        const bool none = frondFirmware::profile(frondFirmware::deviceFromId(device->currentData().toString())).nandSize == 0;
        nand->setEnabled(!none);
        if (auto* choose = findChild<QPushButton*>("nandChoose")) choose->setEnabled(!none);
    };
    connect(device, &QComboBox::currentIndexChanged, this, nandShown);
    nandShown();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto* import = buttons->addButton(tr("Import"), QDialogButtonBox::AcceptRole);
    import->setObjectName("import");
    import->setDefault(true);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(import, &QPushButton::clicked, this, [this] {
        const auto error = importImages();
        if (error.isEmpty()) accept();
        else QMessageBox::critical(this, tr("Frond"), error);
    });
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    if (initialSettings) {
        machineSettings = new MachineSettings(*initialSettings, automaticScale, this);
        auto update = [this] {
            auto selected = frondFirmware::deviceFromId(device->currentData().toString());
            if (selected == Device::automatic) selected = frondFirmware::detectDevice(nor->text());
            machineSettings->setDevice(selected, (frondAudio::qualified(selected, {}, nor->text()) || frondAudio::sonyPreviewEligible(selected, {}, nor->text())));
        };
        connect(device, &QComboBox::currentIndexChanged, this, update);
        connect(nor, &QLineEdit::textChanged, this, update);
        update();
        layout->addWidget(machineSettings);
    }
    layout->addWidget(buttons);
}

QString FirmwareDialog::importImages() {
    auto chosen = frondFirmware::deviceFromId(device->currentData().toString());
    const auto error = frondFirmware::importImages(root, chosen, nor->text(), nand->isEnabled() ? nand->text() : QString(), &chosen);
    if (error.isEmpty()) stored = chosen;
    return error;
}

MachineSettings::MachineSettings(const frondSessions::Preferences& prefs, bool automaticScale, QWidget* parent)
    : QWidget(parent), automaticScale(automaticScale), originalPreferences(prefs) {
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

    showKeyboard = new QCheckBox(tr("Show keyboard"), display);
    showKeyboard->setObjectName("showKeyboard");
    jogControlsLabel = new QLabel(tr("Jog controls"), display);
    jogControls = new QComboBox(display);
    jogControls->setObjectName("jogControls");
    jogControls->addItem(tr("Hidden"), int(frondSessions::JogControls::hidden));
    jogControls->addItem(tr("Left of screen"), int(frondSessions::JogControls::screen));
    jogControls->addItem(tr("Left of keyboard"), int(frondSessions::JogControls::keyboard));
    jogControlsLabel->setBuddy(jogControls);
    showKeyboard->setChecked(prefs.showKeyboard);
    jogControls->setCurrentIndex(jogControls->findData(int(prefs.jogControls)));
    displayLayout->addWidget(scaleLabel);
    displayLayout->addWidget(windowScale);
    displayLayout->addWidget(smoothLcd);
    displayLayout->addWidget(showKeyboard);
    auto* jogRow = new QHBoxLayout;
    jogRow->addWidget(jogControlsLabel);
    jogRow->addWidget(jogControls, 1);
    displayLayout->addLayout(jogRow);
    layout->addWidget(display);
    auto* sound = new QGroupBox(tr("Audio"), this);
    auto* soundLayout = new QVBoxLayout(sound);
    audio = new QCheckBox(tr("Sound"), sound);
    audio->setObjectName("audio");
    fastAudio = new QCheckBox(tr("Low-latency audio"), sound);
    fastAudio->setObjectName("fastAudio");
    soundLayout->addWidget(audio);
    soundLayout->addWidget(fastAudio);
    sonyAudioPreview = new QCheckBox(tr("Sony audio preview"), sound);
    sonyAudioPreview->setObjectName("sonyAudioPreview");
    soundLayout->addWidget(sonyAudioPreview);
    layout->addWidget(sound);
    auto* speed = new QGroupBox(tr("Speed"), this);
    auto* engineRow = new QHBoxLayout(speed);
    auto* engineLabel = new QLabel(tr("Engine"), speed);
    engine = new QComboBox(speed);
    engine->setObjectName("engine");
    engine->addItem(tr("Exact"), false);
    engine->addItem(tr("Fast"), true);
    engine->setCurrentIndex(prefs.fastEngine ? 1 : 0);
    engineLabel->setBuddy(engine);
    engineRow->addWidget(engineLabel);
    engineRow->addWidget(engine, 1);
    layout->addWidget(speed);
    speed->setVisible(qEnvironmentVariable("FROND_ENGINE_SETTING") == QLatin1String("1"));
    windowScale->setValue(prefs.scale);
    smoothLcd->setChecked(prefs.smooth);
    audio->setChecked(prefs.audio);
    fastAudio->setChecked(prefs.fastAudio);
    sonyAudioPreview->setChecked(prefs.sonyAudioPreview);
    connect(windowScale, &QSlider::valueChanged, this, [this](int value) {
        this->automaticScale = false;
        scaleLabel->setText(tr("Size: %1×").arg(value / 4.0));
    });
    scaleLabel->setText(tr("Size: %1×").arg(windowScale->value() / 4.0));
}

int fittingScaleQuarters(Device device, bool keyboard, frondSessions::JogControls jog) {
    const auto& display = frondFirmware::profile(device).frontend;
    int width = display.width + display.sideTabWidth, height = display.height + display.silkscreenHeight + display.stripHeight;
    if (device == frondFirmware::deviceFromId("sony-clie-tg50")) {
        // The TG50 sizes the core lays out: the jog tab adds 38 across unless it is hidden or beside the keyboard.
        const bool hidden = jog == frondSessions::JogControls::hidden, beside = keyboard && jog == frondSessions::JogControls::keyboard;
        width = hidden || beside ? 320 : 358;
        height = keyboard ? (beside ? 518 : hidden ? 544 : 571) : (hidden ? 368 : 374);
    }
    const auto* screen = QGuiApplication::primaryScreen();
    const auto available = screen ? screen->availableGeometry().size() : QSize();
    return int(4 * fittingWindowScale(width, height, available.width(), available.height()));
}

frondSessions::Preferences MachineSettings::values() const {
    auto prefs = originalPreferences;
    prefs.scale = windowScale->value();
    prefs.smooth = smoothLcd->isChecked();
    prefs.audio = audio->isChecked();
    prefs.fastAudio = fastAudio->isChecked();
    prefs.sonyAudioPreview = sonyAudioPreview->isChecked();
    prefs.showKeyboard = showKeyboard->isChecked();
    prefs.jogControls = frondSessions::JogControls(jogControls->currentData().toInt());
    prefs.fastEngine = engine->currentData().toBool();
    return prefs;
}
void MachineSettings::setDevice(Device device, bool qualified) {
    const bool tg50 = device == frondFirmware::deviceFromId("sony-clie-tg50");
    showKeyboard->setVisible(tg50);
    jogControlsLabel->setVisible(tg50);
    jogControls->setVisible(tg50);
    fastAudio->setVisible(!tg50);
    sonyAudioPreview->setVisible(tg50);
    sonyAudioPreview->setEnabled(tg50 && qualified);
    const bool soundSupported = device != Device::automatic && frondFirmware::capabilities(device).sound;
    audio->setEnabled(soundSupported);
    audio->setToolTip(soundSupported ? QString() : device == Device::automatic ? tr("Choose firmware first") :
        tr("Not available on the %1 yet").arg(frondFirmware::deviceName(device)));
    const bool supported = soundSupported && frondFirmware::capabilities(device).fastAudio && qualified;
    fastAudio->setEnabled(supported);
    fastAudio->setToolTip(supported ? QString() : device == Device::automatic ? tr("Choose firmware first") :
        frondFirmware::capabilities(device).fastAudio ? tr("Requires qualified firmware") :
        tr("Not available on the %1 yet").arg(frondFirmware::deviceName(device)));
    if (automaticScale && device != Device::automatic) {
        const QSignalBlocker blocked(windowScale);
        windowScale->setValue(fittingScaleQuarters(device, showKeyboard->isChecked(),
            frondSessions::JogControls(jogControls->currentData().toInt())));
        scaleLabel->setText(tr("Size: %1×").arg(windowScale->value() / 4.0));
    }
}

MachineSettingsDialog::MachineSettingsDialog(Device device, const frondSessions::Preferences& prefs, bool qualified, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Edit settings"));
    setObjectName("machineSettingsDialog");
    auto* layout = new QVBoxLayout(this);
    controls = new MachineSettings(prefs, false, this);
    controls->setDevice(device, qualified);
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
