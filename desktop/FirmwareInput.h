#ifndef FROND_FIRMWARE_INPUT_H
#define FROND_FIRMWARE_INPUT_H

#include <QFile>
#include <QFileInfo>
#include <QString>
#include "device_profile5.h"

namespace frondFirmware {
// Readability and geometry only; no firmware identity or boot claim.
inline QString validateTxFiles(const QString& norPath, const QString& nandPath) {
    QFile nor(norPath), nand(nandPath);
    if (!QFileInfo(norPath).isFile() || !QFileInfo(nandPath).isFile() ||
        !nor.open(QIODevice::ReadOnly) || !nand.open(QIODevice::ReadOnly))
        return QStringLiteral("Choose readable NOR and NAND firmware files for the Palm TX.");
    if (deviceProfileValidateFirmware5(deviceTypeTX, nor.size(), nand.size(), false, true) !=
        DeviceFirmwareError5::none)
        return QStringLiteral("Choose a 16 MiB NOR image and a 132 MiB raw NAND image for the Palm TX.");
    return {};
}
}
#endif
