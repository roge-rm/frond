#include "spitz_pcmcia.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string_view>
#include <sys/file.h>

#include "Dcf660w.h"
#include "FileCardBacking.h"
#include "PosixCompat.h"
#include "mem.h"
#include "pxa_GPIO.h"
#include "savestate/savestateAll.h"
#include "sdcard.h"
#include "spitz_cf_ethernet.h"
#include "storage/Io.h"

namespace {
constexpr uint32_t scoopBase[] = {0x10800000, 0x08800040};
constexpr uint32_t scoopSize = 0x2c;
constexpr uint32_t socketBase[] = {0x20000000, 0x30000000};
constexpr uint32_t socketSize = 0x10000000;
constexpr uint32_t spaceSize = 0x04000000;
constexpr unsigned cfSlot = 0, microdriveSlot = 1;
constexpr unsigned detectPin[] = {94, 93}, readyPin[] = {105, 106};

enum ScoopRegister {
    mcr, cdr, csr, cpr, ccr, irm, imr, isr, gpcr, gpwr, gprr,
};
constexpr uint16_t cardVoltage = 0x03, sdPower = 0x04, powerOutput = 0x80;
constexpr uint16_t powerGood = 0x8040;
constexpr uint16_t cardReset = 0x80;
constexpr uint16_t ioCard = 0x10;
constexpr uint16_t socketReady = 0x02;
constexpr uint16_t socketEmpty = 0x04;
constexpr uint16_t batteryGood = 0x30;
constexpr uint16_t noXVoltage = 0x80;

constexpr uint8_t driveBusy = 0x80, driveReady = 0x40, driveSeekDone = 0x10;
constexpr uint8_t driveDrq = 0x08, driveError = 0x01;
constexpr uint8_t ataAbort = 0x04, ataIdNotFound = 0x10, ataUncorrectable = 0x40;
constexpr uint8_t disableIrq = 0x02, softwareReset = 0x04;
constexpr unsigned sectorBytes = 512;
constexpr uint32_t cylinderSectors = 16 * 63;
constexpr uint32_t maxSectors = 1u << 28;
constexpr uint8_t maxMultiple = 16;
constexpr uint32_t configBase = 0x200;
constexpr uint8_t configReset = 0x80, configModeMask = 0x3f;
constexpr uint32_t savestateVersion = 4;
using MediaId = std::array<uint8_t, SD_CARD_ID_MAX_LEN>;

bool validMicrodriveId(std::string_view id) {
    return id.size() == MediaId{}.size() &&
           std::all_of(id.begin(), id.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool syncFile(int fd) {
    int result;
    do result = fsync(fd); while (result != 0 && errno == EINTR);
    return result == 0;
}

enum class Transfer : uint8_t { none, identify, read, write };

// CF fixed-disk, flat 16-port I/O mode. Attribute bytes appear at even bus addresses.
constexpr uint8_t microdriveCis[] = {
    0x01, 3, 0xdb, 0x01, 0xff,  // DEVICE, 2 KiB function-specific
    0x15, 22, 4, 1, 'H','I','T','A','C','H','I', 0,
                    'm','i','c','r','o','d','r','i','v','e', 0, 0xff,
    0x21, 2, 4, 1,              // FUNCID, fixed disk
    0x22, 2, 1, 1,              // FUNCE, ATA interface
    0x1a, 5, 1, 1, 0, 2, 0x0f, // CONFIG, registers at 0x200
    0x1b, 8, 0xc1, 0x41, 0x19, 1, 0xb5, 0x1e, 0x64, 0x20, // CFTABLE, 3.3 V, level IRQ
    0x14, 0,                   // NO_LINK
    0xff,
};

constexpr uint8_t compactFlashCis[] = {
    0x01, 3, 0xdb, 0x01, 0xff,
    0x15, 22, 4, 1, 'F','r','o','n','d', 0,
                    'C','o','m','p','a','c','t','F','l','a','s','h', 0, 0xff,
    0x21, 2, 4, 1,
    0x22, 2, 1, 1,
    0x1a, 5, 1, 1, 0, 2, 0x0f,
    0x1b, 8, 0xc1, 0x41, 0x19, 1, 0xb5, 0x1e, 0x64, 0x20,
    0x14, 0,
    0xff,
};

struct TaskFile {
    uint8_t features = 0;
    uint8_t error = 1;
    uint8_t count = 1;
    uint8_t sector = 1;
    uint8_t cylinderLow = 0;
    uint8_t cylinderHigh = 0;
    uint8_t drive = 0xa0;
    uint8_t status = driveReady | driveSeekDone;
    uint8_t control = 0;
    uint8_t multiple = 0;
    uint8_t transfer = uint8_t(Transfer::none);
    uint8_t blockSize = 1;
    uint8_t inBlock = 0;
    uint16_t position = 0;
    uint16_t remaining = 0;
    uint32_t lba = 0;
    bool irq = false;
    std::array<uint8_t, sectorBytes> data{};

    template <typename T>
    void SaveLoad(T& chunk) {
        chunk.Do8(features).Do8(error).Do8(count).Do8(sector)
            .Do8(cylinderLow).Do8(cylinderHigh).Do8(drive).Do8(status).Do8(control)
            .Do8(multiple).Do8(transfer).Do8(blockSize).Do8(inBlock)
            .Do16(position).Do16(remaining).Do32(lba).DoBool(irq)
            .DoBuffer(data.data(), data.size());
    }
};

void identifyWord(TaskFile& ata, unsigned word, uint16_t value) {
    ata.data[2 * word] = uint8_t(value);
    ata.data[2 * word + 1] = uint8_t(value >> 8);
}

void identifyString(TaskFile& ata, unsigned word, unsigned bytes, std::string_view text) {
    for (unsigned i = 0; i < bytes; ++i)
        ata.data[2 * word + (i ^ 1)] = i < text.size() ? text[i] : ' ';
}
struct Socket {
    PxaGpio* gpio;
    unsigned slot;
    int disk = -1;
    uint64_t diskDevice = 0, diskInode = 0;
    MediaId mediaId{};
    std::shared_ptr<FileCardBacking> overlay{};
    uint64_t cardEpoch = 0;
    uint32_t sectors = 0;
    bool writable = false;
    uint16_t scoop[11]{};
    uint8_t option = 0, configStatus = 0, pins = 0, socketCopy = 0;
    TaskFile ata{};
    std::optional<SpitzCfEthernet> ethernet{};
    std::optional<Dcf660w> wifi{};

    bool Removable() const { return slot == cfSlot; }
    bool Present() const { return ethernet.has_value() || wifi.has_value() || BlockPresent(); }
    bool BlockPresent() const {
        return Removable() ? sectors && cardEpoch == sdCardStore(1).epoch() : disk >= 0;
    }
    bool Powered() const { return Present() && (scoop[cpr] & cardVoltage); }
    bool CardReset() const { return (scoop[ccr] & cardReset) || (wifi ? wifi->InReset() : option & configReset); }
    bool Ready() const { return Powered() && !CardReset(); }

    bool ReadSector() {
        if (Removable()) return sdCardStore1Read(ata.lba, ata.data.data());
        const uint64_t offset = uint64_t(ata.lba) * sectorBytes;
        if (overlay) return overlay->Read(offset, sectorBytes, ata.data.data());
        return frondStorage::readAllAt(disk, ata.data.data(), sectorBytes, offset);
    }

    bool WriteSector() {
        if (Removable()) return sdCardStore1Write(ata.lba, ata.data.data());
        const uint64_t offset = uint64_t(ata.lba) * sectorBytes;
        if (overlay) return overlay->Write(offset, sectorBytes, ata.data.data());
        return frondStorage::writeAllAt(disk, ata.data.data(), sectorBytes, offset);
    }

    bool Flush() const {
        if (!Present() || !writable) return true;
        if (Removable()) return bool(sdCardStore(1).snapshot());
        if (overlay) return bool(overlay->Snapshot());
        return syncFile(disk);
    }

    void UpdatePins() {
        pxaGpioSetState(gpio, detectPin[slot], !Present());
        // READY is active high in memory mode. IREQ is active low in I/O mode.
        const bool request = wifi ? wifi->IrqAsserted() : ethernet ? ethernet->Irq()
                                                 : ata.irq && !(ata.control & (disableIrq | softwareReset));
        const bool io = (scoop[mcr] & ioCard) || (wifi ? wifi->IoConfigured() : option & configModeMask);
        pxaGpioSetState(gpio, readyPin[slot], io ? !Ready() || !request : Ready());
    }

    void ResetAta() {
        ata = {};
        if (ethernet) ethernet->Reset();
        if (wifi) wifi->Reset();
        UpdatePins();
    }

    void ResetCard() {
        option = configStatus = pins = socketCopy = 0;
        ResetAta();
    }

    void Complete(uint8_t error = 0) {
        ata.transfer = uint8_t(Transfer::none);
        ata.remaining = ata.position = 0;
        ata.error = error;
        ata.status = driveReady | driveSeekDone | (error ? driveError : 0);
        ata.irq = true;
        UpdatePins();
    }

    uint16_t Cylinders() const {
        return std::max(1u, std::min(16383u, sectors / cylinderSectors));
    }

    void Identify() {
        ata.data.fill(0);
        const uint16_t cylinders = Cylinders();
        const uint32_t chsSectors = cylinders * cylinderSectors;
        identifyWord(ata, 0, 0x848a);  // CompactFlash ATA device
        identifyWord(ata, 1, cylinders);
        identifyWord(ata, 3, 16);
        identifyWord(ata, 6, 63);
        identifyString(ata, 10, 20, Removable() ? "00000002" : "00000001");
        identifyString(ata, 23, 8, "1.0");
        identifyString(ata, 27, 40, Removable() ? "Frond CompactFlash" : "Frond Microdrive");
        identifyWord(ata, 47, 0x8000 | maxMultiple);
        identifyWord(ata, 49, 1 << 9);  // LBA
        identifyWord(ata, 51, 2 << 8);  // PIO mode 2
        identifyWord(ata, 53, 1);
        identifyWord(ata, 54, cylinders);
        identifyWord(ata, 55, 16);
        identifyWord(ata, 56, 63);
        identifyWord(ata, 57, chsSectors);
        identifyWord(ata, 58, chsSectors >> 16);
        identifyWord(ata, 59, 0x100 | ata.multiple);
        identifyWord(ata, 60, sectors);
        identifyWord(ata, 61, sectors >> 16);
        identifyWord(ata, 80, 0x001e);  // ATA-1 through ATA-4
        ata.transfer = uint8_t(Transfer::identify);
        ata.position = 0;
        ata.status = driveReady | driveSeekDone | driveDrq;
        ata.irq = true;
        UpdatePins();
    }

    uint32_t CurrentLba() const {
        const uint32_t cylinder = uint16_t(ata.cylinderHigh) << 8 | ata.cylinderLow;
        const uint32_t head = ata.drive & 0x0f;
        if (ata.drive & 0x40) return (head << 24) | (cylinder << 8) | ata.sector;
        if (!ata.sector || ata.sector > 63) return maxSectors;
        return cylinder * cylinderSectors + head * 63 + ata.sector - 1;
    }

    void Advance() {
        if (!--ata.remaining) return;
        ++ata.lba;
        --ata.count;
        if (ata.drive & 0x40) {
            ata.sector = ata.lba;
            ata.cylinderLow = ata.lba >> 8;
            ata.cylinderHigh = ata.lba >> 16;
            ata.drive = (ata.drive & 0xf0) | (ata.lba >> 24);
        } else {
            const auto cylinder = ata.lba / cylinderSectors;
            ata.cylinderLow = cylinder;
            ata.cylinderHigh = cylinder >> 8;
            ata.drive = (ata.drive & 0xf0) | (ata.lba / 63 % 16);
            ata.sector = ata.lba % 63 + 1;
        }
    }

    void BeginSector(bool interrupt) {
        ata.position = 0;
        if (ata.transfer == uint8_t(Transfer::read) && !ReadSector()) {
            Complete(ataUncorrectable);
            return;
        }
        ata.status = driveReady | driveSeekDone | driveDrq;
        if (interrupt) {
            ata.irq = true;
            UpdatePins();
        }
    }

    void FinishSector() {
        if (ata.transfer == uint8_t(Transfer::identify)) {
            ata.transfer = uint8_t(Transfer::none);
            ata.position = 0;
            ata.status = driveReady | driveSeekDone;
            return;
        }
        const bool write = ata.transfer == uint8_t(Transfer::write);
        if (write && !WriteSector()) {
            Complete(ataUncorrectable);
            return;
        }
        Advance();
        if (!ata.remaining) {
            ata.transfer = uint8_t(Transfer::none);
            ata.position = 0;
            ata.status = driveReady | driveSeekDone;
            ata.count = 0;
            if (write) {
                ata.irq = true;
                UpdatePins();
            }
            return;
        }
        const bool newBlock = ++ata.inBlock == ata.blockSize;
        if (newBlock) ata.inBlock = 0;
        BeginSector(newBlock);
    }

    void StartTransfer(bool write, bool multiple) {
        const auto lba = CurrentLba();
        const unsigned count = ata.count ? ata.count : 256;
        if ((write && !writable) || (multiple && !ata.multiple)) {
            Complete(ataAbort);
            return;
        }
        if (lba >= sectors || count > sectors - lba) {
            Complete(ataIdNotFound);
            return;
        }
        ata.lba = lba;
        ata.remaining = count;
        ata.inBlock = 0;
        ata.blockSize = multiple ? ata.multiple : 1;
        ata.transfer = uint8_t(write ? Transfer::write : Transfer::read);
        BeginSector(!write);
    }

    void Command(uint8_t op) {
        ata.irq = false;
        ata.error = 0;
        UpdatePins();

        switch (op) {
            case 0xec:
                Identify();
                break;
            case 0x20: case 0x21:
                StartTransfer(false, false);
                break;
            case 0x30: case 0x31:
                StartTransfer(true, false);
                break;
            case 0xc4:
                StartTransfer(false, true);
                break;
            case 0xc5:
                StartTransfer(true, true);
                break;
            case 0xc6:  // SET MULTIPLE MODE
                if (ata.count <= maxMultiple && !(ata.count & (ata.count - 1))) {
                    ata.multiple = ata.count;
                    Complete();
                } else {
                    Complete(ataAbort);
                }
                break;
            case 0x40: case 0x41: {  // READ VERIFY
                const auto lba = CurrentLba();
                const unsigned count = ata.count ? ata.count : 256;
                Complete(lba < sectors && count <= sectors - lba ? 0 : ataIdNotFound);
                break;
            }
            case 0xe7:  // FLUSH CACHE
                Complete(Flush() ? 0 : ataUncorrectable);
                break;
            case 0xe5:  // CHECK POWER MODE
                ata.count = 0xff;
                Complete();
                break;
            case 0x10: case 0x70: case 0x91:  // RECALIBRATE, SEEK, INITIALIZE DEVICE PARAMETERS
                Complete();
                break;
            case 0xef:  // SET FEATURES, PIO transfer mode
                Complete(ata.features == 0x03 && (ata.count <= 1 || (ata.count >= 8 && ata.count <= 12))
                             ? 0 : ataAbort);
                break;
            default:
                Complete(ataAbort);
                break;
        }
    }

    void DataAccess(uint_fast8_t size, bool write, uint8_t* bytes) {
        while (size) {
            const auto transfer = Transfer(ata.transfer);
            if ((write && transfer != Transfer::write) ||
                (!write && transfer != Transfer::identify && transfer != Transfer::read)) {
                if (!write) memset(bytes, 0xff, size);
                return;
            }
            const unsigned count = std::min(unsigned(size), sectorBytes - ata.position);
            auto* position = ata.data.data() + ata.position;
            if (write) memcpy(position, bytes, count);
            else memcpy(bytes, position, count);
            ata.position += count;
            if (ata.position == sectorBytes) FinishSector();
            bytes += count;
            size -= count;
        }
    }

    uint8_t ReadRegister(uint32_t reg) {
        if (reg == 7) {
            ata.irq = false;
            UpdatePins();
        }
        if (ata.drive & 0x10) return reg == 7 || reg == 0xe ? 0 : 0xff;
        if (ata.control & softwareReset) return driveBusy;
        switch (reg) {
            case 1: case 0xd: return ata.error;
            case 2: return ata.count;
            case 3: return ata.sector;
            case 4: return ata.cylinderLow;
            case 5: return ata.cylinderHigh;
            case 6: return ata.drive;
            case 7: case 0xe: return ata.status;
            case 0xf: return 0xc2 | (((~unsigned(ata.drive)) << 2) & 0x3c);
            default: return 0xff;
        }
    }

    void WriteRegister(uint32_t reg, uint8_t value) {
        if (reg == 0xe) {
            if ((ata.control ^ value) & softwareReset) ResetAta();
            ata.control = value;
            UpdatePins();
        } else if (reg == 6) {
            ata.drive = value;
        } else if (!(ata.control & softwareReset) && !(ata.drive & 0x10)) {
            switch (reg) {
                case 1: case 0xd: ata.features = value; break;
                case 2: ata.count = value; break;
                case 3: ata.sector = value; break;
                case 4: ata.cylinderLow = value; break;
                case 5: ata.cylinderHigh = value; break;
                case 7: Command(value); break;
            }
        }
    }

    uint8_t Attribute(uint32_t offset, bool write, uint8_t value) {
        if (!Powered() || (offset & 1)) return 0xff;
        if (wifi) {
            if (scoop[ccr] & cardReset) return 0xff;
            if (write) {
                wifi->WriteAttribute(offset, value);
                UpdatePins();
            }
            return wifi->ReadAttribute(offset);
        }
        switch (offset) {
            case configBase:
                if (write) {
                    option = value;
                    if (option & configReset) ResetAta();
                    UpdatePins();
                }
                return option;
            case configBase + 2:
                if (write) configStatus = value & 0x74;
                return configStatus | ((ethernet ? ethernet->Irq() : ata.irq) ? 0x02 : 0);
            case configBase + 4:
                if (write) pins = value & 0x02;
                return pins | (Ready() ? 0x20 : 0);
            case configBase + 6:
                if (write) socketCopy = value & 0x0f;
                return socketCopy;
            default: {
                const auto index = offset / 2;
                if (!Ready()) return 0xff;
                if (ethernet) return ethernet->Attribute(offset);
                if (Removable())
                    return index < sizeof(compactFlashCis) ? compactFlashCis[index] : 0xff;
                return index < sizeof(microdriveCis) ? microdriveCis[index] : 0xff;
            }
        }
    }

    template <typename T>
    void SaveLoad(T& chunk) {
        chunk.Do8(option).Do8(configStatus).Do8(pins).Do8(socketCopy);
        ata.SaveLoad(chunk);
    }

    void SetWriteProtect(bool locked) {
        writable = !locked;
        if (locked && ata.transfer == uint8_t(Transfer::write)) Complete(ataAbort);
    }

    bool ValidTransfer() const {
        if (ata.transfer > uint8_t(Transfer::write) || ata.position >= sectorBytes ||
            !ata.blockSize || ata.blockSize > maxMultiple || ata.inBlock >= ata.blockSize)
            return false;
        if (ata.transfer != uint8_t(Transfer::read) && ata.transfer != uint8_t(Transfer::write))
            return true;
        return ata.remaining && ata.remaining <= 256 && ata.lba < sectors &&
               ata.remaining <= sectors - ata.lba;
    }
};
}  // namespace

struct SpitzPcmcia {
    ArmMem* mem;
    Socket sockets[2];
    void (*gpioOutputsChanged)(void*, unsigned){};
    void* gpioOutputsContext{};
    void (*sdPowerChanged)(void*){};
    void* sdPowerContext{};
    bool swapOnLoad = false;
    SpitzCfEthernet::Transmit ethernetTransmit{};
    void* ethernetContext{};
    void (*wifiTransmit)(void*, const uint8_t*, size_t){};
    void* wifiContext{};
    bool wifiNetworkAvailable{};
};

namespace {
bool validAccessSize(uint_fast8_t size) { return size == 1 || size == 2 || size == 4; }

void configureWifiHost(SpitzPcmcia* pcmcia) {
    auto& wifi = *pcmcia->sockets[cfSlot].wifi;
    wifi.transmitFrame = [pcmcia](const auto& frame) {
        if (pcmcia->wifiTransmit) pcmcia->wifiTransmit(pcmcia->wifiContext, frame.data(), frame.size());
    };
    wifi.SetNetworkAvailable(pcmcia->wifiNetworkAvailable);
}

bool scoopAccess(void* context, uint32_t pa, uint_fast8_t size, bool write, void* buf) {
    auto& pcmcia = *static_cast<SpitzPcmcia*>(context);
    const unsigned slot = pa >= scoopBase[0] ? 0 : 1;
    const uint32_t offset = pa - scoopBase[slot];
    auto* data = static_cast<uint8_t*>(buf);
    if (!validAccessSize(size) || (offset & 3) + size > 4) return false;
    auto& socket = pcmcia.sockets[slot];
    auto& regs = socket.scoop;
    const auto reg = offset / 4;
    uint16_t value = regs[reg];
    if (write) {
        if (reg == csr || reg == gprr) return true;
        const bool wasPowered = socket.Powered();
        const bool wasReset = socket.CardReset();
        const bool wasSdPowered = slot == cfSlot && (regs[cpr] & sdPower);
        for (unsigned i = 0; i < size && (offset & 3) + i < 2; ++i) {
            const unsigned shift = ((offset & 3) + i) * 8;
            value = (value & ~(0xffu << shift)) | (uint16_t(data[i]) << shift);
        }
        regs[reg] = reg == cpr ? value & ~powerGood : value;
        if ((reg == cpr && wasPowered != socket.Powered()) ||
            (reg == ccr && !wasReset && socket.CardReset()))
            socket.ResetCard();
        else
            socket.UpdatePins();
        if ((reg == gpcr || reg == gpwr) && pcmcia.gpioOutputsChanged)
            pcmcia.gpioOutputsChanged(pcmcia.gpioOutputsContext, slot);
        if (slot == cfSlot && reg == cpr && wasSdPowered != bool(regs[cpr] & sdPower) &&
            pcmcia.sdPowerChanged)
            pcmcia.sdPowerChanged(pcmcia.sdPowerContext);
    } else {
        if (reg == csr) {
            value = socket.Present() ? batteryGood | noXVoltage : socketEmpty;
            if (socket.Ready()) value |= socketReady;
        }
        if (reg == cpr && (value & powerOutput) && (value & cardVoltage)) value |= powerGood;
        if (reg == gprr) value = regs[gpcr] & regs[gpwr];
        for (unsigned i = 0; i < size; ++i) data[i] = uint32_t(value) >> (((offset & 3) + i) * 8);
    }
    return true;
}

bool socketAccess(void* context, uint32_t pa, uint_fast8_t size, bool write, void* buf) {
    auto& pcmcia = *static_cast<SpitzPcmcia*>(context);
    if (!validAccessSize(size)) return false;
    auto* data = static_cast<uint8_t*>(buf);
    const auto slot = pa >= socketBase[microdriveSlot] ? microdriveSlot : cfSlot;
    auto& socket = pcmcia.sockets[slot];
    uint32_t offset = pa - socketBase[slot];
    const uint32_t space = offset / spaceSize;
    offset %= spaceSize;
    if (space == 2) {
        for (unsigned i = 0; i < size; ++i) {
            const auto byte = socket.Attribute(offset + i, write, write ? data[i] : 0);
            if (!write) data[i] = byte;
        }
        return true;
    }
    if (socket.wifi) {
        if (space == 0 && size >= 2 && !(offset & 1) && socket.Ready()) {
            for (unsigned byte = 0; byte < size; byte += 2) {
                if (write) {
                    socket.wifi->WriteIo(offset + byte, uint16_t(data[byte] | (uint16_t(data[byte + 1]) << 8)));
                } else {
                    const auto word = socket.wifi->ReadIo(offset + byte);
                    data[byte] = uint8_t(word);
                    data[byte + 1] = uint8_t(word >> 8);
                }
            }
            socket.UpdatePins();
        } else if (!write) {
            memset(data, 0xff, size);
        }
        return true;
    }
    const auto mode = socket.option & configModeMask;
    if (socket.ethernet) {
        if (space == 0 && mode == 1 && socket.Ready()) {
            socket.ethernet->Access(offset, write, data, size, pcmcia.ethernetTransmit, pcmcia.ethernetContext);
            socket.UpdatePins();
        } else if (!write) {
            memset(data, 0xff, size);
        }
        return true;
    }
    // Memory mode also has a 1 KiB data FIFO at 0x400.
    if (space == 3 && mode == 0 && (offset & ~0x3ffu) == 0x400) offset &= 1;
    if (space == 0 && mode == 1) offset &= 0xf;
    const bool taskFile = (space == 0 && mode == 1) || (space == 3 && mode == 0);
    if (!taskFile || !socket.Ready() || offset > 0xf) {
        if (!write) memset(data, 0xff, size);
        return true;
    }
    if (offset == 0 || offset == 8 || offset == 9) {
        if (socket.ata.drive & 0x10) {
            if (!write) memset(data, 0xff, size);
        } else {
            socket.DataAccess(size, write, data);
        }
    } else if (write) {
        socket.WriteRegister(offset, data[0]);
    } else {
        memset(data, 0, size);
        data[0] = socket.ReadRegister(offset);
    }
    return true;
}
}  // namespace

SpitzPcmcia* spitzPcmciaInit(ArmMem* mem, PxaGpio* gpio) {
    auto* pcmcia = new SpitzPcmcia{.mem = mem, .sockets = {{gpio, cfSlot}, {gpio, microdriveSlot}}};
    for (unsigned i = 0; i < 2; ++i) {
        if (!memRegionAdd(mem, scoopBase[i], scoopSize, scoopAccess, pcmcia) ||
            !memRegionAdd(mem, socketBase[i], socketSize, socketAccess, pcmcia)) {
            spitzPcmciaDeinit(pcmcia);
            return nullptr;
        }
    }
    spitzPcmciaReset(pcmcia);
    return pcmcia;
}

void spitzPcmciaDeinit(SpitzPcmcia* pcmcia) {
    if (!pcmcia) return;
    for (unsigned i = 0; i < 2; ++i) {
        memRegionRemove(pcmcia->mem, scoopBase[i], scoopSize, pcmcia);
        memRegionRemove(pcmcia->mem, socketBase[i], socketSize, pcmcia);
    }
    auto& drive = pcmcia->sockets[microdriveSlot];
    drive.overlay.reset();
    if (drive.disk >= 0) close(drive.disk);
    delete pcmcia;
}

void spitzPcmciaReset(SpitzPcmcia* pcmcia) {
    for (auto& socket : pcmcia->sockets) {
        memset(socket.scoop, 0, sizeof(socket.scoop));
        socket.scoop[mcr] = 0x0100;
        socket.ResetCard();
    }
}

uint16_t spitzPcmciaGpioOutputs(const SpitzPcmcia* pcmcia, unsigned slot) {
    const auto& regs = pcmcia->sockets[slot].scoop;
    return regs[gpcr] & regs[gpwr];
}

bool spitzPcmciaGpioDrivenLow(const SpitzPcmcia* pcmcia, unsigned slot, uint16_t mask) {
    const auto& regs = pcmcia->sockets[slot].scoop;
    return (regs[gpcr] & ~regs[gpwr] & mask) == mask;
}

void spitzPcmciaObserveGpioOutputs(SpitzPcmcia* pcmcia, void (*callback)(void*, unsigned), void* context) {
    pcmcia->gpioOutputsChanged = callback;
    pcmcia->gpioOutputsContext = context;
}

bool spitzPcmciaSdPowered(const SpitzPcmcia* pcmcia) {
    return pcmcia->sockets[cfSlot].scoop[cpr] & sdPower;
}

void spitzPcmciaObserveSdPower(SpitzPcmcia* pcmcia, void (*callback)(void*), void* context) {
    pcmcia->sdPowerChanged = callback;
    pcmcia->sdPowerContext = context;
}

bool spitzPcmciaMount(SpitzPcmcia* pcmcia, const char* path, bool writable, const char* mediaId) {
    auto& socket = pcmcia->sockets[microdriveSlot];
    if (!path || socket.Present() || (mediaId && !validMicrodriveId(mediaId))) return false;
    int flags = (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC;
    if (mediaId) flags |= O_NOFOLLOW;
#ifdef O_BINARY
    flags |= O_BINARY;
#endif
    frondStorage::Fd candidate(open(path, flags));
    if (candidate.fd < 0 || flock(candidate.fd, (writable ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0)
        return false;
#ifndef _WIN32
    struct flock imageLock{};
    imageLock.l_type = writable ? F_WRLCK : F_RDLCK;
    imageLock.l_whence = SEEK_SET;
    if (fcntl(candidate.fd, F_OFD_SETLK, &imageLock) != 0) return false;
#endif
    struct stat st{};
    frondPosix::FileStamp stamp;
    if (fstat(candidate.fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        !frondPosix::ReadFileStamp(candidate.fd, st, stamp)) return false;
    const auto bytes = frondStorage::sizeOf(candidate.fd);
    if (bytes <= 0 || bytes % sectorBytes || bytes / sectorBytes > maxSectors) return false;

    if (writable && mediaId) {
        const std::filesystem::path image(path);
        struct stat named{};
        if (stat(path, &named) != 0 || !frondPosix::FileMatchesPath(candidate.fd, image, st, named))
            return false;
        const auto root = image.parent_path() / (".gen-" + image.filename().string() + "-root");
        std::error_code ec;
        // The machine save owns the commit point. A working overlay has no published root.
        if (std::filesystem::exists(root, ec) || ec || !FileCardBacking::DiscardGenerations(image))
            return false;
        FileCardOptions options;
        options.identity = mediaId;
        options.recoverRootlessBase = true;
        auto overlay = FileCardBacking::Open(image, bytes, 0, options);
        if (!overlay || !overlay->DurableIs(image) || overlay->RootGenerationId() != 0) return false;
        if (stat(path, &named) != 0 || !frondPosix::FileMatchesPath(candidate.fd, image, st, named))
            return false;
        socket.overlay = std::move(overlay);
    }

    socket.disk = candidate.fd;
    socket.diskDevice = stamp.dev;
    socket.diskInode = stamp.ino;
    if (mediaId) std::copy_n(mediaId, socket.mediaId.size(), socket.mediaId.begin());
    candidate.fd = -1;
    socket.sectors = bytes / sectorBytes;
    socket.writable = writable;
    socket.ResetCard();
    return true;
}

bool spitzPcmciaFlushMicrodrive(const SpitzPcmcia* pcmcia) {
    const auto& drive = pcmcia->sockets[microdriveSlot];
    return !drive.Present() || !drive.writable || syncFile(drive.disk);
}

bool spitzPcmciaForEachMicrodriveChange(const SpitzPcmcia* pcmcia, const CardBacking::OverlayFn& fn) {
    const auto& overlay = pcmcia->sockets[microdriveSlot].overlay;
    if (!overlay) return true;
    auto pin = overlay->Snapshot();
    return pin && overlay->ForEachOverlayIn(*pin, fn);
}

bool spitzPcmciaWriteMicrodriveBase(SpitzPcmcia* pcmcia, uint64_t offset, const void* data, size_t count) {
    const auto& drive = pcmcia->sockets[microdriveSlot];
    return drive.overlay && data && cardRangeOk(drive.overlay->Size(), offset, count) &&
           frondStorage::writeAllAt(drive.disk, data, count, offset);
}

bool spitzPcmciaResetMicrodriveOverlay(SpitzPcmcia* pcmcia) {
    const auto& overlay = pcmcia->sockets[microdriveSlot].overlay;
    return !overlay || overlay->Rollback();
}

bool spitzPcmciaInsertCompactFlash(SpitzPcmcia* pcmcia, bool writeProtected) {
    auto& socket = pcmcia->sockets[cfSlot];
    const auto& card = sdCardStore(1);
    const auto sectors = card.sectorCount();
    if (socket.Present() || !sectors || sectors > maxSectors || !*card.getId()) return false;
    socket.mediaId = {};
    const auto id = std::string_view(card.getId());
    std::copy(id.begin(), id.end(), socket.mediaId.begin());
    socket.cardEpoch = card.epoch();
    socket.sectors = sectors;
    socket.writable = !writeProtected;
    socket.ResetCard();
    return true;
}

void spitzPcmciaEjectCompactFlash(SpitzPcmcia* pcmcia) {
    auto& socket = pcmcia->sockets[cfSlot];
    if (socket.ethernet || socket.wifi) return;
    socket.sectors = 0;
    socket.mediaId = {};
    socket.writable = false;
    socket.ResetCard();
}

bool spitzPcmciaCompactFlashInserted(const SpitzPcmcia* pcmcia) {
    return pcmcia->sockets[cfSlot].BlockPresent();
}

void spitzPcmciaSetCompactFlashWriteProtect(SpitzPcmcia* pcmcia, bool locked) {
    auto& socket = pcmcia->sockets[cfSlot];
    if (socket.BlockPresent()) socket.SetWriteProtect(locked);
}

void spitzPcmciaSetCompactFlashSwapOnLoad(SpitzPcmcia* pcmcia, bool allow) {
    pcmcia->swapOnLoad = allow;
}

bool spitzPcmciaInsertEthernet(SpitzPcmcia* pcmcia) {
    auto& socket = pcmcia->sockets[cfSlot];
    if (socket.Present()) return false;
    socket.ethernet.emplace();
    socket.ResetCard();
    return true;
}

void spitzPcmciaEjectEthernet(SpitzPcmcia* pcmcia) {
    auto& socket = pcmcia->sockets[cfSlot];
    if (!socket.ethernet) return;
    socket.ethernet.reset();
    socket.ResetCard();
}

bool spitzPcmciaEthernetInserted(const SpitzPcmcia* pcmcia) {
    return pcmcia->sockets[cfSlot].ethernet.has_value();
}

bool spitzPcmciaEthernetReceive(SpitzPcmcia* pcmcia, const uint8_t* frame, size_t size) {
    auto& socket = pcmcia->sockets[cfSlot];
    if (!socket.ethernet || !socket.Ready() || (socket.option & configModeMask) != 1) return false;
    const bool accepted = socket.ethernet->Receive(frame, size);
    socket.UpdatePins();
    return accepted;
}

void spitzPcmciaEthernetTransmit(SpitzPcmcia* pcmcia,
                              void (*transmit)(void*, const uint8_t*, size_t), void* context) {
    pcmcia->ethernetTransmit = transmit;
    pcmcia->ethernetContext = context;
}

bool spitzPcmciaInsertWifi(SpitzPcmcia* pcmcia) {
    auto& socket = pcmcia->sockets[cfSlot];
    if (socket.Present()) return false;
    socket.wifi.emplace();
    socket.ResetCard();
    configureWifiHost(pcmcia);
    return true;
}

void spitzPcmciaEjectWifi(SpitzPcmcia* pcmcia) {
    auto& socket = pcmcia->sockets[cfSlot];
    if (!socket.wifi) return;
    socket.wifi.reset();
    socket.ResetCard();
}

bool spitzPcmciaWifiInserted(const SpitzPcmcia* pcmcia) {
    return pcmcia->sockets[cfSlot].wifi.has_value();
}

bool spitzPcmciaWifiAssociated(const SpitzPcmcia* pcmcia) {
    const auto& socket = pcmcia->sockets[cfSlot];
    return socket.wifi && socket.Ready() && socket.wifi->Associated();
}

void spitzPcmciaWifiNetworkAvailable(SpitzPcmcia* pcmcia, bool available) {
    pcmcia->wifiNetworkAvailable = available;
    auto& socket = pcmcia->sockets[cfSlot];
    if (!socket.wifi) return;
    socket.wifi->SetNetworkAvailable(available);
    socket.UpdatePins();
}

bool spitzPcmciaWifiReceive(SpitzPcmcia* pcmcia, const uint8_t* frame, size_t size) {
    if (!frame || size < 14 || size > 1514 || !spitzPcmciaWifiAssociated(pcmcia)) return false;
    auto& socket = pcmcia->sockets[cfSlot];
    const bool accepted = socket.wifi->ReceiveFrame({frame, frame + size});
    socket.UpdatePins();
    return accepted;
}

void spitzPcmciaWifiTransmit(SpitzPcmcia* pcmcia,
                          void (*transmit)(void*, const uint8_t*, size_t), void* context) {
    pcmcia->wifiTransmit = transmit;
    pcmcia->wifiContext = context;
}

template <typename T>
void spitzPcmciaSave(SpitzPcmcia* pcmcia, T& savestate) {
    auto chunk = savestate.GetChunk(ChunkType::spitzPcmcia, savestateVersion);
    if (!chunk) return;
    SaveChunkHelper helper(*chunk);
    auto& internal = pcmcia->sockets[microdriveSlot];
    auto& external = pcmcia->sockets[cfSlot];
    const bool portable = internal.mediaId[0] != 0;
    helper.Do32(internal.sectors).Do64(portable ? 0 : internal.diskDevice)
        .Do64(portable ? 0 : internal.diskInode).DoBool(internal.writable);
    for (auto& socket : pcmcia->sockets) helper.DoBuffer16(socket.scoop, std::size(socket.scoop));
    internal.SaveLoad(helper);
    const bool inserted = external.BlockPresent();
    MediaId absent{};
    helper.DoBuffer(internal.mediaId.data(), internal.mediaId.size())
        .Do32(inserted ? external.sectors : 0)
        .DoBuffer(inserted ? external.mediaId.data() : absent.data(), absent.size());
    const bool ethernet = external.ethernet.has_value(), wifi = external.wifi.has_value();
    if (inserted || ethernet || wifi) external.SaveLoad(helper);
    else {
        auto empty = Socket{external.gpio, cfSlot};
        empty.SaveLoad(helper);
    }
    helper.DoBool(ethernet);
    if (ethernet) external.ethernet->SaveLoad(helper);
    helper.DoBool(wifi);
    if (wifi) {
        auto bytes = external.wifi->Save();
        helper.Do32(uint32_t(bytes.size())).DoBuffer(bytes.data(), bytes.size());
    }
}

template <typename T>
void spitzPcmciaLoad(SpitzPcmcia* pcmcia, T& loader, bool resetting) {
    uint32_t version;
    auto chunk = loader.GetChunkOrFail(ChunkType::spitzPcmcia, savestateVersion, "Spitz PCMCIA", version);
    if (!chunk) return;
    if (version == 0) {
        logPrintf("Spitz PCMCIA state lacks the Microdrive image identity");
        loader.NotifyError();
        return;
    }
    auto restored = *pcmcia;
    auto& internal = restored.sockets[microdriveSlot];
    auto& external = restored.sockets[cfSlot];
    LoadChunkHelper helper(*chunk);
    uint32_t mediaSectors = 0, cfSectors = 0;
    uint64_t diskDevice = 0, diskInode = 0;
    bool writable = false;
    MediaId mediaId{}, cfId{};
    helper.Do32(mediaSectors).Do64(diskDevice).Do64(diskInode).DoBool(writable);
    for (auto& socket : restored.sockets) helper.DoBuffer16(socket.scoop, std::size(socket.scoop));
    internal.SaveLoad(helper);
    if (version >= 2) {
        helper.DoBuffer(mediaId.data(), mediaId.size()).Do32(cfSectors).DoBuffer(cfId.data(), cfId.size());
        external.SaveLoad(helper);
    }
    const bool initial = resetting && !mediaSectors && !diskDevice && !diskInode && !writable && mediaId == MediaId{};
    const bool sameInternal = mediaSectors == internal.sectors && writable == internal.writable &&
        (mediaId == MediaId{} ? diskDevice == internal.diskDevice && diskInode == internal.diskInode :
                               mediaId == internal.mediaId && !diskDevice && !diskInode);
    if (!initial && !sameInternal) {
        logPrintf("Spitz PCMCIA state requires the same Microdrive image and access mode");
        loader.NotifyError();
        return;
    }
    bool ethernet = false;
    std::optional<SpitzCfEthernet> restoredEthernet;
    if (version >= 3) {
        helper.DoBool(ethernet);
        if (ethernet) restoredEthernet.emplace().SaveLoad(helper);
    }
    bool wifi = false;
    std::optional<Dcf660w> restoredWifi;
    if (version >= 4) {
        helper.DoBool(wifi);
        if (wifi) {
            uint32_t length = 0;
            helper.Do32(length);
            if (!length || length > Dcf660w::maxSnapshotBytes) {
                loader.NotifyError();
                return;
            }
            std::vector<uint8_t> bytes(length);
            helper.DoBuffer(bytes.data(), bytes.size());
            if (chunk->HasError() || !restoredWifi.emplace().Load(bytes.data(), bytes.size())) {
                loader.NotifyError();
                return;
            }
        }
    }
    const bool sameExternal = external.BlockPresent() ? cfSectors == external.sectors && cfId == external.mediaId
                                                     : !cfSectors && cfId == MediaId{};
    const bool allowedSwap = pcmcia->swapOnLoad && (!external.BlockPresent() || !cfSectors);
    if ((ethernet && wifi) || ((ethernet || wifi) && (cfSectors || cfId != MediaId{} || external.BlockPresent())) ||
        (!sameExternal && !resetting && !allowedSwap)) {
        logPrintf("Spitz PCMCIA state requires the same CompactFlash card");
        loader.NotifyError();
        return;
    }
    if (chunk->HasError() || !internal.ValidTransfer() ||
        (sameExternal && !external.ValidTransfer()) ||
        (internal.ata.transfer == uint8_t(Transfer::write) && !internal.writable)) {
        loader.NotifyError();
        return;
    }
    if (!sameExternal || version == 1 || (resetting && (external.ethernet || external.wifi))) {
        external.option = external.configStatus = external.pins = external.socketCopy = 0;
        external.ata = {};
        if (!resetting) external.scoop[cpr] &= ~cardVoltage;
    }
    if (!resetting) {
        external.ethernet = std::move(restoredEthernet);
        external.wifi = std::move(restoredWifi);
    } else {
        if (external.ethernet) external.ethernet->Reset();
        if (external.wifi) external.wifi->Reset();
    }
    *pcmcia = std::move(restored);
    if (pcmcia->sockets[cfSlot].wifi) configureWifiHost(pcmcia);
    for (auto& socket : pcmcia->sockets) socket.UpdatePins();
    auto& liveExternal = pcmcia->sockets[cfSlot];
    if (!liveExternal.writable && liveExternal.ata.transfer == uint8_t(Transfer::write))
        liveExternal.Complete(ataAbort);
}

template void spitzPcmciaSave(SpitzPcmcia*, Savestate<ChunkType>&);
template void spitzPcmciaSave(SpitzPcmcia*, SavestateProbe<ChunkType>&);
template void spitzPcmciaLoad(SpitzPcmcia*, SavestateLoader<ChunkType>&, bool);
