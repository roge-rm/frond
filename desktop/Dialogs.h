#pragma once
#include <functional>
#include <QDialog>
#include <QTemporaryDir>
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

// Fills a speed combo with the choices for the device family (data is the core's name), keeping name selected.
void fillSpeedBox(QComboBox* box, bool m68k, const QString& name);

// The core that lists the MIDI output ports for the Zodiac's Music output (its --music-midi-ports).
void setMidiPortsCore(const QString& core);
// Asks the core for the MIDI output port names in the background; done gets them on context's thread, empty when
// it has none or can't run (5 s at most). Nothing comes if context goes first.
void listMidiOutputPorts(QObject* context, std::function<void(const QStringList&)> done);
// A port's name without the numbers that change with plug order (as the core's zodiacMidiPortKey), in lower case.
QString midiPortKey(const QString& name);

class QPushButton;
class MachineSettings : public QWidget {
    Q_OBJECT
 public:
    MachineSettings(const frondSessions::Preferences& prefs, bool automaticScale, QWidget* parent = nullptr);
    frondSessions::Preferences values() const;
    void setDevice(frondFirmware::Device device);
    QSlider* windowScale;
    QLabel* scaleLabel;
    // Battery: Maximum, Set level or Host battery, its level, and the Charging switch.
    QComboBox* batteryMode;
    QSlider* batteryLevel;
    QCheckBox* batteryCharging;
    QCheckBox *smoothLcd, *showMips, *audio, *fastAudio, *rumble, *showKeyboard;
    // Zodiac: Music output (the game's own FM, a SoundFont or a MIDI device), the SoundFont file and the port.
    QComboBox* musicOutput;
    QLineEdit* soundFont;
    QPushButton* soundFontChoose;
    QComboBox* midiPort;
    DanaViewControls* danaView;
    QComboBox* jogControls;
    QLabel* jogControlsLabel;
    // TG50: the jog controls in landscape with the game controller.
    QComboBox* jogLandscape;
    QLabel* jogLandscapeLabel;
    // Never shown for 68k devices.
    QComboBox* engine;
    // Zodiac only, used in Fast only.
    QCheckBox* directStick;
    // 68k devices only.
    QSlider* contrast;
    QComboBox* speed;
    // What the user chose, kept while a 68k device hides 8x.
    QString chosenSpeed;
    // Infrared, Serial and Bluetooth: the local choices and the connections. Shown once showPeripherals() is called.
    QComboBox* infrared;
    QSpinBox* infraredPort;
    QComboBox* serial;
    QSpinBox* serialPort;
    // Bluetooth: Off, This Frond, a controller socket, the connections.
    QComboBox* bluetooth;
    QLineEdit* bluetoothSocket;
    void showPeripherals(bool shown);
    // Sync Manager (data "sync") on the serial and infrared pickers, where the main window offers it.
    void offerSync(bool serialSync, bool infraredSync);
    // A new machine: the Engine follows the chosen device's default until it is changed by hand.
    void setNewMachine();
 private:
    QLabel *batteryLevelLabel;
    QLabel* musicOutputLabel;
    QWidget *soundFontRow, *midiPortRow;
    bool midiPortsListed{false};
    void showMusic();
    QLabel *contrastLabel, *engineLabel, *speedLabel, *infraredLabel, *serialLabel, *bluetoothLabel;
    QGroupBox* peripherals;
    bool peripheralsShown{false};
    frondFirmware::Device device{frondFirmware::Device::automatic};
    QWidget* speedGroup;
    bool automaticScale;
    bool automaticEngine{false};
    frondSessions::Preferences originalPreferences;
    // The controller map being edited (controllerMapSet and controllerMap only).
    frondSessions::Preferences controllerPrefs;
    QGroupBox* controllerGroup;
};

class MachineSettingsDialog : public QDialog {
    Q_OBJECT
 public:
    MachineSettingsDialog(frondFirmware::Device device, const frondSessions::Preferences& prefs, QWidget* parent = nullptr);
    MachineSettings* controls;
};

// Copies firmware images into the firmware folder under 'root'.
class QFormLayout;
class FirmwareDialog : public QDialog {
    Q_OBJECT
 public:
    FirmwareDialog(const QString& root, frondFirmware::Device preselect, QWidget* parent = nullptr,
                   const frondSessions::Preferences* initialSettings = nullptr, bool automaticScale = false);
    MachineSettings* machineSettings{nullptr};
    // Validates and copies; returns an error message, or empty once the images are stored.
    QString importImages();
    frondFirmware::Device imported() const { return stored; }
    // A card for each slot of a two-slot machine, from the drawer of the Cards library beside
    // `sessions`; chosenCards() has a card id or an empty string per slot.
    void offerCards(const QString& sessions);
    QStringList chosenCards() const;
    QString storeZaurus(QString& path);
 private:
    QString prepareZaurus();
    void fillCards();
    QString root, sessions;
    QFormLayout* form{nullptr};
    QComboBox* cards[2]{};
    QComboBox* device;
    QLineEdit *nor, *nand;
    QComboBox* cfMode{nullptr};
    QLineEdit *cfImage{nullptr}, *cfArchive{nullptr};
    QSpinBox* cfSize{nullptr};
    std::unique_ptr<QTemporaryDir> zaurusStage;
    QLabel* norLabel{nullptr};
    QLabel* nandLabel{nullptr};
    QLabel *cfImageLabel{nullptr}, *cfArchiveLabel{nullptr};
    QCheckBox* anyFirmware{nullptr};
    frondFirmware::Device stored{frondFirmware::Device::automatic};
};

// Only builds the controls; MainWindow keeps the settings.
class SettingsDialog : public QDialog {
    Q_OBJECT
 public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    QCheckBox* resume;
};
