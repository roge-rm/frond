#pragma once
#include <optional>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <vector>
#include "FirmwareInput.h"

// Rules for the running bar and the Machine menu, kept free of widgets so tests can run them
// alone. The same rows as the Android bar's menu.
namespace runningRules {

// What a device has. A 68k device lists only the hardware it has, the others grey out what
// isn't done yet.
struct Traits {
    bool runs68k{false}, holdPresent{false}, holdSupported{false}, keyboard{false}, microphone{false},
        network{false}, sound{false}, powerKey{false}, cardSlot{false}, guestServices{true};
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
    t.guestServices = frondFirmware::guestServices(device);
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
    bool embedded{false}, fullScreen{false};  // full screen is only offered with the Palm embedded
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
    out.push_back({Id::FullScreen, s.fullScreen ? QObject::tr("Leave full screen") : QObject::tr("Full screen"), std::nullopt, {},
                   s.embedded, false, 2});
    out.push_back({Id::Save, QObject::tr("Save"), std::nullopt, {}, ready, false, 2});
    out.push_back({Id::SaveCopy, QObject::tr("Save a copy…"), std::nullopt, {}, ready, false, 2});
    out.push_back({Id::Log, QObject::tr("Emulator log"), std::nullopt, {}, true, false, 2});
    out.push_back({Id::Reset, QObject::tr("Reset…"), std::nullopt, {}, ready, false, 3});
    out.push_back({Id::HardReset, QObject::tr("Hard reset…"), std::nullopt, {}, ready && t.guestServices, true, 3});
    out.push_back({Id::SaveStop, QObject::tr("Save and Stop"), std::nullopt, {}, s.running && ready, false, 4});
    return out;
}

// The card button's icon: none without a slot, out with no card, in or ejected.
// The keys shared by the Qt window and the Palm window. The core sends a name such as "ctrl+s" or
// "f12" as a shortcut event when the key is pressed over the Palm.
enum class Shortcut { Save, SaveCopy, SaveStop, Install, Applications, Card, Display, Connections, Sound, HideBar,
                      Log, Reset, Screenshot, FullScreen, Quit };

struct ShortcutKey {
    const char* name;  // as the core sends it
    const char* keys;  // as Qt writes it
    Shortcut what;
};

inline const std::vector<ShortcutKey>& shortcutKeys() {
    static const std::vector<ShortcutKey> all = {
        {"ctrl+s", "Ctrl+S", Shortcut::Save},           {"ctrl+shift+s", "Ctrl+Shift+S", Shortcut::SaveCopy},
        {"ctrl+w", "Ctrl+W", Shortcut::SaveStop},       {"ctrl+i", "Ctrl+I", Shortcut::Install},
        {"ctrl+1", "Ctrl+1", Shortcut::Applications},   {"ctrl+2", "Ctrl+2", Shortcut::Card},
        {"ctrl+3", "Ctrl+3", Shortcut::Display},        {"ctrl+4", "Ctrl+4", Shortcut::Connections},
        {"ctrl+m", "Ctrl+M", Shortcut::Sound},          {"ctrl+b", "Ctrl+B", Shortcut::HideBar},
        {"ctrl+l", "Ctrl+L", Shortcut::Log},            {"ctrl+r", "Ctrl+R", Shortcut::Reset},
        {"f11", "F11", Shortcut::FullScreen},           {"f12", "F12", Shortcut::Screenshot},
        {"ctrl+q", "Ctrl+Q", Shortcut::Quit}};
    return all;
}

inline std::optional<Shortcut> shortcutFor(const QString& name) {
    for (const auto& key : shortcutKeys()) if (name == QLatin1String(key.name)) return key.what;
    return std::nullopt;
}

// The menu row a shortcut belongs to, if it has one.
inline std::optional<Id> rowFor(Shortcut what) {
    switch (what) {
        case Shortcut::Save: return Id::Save;
        case Shortcut::SaveCopy: return Id::SaveCopy;
        case Shortcut::SaveStop: return Id::SaveStop;
        case Shortcut::Display: return Id::Display;
        case Shortcut::Connections: return Id::Connections;
        case Shortcut::HideBar: return Id::HideBar;
        case Shortcut::Log: return Id::Log;
        case Shortcut::Reset: return Id::Reset;
        case Shortcut::Screenshot: return Id::Screenshot;
        case Shortcut::FullScreen: return Id::FullScreen;
        default: return std::nullopt;
    }
}

// Embedding the Palm's window needs Qt on X11 (xcb). `forced` tries it anyway, for tests.
inline bool embedPossible(const QString& platform, bool forced) { return forced || platform == QLatin1String("xcb"); }

// The X11 window id out of the core's `window-id` reply ("x11 ID"), or 0.
inline quint64 parseWindowId(const QString& reply) {
    const auto parts = reply.trimmed().split(' ');
    if (parts.size() != 2 || parts[0] != QLatin1String("x11")) return 0;
    bool ok = false;
    const auto id = parts[1].toULongLong(&ok);
    return ok ? id : 0;
}

// The side panel's width and the strip left of a hidden bar.
constexpr int panelWidth = 300, handleHeight = 8;
// Full screen: the bar goes this long after the pointer leaves it, and comes back when the
// pointer is this close to the top edge of the screen.
constexpr int fullScreenHideMs = 3000, edgeRevealPixels = 2;
// The bar stays in full screen while the pointer is on it, a menu from it is open or a panel is.
inline bool barStaysInFullScreen(bool pointerOnBar, bool menuOpen, bool panelOpen) { return pointerOnBar || menuOpen || panelOpen; }
// The hidden bar comes back with the pointer at the top edge of the full-screen area.
inline bool revealBar(QPoint pointer, QRect area) {
    return pointer.x() >= area.left() && pointer.x() <= area.right() && pointer.y() >= area.top() &&
           pointer.y() < area.top() + edgeRevealPixels;
}
// Where the panel goes in full screen: over the right edge, under the bar when that shows.
inline QRect overlayPanel(QSize area, int barHeight) {
    return QRect(area.width() - panelWidth, barHeight, panelWidth, std::max(0, area.height() - barHeight));
}
// The keyboard with the Palm embedded. The X server sends keys to the focus window, or to the
// window under the pointer when that is inside the focus window, so the focus never stays on a
// window that holds the Palm (Frond's top-level): it goes to the Palm or to Frond's key window,
// which doesn't hold it.
// Where the X focus is: in another application or a dialog, on the Palm, on one of Frond's
// windows that doesn't hold the Palm, or on one that does.
enum class FocusAt { Elsewhere, Palm, Frond, HoldsPalm };
// What a mouse press was on: the Palm, a Frond widget that takes the keys (anything but the bar
// and the menu bar), or neither.
enum class Press { None, OnPalm, OnFrond };
// Whether the Palm should have the keys after a press: a press on it gives them, a press on a
// Frond widget takes them, anything else leaves the choice as it was.
inline bool palmWantsKeys(bool wanted, Press press) {
    if (press == Press::OnPalm) return true;
    if (press == Press::OnFrond) return false;
    return wanted;
}
// Where to move the X focus. Nothing changes while another application or a dialog has it, or a
// menu is open (it grabs the keyboard).
enum class KeyTarget { Keep, Palm, Frond };
inline KeyTarget keyTarget(FocusAt at, bool palmWanted, bool menuOpen) {
    if (at == FocusAt::Elsewhere || menuOpen) return KeyTarget::Keep;
    if (palmWanted) return at == FocusAt::Palm ? KeyTarget::Keep : KeyTarget::Palm;
    return at == FocusAt::Frond ? KeyTarget::Keep : KeyTarget::Frond;
}

// Window width for a Palm of `palm` pixels: a panel opens at its right, so the Palm keeps its size.
inline int windowWidth(int palm, bool panelOpen) { return palm + (panelOpen ? panelWidth : 0); }

// The Palm's size after the Size setting goes from `from` to `to` (quarter steps).
inline QSize scaledPalm(QSize size, int from, int to) {
    if (from <= 0) return size;
    return {int(qint64(size.width()) * to / from), int(qint64(size.height()) * to / from)};
}

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
    // Storage: the slot where there is one, and Internal on every machine.
    b.card = true;
    b.power = t.powerKey;
    b.keyboard = s.keyboardAttached;
    b.soundEnabled = s.ready && t.sound;
    b.powerEnabled = s.ready && t.powerKey;
    b.cardEnabled = s.ready;
    return b;
}

// The panel's Hard reset button: off whenever the other running controls are, and on a device
// without guest calls.
inline bool hardResetEnabled(bool controlsEnabled, const Traits& t) { return controlsEnabled && t.guestServices; }

// The core's reason when a machine stopped by itself, as the failure's main text. The core's last
// line like "Tapwave Zodiac 2: the machine stopped: REASON" gives "The Tapwave Zodiac 2 stopped:
// REASON."; empty when the log has no such line.
inline QString machineStopText(const QString& log) {
    static const QString marker = QStringLiteral(": the machine stopped: ");
    const auto lines = log.split('\n');
    for (auto i = lines.size() - 1; i >= 0; --i) {
        const auto at = lines[i].indexOf(marker);
        const auto reason = at > 0 ? lines[i].mid(at + marker.size()).trimmed() : QString();
        if (!reason.isEmpty()) return QObject::tr("The %1 stopped: %2.").arg(lines[i].left(at).trimmed(), reason);
    }
    return {};
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

// A RAM database as `db-list` gives it.
struct Database {
    QString name, type, creator;
    bool resource{false};
    bool application() const { return resource && type == QLatin1String("appl"); }
};

// db-list's reply: per line, type, creator and attributes in hex, then the name's bytes in hex.
// Lines that don't parse are left out. Sorted by name, ignoring case.
// The file a database exports as: its name with .prc for a resource database, else .pdb, with
// the characters a file name can't have replaced.
inline QString databaseFile(const Database& d) {
    QString name = d.name;
    for (auto c : QStringLiteral("\\/:*?\"<>|")) name.replace(c, '_');
    if (name.trimmed().isEmpty() || name == "." || name == "..") name = QStringLiteral("Database");
    return name + (d.resource ? QStringLiteral(".prc") : QStringLiteral(".pdb"));
}

inline std::vector<Database> parseDatabases(const QString& text) {
    std::vector<Database> out;
    auto bytes = [](const QString& hex, bool* ok) {
        QByteArray raw;
        if (hex.size() % 2) { *ok = false; return raw; }
        for (int i = 0; i < hex.size(); i += 2) {
            bool good = false;
            const int v = hex.mid(i, 2).toInt(&good, 16);
            if (!good) { *ok = false; return raw; }
            raw.append(char(v));
        }
        return raw;
    };
    for (const auto& line : text.split('\n')) {
        const auto parts = line.split(' ');
        if (parts.size() != 4 || parts[0].size() != 8 || parts[1].size() != 8 || parts[3].isEmpty()) continue;
        bool ok = true;
        const auto type = bytes(parts[0], &ok), creator = bytes(parts[1], &ok), name = bytes(parts[3], &ok);
        bool attributes = false;
        const int flags = parts[2].toInt(&attributes, 16);
        if (!ok || !attributes) continue;
        out.push_back({QString::fromLatin1(name), QString::fromLatin1(type), QString::fromLatin1(creator), (flags & 1) != 0});
    }
    std::stable_sort(out.begin(), out.end(), [](const Database& a, const Database& b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return out;
}

inline std::vector<Database> applications(const std::vector<Database>& all) {
    std::vector<Database> out;
    for (const auto& d : all) if (d.application()) out.push_back(d);
    return out;
}

// The databases that go with an application: the others with its creator.
inline std::vector<Database> dataOf(const std::vector<Database>& all, const Database& app) {
    std::vector<Database> out;
    for (const auto& d : all) if (d.creator == app.creator && d.name != app.name) out.push_back(d);
    return out;
}

}  // namespace runningRules
