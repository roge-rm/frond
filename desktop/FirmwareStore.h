#ifndef FROND_FIRMWARE_STORE_H
#define FROND_FIRMWARE_STORE_H

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include "FirmwareInput.h"

// Firmware copies the desktop keeps: <root>/<device name>/nor.bin and nand.bin.
namespace frondFirmware {
inline QString folder(const QString& root, Device device) { return root + '/' + deviceId(device); }
inline QString storedNor(const QString& root, Device device) { return folder(root, device) + "/nor.bin"; }
inline QString storedNand(const QString& root, Device device) {
    const auto path = folder(root, device) + "/nand.bin";
    return QFileInfo(path).isFile() ? path : QString();
}

// The stored images pass the same checks as an import.
inline bool ready(const QString& root, Device device) {
    if (device == Device::automatic) return false;
    return validateFiles(device, storedNor(root, device), storedNand(root, device)).isEmpty();
}

// Validates, then copies the images in (to .part files first, so a failure leaves the old ones).
// A device whose NAND is optional loses its old NAND when the new import has none.
// 'resolved' gets the device the images were stored for. Returns an error message or empty.
inline QString importImages(const QString& root, Device chosen, const QString& norPath, const QString& nandPath,
                            Device* resolved = nullptr) {
    Device device = chosen;
    if (const auto error = validateFiles(chosen, norPath, nandPath, &device); !error.isEmpty()) return error;
    const auto directory = folder(root, device);
    if (!QDir().mkpath(directory)) return QStringLiteral("Couldn't save the firmware.");
    const auto nor = storedNor(root, device), nand = directory + "/nand.bin";
    auto cleanup = [&] { QFile::remove(nor + ".part"); QFile::remove(nand + ".part"); };
    cleanup();
    if (!QFile::copy(norPath, nor + ".part") || (!nandPath.isEmpty() && !QFile::copy(nandPath, nand + ".part"))) {
        cleanup();
        return QStringLiteral("Couldn't save the firmware.");
    }
    QFile::remove(nor);
    if (!QFile::rename(nor + ".part", nor)) { cleanup(); return QStringLiteral("Couldn't save the firmware."); }
    QFile::remove(nand);
    if (!nandPath.isEmpty() && !QFile::rename(nand + ".part", nand)) { cleanup(); return QStringLiteral("Couldn't save the firmware."); }
    if (resolved) *resolved = device;
    return {};
}
}
#endif
