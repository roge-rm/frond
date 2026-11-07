#pragma once
#include <optional>
#include <QString>
#include <QStringList>
#include <vector>
#include "FirmwareInput.h"

// Rules for the running bar and the Machine menu, kept free of widgets so tests can run them
// alone. The same rows as the Android bar's menu.
namespace runningRules {

// What a device has. A 68k device lists only the hardware it has, the others grey out what
// isn't done yet.
struct Traits {
    bool runs68k{false}, holdPresent{false}, holdSupported{false}, keyboard{false}, microphone{false},
        network{false}, sound{false}, powerKey{false}, cardSlot{false};
};

inline Traits traitsFor(frondFirmware::Device device) {
    const auto& profile = frondFirmware::profile(device);
    const auto caps = frondFirmware::capabilities(device);
    Traits t;
    t.runs68k = frondFirmware::runs68k(device);
    t.holdPresent = profile.frontend.holdPresent;
    t.holdSupported = profile.frontend.holdSupported;
    t.keyboard = caps.keyboard;
    t.microphone = caps.microphone;
    t.network = caps.network;
    t.sound = caps.sound;
    t.cardSlot = profile.frontend.sdSlot || profile.frontend.memoryStick;
    t.powerKey = QString::fromLatin1(profile.frontend.buttons).split('|').contains(QLatin1String("power"));
    return t;
}

enum class Id { Hold, Keyboard, Microphone, Network, Display, Connections, Screenshot, HideBar, FullScreen,
                Save, SaveCopy, Log, Reset, HardReset, SaveStop };

struct Row {
    Id id;
    QString label;
    std::optional<bool> on;  // set for a row with a tick
    QString value;           // the state at the row's end, when it is not a tick
    bool enabled{true}, destructive{false};
    int group{0};
};

// What the window and the core have said so far; an unset hold is not known yet.
struct State {
    bool ready{false};  // the core takes commands now
    bool running{true};
    std::optional<bool> hold;
    bool keyboardAttached{false}, beamOff{true};
    QString microphone{"off"};  // on, off, or "off <reason>"
    QString network;            // the current mode, empty before it is known
    bool barShown{true};
};

inline QString microphoneValue(const QString& state) {
    if (state == "on" || state == "pending") return "On";
    if (state.startsWith("off ")) return state.mid(4);
    return "Off";
}

inline std::vector<Row> rows(const Traits& t, const State& s) {
    const bool ready = s.ready;
    std::vector<Row> out;
    auto has = [&](bool present) { return !t.runs68k || present; };
    if (t.holdPresent) out.push_back({Id::Hold, QObject::tr("Hold"), s.hold.value_or(false), {},
                                      ready && t.holdSupported && s.hold.has_value(), false, 0});
    // The keyboard and beaming share the Palm's infrared port.
    if (has(t.keyboard)) out.push_back({Id::Keyboard, QObject::tr("Infrared keyboard"), s.keyboardAttached, {},
                                        ready && t.keyboard && s.beamOff, false, 0});
    if (has(t.microphone)) out.push_back({Id::Microphone, QObject::tr("Microphone"), microphoneValue(s.microphone) == "On",
                                          microphoneValue(s.microphone) == "On" || microphoneValue(s.microphone) == "Off" ? QString() : microphoneValue(s.microphone),
                                          ready && t.microphone, false, 0});
    if (has(t.network)) out.push_back({Id::Network, QObject::tr("Network"), std::nullopt, s.network,
                                       ready && t.network && !s.network.isEmpty(), false, 0});
    out.push_back({Id::Display, QObject::tr("Display…"), std::nullopt, {}, true, false, 1});
    out.push_back({Id::Connections, QObject::tr("Connections…"), std::nullopt, {}, true, false, 1});
    out.push_back({Id::Screenshot, QObject::tr("Screenshot"), std::nullopt, {}, ready, false, 1});
    out.push_back({Id::HideBar, s.barShown ? QObject::tr("Hide bar") : QObject::tr("Show bar"), std::nullopt, {}, true, false, 2});
    // The Palm is in its own window until it can be embedded.
    out.push_back({Id::FullScreen, QObject::tr("Full screen"), std::nullopt, {}, false, false, 2});
    out.push_back({Id::Save, QObject::tr("Save"), std::nullopt, {}, ready, false, 2});
    out.push_back({Id::SaveCopy, QObject::tr("Save a copy…"), std::nullopt, {}, ready, false, 2});
    out.push_back({Id::Log, QObject::tr("Emulator log"), std::nullopt, {}, true, false, 2});
    out.push_back({Id::Reset, QObject::tr("Reset…"), std::nullopt, {}, ready, false, 3});
    out.push_back({Id::HardReset, QObject::tr("Hard reset…"), std::nullopt, {}, ready, true, 3});
    out.push_back({Id::SaveStop, QObject::tr("Save and Stop"), std::nullopt, {}, s.running && ready, false, 4});
    return out;
}

// The card button's icon: none without a slot, out with no card, in or ejected.
enum class CardIcon { None, Out, In, Ejected };
inline CardIcon cardIcon(bool slot, bool known, bool present, bool inserted) {
    if (!slot) return CardIcon::None;
    if (!known || !present) return CardIcon::Out;
    return inserted ? CardIcon::In : CardIcon::Ejected;
}

// What the toolbar shows beyond the menu's rows.
struct Buttons {
    bool card{false}, power{false}, keyboard{false};
    bool soundEnabled{false}, powerEnabled{false}, cardEnabled{false};
};
inline Buttons buttons(const Traits& t, const State& s) {
    Buttons b;
    b.card = t.cardSlot;
    b.power = t.powerKey;
    b.keyboard = s.keyboardAttached;
    b.soundEnabled = s.ready && t.sound;
    b.powerEnabled = s.ready && t.powerKey;
    b.cardEnabled = s.ready && t.cardSlot;
    return b;
}

// The line under the machine's name: the engine on ARM devices, else the device.
inline QString subtitle(const Traits& t, bool fastEngine, const QString& deviceName) {
    return t.runs68k ? deviceName : fastEngine ? QObject::tr("Fast") : QObject::tr("Exact");
}

// "Frond NAME STAMP.png" with the characters a file name can't have replaced.
inline QString screenshotName(const QString& machine, const QString& stamp) {
    QString name = machine;
    for (auto c : QStringLiteral("\\/:*?\"<>|")) name.replace(c, '_');
    name = name.trimmed();
    if (name.isEmpty()) name = QStringLiteral("Palm");
    return "Frond " + name + " " + stamp + ".png";
}

// One input replay line that saves the screen, the path quoted the way the core reads it.
inline QString snapshotReplay(const QString& path) {
    QString quoted = path;
    quoted.replace('\\', "\\\\").replace('"', "\\\"");
    return "0 snapshot \"" + quoted + "\"\n";
}
inline QString powerReplay() { return QStringLiteral("0 key-down power\n80 key-up power\n"); }

}  // namespace runningRules
