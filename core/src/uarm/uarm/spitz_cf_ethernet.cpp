#include "spitz_cf_ethernet.h"

#include <algorithm>
#include <cstring>

#include "zip/miniz.h"

namespace {
constexpr uint8_t stop = 0x01, start = 0x02, transmitPacket = 0x04;
constexpr uint8_t remoteMask = 0x38, remoteRead = 0x08, remoteWrite = 0x10;
constexpr uint8_t received = 0x01, transmitted = 0x02, transmitError = 0x08;
constexpr uint8_t overrun = 0x10, counterOverflow = 0x20, remoteDone = 0x40, reset = 0x80;
constexpr uint8_t acceptBroadcast = 0x04, acceptMulticast = 0x08, promiscuous = 0x10, monitor = 0x20;
constexpr unsigned pageBytes = 256, packetHeader = 4, fcsBytes = 4;
constexpr unsigned minimumFrame = 60, maximumFrame = 1518, ethernetHeader = 14;

// The stock PC Card table matches these version strings. A 32-port I/O
// configuration lets pcnet_cs read the station PROM through the 8390.
constexpr uint8_t cis[] = {
    0x01, 3, 0xdb, 0x01, 0xff, // DEVICE
    0x15, 26, 4, 1, 'C','F',0, '1','0','B','a','s','e','-','E','t','h','e','r','n','e','t',0,
                     '1','.','0',0, 0xff, // VERS_1
    0x21, 2, 6, 0, // FUNCID, network
    0x1a, 5, 1, 1, 0, 2, 1, // CONFIG, COR at physical 0x200
    0x1b, 8, 0xc1, 0x41, 0x19, 1, 0xb5, 0x1e, 0x65, 0x20, // CFTABLE, 3.3 V, 32 ports and level IRQ
    0x14, 0, // NO_LINK
    0xff,
};

uint8_t stationProm(uint16_t address) {
    // NE2000 station PROM bytes repeat on both lanes. The last two words
    // carry the clone signature used by pcnet_cs.
    const unsigned index = address / 2;
    if (index < SpitzCfEthernet::mac.size()) return SpitzCfEthernet::mac[index];
    return index >= 14 ? 0x57 : 0;
}
}  // namespace

uint8_t SpitzCfEthernet::Attribute(uint32_t offset) {
    const unsigned index = offset / 2;
    return !(offset & 1) && index < sizeof cis ? cis[index] : 0xff;
}

void SpitzCfEthernet::Reset() { *this = {}; }

void SpitzCfEthernet::ResetRegisters() {
    command = 0x21;
    interruptStatus = reset;
    interruptMask = 0;
    remoteAddress = remoteBytes = 0;
}

void SpitzCfEthernet::Access(uint32_t port, bool write, uint8_t* data, unsigned size,
                             Transmit transmit, void* context) {
    port &= 0x1f;
    if (port == 0x10) {
        RemoteAccess(write, data, size);
        return;
    }

    if (!write) std::fill_n(data, size, 0xff);
    if (port == 0x1f) {
        if (!write) {
            ResetRegisters();
            data[0] = 0;
        }
    } else if (port < 0x10) {
        if (write) WriteRegister(port, data[0], transmit, context);
        else data[0] = ReadRegister(port);
    }
}

uint8_t SpitzCfEthernet::ReadRegister(unsigned reg) {
    if (reg == 0) return command;
    switch (command >> 6) {
        case 0:
            switch (reg) {
                case 3: return boundary;
                case 4: return transmitStatus;
                case 5: case 6: return 0;
                case 7: return interruptStatus;
                case 8: return uint8_t(remoteAddress);
                case 9: return uint8_t(remoteAddress >> 8);
                case 0xc: return receiveStatus;
                case 0xd: case 0xe: return 0;
                case 0xf: {
                    const uint8_t count = missed;
                    missed = 0;
                    return count;
                }
                default: return 0;
            }
        case 1:
            if (reg <= 6) return physical[reg - 1];
            if (reg == 7) return currentPage;
            return multicast[reg - 8];
        case 2:
            switch (reg) {
                case 1: return pageStart;
                case 2: return pageStop;
                case 4: return transmitPage;
                case 0xc: return receiveConfig;
                case 0xd: return transmitConfig;
                case 0xe: return dataConfig;
                case 0xf: return interruptMask;
                default: return 0;
            }
        default: return 0xff;
    }
}

void SpitzCfEthernet::WriteRegister(unsigned reg, uint8_t value, Transmit transmit, void* context) {
    if (reg == 0) {
        command = value & ~transmitPacket;
        if (value & stop) interruptStatus |= reset;
        else if (value & start) interruptStatus &= ~reset;
        if (!remoteBytes && (value & remoteMask) == remoteRead && (value & start))
            interruptStatus |= remoteDone;
        if (value & transmitPacket) TransmitPacket(transmit, context);
        return;
    }

    switch (command >> 6) {
        case 0:
            switch (reg) {
                case 1: pageStart = value; break;
                case 2: pageStop = value; break;
                case 3: boundary = value; break;
                case 4: transmitPage = value; break;
                case 5: transmitBytes = (transmitBytes & 0xff00) | value; break;
                case 6: transmitBytes = (transmitBytes & 0x00ff) | (uint16_t(value) << 8); break;
                case 7: interruptStatus &= ~value; break;
                case 8: remoteAddress = (remoteAddress & 0xff00) | value; break;
                case 9: remoteAddress = (remoteAddress & 0x00ff) | (uint16_t(value) << 8); break;
                case 0xa: remoteBytes = (remoteBytes & 0xff00) | value; break;
                case 0xb: remoteBytes = (remoteBytes & 0x00ff) | (uint16_t(value) << 8); break;
                case 0xc: receiveConfig = value; break;
                case 0xd: transmitConfig = value; break;
                case 0xe: dataConfig = value; break;
                case 0xf: interruptMask = value & 0x7f; break;
            }
            break;
        case 1:
            if (reg <= 6) physical[reg - 1] = value;
            else if (reg == 7) currentPage = value;
            else multicast[reg - 8] = value;
            break;
    }
}

void SpitzCfEthernet::RemoteAccess(bool write, uint8_t* data, unsigned size) {
    if (!write) std::fill_n(data, size, 0xff);
    if ((command & remoteMask) != (write ? remoteWrite : remoteRead) || !remoteBytes) return;
    const bool swapWords = (dataConfig & 3) == 3;
    for (unsigned i = 0; i < size && remoteBytes; ++i) {
        const unsigned lane = swapWords && (i | 1u) < size ? i ^ 1u : i;
        const unsigned address = remoteAddress;
        if (address >= ramBase && address < ramBase + ramSize) {
            if (write) ram[address - ramBase] = data[lane];
            else data[lane] = ram[address - ramBase];
        } else if (!write && address < 32) {
            data[lane] = stationProm(address);
        }
        ++remoteAddress;
        if (ValidRing() && remoteAddress == unsigned(pageStop) * pageBytes)
            remoteAddress = unsigned(pageStart) * pageBytes;
        if (!--remoteBytes) interruptStatus |= remoteDone;
    }
}

void SpitzCfEthernet::TransmitPacket(Transmit transmit, void* context) {
    const unsigned address = unsigned(transmitPage) * pageBytes;
    if ((command & stop) || address < ramBase || address + transmitBytes > ramBase + ramSize ||
        transmitBytes < ethernetHeader || transmitBytes > maximumFrame) {
        transmitStatus = 0x08;
        interruptStatus |= transmitError;
        return;
    }
    const uint8_t* frame = ram.data() + address - ramBase;
    if (transmitConfig & 0x06) Receive(frame, transmitBytes);
    else if (transmit) transmit(context, frame, transmitBytes);
    transmitStatus = 0x01;
    interruptStatus |= transmitted;
}

bool SpitzCfEthernet::AddressMatches(const uint8_t* data) const {
    if (receiveConfig & promiscuous) return true;
    if (!(data[0] & 1)) return std::equal(physical.begin(), physical.end(), data);
    if (std::all_of(data, data + physical.size(), [](uint8_t byte) { return byte == 0xff; }))
        return receiveConfig & acceptBroadcast;
    if (!(receiveConfig & acceptMulticast)) return false;

    // The 8390 takes the high six bits of its uncomplemented, MSB-first CRC.
    uint32_t crc = ~uint32_t(mz_crc32(0, data, physical.size()));
    unsigned hash = 0;
    for (unsigned bit = 0; bit < 6; ++bit, crc >>= 1) hash = (hash << 1) | (crc & 1);
    return multicast[hash / 8] & (1u << (hash % 8));
}

bool SpitzCfEthernet::ValidRing() const {
    return pageStart >= ramBase / pageBytes && pageStart < pageStop &&
           pageStop <= (ramBase + ramSize) / pageBytes;
}

bool SpitzCfEthernet::Receive(const uint8_t* data, size_t size) {
    if (!data || size < ethernetHeader || size > maximumFrame ||
        !(command & start) || (command & stop) || (receiveConfig & monitor) ||
        !ValidRing() || currentPage < pageStart || currentPage >= pageStop ||
        boundary < pageStart || boundary >= pageStop || !AddressMatches(data))
        return false;

    const unsigned payload = std::max<unsigned>(size, minimumFrame);
    const unsigned packetSize = payload + fcsBytes;
    const unsigned pages = (packetHeader + packetSize + pageBytes - 1) / pageBytes;
    const unsigned ringPages = pageStop - pageStart;
    const unsigned distance = (boundary + ringPages - currentPage) % ringPages;
    if (pages >= distance) {
        receiveStatus = 0x10;
        interruptStatus |= overrun;
        if (++missed == 0x80) interruptStatus |= counterOverflow;
        return false;
    }

    const uint8_t next = pageStart + (currentPage - pageStart + pages) % ringPages;
    receiveStatus = 0x01 | ((data[0] & 1) ? 0x20 : 0);
    unsigned position = unsigned(currentPage) * pageBytes;
    auto store = [&](uint8_t byte) {
        ram[position - ramBase] = byte;
        if (++position == unsigned(pageStop) * pageBytes) position = unsigned(pageStart) * pageBytes;
    };
    store(receiveStatus);
    store(next);
    store(uint8_t(packetSize));
    store(uint8_t(packetSize >> 8));

    uint32_t crc = mz_crc32(0, data, size);
    std::array<uint8_t, minimumFrame> padding{};
    if (payload > size) crc = mz_crc32(crc, padding.data(), payload - size);
    for (unsigned i = 0; i < payload; ++i) store(i < size ? data[i] : 0);
    for (unsigned i = 0; i < fcsBytes; ++i, crc >>= 8) store(uint8_t(crc));
    currentPage = next;
    interruptStatus |= received;
    return true;
}
