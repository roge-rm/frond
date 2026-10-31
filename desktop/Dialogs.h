#pragma once
#include <QDialog>
#include "FirmwareInput.h"
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSlider;

// Copies firmware images into the firmware folder under 'root'.
class FirmwareDialog : public QDialog {
    Q_OBJECT
 public:
    FirmwareDialog(const QString& root, frondFirmware::Device preselect, QWidget* parent = nullptr);
    // Validates and copies; returns an error message, or empty once the images are stored.
    QString importImages();
    frondFirmware::Device imported() const { return stored; }
 private:
    QString root;
    QComboBox* device;
    QLineEdit *nor, *nand;
    frondFirmware::Device stored{frondFirmware::Device::automatic};
};

// Lists the devices Frond runs, each Ready or Needs firmware; choosing one accepts.
class AddDeviceDialog : public QDialog {
    Q_OBJECT
 public:
    AddDeviceDialog(const QString& root, QWidget* parent = nullptr);
    frondFirmware::Device device() const { return chosen; }
 private:
    QListWidget* devices;
    frondFirmware::Device chosen{frondFirmware::Device::automatic};
};

// Only builds the controls; MainWindow keeps the settings.
class SettingsDialog : public QDialog {
    Q_OBJECT
 public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    QSlider* windowScale;
    QLabel* scaleLabel;
    QCheckBox *smoothLcd, *audio, *fastAudio, *resume;
};
