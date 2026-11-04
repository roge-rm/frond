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

// Window scale in quarters that fits the screen, from the layout the core draws for the device.
int fittingScaleQuarters(frondFirmware::Device device, bool keyboard, frondSessions::JogControls jog);

class MachineSettings : public QWidget {
    Q_OBJECT
 public:
    MachineSettings(const frondSessions::Preferences& prefs, bool automaticScale, QWidget* parent = nullptr);
    frondSessions::Preferences values() const;
    void setDevice(frondFirmware::Device device, bool qualified = false);
    QSlider* windowScale;
    QLabel* scaleLabel;
    QCheckBox *smoothLcd, *audio, *fastAudio, *showKeyboard, *sonyAudioPreview;
    QComboBox* jogControls;
    QLabel* jogControlsLabel;
    // Only shown with FROND_ENGINE_SETTING=1 until the Fast engine is qualified.
    QComboBox* engine;
 private:
    bool automaticScale;
    frondSessions::Preferences originalPreferences;
};

class MachineSettingsDialog : public QDialog {
    Q_OBJECT
 public:
    MachineSettingsDialog(frondFirmware::Device device, const frondSessions::Preferences& prefs, bool qualified, QWidget* parent = nullptr);
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
    frondFirmware::Device stored{frondFirmware::Device::automatic};
};

// Only builds the controls; MainWindow keeps the settings.
class SettingsDialog : public QDialog {
    Q_OBJECT
 public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    QCheckBox* resume;
};
