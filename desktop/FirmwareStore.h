#ifndef FROND_FIRMWARE_STORE_H
#define FROND_FIRMWARE_STORE_H

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>
#include "FirmwareInput.h"
#include "../zodiac-rom/ZodiacRom.h"

// Firmware copies the desktop keeps: <root>/<device name>/nor.bin and nand.bin.
namespace frondFirmware {
inline QString folder(const QString& root, Device device) { return root + '/' + deviceId(device); }
inline QString storedNor(const QString& root, Device device) { return folder(root, device) + "/nor.bin"; }
inline QString storedNand(const QString& root, Device device) {
    const auto path = folder(root, device) + "/nand.bin";
    return QFileInfo(path).isFile() ? path : QString();
}

// The Zodiac's NOR can also be picked as Tapwave's Zodiac 1.1 update (the zip or the PRC), which holds the ROM.
inline bool takesZodiacUpdate(Device device) {
    return device == Device::automatic || profile(device).board == DeviceBoard5::imx1Zodiac;
}

// The file to use as the NOR: the ROM made from a Zodiac update into 'scratch', or 'path' itself for anything
// else, so the usual checks still apply. Empty with 'error' set for an update that isn't the right one.
inline QString norFile(const QString& path, Device device, QTemporaryDir& scratch, QString* error = nullptr) {
    if (!takesZodiacUpdate(device) || !QFileInfo(path).isFile()) return path;
    const auto result = frondZodiacRom::extractFile(path.toStdString());
    if (result.status == frondZodiacRom::Result::Status::notUpdate) return path;
    if (result.status == frondZodiacRom::Result::Status::error) {
        if (error) *error = QString::fromStdString(result.error);
        return {};
    }
    const auto rom = scratch.filePath("zodiac.rom");
    QFile out(rom);
    if (!scratch.isValid() || !out.open(QIODevice::WriteOnly) ||
        out.write(reinterpret_cast<const char*>(result.rom.data()), qint64(result.rom.size())) != qint64(result.rom.size())) {
        if (error) *error = QStringLiteral("Couldn't save the firmware.");
        return {};
    }
    return rom;
}

// The device a picked NOR is for, also from a Zodiac update.
inline Device detectPicked(const QString& path) {
    QTemporaryDir scratch;
    const auto file = norFile(path, Device::automatic, scratch);
    return file.isEmpty() ? Device::automatic : detectDevice(file);
}

// The stored images pass the same checks as an import.
inline bool ready(const QString& root, Device device) {
    if (device == Device::automatic) return false;
    return validateFiles(device, storedNor(root, device), storedNand(root, device)).isEmpty();
}

// Validates, then copies the images in (to .part files first, so a failure leaves the old ones).
// A device whose NAND is optional loses its old NAND when the new import has none.
// 'resolved' gets the device the images were stored for. Returns an error message or empty.
inline QString importImages(const QString& root, Device chosen, const QString& norPicked, const QString& nandPath,
                            Device* resolved = nullptr) {
    Device device = chosen;
    QTemporaryDir scratch;
    QString updateError;
    const auto norPath = norPicked.isEmpty() ? norPicked : norFile(norPicked, chosen, scratch, &updateError);
    if (!updateError.isEmpty()) return updateError;
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
