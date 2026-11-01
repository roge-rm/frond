#ifndef FROND_FIRMWARE_INPUT_H
#define FROND_FIRMWARE_INPUT_H

#include <QFile>
#include <QFileInfo>
#include <QString>
#include "device_profile5.h"

namespace frondFirmware {
// The devices the desktop starts. Settings and the core get the device's fixed name from
// device_profile5.h, never this enum.
enum class Device { automatic, tx, e2, zire72 };
inline constexpr Device devices[] = {Device::tx, Device::e2, Device::zire72};

// The device's table entry; automatic counts as the TX.
inline const DeviceProfile5& profile(Device device) {
    return *deviceProfileNamed5(device == Device::e2 ? "palm-e2" : device == Device::zire72 ? "palm-zire-72" : "palm-tx");
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

// Looks for the Palm card header the way the core does (every 32 KiB) and says
// whether it names an E2 or a Zire 72. 'other' is a header for another device, 'none' no header.
// A Zire 72 NOR has a small card at 0x0 and a big card at 0x20000 to the end, both PalmCard aZ72.
enum class Header { none, e2, e2WrongPlace, zire72, zire72BadLayout, other };
inline Header norHeader(QFile& nor) {
    const quint32 palm = 0x50616c6d, hspr = 0x68737072, az72 = 0x615a3732, signature = 0xfeedbeef;
    auto read32 = [&](qint64 at, quint32& value) {
        uchar bytes[4];
        if (!nor.seek(at) || nor.read(reinterpret_cast<char*>(bytes), 4) != 4) return false;
        value = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (quint32(bytes[3]) << 24);
        return true;
    };
    auto palmCard = [&](qint64 at) {
        if (!nor.seek(at + 0x10)) return false;
        return nor.read(9) == QByteArray("PalmCard", 9);
    };
    for (qint64 at = 0; at + 12 <= nor.size(); at += 32 * 1024) {
        quint32 sig, company, hal;
        if (!read32(at + 8, sig) || sig != signature) continue;
        if (!read32(at + 0x80, company) || !read32(at + 0x84, hal)) return Header::none;
        if (company == palm && hal == hspr) return size_t(at) == e2NorHeaderOffset5 ? Header::e2 : Header::e2WrongPlace;
        if (company == palm && hal == az72) {
            const qint64 big = 0x20000;
            quint32 bigSig, bigCompany, bigHal, bigLength;
            const bool cards = at == 0 && palmCard(0) && read32(big + 8, bigSig) && bigSig == signature &&
                               palmCard(big) && read32(big + 0x80, bigCompany) && bigCompany == palm &&
                               read32(big + 0x84, bigHal) && bigHal == az72 && read32(big + 0x6c, bigLength) &&
                               big + bigLength == nor.size();
            return cards ? Header::zire72 : Header::zire72BadLayout;
        }
        return Header::other;
    }
    return Header::none;
}

// Picks the device from the NOR: an E2 or Zire 72 card header, else the Palm TX size.
// Returns automatic when it can't tell.
inline Device detectDevice(const QString& norPath) {
    QFile nor(norPath);
    if (!QFileInfo(norPath).isFile() || !nor.open(QIODevice::ReadOnly)) return Device::automatic;
    const auto header = norHeader(nor);
    if (header == Header::e2 || header == Header::e2WrongPlace) return Device::e2;
    if (header == Header::zire72 || header == Header::zire72BadLayout) return Device::zire72;
    if (size_t(nor.size()) == profile(Device::tx).norSize) return Device::tx;
    return Device::automatic;
}

// Readability, geometry and which device the NOR header names; no boot claim.
// A chosen device is never swapped for another one. 'resolved' gets the device to start.
inline QString validateFiles(Device chosen, const QString& norPath, const QString& nandPath, Device* resolved = nullptr) {
    QFile nor(norPath);
    if (!QFileInfo(norPath).isFile() || !nor.open(QIODevice::ReadOnly))
        return QStringLiteral("Choose a readable NOR firmware file.");
    const auto header = norHeader(nor);
    Device device = chosen;
    if (device == Device::automatic) {
        device = detectDevice(norPath);
        if (device == Device::automatic)
            return QStringLiteral("Frond can't tell which device this NOR image is for. Choose the device.");
    }
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
    if (device == Device::e2 && header != Header::e2)
        return header == Header::e2WrongPlace ? QStringLiteral("This E2 NOR image doesn't have its header at 0x20000.")
               : header == Header::other ? QStringLiteral("This NOR image is not for a Tungsten E2.")
                                         : QStringLiteral("This NOR image has no Tungsten E2 header. Check the device choice.");
    const auto* profile = &frondFirmware::profile(device);
    const bool nandGiven = !nandPath.isEmpty();
    QFile nand(nandPath);
    if (device == Device::tx || nandGiven) {
        if (!QFileInfo(nandPath).isFile() || !nand.open(QIODevice::ReadOnly))
            return device == Device::tx ? QStringLiteral("Choose readable NOR and NAND firmware files for the Palm TX.")
                                        : QStringLiteral("Choose a readable NAND file, or leave it empty.");
    }
    if (deviceProfileValidateFirmware5(profile->deviceType, nor.size(), nandGiven ? nand.size() : profile->nandSize,
                                       false, nandGiven) != DeviceFirmwareError5::none)
        return device == Device::tx ? QStringLiteral("Choose a 16 MiB NOR image and a 132 MiB raw NAND image for the Palm TX.")
               : device == Device::zire72 ? QStringLiteral("Choose a 14,942,208-byte NOR image for the Zire 72.")
                                    : nor.size() != qint64(profile->norSize)
                                          ? QStringLiteral("Choose an 8 MiB NOR image for the Tungsten E2.")
                                          : QStringLiteral("The Tungsten E2 NAND image must be a raw 34,603,008-byte file, or empty.");
    if (resolved) *resolved = device;
    return {};
}

inline QString validateTxFiles(const QString& norPath, const QString& nandPath) {
    return validateFiles(Device::tx, norPath, nandPath);
}
}
#endif
