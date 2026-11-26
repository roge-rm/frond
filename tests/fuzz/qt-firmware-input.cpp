// The desktop's firmware checks (FirmwareInput.h): device detection from the NOR or 68k ROM header,
// the per-device validation and the rx1950 OS image scan, on files in the scratch folder. QtCore only.
#include <unistd.h>

#include <QFile>
#include <fstream>

#include "FirmwareInput.h"
#include "Scratch.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    using namespace frondFirmware;
    static unsigned runs = 0;
    if (!size) return 0;
    // A new name each run: the checks keep results by path, size and modified time.
    const auto folder = FuzzScratch("qt-firmware-input", runs);
    const auto nor = QString::fromStdString(folder + "/nor-" + std::to_string(runs));
    const auto nand = QString::fromStdString(folder + "/nand-" + std::to_string(runs));
    const size_t split = bytes[0] * size / 256;
    std::ofstream(nor.toStdString(), std::ios::binary).write(reinterpret_cast<const char*>(bytes + 1), std::streamsize(size - 1));
    std::ofstream(nand.toStdString(), std::ios::binary).write(reinterpret_cast<const char*>(bytes + 1 + std::min(split, size - 1)),
                                                              std::streamsize(size - 1 - std::min(split, size - 1)));
    detectDevice(nor);
    QFile rom(nor);
    if (rom.open(QIODevice::ReadOnly)) {
        inspectRom68k(rom);
        norHeader(rom);
    }
    rx1950OsImage(nand);
    Device resolved{};
    validateFiles(Device::automatic, nor, QString(), &resolved);
    for (const auto device : devices) {
        validateFiles(device, nor, nand, &resolved, (bytes[0] & 1) != 0, nor);
        importWarning(device, nand);
    }
    validateTxFiles(nor, nand);
    QFile::remove(nor);
    QFile::remove(nand);
    return 0;
}
