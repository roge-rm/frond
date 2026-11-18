#pragma once
#include <QDir>
#include <QFile>
#include <QString>
#include <algorithm>

// The machine's Battery setting as the core's --battery text, and the host's own battery.
namespace frondBattery {

// What the host reports: its level, and whether it is on external power.
struct HostState {
    int percent{100};
    bool charging{true};
};

inline bool validMode(const QString& mode) { return mode == "max" || mode == "level" || mode == "host"; }

// "max [charging|battery]", "level N [...]" or "host N [...]". The host values are used in host mode only.
inline QString spec(const QString& mode, int level, bool charging, const HostState& host) {
    if (mode == "host") return QString("host %1 %2").arg(std::clamp(host.percent, 0, 100)).arg(host.charging ? "charging" : "battery");
    const auto power = charging ? QStringLiteral("charging") : QStringLiteral("battery");
    if (mode == "level") return QString("level %1 %2").arg(std::clamp(level, 0, 100)).arg(power);
    return "max " + power;
}

inline QString readLine(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
}

// Reads a Linux power_supply folder: the first battery (BAT0, ...) gives the level, and the host
// counts as on external power when a mains or USB supply is online or the battery says it is
// charging or full. Without a battery it reads 100% on external power.
inline HostState parsePowerSupply(const QString& root) {
    HostState state;
    bool battery = false, online = false, charging = false;
    const auto names = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto& name : names) {
        const auto dir = root + "/" + name + "/";
        const auto type = readLine(dir + "type");
        if (type == "Battery") {
            if (battery || readLine(dir + "scope") == "Device") continue;
            bool ok = false;
            const int capacity = readLine(dir + "capacity").toInt(&ok);
            if (!ok) continue;
            battery = true;
            state.percent = std::clamp(capacity, 0, 100);
            const auto status = readLine(dir + "status");
            charging = status == "Charging" || status == "Full" || status == "Not charging";
        } else if (type == "Mains" || type == "USB" || type == "USB_C" || type == "USB_PD" || type == "USB_DCP" ||
                   type == "USB_CDP" || type == "USB_ACA" || type == "Wireless") {
            if (readLine(dir + "online") == "1") online = true;
        }
    }
    if (!battery) return {100, true};
    state.charging = online || charging;
    return state;
}

// The host's battery now: Linux power_supply, Windows GetSystemPowerStatus, macOS IOKit (HostBattery.cpp).
HostState readHost();

}  // namespace frondBattery
