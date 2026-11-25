#ifndef FROND_FIRMWARE_INPUT_H
#define FROND_FIRMWARE_INPUT_H

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
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
// A 68k device whose slots take linked card files through frond-68k's sd-, sd2- and ms- commands:
// the Dana's two slots and a VZ Palm's one.
inline bool linkedCards(Device device) {
    return runs68k(device) && (deviceProfileLinkedCards5(profile(device)) || deviceProfileLinkedStick5(profile(device)));
}
// A 68k grey LCD has a contrast setting; a colour one is shown as the guest draws it.
inline bool lcdContrast(Device device) { return runs68k(device) && !deviceProfileColourLcd5(int(device)); }

// Install, the application list and hard reset. Every Palm OS device has them; callers also check palmOs.
inline bool guestServices(Device) { return true; }
// Install, the application list and hard reset are Palm OS guest services. A Windows CE device has its own
// (installsToCard, ceHardReset) and greys out the rest. Callers check this and guestServices.
inline bool palmOs(Device device) {
    return device == Device::automatic || deviceProfileOs5(profile(device)) == DeviceOs5::palmOs;
}
// Windows CE: Install copies the file onto the SD card, where the guest's File Explorer opens it (wceload for a
// CAB); the application list stays greyed. The rx1950 only so far.
inline bool installsToCard(Device device) {
    return device != Device::automatic && profile(device).board == DeviceBoard5::s3c2442Rx1950;
}
// Windows CE hard reset: a clean boot with the user store wiped (the rx1950 only so far).
inline bool ceHardReset(Device device) { return installsToCard(device); }
// The device's USB is a network link (RNDIS) with no host endpoint: on or off.
inline bool usbNetwork(Device device) { return device != Device::automatic && !palmOs(device); }
// New machines of these devices start on the Fast engine: the Zodiac, rx1950, T|X and Zire 72, which fall behind
// Original speed on Exact.
inline bool fastByDefault(Device device) {
    return device == Device::tx || device == Device::zire72 ||
           (device != Device::automatic && (profile(device).board == DeviceBoard5::imx1Zodiac ||
                                            profile(device).board == DeviceBoard5::s3c2442Rx1950));
}
// Whether the file at path is the firmware file by size and SHA-256. The hash of a file is kept by path,
// size and modified time, so checking the 67 MB CEOS.nbf again (each time the dialog validates) is free.
// UI thread only.
inline bool firmwareFileMatches(const QString& path, const DeviceFirmwareFile5& expected) {
    const QFileInfo info(path);
    if (!info.isFile() || info.size() != qint64(expected.bytes)) return false;
    struct Hashed { qint64 size; QDateTime modified; QByteArray sha256; };
    static QHash<QString, Hashed> hashes;
    const QString key = info.absoluteFilePath();
    const QDateTime modified = info.lastModified();
    auto it = hashes.constFind(key);
    if (it == hashes.constEnd() || it->size != info.size() || it->modified != modified) {
        QFile file(path);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) return false;
        it = hashes.insert(key, {info.size(), modified, hash.result().toHex()});
    }
    return it->sha256 == QByteArray(expected.sha256);
}

// The rx1950's OS image: HP's CEOS.nbf (by SHA-256), another .nbf that rx1950NandImage can lay out (a header, then
// 512-byte sectors each with 8 bytes of SectorInfo, the logical sector erased or inside the NAND), or neither.
// Kept by path, size and modified time like firmwareFileMatches. UI thread only.
enum class Rx1950Os { unusable, hp, other };
inline Rx1950Os rx1950OsImage(const QString& path) {
    constexpr qint64 header = 0xa5a, sector = 520, maxSectors = (4096 - 21) * 32;
    const QFileInfo info(path);
    if (!info.isFile() || info.size() < header || (info.size() - header) % sector || (info.size() - header) / sector > maxSectors)
        return Rx1950Os::unusable;
    if (firmwareFileMatches(path, rx1950Ceos5)) return Rx1950Os::hp;
    struct Scanned { qint64 size; QDateTime modified; bool ok; };
    static QHash<QString, Scanned> scans;
    const QString key = info.absoluteFilePath();
    auto it = scans.constFind(key);
    if (it == scans.constEnd() || it->size != info.size() || it->modified != info.lastModified()) {
        bool ok = false;
        QFile file(path);
        if (file.open(QIODevice::ReadOnly) && file.seek(header)) {
            ok = true;
            while (ok && !file.atEnd()) {
                const QByteArray chunk = file.read(sector * 2000);
                if (chunk.isEmpty() || chunk.size() % sector) { ok = false; break; }
                for (qint64 at = 0; at < chunk.size(); at += sector) {
                    const auto* p = reinterpret_cast<const uint8_t*>(chunk.constData() + at + 512);
                    const uint32_t logical = p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24;
                    if (logical != 0xffffffffu && logical >= uint32_t(maxSectors)) { ok = false; break; }
                }
            }
        }
        it = scans.insert(key, {info.size(), info.lastModified(), ok});
    }
    return it->ok ? Rx1950Os::other : Rx1950Os::unusable;
}
inline QString rx1950OsMissing() { return QStringLiteral("Choose the OS image (CEOS.nbf) for the HP iPAQ rx1950."); }
inline QString rx1950OsUnusable() {
    return QStringLiteral("The OS image isn't a usable .nbf (512-byte sectors with SectorInfo). Choose CEOS.nbf or another OS .nbf.");
}
inline QString rx1950OsWarning() { return QStringLiteral("This OS image isn't HP's tested CEOS.nbf, so it may not boot."); }
// A warning for an OS image that isn't HP's, or nothing.
inline QString importWarning(Device device, const QString& nandPath, bool anyFirmware = false) {
    if (anyFirmware || device == Device::automatic || profile(device).board != DeviceBoard5::s3c2442Rx1950 || nandPath.isEmpty()) return {};
    return rx1950OsImage(nandPath) == Rx1950Os::other ? rx1950OsWarning() : QString();
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
    // The Dana Wireless runs the Dana's ROM, and the EZ devices share ROMs the header can't tell apart.
    if (!deviceProfileTakes68kRom5(profile(device), info))
        return QStringLiteral("This is a %1 ROM. Choose %1.").arg(QString::fromUtf8(info.profile->displayName));
    if (!nandPath.isEmpty()) return QStringLiteral("The %1 has no NAND. Leave the NAND image empty.").arg(name);
    return {};
}

// Readability, geometry and which device the NOR header names; no boot claim.
// A chosen device is never swapped for another one. 'resolved' gets the device to start.
// With anyFirmware on, the rx1950 takes any NOR of HP's size, and an empty one reuses storedNorPath.
inline QString validateFiles(Device chosen, const QString& norPicked, const QString& nandPath, Device* resolved = nullptr,
                             bool anyFirmware = false, const QString& storedNorPath = {}) {
    const bool anyRx1950 = anyFirmware && chosen != Device::automatic && profile(chosen).board == DeviceBoard5::s3c2442Rx1950;
    if (anyRx1950 && norPicked.isEmpty() && !QFileInfo(storedNorPath).isFile())
        return QStringLiteral("Choose a NOR image (CEBL.nbf). There's no earlier one to reuse.");
    const QString norPath = anyRx1950 && norPicked.isEmpty() ? storedNorPath : norPicked;
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
    if (profile(device).board == DeviceBoard5::s3c2442Rx1950) {
        if (nandPath.isEmpty()) return rx1950OsMissing();
        if (anyFirmware && QFileInfo(norPath).size() != qint64(rx1950Cebl5.bytes))
            return QStringLiteral("The NOR image must be a 264,794-byte CEBL.nbf.");
        if (!anyFirmware && !firmwareFileMatches(norPath, rx1950Cebl5))
            return QStringLiteral("The NOR image must be CEBL.nbf from HP's WM5 ROM 1.01.00 ENG.");
        if (rx1950OsImage(nandPath) == Rx1950Os::unusable) return rx1950OsUnusable();
        if (resolved) *resolved = device;
        return {};
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
