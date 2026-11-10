#ifndef FROND_FIRMWARE_INPUT_H
#define FROND_FIRMWARE_INPUT_H

#include <QFile>
#include <QFileInfo>
#include <QString>
#include <vector>
#include "device_profile5.h"

namespace frondFirmware {
// The devices the desktop starts. Settings and the core get the device's fixed name from
// device_profile5.h, never this enum.
enum class Device { automatic = -1, tx = deviceTypeTX, e2 = deviceTypeE2, zire72 = deviceTypeZire72 };
// The Dana and the Dana Wireless share one board, ROM, art and keyboard.
inline bool isDana(Device device) { return device != Device::automatic && deviceTypeIsDana5(int(device)); }
inline const auto devices = [] {
    std::vector<Device> result;
    for (const auto& p : deviceProfiles5)
        if (p.frontend.available) result.push_back(static_cast<Device>(p.deviceType));
    return result;
}();

inline const DeviceProfile5& profile(Device device) {
    const auto* p = deviceProfile5(int(device));
    return p ? *p : *deviceProfile5(deviceTypeTX);
}

// The fixed name, like palm-tx; empty for automatic.
inline QString deviceId(Device device) {
    return device == Device::automatic ? QString() : QString::fromLatin1(profile(device).name);
}

// The device for a fixed name; automatic for an empty or unknown one.
inline Device deviceFromId(const QString& id) {
    for (auto device : devices)
        if (id == QLatin1String(profile(device).name)) return device;
    return Device::automatic;
}

inline QString deviceName(Device device) { return QString::fromUtf8(profile(device).displayName); }

// Which settings a device supports, from the device table.
using Capabilities = DeviceCapabilities5;
inline Capabilities capabilities(Device device) { return profile(device).capabilities; }
inline bool infraredKeyboard(Device device) { return capabilities(device).keyboard && !isDana(device); }

// DragonBall devices run in frond-68k. Their capabilities list all the hardware they have, so
// controls for anything else are hidden rather than greyed.
inline bool runs68k(Device device) { return device != Device::automatic && deviceProfileRuns68k5(int(device)); }

// The Zodiac runs with no guest calls yet: no install, application list, hard reset or Fast engine.
inline bool guestServices(Device device) {
    return device == Device::automatic || profile(device).board != DeviceBoard5::imx1Zodiac;
}

// Hardware a device doesn't have at all, which is left out rather than greyed: the Zire 72 has no serial port.
inline bool serialAbsent(Device device) { return device == Device::zire72; }
// The connector or cradle has a separate HotSync button on these boards.
inline bool hasHotSyncButton(Device device) { return deviceProfileHasHotSyncButton5(int(device)); }

// A 68k ROM's big-endian card header, read the way frond-68k reads it before its own full check.
inline Device68kRomInfo5 inspectRom68k(QFile& rom) {
    return deviceProfileInspect68kRom5(rom.size(), [&](uint64_t at, uint8_t* bytes, size_t count) {
        return rom.seek(at) && rom.read(reinterpret_cast<char*>(bytes), count) == qint64(count);
    });
}

// Looks for the Palm card header the way the core does (every 32 KiB) and says
// whether it names an E2 or a Zire 72. 'other' is a header for another device, 'none' no header.
// A Zire 72 NOR has a small card at 0x0 and a big card at 0x20000 to the end, both PalmCard aZ72.
enum class Header { none, e2, e2WrongPlace, zire72, zire72BadLayout, other };
inline DeviceNorInfo5 inspectNor(QFile& nor) {
    return deviceProfileInspectNor5(nor.size(), [&](uint64_t at, uint8_t* bytes, size_t count) {
        return nor.seek(at) && nor.read(reinterpret_cast<char*>(bytes), count) == qint64(count);
    });
}
inline Header norHeader(QFile& nor) {
    const auto info = inspectNor(nor);
    if (!info.profile) return info.hasHeader ? Header::other : Header::none;
    if (info.profile->deviceType == deviceTypeE2) return info.validLayout ? Header::e2 : Header::e2WrongPlace;
    if (info.profile->deviceType == deviceTypeZire72) return info.validLayout ? Header::zire72 : Header::zire72BadLayout;
    return Header::other;
}

inline Device detectDevice(const QString& norPath) {
    QFile nor(norPath);
    if (!QFileInfo(norPath).isFile() || !nor.open(QIODevice::ReadOnly)) return Device::automatic;
    const auto info = inspectNor(nor);
    if (info.profile && info.profile->frontend.available) return static_cast<Device>(info.profile->deviceType);
    const auto rom68k = inspectRom68k(nor);
    if (rom68k.profile && rom68k.profile->frontend.available) return static_cast<Device>(rom68k.profile->deviceType);
    for (auto device : devices) {
        const auto& p = profile(device);
        if (p.detect == DeviceDetect5::norSize && size_t(nor.size()) == p.norSize) return device;
    }
    return Device::automatic;
}

// A 68k device takes one ROM file and no NAND. The header check only; frond-68k checks the rest.
inline QString validateRom68k(Device device, QFile& rom, const QString& nandPath) {
    const auto info = inspectRom68k(rom);
    const auto name = deviceName(device);
    if (!info.hasHeader) return QStringLiteral("This file isn't a %1 ROM.").arg(name);
    if (!info.profile) return QStringLiteral("This ROM isn't one the %1 can run.").arg(name);
    // The Dana Wireless runs the Dana's ROM, whose header still says Dana.
    const bool danaRom = int(device) == deviceTypeDanaWireless && info.profile->deviceType == deviceTypeDana;
    if (info.profile != &profile(device) && !danaRom)
        return QStringLiteral("This is a %1 ROM. Choose %1.").arg(QString::fromUtf8(info.profile->displayName));
    if (!nandPath.isEmpty()) return QStringLiteral("The %1 has no NAND. Leave the NAND image empty.").arg(name);
    return {};
}

// Readability, geometry and which device the NOR header names; no boot claim.
// A chosen device is never swapped for another one. 'resolved' gets the device to start.
inline QString validateFiles(Device chosen, const QString& norPath, const QString& nandPath, Device* resolved = nullptr) {
    QFile nor(norPath);
    if (!QFileInfo(norPath).isFile() || !nor.open(QIODevice::ReadOnly))
        return QStringLiteral("Choose a readable NOR firmware file.");
    const auto header = norHeader(nor);
    const auto detected = inspectNor(nor).profile;
    if (detected && !detected->frontend.available)
        return QStringLiteral("The %1 isn't supported yet.").arg(QString::fromUtf8(detected->displayName));
    Device device = chosen;
    if (device == Device::automatic) {
        device = detectDevice(norPath);
        if (device == Device::automatic)
            return QStringLiteral("Frond can't tell which device this NOR image is for. Choose the device.");
    }
    if (runs68k(device)) {
        const auto error = validateRom68k(device, nor, nandPath);
        if (error.isEmpty() && resolved) *resolved = device;
        return error;
    }
    if (const auto rom68k = inspectRom68k(nor); rom68k.profile && rom68k.profile->frontend.available)
        return QStringLiteral("This is a %1 ROM. Choose %1.").arg(QString::fromUtf8(rom68k.profile->displayName));
    if (device == Device::tx && (header == Header::e2 || header == Header::e2WrongPlace))
        return QStringLiteral("This is a Tungsten E2 NOR image. Choose Tungsten E2.");
    if (device != Device::zire72 && (header == Header::zire72 || header == Header::zire72BadLayout))
        return QStringLiteral("This is a Zire 72 NOR image. Choose Zire 72.");
    if (device == Device::zire72 && size_t(nor.size()) != profile(device).norSize)
        return QStringLiteral("Choose a 14,942,208-byte NOR image for the Zire 72.");
    if (device == Device::zire72 && header != Header::zire72)
        return header == Header::zire72BadLayout ? QStringLiteral("This Zire 72 NOR image doesn't have a small card at 0x0 and a big card at 0x20000.")
               : header == Header::none ? QStringLiteral("This NOR image has no Zire 72 header. Check the device choice.")
                                        : QStringLiteral("This NOR image is not for a Zire 72.");
    if (device == Device::zire72 && !nandPath.isEmpty())
        return QStringLiteral("The Zire 72 has no NAND. Leave the NAND image empty.");
    if (device == Device::e2 && header != Header::e2) {
        const auto other = inspectNor(nor).profile;
        if (other && other != &profile(device) && other->deviceType != deviceTypeZire72)
            return QStringLiteral("This is a %1 NOR image. Choose %1.").arg(QString::fromUtf8(other->displayName));
    }
    if (device == Device::e2 && header != Header::e2)
        return header == Header::e2WrongPlace ? QStringLiteral("This E2 NOR image doesn't have its header at 0x20000.")
               : header == Header::other ? QStringLiteral("This NOR image is not for a Tungsten E2.")
                                         : QStringLiteral("This NOR image has no Tungsten E2 header. Check the device choice.");
    const auto info = inspectNor(nor);
    const auto& selected = profile(device);
    if (info.profile && info.profile != &selected && info.profile->deviceType != deviceTypeE2 &&
        info.profile->deviceType != deviceTypeZire72)
        return QStringLiteral("This is a %1 NOR image. Choose %1.").arg(QString::fromUtf8(info.profile->displayName));
    if (device != Device::tx && device != Device::e2 && device != Device::zire72) {
        if (info.profile && info.profile != &selected)
            return QStringLiteral("This is a %1 NOR image. Choose %1.").arg(QString::fromUtf8(info.profile->displayName));
        if (selected.detect == DeviceDetect5::cardHeader && info.profile != &selected)
            return QStringLiteral("This NOR image has no %1 header.").arg(deviceName(device));
        if (!info.validLayout) return QStringLiteral("This %1 NOR image has an invalid card layout.").arg(deviceName(device));
        if (!selected.nandSize && !nandPath.isEmpty()) return QStringLiteral("This device has no NAND. Leave the NAND image empty.");
    }
    const auto* profile = &frondFirmware::profile(device);
    const bool nandGiven = !nandPath.isEmpty();
    QFile nand(nandPath);
    if (profile->requiresExplicitNand || nandGiven) {
        if (!QFileInfo(nandPath).isFile() || !nand.open(QIODevice::ReadOnly))
            return device == Device::tx ? QStringLiteral("Choose readable NOR and NAND firmware files for the Palm TX.")
                                        : QStringLiteral("Choose a readable NAND file, or leave it empty.");
    }
    if (deviceProfileValidateFirmware5(profile->deviceType, nor.size(), nandGiven ? nand.size() : profile->nandSize,
                                       false, nandGiven, info.variant) != DeviceFirmwareError5::none)
        return device == Device::tx ? QStringLiteral("Choose a 16 MiB NOR image and a 132 MiB raw NAND image for the Palm TX.")
               : device == Device::zire72 ? QStringLiteral("Choose a 14,942,208-byte NOR image for the Zire 72.")
                                    : device != Device::e2 ? QStringLiteral("The firmware sizes do not match %1.").arg(deviceName(device))
                                    : nor.size() != qint64(info.variant ? info.variant->norSize : profile->norSize)
                                          ? QStringLiteral("Choose an 8 MiB NOR image or a supported de-NVFS image for the Tungsten E2.")
                                          : QStringLiteral("The Tungsten E2 NAND image must be a raw 34,603,008-byte file, or empty.");
    if (resolved) *resolved = device;
    return {};
}

inline QString validateTxFiles(const QString& norPath, const QString& nandPath) {
    return validateFiles(Device::tx, norPath, nandPath);
}
}
#endif
