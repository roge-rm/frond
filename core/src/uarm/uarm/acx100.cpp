#include "acx100.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {
    // Registers (byte offsets in the chip's window).
    enum : uint32_t {
        regSoftReset = 0x00, regRegAddress = 0x04, regRegData = 0x08, regMemAddress = 0x14, regMemData = 0x18,
        regMemControl = 0x1c, regEndianControl = 0x20, regIntTrigger = 0x7c, regIrqMask = 0x98, regIrqStatus = 0xa4,
        regIrqAck = 0xac, regTime = 0x204, regEepromControl = 0x250, regEepromAddress = 0x254, regEepromData = 0x258,
        regPhyAddress = 0x268, regPhyData = 0x26c, regPhyControl = 0x270, regCommandMailbox = 0x2a4,
        regInfoMailbox = 0x2a8, regEepromStart = 0x2d0, regEcpuControl = 0x2d8, regChipId = 0x900,
    };
    constexpr uint32_t memAutoIncrement = 0x10000;
    // Host to chip triggers in regIntTrigger.
    constexpr uint32_t triggerCommand = 1, triggerTx = 4, triggerRxDone = 8;
    // Descriptor control bits (Ctl_8).
    constexpr uint8_t ctlHostOwn = 0x80, ctlAcxDone = 0x40, ctlHostDone = 0x20, ctlRecycle = 0x08;
    // Memory block links: the next block's address over 32 in bits 18:0, bit 25 on the last block.
    constexpr uint32_t linkMask = 0x7ffff, linkLast = 0x2000000;
    // Commands.
    enum : uint16_t {
        cmdInterrogate = 1, cmdConfigure = 2, cmdEnableRx = 3, cmdEnableTx = 4, cmdDisableRx = 5, cmdDisableTx = 6,
        cmdScan = 8, cmdStopScan = 9, cmdJoin = 0x0b, cmdProbeTemplate = 0x16, cmdRadioInit = 0x18,
    };
    // Settings (IEs) the stand-in reads.
    enum : uint16_t {
        ieQueueConfig = 3, ieBlockSize = 4, ieMemoryConfig = 5, ieMemoryMap = 8, ieRxConfig = 0x10, ieStationId = 0x1001,
    };
    // RX config word 1: the 4 byte PHY header goes in front of each received frame.
    constexpr uint16_t rxIncludePhyHeader = 0x0002;
    // Memory map words after the IE header: CodeStart, CodeEnd, WEPCacheStart, WEPCacheEnd, PacketTemplateStart,
    // PacketTemplateEnd, QueueStart, QueueEnd, PoolStart, PoolEnd.
    enum { mmCodeStart, mmCodeEnd, mmWepStart, mmWepEnd, mmTemplateStart, mmTemplateEnd, mmQueueStart, mmQueueEnd,
           mmPoolStart, mmPoolEnd, mmWords };
    // ASSUMPTION "ACX100 area sizes": the firmware keeps 512 bytes of WEP key cache and 2 KB of frame templates;
    // the real sizes come from the key count and the templates, which only this stand-in reads.
    constexpr uint32_t wepCacheBytes = 0x200, templateBytes = 0x800;
    // ASSUMPTION "ACX100 start time": the firmware reports itself started 1 ms after the CPU is let go.
    constexpr uint64_t bootNs = 1000000;
    // ASSUMPTION "ACX100 scan channels": a scan visits channels 1 to 11, chan_duration TU each (100 TU when the
    // driver gives none).
    constexpr unsigned scanFirst = 1, scanLast = 11;
    constexpr uint64_t tuNs = 1024000;
    // The station address the firmware reports (locally administered, "FrACX").
    const uint8_t defaultMac[6] = {0x02, 0x46, 0x72, 0x41, 0x43, 0x58};
    uint32_t le32(const std::vector<uint8_t>& b, size_t at) {
        return at + 4 <= b.size() ? uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24 : 0;
    }
    uint16_t le16(const std::vector<uint8_t>& b, size_t at) {
        return at + 2 <= b.size() ? uint16_t(b[at] | b[at + 1] << 8) : 0;
    }
    void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
        if (b.size() < at + 4) b.resize(at + 4);
        for (int i = 0; i < 4; ++i) b[at + i] = uint8_t(v >> (8 * i));
    }
}

Acx100::Acx100() : memory(memoryBytes) {
    std::memcpy(stationMac, defaultMac, 6);
    reset();
}

void Acx100::log(const char* format, ...) {
    if (!trace) return;
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    trace(line);
}

void Acx100::reset() {
    std::memset(miscRegs, 0, sizeof miscRegs);
    std::fill(memory.begin(), memory.end(), 0);
    std::memset(phy, 0, sizeof phy);
    started = booting = false;
    bootAt = 0;
    regAddress = memAddress = memControl = endianControl = irqStatus = 0;
    irqMask = 0xffff;
    ecpuControl = 1;
    eepromControl = eepromAddress = phyAddress = phyData = phyControl = 0;
    mailboxCommand = mailboxInfo = halfLatch = 0;
    memWrites = 0;
    rxRingStart = rxNext = txRingStart = txNext = blockSize = rxPool = rxBlocks = 0;
    rxBlockUsed.clear();
    channel = 0;
    rxOn = txOn = joined = false;
    std::memset(bssid, 0, sizeof bssid);
    scanning = scanActive = false;
    scanChannel = scanLeft = scanChannelBefore = 0;
    scanStepAt = scanDwell = 0;
    rxHeaderExtra = 0;
    ies.clear();
    probeTemplate.clear();
}

uint32_t Acx100::mem32(uint32_t a) const {
    return uint32_t(mem8(a)) | uint32_t(mem8(a + 1)) << 8 | uint32_t(mem8(a + 2)) << 16 | uint32_t(mem8(a + 3)) << 24;
}
void Acx100::setMem32(uint32_t a, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) setMem8(a + i, uint8_t(v >> (8 * i)));
}
uint16_t Acx100::mem16(uint32_t a) const { return uint16_t(mem8(a) | mem8(a + 1) << 8); }
void Acx100::setMem16(uint32_t a, uint16_t v) {
    setMem8(a, uint8_t(v));
    setMem8(a + 1, uint8_t(v >> 8));
}
void Acx100::memRead(uint32_t a, uint8_t* out, size_t n) const {
    for (size_t i = 0; i < n; ++i) out[i] = mem8(uint32_t(a + i));
}
void Acx100::memWrite(uint32_t a, const uint8_t* in, size_t n) {
    for (size_t i = 0; i < n; ++i) setMem8(uint32_t(a + i), in[i]);
}

uint32_t Acx100::read(uint32_t offset, unsigned size, uint64_t now) {
    offset &= 0xfff;
    if ((offset & ~3u) == regMemData) {
        if (size == 4 || !(offset & 2)) {
            // The port works on whole words: the low two address bits are ignored, and the driver's byte and
            // halfword helpers pick their part out of the word.
            const uint32_t v = mem32(memAddress & ~3u);
            if (size == 4) {
                if (memControl & memAutoIncrement) memAddress += 4;
                return v;
            }
            halfLatch = v >> 16;
            return v & 0xffff;
        }
        // The high half of the word the low half's read fetched.
        const uint32_t v = halfLatch;
        if (memControl & memAutoIncrement) memAddress += 4;
        return v;
    }
    const uint32_t v = reg(offset & ~3u, now);
    if (size == 4) return v;
    return (v >> (8 * (offset & 3))) & (size == 2 ? 0xffff : 0xff);
}

void Acx100::write(uint32_t offset, unsigned size, uint32_t value, uint64_t now) {
    offset &= 0xfff;
    if ((offset & ~3u) == regMemData) {
        if (size == 4) {
            setMem32(memAddress & ~3u, value);
        } else if (!(offset & 2)) {
            halfLatch = value & 0xffff;
            return;
        } else {
            setMem32(memAddress & ~3u, halfLatch | (value & 0xffff) << 16);
        }
        ++memWrites;
        if (memControl & memAutoIncrement) memAddress += 4;
        return;
    }
    if (size != 4) {
        // A halfword write merges into the register.
        const uint32_t shift = 8 * (offset & 3), mask = (size == 2 ? 0xffffu : 0xffu) << shift;
        value = (reg(offset & ~3u, now) & ~mask) | ((value << shift) & mask);
    }
    setReg(offset & ~3u, value, now);
}

uint32_t Acx100::reg(uint32_t offset, uint64_t now) {
    switch (offset) {
        case regSoftReset: return 0;
        case regRegAddress: return regAddress;
        case regRegData: return regAddress == regRegData || regAddress == regRegAddress ? 0 : reg(regAddress & 0xffc, now);
        case regMemAddress: return memAddress;
        case regMemControl: return memControl;
        case regEndianControl: return endianControl;
        case regIntTrigger: return 0;
        case regIrqMask: return irqMask;
        case regIrqStatus: return irqStatus;
        case regIrqAck: return 0;
        case regTime: return uint32_t(now / 1000);
        case regEepromControl: return eepromControl;
        case regEepromAddress: return eepromAddress;
        // ASSUMPTION "ACX100 EEPROM blank": every EEPROM byte reads 0 (the driver only reads byte 3).
        case regEepromData: return 0;
        case regPhyAddress: return phyAddress;
        case regPhyData: return phyData;
        case regPhyControl: return phyControl;
        case regCommandMailbox: return mailboxCommand;
        case regInfoMailbox: return mailboxInfo;
        case regEepromStart: return 0;
        case regEcpuControl: return ecpuControl;
        case regChipId: return chipId;
        default: return miscRegs[(offset >> 2) % miscCount];
    }
}

void Acx100::setReg(uint32_t offset, uint32_t value, uint64_t now) {
    switch (offset) {
        case regSoftReset:
            if (value & 1) {
                log("soft reset");
                started = booting = scanning = false;
                irqStatus = 0;
                irqMask = 0xffff;
                mailboxCommand = mailboxInfo = 0;
                rxOn = txOn = joined = false;
            }
            return;
        case regRegAddress: regAddress = value; return;
        case regRegData:
            if (regAddress != regRegData && regAddress != regRegAddress) setReg(regAddress & 0xffc, value, now);
            return;
        case regMemAddress: memAddress = value; return;
        case regMemControl: memControl = value; return;
        case regEndianControl: endianControl = value; return;
        case regIntTrigger:
            if ((value & triggerCommand) && started) command(now);
            if ((value & triggerTx) && started) transmit(now);
            if ((value & triggerRxDone) && started) reclaimRx();
            return;
        case regIrqMask: irqMask = value & 0xffff; return;
        case regIrqStatus: return;
        case regIrqAck: irqStatus &= ~value; return;
        case regEepromControl:
            // A read (2) or write (1) is done at once.
            eepromControl = 0;
            return;
        case regEepromAddress: eepromAddress = value; return;
        case regPhyAddress: phyAddress = value; return;
        case regPhyData: phyData = value; return;
        case regPhyControl:
            if (value & 1) phy[phyAddress & 0xff] = uint8_t(phyData);
            if (value & 2) phyData = phy[phyAddress & 0xff];
            phyControl = 0;
            return;
        case regCommandMailbox: mailboxCommand = value; return;
        case regInfoMailbox: mailboxInfo = value; return;
        case regEepromStart: return;
        case regEcpuControl: {
            const bool wasHalted = ecpuControl & 1;
            ecpuControl = value;
            if (!(value & 1) && wasHalted) {
                if (memWrites) {
                    booting = true;
                    bootAt = now + bootNs;
                    log("CPU let go after %llu word writes", (unsigned long long)memWrites);
                } else {
                    log("CPU let go with nothing downloaded");
                }
            } else if (value & 1) {
                started = booting = scanning = false;
            }
            return;
        }
        default: miscRegs[(offset >> 2) % miscCount] = value; return;
    }
}

void Acx100::advance(uint64_t now) {
    if (booting && now >= bootAt) {
        booting = false;
        started = true;
        mailboxCommand = commandMailbox;
        mailboxInfo = infoMailbox;
        std::vector<uint8_t> map(mmWords * 4, 0);
        put32(map, 4 * mmCodeEnd, codeEnd);
        for (int w = mmWepStart; w <= mmPoolStart; ++w) put32(map, 4 * w, codeEnd);
        put32(map, 4 * mmPoolEnd, memoryBytes);
        ies[ieMemoryMap] = map;
        // MAC addresses in the station ID setting and the join command go last byte first.
        std::vector<uint8_t> id(stationMac, stationMac + 6);
        std::reverse(id.begin(), id.end());
        ies[ieStationId] = id;
        irqStatus |= irqStarted;
        log("firmware started");
    }
    while (scanning && now >= scanStepAt) scanStep(scanStepAt);
}

uint64_t Acx100::nextEvent() const {
    uint64_t at = UINT64_MAX;
    if (booting) at = std::min(at, bootAt);
    if (scanning) at = std::min(at, scanStepAt);
    return at;
}

unsigned Acx100::listening() const {
    if (!started) return 0;
    if (scanning) return scanChannel;
    return rxOn ? channel : 0;
}

void Acx100::command(uint64_t now) {
    const uint32_t m = commandMailbox;
    const uint16_t cmd = mem16(m);
    ++commands;
    uint16_t status = 1;
    switch (cmd) {
        case cmdInterrogate: {
            const uint16_t type = mem16(m + 4), length = mem16(m + 6);
            const auto data = interrogate(type, length);
            memWrite(m + 8, data.data(), std::min<size_t>(data.size(), length));
            log("interrogate %04x length %u", type, length);
            break;
        }
        case cmdConfigure: {
            const uint16_t type = mem16(m + 4), length = mem16(m + 6);
            std::vector<uint8_t> data(std::min<uint16_t>(length, 1024));
            memRead(m + 8, data.data(), data.size());
            std::string bytes;
            for (size_t i = 0; i < std::min<size_t>(data.size(), 48); ++i) {
                char hex[4];
                std::snprintf(hex, sizeof hex, "%02x", data[i]);
                bytes += hex;
            }
            log("configure %04x length %u: %s", type, length, bytes.c_str());
            configure(type, data);
            break;
        }
        case cmdEnableRx:
            channel = mem8(m + 4);
            rxOn = true;
            log("enable RX channel %u", channel);
            break;
        case cmdEnableTx:
            channel = mem8(m + 4);
            txOn = true;
            log("enable TX channel %u", channel);
            break;
        case cmdDisableRx: rxOn = false; log("disable RX"); break;
        case cmdDisableTx: txOn = false; log("disable TX"); break;
        case cmdScan: {
            std::vector<uint8_t> params(12);
            memRead(m + 4, params.data(), params.size());
            startScan(params, now);
            break;
        }
        case cmdStopScan:
            log("stop scan");
            if (scanning) endScan();
            break;
        case cmdJoin: {
            memRead(m + 4, bssid, 6);
            std::reverse(bssid, bssid + 6);
            channel = mem8(m + 4 + 14);
            joined = true;
            log("join %02x:%02x:%02x:%02x:%02x:%02x channel %u mode %u", bssid[0], bssid[1], bssid[2], bssid[3], bssid[4],
                bssid[5], channel, mem8(m + 4 + 13));
            break;
        }
        case cmdProbeTemplate: {
            const uint16_t size = std::min<uint16_t>(mem16(m + 4), 1000);
            probeTemplate.assign(size, 0);
            memRead(m + 6, probeTemplate.data(), size);
            log("probe request template %u bytes", size);
            break;
        }
        case cmdRadioInit: {
            // The radio code sits at CodeEnd; afterwards CodeEnd moves past it.
            const uint32_t at = mem32(m + 4), length = mem32(m + 8);
            auto& map = ies[ieMemoryMap];
            if (map.size() >= mmWords * 4 && at == le32(map, 4 * mmCodeEnd)) put32(map, 4 * mmCodeEnd, (at + length + 3) & ~3u);
            log("radio init at %05x, %u bytes", at, length);
            break;
        }
        default:
            log("command %04x", cmd);
            break;
    }
    setMem16(m + 2, status);
    irqStatus |= irqCommand;
}

std::vector<uint8_t> Acx100::interrogate(uint16_t type, uint16_t length) {
    std::vector<uint8_t> out;
    auto found = ies.find(type);
    if (found != ies.end()) out = found->second;
    // The length field is the data's, after the type and length.
    out.resize(length, 0);
    return out;
}

void Acx100::configure(uint16_t type, const std::vector<uint8_t>& data) {
    if (type == ieMemoryMap && data.size() >= mmWords * 4) {
        auto map = data;
        auto& old = ies[ieMemoryMap];
        old.resize(mmWords * 4, 0);
        // The firmware sizes an area when the host moves its start.
        auto area = [&](int start, int end, uint32_t bytes) {
            if (le32(map, 4 * start) != le32(old, 4 * start) || le32(map, 4 * end) < le32(map, 4 * start))
                put32(map, 4 * end, le32(map, 4 * start) + bytes);
        };
        area(mmWepStart, mmWepEnd, wepCacheBytes);
        area(mmTemplateStart, mmTemplateEnd, templateBytes);
        put32(map, 4 * mmPoolEnd, memoryBytes);
        ies[ieMemoryMap] = map;
        return;
    }
    ies[type] = data;
    if (type == ieQueueConfig && data.size() >= 0x1c) {
        // AreaSize, RxQueueStart, options, NumTxQueues, QueueEnd, HostQueueEnd, then each TX queue's start.
        rxRingStart = rxNext = le32(data, 4);
        txRingStart = txNext = le32(data, 0x14);
        auto& map = ies[ieMemoryMap];
        if (map.size() >= mmWords * 4) put32(map, 4 * mmQueueEnd, le32(data, 0x0c));
        log("queues: RX ring %05x, %u TX rings from %05x, end %05x", rxRingStart, data[9], txRingStart, le32(data, 0x0c));
    } else if (type == ieRxConfig && data.size() >= 2) {
        rxHeaderExtra = (le16(data, 0) & rxIncludePhyHeader) ? 4 : 0;
    } else if (type == ieBlockSize && data.size() >= 2) {
        blockSize = le16(data, 0);
    } else if (type == ieMemoryConfig && data.size() >= 0x14) {
        rxPool = le32(data, 8);
        rxBlocks = std::min<uint32_t>(le16(data, 0x10), 4096);
        rxBlockUsed.assign(rxBlocks, 0);
        // The TX pool goes to the host as one free list in address order, the last block's link empty: the
        // driver takes blocks off its head and puts sent ones back on its tail.
        const uint32_t txPool = le32(data, 0x0c), txBlocks = le16(data, 0x12);
        if (blockSize >= 32)
            for (uint32_t i = 0; i < txBlocks; ++i) {
                const uint32_t block = txPool + i * blockSize;
                setMem32(block, i + 1 < txBlocks ? ((block + blockSize) >> 5) & linkMask : linkLast);
            }
        log("pool: %u RX blocks of %u from %05x, %u TX blocks from %05x", rxBlocks, blockSize, rxPool, le16(data, 0x12),
            le32(data, 0x0c));
    }
}

void Acx100::transmit(uint64_t now) {
    if (!txRingStart || blockSize <= 4) return;
    for (unsigned guard = 0; guard < 256; ++guard) {
        const uint32_t d = txNext;
        const uint8_t ctl = mem8(d + 0x24);
        if (ctl & (ctlHostOwn | ctlAcxDone)) break;
        const uint32_t length = std::min<uint32_t>(mem16(d + 0x10), 2400);
        std::vector<uint8_t> frame(length);
        uint32_t block = mem32(d + 8), done = 0;
        for (unsigned b = 0; b < 64 && done < length && block; ++b) {
            const uint32_t n = std::min(length - done, blockSize - 4);
            memRead(block + 4, frame.data() + done, n);
            done += n;
            const uint32_t link = mem32(block);
            block = (link & linkLast) ? 0 : (link & linkMask) << 5;
        }
        frame.resize(done);
        ++txFrames;
        if (trace) {
            std::string bytes;
            for (size_t i = 0; i < std::min<size_t>(frame.size(), 40); ++i) {
                char hex[4];
                std::snprintf(hex, sizeof hex, "%02x", frame[i]);
                bytes += hex;
            }
            log("TX desc %05x length %u ctl %02x/%02x rate %u: %s", d, length, ctl, mem8(d + 0x25), mem8(d + 0x2a),
                bytes.c_str());
        }
        if (airSend && txOn && !frame.empty()) airSend(frame.data(), frame.size(), channel, now);
        // Error, ACK failures, RTS failures and RTS successes: sent first time.
        setMem8(d + 0x26, 0);
        setMem8(d + 0x27, 0);
        setMem8(d + 0x28, 0);
        setMem8(d + 0x29, 0);
        setMem8(d + 0x24, ctl | ctlAcxDone);
        txNext = mem32(d);
        irqStatus |= irqTxComplete;
    }
}

void Acx100::reclaimRx() {
    if (!rxRingStart || !blockSize || !rxBlocks) return;
    uint32_t d = rxRingStart;
    for (unsigned guard = 0; guard < 256; ++guard) {
        const uint8_t ctl = mem8(d + 0x28);
        if ((ctl & (ctlHostOwn | ctlHostDone | ctlRecycle)) == (ctlHostDone | ctlRecycle)) {
            uint32_t block = mem32(d + 8);
            for (unsigned b = 0; b < rxBlocks && block >= rxPool; ++b) {
                const uint32_t index = (block - rxPool) / blockSize;
                if (index >= rxBlocks) break;
                rxBlockUsed[index] = 0;
                const uint32_t link = mem32(block);
                if (link & linkLast) break;
                block = (link & linkMask) << 5;
            }
            setMem8(d + 0x28, 0xc8);
        }
        d = mem32(d);
        if (d == rxRingStart) break;
    }
}

bool Acx100::airReceive(const uint8_t* frame, size_t size, unsigned onChannel, uint64_t now) {
    if (!started || !onChannel || onChannel != listening() || size < 10 || size > 2346) return false;
    if (!rxRingStart || blockSize <= 16 || !rxBlocks) {
        ++rxDropped;
        return false;
    }
    reclaimRx();
    const uint32_t d = rxNext;
    if (mem8(d + 0x28) & (ctlHostOwn | ctlAcxDone | ctlHostDone)) {
        ++rxDropped;
        log("RX ring full at %05x (ctl %02x)", d, mem8(d + 0x28));
        return false;
    }
    const uint32_t total = 12 + rxHeaderExtra + uint32_t(size), room = blockSize - 4;
    const uint32_t need = (total + room - 1) / room;
    std::vector<uint32_t> chain;
    for (uint32_t i = 0; i < rxBlocks && chain.size() < need; ++i)
        if (!rxBlockUsed[i]) chain.push_back(rxPool + i * blockSize);
    if (chain.size() < need) {
        ++rxDropped;
        log("RX pool full");
        return false;
    }
    std::vector<uint8_t> bytes(12 + rxHeaderExtra, 0);
    // The RX buffer header: received length, blocks, status, baseband status, PLCP signal, level, SNR, time.
    const size_t counted = size + rxHeaderExtra;
    bytes[0] = uint8_t(counted);
    bytes[1] = uint8_t((counted >> 8) & 0x0f);
    bytes[2] = uint8_t(need);
    bytes[5] = 110;
    bytes[6] = 60;
    bytes[7] = 40;
    put32(bytes, 8, uint32_t(now / 1000));
    bytes.insert(bytes.end(), frame, frame + size);
    size_t done = 0;
    for (size_t i = 0; i < chain.size(); ++i) {
        rxBlockUsed[(chain[i] - rxPool) / blockSize] = 1;
        setMem32(chain[i], i + 1 < chain.size() ? (chain[i + 1] >> 5) & linkMask : linkLast);
        const size_t n = std::min<size_t>(bytes.size() - done, room);
        memWrite(chain[i] + 4, bytes.data() + done, n);
        done += n;
    }
    setMem32(d + 0x08, chain[0]);
    setMem32(d + 0x0c, uint32_t(now / 1000));
    setMem16(d + 0x10, uint16_t(size + rxHeaderExtra));
    setMem8(d + 0x29, 110);
    setMem8(d + 0x2a, 0);
    setMem8(d + 0x2b, 40);
    setMem8(d + 0x2c, 60);
    setMem8(d + 0x28, ctlHostOwn | ctlAcxDone);
    rxNext = mem32(d);
    ++rxFrames;
    // The driver takes RX data (bit 0) and leaves RX complete masked.
    irqStatus |= irqRxData | irqRxComplete;
    log("RX %zu bytes into desc %05x (fc %02x%02x)", size, d, frame[0], frame[1]);
    return true;
}

void Acx100::startScan(const std::vector<uint8_t>& p, uint64_t now) {
    const uint16_t count = le16(p, 0), duration = le16(p, 8);
    scanActive = !(p[7] & 1);
    scanDwell = (duration && duration < 1000 ? duration : 100) * tuNs;
    scanLeft = (count == 0 ? 1 : std::min<uint16_t>(count, 4)) * (scanLast - scanFirst + 1);
    scanChannelBefore = channel;
    scanning = true;
    scanChannel = 0;
    log("scan: count %u, start %u, flags %04x, rate %u, options %02x, %u TU a channel", count, le16(p, 2), le16(p, 4),
        p[6], p[7], duration);
    scanStepAt = now;
    scanStep(now);
}

void Acx100::scanStep(uint64_t now) {
    if (!scanLeft) {
        endScan();
        return;
    }
    --scanLeft;
    scanChannel = scanChannel >= scanLast || scanChannel < scanFirst ? scanFirst : scanChannel + 1;
    scanStepAt = now + scanDwell;
    if (airTuned) airTuned(scanChannel, now);
    if (scanActive && !probeTemplate.empty() && airSend) airSend(probeTemplate.data(), probeTemplate.size(), scanChannel, now);
}

void Acx100::endScan() {
    scanning = false;
    channel = scanChannelBefore;
    irqStatus |= irqScan;
    log("scan complete");
}

std::vector<uint8_t> Acx100::flattenSettings() const {
    std::vector<uint8_t> out;
    for (const auto& kv : ies) {
        out.push_back(uint8_t(kv.first));
        out.push_back(uint8_t(kv.first >> 8));
        out.push_back(uint8_t(kv.second.size()));
        out.push_back(uint8_t(kv.second.size() >> 8));
        out.insert(out.end(), kv.second.begin(), kv.second.end());
    }
    return out;
}

bool Acx100::unflattenSettings(const std::vector<uint8_t>& flat) {
    std::map<uint16_t, std::vector<uint8_t>> parsed;
    size_t at = 0;
    while (at + 4 <= flat.size()) {
        const uint16_t type = le16(flat, at), length = le16(flat, at + 2);
        at += 4;
        if (length > 1024 || at + length > flat.size()) return false;
        parsed[type].assign(flat.begin() + at, flat.begin() + at + length);
        at += length;
    }
    if (at != flat.size()) return false;
    ies = std::move(parsed);
    return true;
}
