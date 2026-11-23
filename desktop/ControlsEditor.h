#pragma once
#include <QDialog>
#include <QString>
#include "ControllerMap.h"
#include "SessionStore.h"
class QGridLayout;
class QLabel;
class QProcess;
class QPushButton;

// The ARM core, which listens for a controller button for press-to-assign (its --controller-listen),
// for the 68k devices too.
void setControllerCore(const QString& core);

// The map a machine's settings give: the saved one, or the device's defaults.
inline ControllerMap controllerMapFor(const frondSessions::Preferences& prefs, const DeviceProfile5& profile) {
    return prefs.controllerMapSet ? decodeControllerMap(prefs.controllerMap.toStdString(), profile) : controllerDefaults(profile);
}
// Whether the device has Palm buttons a controller can press.
inline bool controlsShown(const DeviceProfile5& profile) { return !controllerPalmKeys(profile).empty(); }

// One device's controller map: each Palm button with the controller buttons that press it. Add waits
// for the next controller button; a button's x takes it off; Reset to defaults puts the device's back.
// Each change is sent at once with changed().
class ControlsDialog : public QDialog {
    Q_OBJECT
 public:
    ControlsDialog(const DeviceProfile5& profile, const ControllerMap& map, QWidget* parent = nullptr);
    ~ControlsDialog() override;
    const ControllerMap& map() const { return current; }
    // The map is the device's defaults after Reset to defaults and no change since.
    bool isDefault() const { return defaulted; }
 signals:
    void changed(const ControllerMap& map, bool defaults);
 private:
    void fill();
    void capture(PalmKey button);
    void stopCapture();
    void take(PadKey key);
    void keyReceived();
    const DeviceProfile5& profile;
    ControllerMap current;
    bool defaulted{false};
    QGridLayout* grid;
    QLabel* status;
    QProcess* listener{nullptr};
    QByteArray pending;
    PalmKey waiting{PalmKey::count};
};
