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

// "max [charging|battery]", "level N [...]" or "host N [...]". Host mode takes the host's level and
// charging state. Where the machine can't charge (chargingSupported false) it always says battery.
inline QString spec(const QString& mode, int level, bool charging, const HostState& host, bool chargingSupported = true) {
    const auto word = [&](bool on) { return on && chargingSupported ? QStringLiteral("charging") : QStringLiteral("battery"); };
    if (mode == "host") return QString("host %1 %2").arg(std::clamp(host.percent, 0, 100)).arg(word(host.charging));
    if (mode == "level") return QString("level %1 %2").arg(std::clamp(level, 0, 100)).arg(word(charging));
    return "max " + word(charging);
}

inline QString readLine(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
}

// Reads a Linux power_supply folder. The level is all the system batteries together: energy_now over
// energy_full, else charge_now over charge_full, else the average of their capacity files. The host
// counts as on external power when a mains or USB supply is online or a battery says it is charging
// or full. Without a battery it reads 100% on external power.
inline HostState parsePowerSupply(const QString& root) {
    bool online = false, charging = false;
    double now = 0, full = 0, capacitySum = 0;
    int batteries = 0, capacities = 0;
    const auto names = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto& name : names) {
        const auto dir = root + "/" + name + "/";
        const auto type = readLine(dir + "type");
        if (type == "Battery") {
            if (readLine(dir + "scope") == "Device") continue;
            ++batteries;
            const auto status = readLine(dir + "status");
            if (status == "Charging" || status == "Full" || status == "Not charging") charging = true;
            bool haveNow = false, haveFull = false;
            for (const auto* prefix : {"energy", "charge"}) {
                const double have = readLine(dir + prefix + QStringLiteral("_now")).toDouble(&haveNow);
                const double total = readLine(dir + prefix + QStringLiteral("_full")).toDouble(&haveFull);
                if (haveNow && haveFull && total > 0) { now += have; full += total; break; }
                haveNow = false;
            }
            if (haveNow) continue;
            bool ok = false;
            const int capacity = readLine(dir + "capacity").toInt(&ok);
            if (ok) { capacitySum += std::clamp(capacity, 0, 100); ++capacities; }
        } else if (type == "Mains" || type == "USB" || type == "USB_C" || type == "USB_PD" || type == "USB_DCP" ||
                   type == "USB_CDP" || type == "USB_ACA" || type == "Wireless") {
            if (readLine(dir + "online") == "1") online = true;
        }
    }
    if (!batteries || (full <= 0 && !capacities)) return {100, true};
    // Batteries with a capacity file only are averaged, and counted as one more battery's worth of energy.
    double percent = full > 0 ? now * 100 / full : 0;
    if (capacities) percent = full > 0 ? (percent * (batteries - capacities) + capacitySum) / batteries : capacitySum / capacities;
    return {std::clamp(int(percent + 0.5), 0, 100), online || charging};
}

// Windows: ACLineStatus 255 is unknown, then only the battery's charging flag (8) says.
inline bool windowsCharging(int acLineStatus, int batteryFlag) { return acLineStatus == 255 ? (batteryFlag & 8) != 0 : acLineStatus != 0; }

// The host's battery now: Linux power_supply, Windows GetSystemPowerStatus, macOS IOKit (HostBattery.cpp).
HostState readHost();

}  // namespace frondBattery
