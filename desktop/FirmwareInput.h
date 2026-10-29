#ifndef FROND_FIRMWARE_INPUT_H
#define FROND_FIRMWARE_INPUT_H

#include <QFile>
#include <QFileInfo>
#include <QString>
#include "device_profile5.h"

namespace frondFirmware {
enum class Device { automatic, tx, e2 };

inline QString deviceName(Device device) {
    return device == Device::tx ? QStringLiteral("Palm TX") : QStringLiteral("Tungsten E2");
}

// Which settings a device supports. One place for the launcher and the running state.
struct Capabilities { bool network, keyboard, serial, usb, fastAudio, infrared, bluetooth; };
inline Capabilities capabilities(Device device) {
    if (device == Device::e2) return {false, true, false, true, true, true, true};
    return {true, true, true, true, true, true, true};
}

// Looks for the Palm card header the way the core does (every 32 KiB) and says
// whether it names an E2. 'other' is a header for another device, 'none' no header.
enum class Header { none, e2, e2WrongPlace, other };
inline Header norHeader(QFile& nor) {
    const quint32 palm = 0x50616c6d, hspr = 0x68737072, signature = 0xfeedbeef;
    auto read32 = [&](qint64 at, quint32& value) {
        uchar bytes[4];
        if (!nor.seek(at) || nor.read(reinterpret_cast<char*>(bytes), 4) != 4) return false;
        value = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (quint32(bytes[3]) << 24);
        return true;
    };
    for (qint64 at = 0; at + 12 <= nor.size(); at += 32 * 1024) {
        quint32 sig, company, hal;
        if (!read32(at + 8, sig) || sig != signature) continue;
        if (!read32(at + 0x80, company) || !read32(at + 0x84, hal)) return Header::none;
        if (company == palm && hal == hspr) return size_t(at) == e2NorHeaderOffset5 ? Header::e2 : Header::e2WrongPlace;
        return Header::other;
    }
    return Header::none;
}

// Picks the device from the NOR: an E2 card header, else the Palm TX size.
// Returns automatic when it can't tell.
inline Device detectDevice(const QString& norPath) {
    QFile nor(norPath);
    if (!QFileInfo(norPath).isFile() || !nor.open(QIODevice::ReadOnly)) return Device::automatic;
    const auto header = norHeader(nor);
    if (header == Header::e2 || header == Header::e2WrongPlace) return Device::e2;
    if (size_t(nor.size()) == deviceProfile5(deviceTypeTX)->norSize) return Device::tx;
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
    if (device == Device::e2 && header != Header::e2)
        return header == Header::e2WrongPlace ? QStringLiteral("This E2 NOR image doesn't have its header at 0x20000.")
               : header == Header::other ? QStringLiteral("This NOR image is not for a Tungsten E2.")
                                         : QStringLiteral("This NOR image has no Tungsten E2 header. Check the device choice.");
    const auto* profile = deviceProfile5(device == Device::tx ? deviceTypeTX : deviceTypeE2);
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
