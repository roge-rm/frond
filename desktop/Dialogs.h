#pragma once
#include <QDialog>
#include "FirmwareInput.h"
#include "SessionStore.h"
#include <QWidget>
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSlider;
class QSpinBox;
class QGroupBox;
class DanaViewControls;

// Window scale in quarters that fits the screen, from the layout the core draws for the device.
int fittingScaleQuarters(frondFirmware::Device device, bool keyboard, frondSessions::JogControls jog);

class MachineSettings : public QWidget {
    Q_OBJECT
 public:
    MachineSettings(const frondSessions::Preferences& prefs, bool automaticScale, QWidget* parent = nullptr);
    frondSessions::Preferences values() const;
    void setDevice(frondFirmware::Device device);
    QSlider* windowScale;
    QLabel* scaleLabel;
    QCheckBox *smoothLcd, *audio, *fastAudio, *showKeyboard;
    DanaViewControls* danaView;
    QComboBox* jogControls;
    QLabel* jogControlsLabel;
    // Never shown for 68k devices.
    QComboBox* engine;
    // 68k devices only.
    QSlider* contrast;
    QComboBox* speed;
    // Infrared, Serial and Bluetooth: the local choices and the connections. Shown once showPeripherals() is called.
    QComboBox* infrared;
    QSpinBox* infraredPort;
    QComboBox* serial;
    QSpinBox* serialPort;
    // Bluetooth: Off, This Frond, a controller socket, the connections.
    QComboBox* bluetooth;
    QLineEdit* bluetoothSocket;
    void showPeripherals(bool shown);
    // A new machine: the Engine follows the chosen device's default until it is changed by hand.
    void setNewMachine();
 private:
    QLabel *contrastLabel, *engineLabel, *speedLabel, *infraredLabel, *serialLabel, *bluetoothLabel;
    QGroupBox* peripherals;
    bool peripheralsShown{false};
    frondFirmware::Device device{frondFirmware::Device::automatic};
    QWidget* speedGroup;
    bool automaticScale;
    bool automaticEngine{false};
    frondSessions::Preferences originalPreferences;
};

class MachineSettingsDialog : public QDialog {
    Q_OBJECT
 public:
    MachineSettingsDialog(frondFirmware::Device device, const frondSessions::Preferences& prefs, QWidget* parent = nullptr);
    MachineSettings* controls;
};

// Copies firmware images into the firmware folder under 'root'.
class FirmwareDialog : public QDialog {
    Q_OBJECT
 public:
    FirmwareDialog(const QString& root, frondFirmware::Device preselect, QWidget* parent = nullptr,
                   const frondSessions::Preferences* initialSettings = nullptr, bool automaticScale = false);
    MachineSettings* machineSettings{nullptr};
    // Validates and copies; returns an error message, or empty once the images are stored.
    QString importImages();
    frondFirmware::Device imported() const { return stored; }
 private:
    QString root;
    QComboBox* device;
    QLineEdit *nor, *nand;
    QLabel* norLabel{nullptr};
    frondFirmware::Device stored{frondFirmware::Device::automatic};
};

// Only builds the controls; MainWindow keeps the settings.
class SettingsDialog : public QDialog {
    Q_OBJECT
 public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    QCheckBox* resume;
};
