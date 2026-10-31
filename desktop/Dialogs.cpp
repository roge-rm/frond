#include "Dialogs.h"
#include "FirmwareStore.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

using frondFirmware::Device;

FirmwareDialog::FirmwareDialog(const QString& root, Device preselect, QWidget* parent) : QDialog(parent), root(root) {
    setWindowTitle(tr("Import firmware"));
    setObjectName("firmwareDialog");
    auto* form = new QFormLayout;
    device = new QComboBox(this);
    device->setObjectName("device");
    // Items hold the device's fixed name; empty detects.
    device->addItem(tr("Detect from the NOR image"), QString());
    for (auto d : {Device::tx, Device::e2}) device->addItem(frondFirmware::deviceName(d), frondFirmware::deviceId(d));
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
    layout->addWidget(buttons);
}

QString FirmwareDialog::importImages() {
    auto chosen = frondFirmware::deviceFromId(device->currentData().toString());
    const auto error = frondFirmware::importImages(root, chosen, nor->text(), nand->text(), &chosen);
    if (error.isEmpty()) stored = chosen;
    return error;
}

AddDeviceDialog::AddDeviceDialog(const QString& root, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Add device"));
    setObjectName("addDeviceDialog");
    devices = new QListWidget(this);
    devices->setObjectName("devices");
    for (auto d : {Device::tx, Device::e2}) {
        auto* item = new QListWidgetItem(tr("%1\n%2").arg(frondFirmware::deviceName(d),
                                                         frondFirmware::ready(root, d) ? tr("Ready") : tr("Needs firmware")), devices);
        item->setData(Qt::UserRole, frondFirmware::deviceId(d));
    }
    devices->setCurrentRow(0);
    devices->setMinimumWidth(260);
    auto choose = [this] {
        if (!devices->currentItem()) return;
        chosen = frondFirmware::deviceFromId(devices->currentItem()->data(Qt::UserRole).toString());
        accept();
    };
    connect(devices, &QListWidget::itemActivated, this, choose);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto* add = buttons->addButton(tr("Add"), QDialogButtonBox::AcceptRole);
    add->setDefault(true);
    connect(add, &QPushButton::clicked, this, choose);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(devices);
    layout->addWidget(buttons);
}

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Settings"));
    setObjectName("settingsDialog");
    auto* layout = new QVBoxLayout(this);
    auto* display = new QGroupBox(tr("Display"), this);
    auto* displayLayout = new QVBoxLayout(display);
    scaleLabel = new QLabel(display);
    windowScale = new QSlider(Qt::Horizontal, display);
    windowScale->setObjectName("windowScale");
    windowScale->setRange(4, 8);
    windowScale->setTickInterval(1);
    windowScale->setTickPosition(QSlider::TicksBelow);
    windowScale->setToolTip(tr("1× to 2× in 0.25 steps. Applies on the next start."));
    smoothLcd = new QCheckBox(tr("Smooth pixels"), display);
    smoothLcd->setObjectName("smoothLcd");
    smoothLcd->setToolTip(tr("Applies on the next start."));
    displayLayout->addWidget(scaleLabel);
    displayLayout->addWidget(windowScale);
    displayLayout->addWidget(smoothLcd);
    layout->addWidget(display);
    auto* sound = new QGroupBox(tr("Audio"), this);
    auto* soundLayout = new QVBoxLayout(sound);
    audio = new QCheckBox(tr("Sound"), sound);
    audio->setObjectName("audio");
    fastAudio = new QCheckBox(tr("Low-latency audio"), sound);
    fastAudio->setObjectName("fastAudio");
    soundLayout->addWidget(audio);
    soundLayout->addWidget(fastAudio);
    layout->addWidget(sound);
    resume = new QCheckBox(tr("Resume last session on startup"), this);
    resume->setObjectName("resume");
    layout->addWidget(resume);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    layout->addWidget(buttons);
}
