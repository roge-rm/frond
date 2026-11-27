#include "pxa270_OHCI.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "mem.h"
#include "storage/Io.h"
#include "savestate/ChunkTypeUarm.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"
#include "savestate/SavestateProbe.h"

namespace {
constexpr uint32_t base = 0x4c000000;
constexpr uint64_t frameNs = 1000000;
constexpr uint32_t suspended = 3u << 6, operational = 2u << 6;
constexpr uint32_t operationalMask = 3u << 6;
constexpr uint32_t periodicEnable = 1u << 2, controlEnable = 1u << 4, bulkEnable = 1u << 5;
constexpr uint32_t controlFilled = 1u << 1, bulkFilled = 1u << 2;
constexpr uint32_t writebackDone = 1u << 1, startOfFrame = 1u << 2, error = 1u << 4;
constexpr uint32_t frameOverflow = 1u << 5, rootChange = 1u << 6, masterEnable = 1u << 31;
constexpr uint32_t connected = 1u << 0, enabled = 1u << 1, portSuspend = 1u << 2;
constexpr uint32_t portReset = 1u << 4, portPower = 1u << 8;
constexpr uint32_t connectChange = 1u << 16, enableChange = 1u << 17;
constexpr uint32_t suspendChange = 1u << 18, resetChange = 1u << 20;
constexpr uint32_t portChanges = 0x1f0000;
constexpr unsigned revision = 0, control = 1, command = 2, interrupt = 3, interruptEnable = 4;
constexpr unsigned hcca = 6, controlHead = 8, controlCurrent = 9, bulkHead = 10, bulkCurrent = 11;
constexpr unsigned doneHead = 12;
constexpr unsigned frameInterval = 13, frameNumber = 15, periodicStart = 16, lowSpeedThreshold = 17;
constexpr unsigned hubA = 18, hubB = 19, hubStatus = 20, port2 = 22, pxaReset = 25, pxaEnable = 26;
constexpr unsigned sectorSize = 512;

constexpr uint32_t ptrMask = ~0xfu;
constexpr int nak = -1, stall = -2, noResponse = -3;

uint16_t le16(const uint8_t* bytes) {
    return bytes[0] | (uint16_t(bytes[1]) << 8);
}
uint32_t le32(const uint8_t* bytes) {
    return le16(bytes) | (uint32_t(le16(bytes + 2)) << 16);
}
uint32_t be32(const uint8_t* bytes) {
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | bytes[3];
}
void putLe32(uint8_t* bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = value >> (8 * i);
}
void putBe32(uint8_t* bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[3 - i] = value >> (8 * i);
}
}

namespace {
struct DriveProtocol {
    enum class Stage { command, reply, dataIn, discardOut, status };
    std::array<uint8_t, 32> controlReply{};
    std::array<uint8_t, sectorSize> data{};
    unsigned address = 0, pendingAddress = 0, configuration = 0, pendingConfig = 0;
    unsigned controlLength = 0, controlPos = 0;
    bool controlStatusOut = false, controlStatusIn = false, controlZero = false;
    Stage stage = Stage::command;
    unsigned dataPos = 0, dataLength = 0;
    uint32_t tag = 0, residue = 0, readLba = 0, readBytes = 0, discardBytes = 0;
    uint8_t key = 0, asc = 0, commandStatus = 0;
};
}

struct Pxa270Ohci::UsbDrive : DriveProtocol {
    static constexpr unsigned ep0Packet = 8;
    uint32_t blocks = 0;
    frondStorage::Fd file{-1};

    bool Open(const std::string& path) {
        struct stat info{};
        int flags = O_RDONLY | O_NONBLOCK | O_CLOEXEC;
#ifdef O_BINARY
        flags |= O_BINARY;
#endif
        file.fd = open(path.c_str(), flags);
        const int fd = file.fd;
        if (fd < 0 || fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size < sectorSize ||
            info.st_size % sectorSize || uint64_t(info.st_size) / sectorSize > UINT32_MAX ||
            flock(fd, LOCK_SH | LOCK_NB)) return false;
        blocks = uint64_t(info.st_size) / sectorSize;
        return true;
    }

    void ResetTransport() {
        stage = Stage::command;
        discardBytes = readBytes = 0;
        key = 6;
        asc = 0x28;
    }

    void Reset() {
        address = pendingAddress = configuration = pendingConfig = 0;
        controlLength = controlPos = 0;
        controlZero = controlStatusIn = controlStatusOut = false;
        ResetTransport();
    }

    int Setup(const uint8_t* packet, unsigned length) {
        if (length != 8) return stall;
        controlLength = controlPos = 0;
        controlZero = controlStatusIn = controlStatusOut = false;
        pendingAddress = address;
        pendingConfig = configuration;
        const unsigned type = packet[0], request = packet[1], value = le16(packet + 2);
        const unsigned index = le16(packet + 4), expected = le16(packet + 6);
        const auto reply = [&](const uint8_t* data, unsigned size) {
            controlLength = std::min(size, expected);
            std::copy_n(data, controlLength, controlReply.begin());
            controlZero = controlLength < expected && controlLength % ep0Packet == 0;
            controlStatusOut = true;
            return 8;
        };
        if (type == 0x80 && request == 6) {
            // Full-speed generic SCSI transparent Bulk-Only drive, with one interface.
            static constexpr uint8_t deviceDescriptor[] = {
                18, 1, 0x10, 1, 0, 0, 0, 8, 0, 0, 0, 0, 0, 1, 1, 2, 0, 1};
            static constexpr uint8_t config[] = {
                9, 2, 32, 0, 1, 1, 0, 0x80, 50,
                9, 4, 0, 0, 2, 8, 6, 0x50, 0,
                7, 5, 0x81, 2, 64, 0, 0,
                7, 5, 0x02, 2, 64, 0, 0};
            if (value == 0x0100) return reply(deviceDescriptor, sizeof(deviceDescriptor));
            if (value == 0x0200) return reply(config, sizeof(config));
            if (value == 0x0300) {
                const uint8_t languages[] = {4, 3, 9, 4};
                return reply(languages, sizeof(languages));
            }
            if ((value == 0x0301 || value == 0x0302) && index == 0x0409) {
                const std::string name = value == 0x0301 ? "Frond" : "USB Drive";
                std::array<uint8_t, 20> bytes{};
                bytes[0] = 2 + name.size() * 2;
                bytes[1] = 3;
                for (size_t i = 0; i < name.size(); ++i) bytes[2 + 2 * i] = name[i];
                return reply(bytes.data(), bytes[0]);
            }
            return stall;
        }
        if (request == 0 && (type == 0x80 || type == 0x81 || type == 0x82)) {
            const uint8_t status[] = {0, 0};
            return reply(status, sizeof(status));
        }
        if (request == 8 && type == 0x80) {
            const uint8_t active = configuration;
            return reply(&active, 1);
        }
        if (type == 0x81 && request == 10 && !index) {
            const uint8_t alternate = 0;
            return reply(&alternate, 1);
        }
        if (type == 0xa1 && request == 0xfe && !index && !value) {
            const uint8_t maxLun = 0;
            return reply(&maxLun, 1);
        }

        if (expected) return stall;
        if (type == 0 && request == 5 && value < 128 && !index)
            pendingAddress = value;
        else if (type == 0 && request == 9 && value <= 1 && !index)
            pendingConfig = value;
        else if (type == 1 && request == 11 && !value && !index) {}
        else if (type == 2 && request == 1 && !value && (index == 0x81 || index == 2)) {}
        else if (type == 0x21 && request == 0xff && !index && !value) ResetTransport();
        else return stall;
        controlStatusIn = true;
        return 8;
    }

    int ControlPacket(unsigned pid, uint8_t* buffer, unsigned length) {
        if (pid == 0) return Setup(buffer, length);
        if (pid == 2 && controlStatusIn && length == 0) {
            address = pendingAddress;
            if (configuration != pendingConfig) ResetTransport();
            configuration = pendingConfig;
            controlStatusIn = false;
            return 0;
        }
        if (pid == 2 && controlStatusOut) {
            const unsigned count = std::min(length, controlLength - controlPos);
            if (!count) {
                if (!std::exchange(controlZero, false)) return nak;
            } else {
                std::copy_n(controlReply.begin() + controlPos, count, buffer);
                controlPos += count;
            }
            return count;
        }
        if (pid == 1 && controlStatusOut && !length) {
            controlStatusOut = false;
            return 0;
        }
        return stall;
    }

    void Fail(uint8_t nextKey, uint8_t nextAsc) {
        key = nextKey;
        asc = nextAsc;
        commandStatus = 1;
    }

    int BulkOut(const uint8_t* packet, unsigned length) {
        if (stage == Stage::discardOut) {
            if (length > discardBytes) return stall;
            discardBytes -= length;
            if (!discardBytes || length < 64) stage = Stage::status;
            return length;
        }
        if (stage != Stage::command) return nak;
        if (length != 31 || le32(packet) != 0x43425355 || packet[12] & 0x7f ||
            packet[13] || packet[14] < 1 || packet[14] > 16) return stall;

        tag = le32(packet + 4);
        residue = le32(packet + 8);
        commandStatus = 0;
        dataLength = dataPos = readBytes = 0;
        const bool directionIn = packet[12] & 0x80;
        const auto* cdb = packet + 15;
        std::fill(data.begin(), data.end(), 0);
        switch (cdb[0]) {
            case 0x00:  // TEST UNIT READY
                if (key == 6) Fail(key, asc);
                break;
            case 0x03:  // REQUEST SENSE
                data[0] = 0x70;
                data[2] = key;
                data[7] = 10;
                data[12] = asc;
                key = asc = 0;
                dataLength = std::min(18u, unsigned(cdb[4]));
                break;
            case 0x12:  // INQUIRY
                if (cdb[1] & 3) {
                    Fail(5, 0x24);
                    break;
                }
                data[1] = 0x80;
                data[2] = 2;
                data[3] = 2;
                data[4] = 31;
                std::memcpy(data.data() + 8, "Frond   USB Drive       1.00", 28);
                dataLength = std::min(36u, unsigned(cdb[4]));
                break;
            case 0x1a:  // MODE SENSE(6)
                data[0] = 3;
                data[2] = 0x80;
                dataLength = std::min(4u, unsigned(cdb[4]));
                break;
            case 0x5a:  // MODE SENSE(10)
                data[1] = 6;
                data[3] = 0x80;
                dataLength = std::min(8u, unsigned(cdb[7]) * 256 + cdb[8]);
                break;
            case 0x25:  // READ CAPACITY(10)
                putBe32(data.data(), blocks - 1);
                putBe32(data.data() + 4, sectorSize);
                dataLength = 8;
                break;
            case 0x28: {  // READ(10)
                readLba = be32(cdb + 2);
                const uint32_t count = unsigned(cdb[7]) * 256 + cdb[8];
                if (uint64_t(readLba) + count > blocks) Fail(5, 0x21);
                else readBytes = std::min(residue, count * sectorSize);
                break;
            }
            case 0x1b:  // START STOP UNIT
            case 0x1e:  // PREVENT ALLOW MEDIUM REMOVAL
            case 0x35:  // SYNCHRONIZE CACHE
                break;
            case 0x2a:  // WRITE(10)
                Fail(7, 0x27);
                break;
            default:
                Fail(5, 0x20);
        }
        if (residue && !directionIn) {
            if (!commandStatus) Fail(5, 0x24);
            discardBytes = residue;
            stage = Stage::discardOut;
        } else if (residue && directionIn) {
            if (readBytes) stage = Stage::dataIn;
            else {
                dataLength = std::min(dataLength, residue);
                stage = Stage::reply;
            }
        } else {
            stage = Stage::status;
        }
        return length;
    }

    int BulkIn(uint8_t* buffer, unsigned length, uint64_t& sectorsRead) {
        if (stage == Stage::reply) {
            const unsigned count = std::min(length, dataLength - dataPos);
            std::copy_n(data.data() + dataPos, count, buffer);
            dataPos += count;
            residue -= count;
            if (count < length || !residue) stage = Stage::status;
            return count;
        }
        if (stage == Stage::dataIn) {
            if (!dataLength) {
                if (!frondStorage::readAllAt(file.fd, data.data(), data.size(), uint64_t(readLba) * sectorSize)) {
                    Fail(3, 0x11);
                    stage = Stage::status;
                    return 0;
                }
                ++readLba;
                ++sectorsRead;
                dataPos = 0;
                dataLength = std::min(readBytes, sectorSize);
            }
            const unsigned count = std::min(length, dataLength - dataPos);
            std::copy_n(data.data() + dataPos, count, buffer);
            dataPos += count;
            residue -= count;
            readBytes -= count;
            if (dataPos == dataLength) dataLength = 0;
            if (!readBytes) stage = residue && count == length ? Stage::reply : Stage::status;
            if (stage == Stage::reply) dataPos = dataLength = 0;
            return count;
        }
        if (stage != Stage::status) return nak;
        if (length < 13) return stall;
        putLe32(buffer, 0x53425355);
        putLe32(buffer + 4, tag);
        putLe32(buffer + 8, residue);
        buffer[12] = commandStatus;
        stage = Stage::command;
        return 13;
    }

    int Packet(unsigned deviceAddress, unsigned endpoint, unsigned pid, uint8_t* data,
               unsigned length, uint64_t& sectorsRead) {
        if (deviceAddress != address) return noResponse;
        if (!endpoint) return ControlPacket(pid, data, length);
        if (configuration != 1) return stall;
        if (pid == 2 && endpoint == 1) return BulkIn(data, length, sectorsRead);
        if (pid == 1 && endpoint == 2) return BulkOut(data, length);
        return stall;
    }
};

Pxa270Ohci::Pxa270Ohci(ArmMem* memory, std::function<void(bool)> irq,
                       std::function<void()> reschedule)
    : memory(memory), irq(std::move(irq)), reschedule(std::move(reschedule)) {
    if (!memRegionAdd(memory, base, registers.size() * sizeof(uint32_t), Access, this))
        std::abort();
    Reset();
}

Pxa270Ohci::~Pxa270Ohci() = default;

bool Pxa270Ohci::OpenDrive(const std::string& path) {
    if (drive) return false;
    auto candidate = std::make_unique<UsbDrive>();
    if (!candidate->Open(path)) return false;
    drive = std::move(candidate);
    drive->Reset();
    setupPackets = sectorsRead = 0;
    SyncPort();
    return true;
}

void Pxa270Ohci::Eject() {
    drive.reset();
    SyncPort();
}

void Pxa270Ohci::SetBus(bool clock, bool powered, uint64_t time) {
    now = time;
    if (clock == clockEnabled && powered == portPowered) return;
    const bool starting = clock && !clockEnabled;
    if (starting) nextFrame = now + frameNs;
    clockEnabled = clock;
    portPowered = powered;
    SyncPort();
    SyncIrq();
    if (starting && Wake() != UINT64_MAX) reschedule();
}

uint64_t Pxa270Ohci::Wake() const {
    if (!clockEnabled) return UINT64_MAX;
    const bool running = (registers[control] & operationalMask) == operational;
    return std::min(running ? nextFrame : UINT64_MAX, portResetAt ? portResetAt : UINT64_MAX);
}

std::string Pxa270Ohci::Diagnostics() const {
    if (!drive) return "off";
    return "drive port=" + std::to_string(registers[port2]) + " address=" + std::to_string(drive->address) +
           " config=" + std::to_string(drive->configuration) + " setups=" + std::to_string(setupPackets) +
           " sectors=" + std::to_string(sectorsRead);
}

void Pxa270Ohci::Reset() {
    registers = {};
    registers[revision] = 0x10;
    registers[frameInterval] = 0x2edf;
    registers[lowSpeedThreshold] = 0x628;
    registers[hubA] = 3 | (10u << 24);
    portResetAt = doneAt = 0;
    nextFrame = now + frameNs;
    SyncIrq();
}

void Pxa270Ohci::SyncPort() {
    auto& port = registers[port2];
    const bool present = drive && portPowered && (port & portPower);
    if (present == bool(port & connected)) return;
    port |= connectChange;
    if (present) port |= connected;
    else {
        if (port & enabled) port |= enableChange;
        port &= ~(connected | enabled | portSuspend | portReset);
        portResetAt = 0;
        if (drive) drive->Reset();
    }
    registers[interrupt] |= rootChange;
    SyncIrq();
}

void Pxa270Ohci::SyncIrq() {
    irq(clockEnabled && (registers[interruptEnable] & masterEnable) &&
        (registers[interrupt] & registers[interruptEnable]));
}

bool Pxa270Ohci::Access(void* context, uint32_t address, uint_fast8_t size, bool write, void* buffer) {
    if (size != 4 || address % 4) return false;
    auto& self = *static_cast<Pxa270Ohci*>(context);
    const unsigned reg = (address - base) / 4;
    if (write) {
        uint32_t value;
        std::memcpy(&value, buffer, sizeof(value));
        self.Write(reg, value);
    } else {
        const uint32_t value = self.Read(reg);
        std::memcpy(buffer, &value, sizeof(value));
    }
    return true;
}

uint32_t Pxa270Ohci::Read(unsigned reg) const {
    if (reg == 5) return registers[interruptEnable];
    if (reg == 14) return registers[frameInterval] & 0x3fff;
    return registers[reg];
}

void Pxa270Ohci::Write(unsigned reg, uint32_t value) {
    switch (reg) {
        case control:
            if ((registers[reg] & operationalMask) != operational && (value & operationalMask) == operational)
                nextFrame = now + frameNs;
            registers[reg] = value & 0x7ff;
            if (Wake() != UINT64_MAX) reschedule();
            break;
        case command:
            if (value & 1) {
                const auto old = registers;
                const uint64_t resetDeadline = portResetAt;
                Reset();
                // HCR resets the schedule, leaving root hub and PXA extension registers intact.
                std::copy(old.begin() + hubA, old.end(), registers.begin() + hubA);
                portResetAt = resetDeadline;
                registers[control] = suspended;
            } else {
                registers[reg] |= value & (controlFilled | bulkFilled);
            }
            break;
        case interrupt: registers[reg] &= ~value; break;
        case interruptEnable: registers[reg] |= value & 0xc000007f; break;
        case 5: registers[interruptEnable] &= ~value; break;
        case hcca: registers[reg] = value & ~0xffu; break;
        case controlHead:
        case bulkHead:
        case controlCurrent:
        case bulkCurrent: registers[reg] = value & ptrMask; break;
        case frameInterval:
        case periodicStart:
        case lowSpeedThreshold:
        case hubB:
        case pxaEnable:
        case 27: registers[reg] = value; break;
        case hubA:
            registers[reg] = 3 | (value & 0xff001f00);
            if (value & (1u << 9))
                for (unsigned port = port2 - 1; port <= port2 + 1; ++port) registers[port] |= portPower;
            SyncPort();
            break;
        case hubStatus:
            for (unsigned port = port2 - 1; port <= port2 + 1; ++port) {
                if (value & (1u << 16)) registers[port] |= portPower;
                if ((value & 1) && !(registers[hubA] & (1u << 9))) registers[port] &= ~portPower;
            }
            if (value & (1u << 15)) registers[reg] |= 1u << 15;
            if (value & (1u << 31)) registers[reg] &= ~(1u << 15);
            SyncPort();
            break;
        case port2 - 1:
        case port2:
        case port2 + 1:
            registers[reg] &= ~(value & portChanges);
            if (value & connected) registers[reg] &= ~enabled;
            if (value & portPower) registers[reg] |= portPower;
            if ((value & (1u << 9)) && !(registers[hubA] & (1u << 9))) registers[reg] &= ~portPower;
            SyncPort();
            if (!(registers[reg] & connected)) break;
            if (value & enabled) registers[reg] |= enabled;
            if (value & portSuspend) registers[reg] |= portSuspend;
            if ((value & (1u << 3)) && (registers[reg] & portSuspend)) {
                registers[reg] = (registers[reg] & ~portSuspend) | suspendChange;
                registers[interrupt] |= rootChange;
            }
            if (value & portReset) {
                registers[reg] |= portReset;
                portResetAt = now + 10 * frameNs;
                if (clockEnabled) reschedule();
            }
            break;
        case pxaReset:
            if (value & 2) Reset();
            // FSBIR clears once the host bus interface is reset.
            registers[reg] = value & 0xffe;
            break;
        default: break;
    }
    SyncIrq();
}

bool Pxa270Ohci::Dma(uint32_t address, void* data, unsigned size, bool write) {
    if (!size) return true;
    if (address + size - 1 < address || !memIsBufferRange(memory, address, size, true)) return false;
    auto* bytes = static_cast<uint8_t*>(data);
    while (size) {
        const unsigned count = address % 4 == 0 && size >= 4 ? 4 : 1;
        if (!memAccess(memory, address, count, write, bytes)) return false;
        address += count;
        bytes += count;
        size -= count;
    }
    return true;
}

bool Pxa270Ohci::Transfer(uint32_t start, uint32_t end, uint8_t* data, unsigned count, bool write) {
    const unsigned first = std::min(count, 0x1000 - (start & 0xfff));
    const uint32_t secondPage = end & ~0xfffu;
    if (count != first && !memIsBufferRange(memory, secondPage, count - first, true)) return false;
    return Dma(start, data, first, write) && Dma(secondPage, data + first, count - first, write);
}

bool Pxa270Ohci::ServiceList(uint32_t& current, unsigned& budget, bool& foundTransfer) {
    unsigned descriptors = 0;
    while (current && budget > 16) {
        if (++descriptors > 128) return false;
        const uint32_t ed = current;

        uint32_t ep[4];
        if (!Dma(ed, ep, sizeof(ep), false)) return false;
        for (auto& word : ep) word = le32(reinterpret_cast<const uint8_t*>(&word));
        const unsigned maxPacket = ep[0] >> 16 & 0x7ff;
        if (ep[0] & 0xc000 || ep[2] & 1 || !maxPacket || maxPacket > 64) {
            current = ep[3] & ptrMask;
            continue;
        }
        while ((ep[2] & ptrMask) && (ep[2] & ptrMask) != (ep[1] & ptrMask) && budget > 16) {
            foundTransfer = true;
            const uint32_t tdAddress = ep[2] & ptrMask;
            uint32_t td[4];
            if (!Dma(tdAddress, td, sizeof(td), false)) return false;
            for (auto& word : td) word = le32(reinterpret_cast<const uint8_t*>(&word));
            const uint32_t start = td[1], end = td[3];
            unsigned length = 0;
            if (start && start >> 12 != end >> 12)
                length = 0x1000 - (start & 0xfff) + (end & 0xfff) + 1;
            else if (start && end >= start)
                length = end - start + 1;
            const unsigned count = std::min(length, maxPacket);
            if (count + 16 > budget) return true;
            budget -= count + 16;
            const unsigned fixedDirection = ep[0] >> 11 & 3;
            const unsigned pid = fixedDirection == 1 || fixedDirection == 2 ? fixedDirection : (td[0] >> 19 & 3);
            uint8_t packet[64]{};
            if (start && !length) return false;
            if (pid < 2 && !Transfer(start, end, packet, count, false)) return false;
            const auto port = registers[port2];
            const int result = drive && (port & (enabled | portSuspend | portReset)) == enabled
                ? drive->Packet(ep[0] & 0x7f, ep[0] >> 7 & 15, pid, packet, count, sectorsRead)
                : noResponse;
            if (result == nak) break;
            if (result == 8 && pid == 0) ++setupPackets;
            if (result > 0 && pid == 2 && !Transfer(start, end, packet, result, true)) return false;
            if (result > 0) {
                if (unsigned(result) >= length) td[1] = 0;
                else if (unsigned(result) >= 0x1000 - (start & 0xfff))
                    td[1] = (end & ~0xfffu) + unsigned(result) - (0x1000 - (start & 0xfff));
                else td[1] += result;
            }
            const unsigned toggle = td[0] >> 24 & 3;
            const uint32_t carry = toggle & 2 ? ((toggle & 1) ^ 1) << 1 : (ep[2] ^ 2) & 2;
            unsigned condition = 0;
            if (result == stall) condition = 4;
            else if (result == noResponse) condition = 5;
            else if (unsigned(result) < count && !(td[0] & (1u << 18))) condition = 9;
            const uint32_t next = td[2] & ptrMask;
            const bool complete = condition || unsigned(result) < count || !td[1];
            if (complete) {
                const unsigned delay = td[0] >> 21 & 7;
                if (delay < 7) {
                    const uint64_t when = nextFrame + delay * frameNs;
                    if (!doneAt || when < doneAt) doneAt = when;
                }
                td[0] = (td[0] & 0x03ffffff) | (condition << 28);
                td[2] = registers[doneHead];
                registers[doneHead] = tdAddress;
                ep[2] = next | carry | (condition ? 1u : 0u);
            } else {
                td[0] &= ~(3u << 24);
                ep[2] = tdAddress | carry;
            }
            for (auto& word : td) putLe32(reinterpret_cast<uint8_t*>(&word), word);
            uint8_t headBytes[4];
            putLe32(headBytes, ep[2]);
            if (!Dma(tdAddress, td, sizeof(td), true) || !Dma(ed + 8, headBytes, 4, true)) return false;
            if (condition) break;
        }
        if (budget <= 16 && !(ep[2] & 1) && (ep[2] & ptrMask) != (ep[1] & ptrMask)) return true;
        current = ep[3] & ptrMask;
    }
    return true;
}

void Pxa270Ohci::RunFrame() {
    auto& status = registers[interrupt];
    if (++registers[frameNumber] == 0x10000) {
        registers[frameNumber] = 0;
        status |= frameOverflow;
    }
    status |= startOfFrame;
    if (!registers[hcca]) return;
    const auto fail = [&] {
        status |= error;
        registers[control] = (registers[control] & ~operationalMask) | suspended;
    };
    uint8_t number[2] = {uint8_t(registers[frameNumber]), uint8_t(registers[frameNumber] >> 8)};
    if (!Dma(registers[hcca] + 0x80, number, 2, true)) return fail();
    unsigned budget = 1200;  // Full-speed frames have 12,000 bits including token and handshake overhead.
    if (registers[control] & periodicEnable) {
        uint8_t periodic[4];
        if (!Dma(registers[hcca] + (registers[frameNumber] & 31) * 4, periodic, 4, false)) return fail();
        uint32_t current = le32(periodic) & ptrMask;
        bool found = false;
        if (!ServiceList(current, budget, found)) return fail();
    }

    const auto serviceQueue = [&](uint32_t enable, uint32_t filled, unsigned head, unsigned cursor) {
        if (!(registers[control] & enable)) return true;
        auto& current = registers[cursor];
        if (!current) {
            if (!(registers[command] & filled)) return true;
            registers[command] &= ~filled;
            current = registers[head];
        }
        bool found = false;
        if (!ServiceList(current, budget, found)) return false;
        if (found) registers[command] |= filled;
        return true;
    };
    if (!serviceQueue(controlEnable, controlFilled, controlHead, controlCurrent) ||
        !serviceQueue(bulkEnable, bulkFilled, bulkHead, bulkCurrent)) return fail();

    if (doneAt && doneAt <= nextFrame && !(status & writebackDone)) {
        uint8_t done[4];
        putLe32(done, registers[doneHead] | ((status & registers[interruptEnable]) ? 1u : 0u));
        if (!Dma(registers[hcca] + 0x84, done, 4, true)) return fail();
        status |= writebackDone;
        registers[doneHead] = 0;
        doneAt = 0;
    }
}

void Pxa270Ohci::Tick(uint64_t time) {
    now = time;
    if (!clockEnabled) return;
    if (portResetAt && portResetAt <= now) {
        portResetAt = 0;
        if (registers[port2] & connected) {
            registers[port2] = (registers[port2] & ~(portReset | portSuspend)) | enabled | resetChange;
            if (drive) drive->Reset();
            registers[interrupt] |= rootChange;
        }
    }
    if ((registers[control] & operationalMask) == operational && nextFrame <= now) {
        RunFrame();
        nextFrame = now + frameNs;
    }
    SyncIrq();
}

void Pxa270Ohci::MediaStateReplaced() {
    // A mounted host file never comes from a savestate. Replace its bus session on load.
    const bool wasAttached = registers[port2] & connected;
    if (drive) drive->Reset();
    registers[port2] &= ~(connected | enabled | portSuspend | portReset);
    portResetAt = 0;
    if (wasAttached) {
        registers[port2] |= connectChange;
        registers[interrupt] |= rootChange;
    }
    SyncPort();
    SyncIrq();
}

std::function<void()> Pxa270Ohci::CaptureDriveState() {
    const auto protocol = drive ? static_cast<DriveProtocol>(*drive) : DriveProtocol{};
    const uint64_t setups = setupPackets, sectors = sectorsRead, time = now;
    const bool clock = clockEnabled, powered = portPowered;
    auto* media = drive.get();
    return [this, protocol, setups, sectors, time, clock, powered, media] {
        clockEnabled = clock;
        portPowered = powered;
        now = time;
        if (drive && drive.get() == media) {
            static_cast<DriveProtocol&>(*drive) = protocol;
            setupPackets = setups;
            sectorsRead = sectors;
        }
    };
}

template <typename T>
void Pxa270Ohci::Save(T& state) {
    auto* chunk = state.GetChunk(ChunkType::spitzOhci, 0);
    if (!chunk) return;
    for (uint32_t value : registers) chunk->Put32(value);
    chunk->Put64(nextFrame);
    chunk->Put64(portResetAt);
    chunk->Put64(doneAt);
}

void Pxa270Ohci::Load(SavestateLoader<ChunkType>& loader) {
    if (!loader.HasChunk(ChunkType::spitzOhci)) return Reset();
    auto* chunk = loader.GetChunk(ChunkType::spitzOhci, 0, "Spitz USB host");
    if (!chunk) return;
    for (auto& value : registers) value = chunk->Get32();
    nextFrame = chunk->Get64();
    portResetAt = chunk->Get64();
    doneAt = chunk->Get64();
    if (registers[revision] != 0x10 || (registers[hubA] & 0xff) != 3 ||
        registers[frameNumber] > 0xffff || (registers[hcca] & 0xff))
        loader.NotifyError();
}

template void Pxa270Ohci::Save(Savestate<ChunkType>&);
template void Pxa270Ohci::Save(SavestateProbe<ChunkType>&);
