#ifndef FROND_ACX100_H
#define FROND_ACX100_H

// The TI ACX100 (TNETW1100B) Wi-Fi MAC as the HP iPAQ rx1950's tiacxwln.dll drives it on nGCS4: the slave
// register window, the chip's memory behind it, the firmware download and the firmware's side of the host
// interface (command and info mailboxes, the TX and RX descriptor rings and the memory block pool).
//
// The chip's own CPU isn't emulated: once the downloaded firmware is started, this class answers as that
// firmware does (ASSUMPTION "ACX100 firmware stand-in"). Frames go to and come from the air through
// airSend and airReceive, as whole 802.11 frames without the FCS.
//
// Register window (offsets from the chip's base, 32-bit registers; the driver reaches the ones from 0x20 up
// through the indirect pair at 0x04 and 0x08):
//   0x00 soft reset, 0x04/0x08 register address and data, 0x14/0x18 memory address and data (0x1c bit 16
//   auto-increments by 4), 0x20 memory endian control, 0x7c host to chip triggers (1 command, 4 TX, 8 RX
//   done), 0x98 interrupt mask, 0xa4 interrupt status, 0xac interrupt acknowledge, 0x250-0x258 EEPROM,
//   0x268-0x270 radio registers, 0x2a4/0x2a8 command and info mailbox addresses, 0x2d0 EEPROM load,
//   0x2d8 ECPU control (bit 0 halts the chip's CPU), 0x900 the chip ID.
//
// Interrupt status bits: 0x0002 TX complete, 0x0008 RX complete, 0x0200 command complete, 0x0400 info,
// 0x2000 scan complete, 0x4000 firmware started. irq() is the level the chip drives.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

class Acx100 {
   public:
    // ASSUMPTION "ACX100 memory": 128 KB of chip memory; the firmware image ends below 0xb000 and the
    // stand-in keeps its mailboxes after it, below CodeEnd.
    static constexpr uint32_t memoryBytes = 0x20000;
    static constexpr uint32_t codeEnd = 0xc000, commandMailbox = 0xb000, infoMailbox = 0xb800;
    static constexpr uint32_t chipId = 0x8400104c;
    enum : uint32_t {
        irqRxData = 0x1, irqTxComplete = 0x2, irqRxComplete = 0x8, irqCommand = 0x200, irqInfo = 0x400,
        irqScan = 0x2000, irqStarted = 0x4000,
    };

    Acx100();
    // Power on or the reset line: registers to their reset values, the CPU halted, memory cleared.
    void reset();
    // Bus accesses at an offset in the chip's window, 2 or 4 bytes; now is guest nanoseconds.
    uint32_t read(uint32_t offset, unsigned size, uint64_t now);
    void write(uint32_t offset, unsigned size, uint32_t value, uint64_t now);
    bool irq() const { return (irqStatus & ~irqMask & 0xffff) != 0; }
    // Runs what's due by now (scan steps, the firmware start). nextEvent is when the next one is due,
    // UINT64_MAX with none.
    void advance(uint64_t now);
    uint64_t nextEvent() const;

    // A frame on the air on a channel. Taken when the receiver is on that channel, the RX ring has a free
    // descriptor and the pool has room; false when it's dropped.
    bool airReceive(const uint8_t* frame, size_t size, unsigned channel, uint64_t now);
    // Frames the chip sends: the 802.11 frame and the channel.
    std::function<void(const uint8_t* frame, size_t size, unsigned channel, uint64_t now)> airSend;
    // The receiver tuned to a channel during a scan.
    std::function<void(unsigned channel, uint64_t now)> airTuned;
    // The channel the receiver listens on, 0 while it doesn't.
    unsigned listening() const;
    bool running() const { return started; }
    const uint8_t* mac() const { return stationMac; }

    // Trace lines (FROND_ACX_TRACE), or nothing.
    std::function<void(const std::string&)> trace;

    // Counts for tools and tests.
    uint64_t commands = 0, txFrames = 0, rxFrames = 0, rxDropped = 0;

    template <class A> void state(A& a) {
        a.DoBool(started).DoBool(booting).Do64(bootAt).Do32(regAddress).Do32(memAddress).Do32(memControl);
        a.Do32(endianControl).Do32(irqMask).Do32(irqStatus).Do32(ecpuControl).Do32(eepromControl);
        a.Do32(eepromAddress).Do32(phyAddress).Do32(phyData).Do32(phyControl).Do32(mailboxCommand).Do32(mailboxInfo);
        a.Do32(halfLatch).Do64(memWrites);
        a.DoBuffer32(miscRegs, miscCount);
        a.DoBuffer(phy, sizeof phy);
        a.DoVector(memory, memoryBytes);
        a.Require(memory.size() == memoryBytes, "ACX100 memory");
        a.Do32(rxRingStart).Do32(rxNext).Do32(txRingStart).Do32(txNext).Do32(blockSize).Do32(rxPool).Do32(rxBlocks);
        a.DoVector(rxBlockUsed, 4096);
        a.Do32(channel).DoBool(rxOn).DoBool(txOn).DoBool(joined).DoBuffer(bssid, 6);
        a.DoBool(scanning).Do32(scanChannel).Do32(scanLeft).Do64(scanStepAt).Do64(scanDwell).Do32(scanChannelBefore);
        a.DoBool(scanActive).Do32(rxHeaderExtra);
        a.Do64(commands).Do64(txFrames).Do64(rxFrames).Do64(rxDropped);
        std::vector<uint8_t> flat = flattenSettings();
        a.DoVector(flat, 1u << 18);
        a.Require(unflattenSettings(flat), "ACX100 settings");
        a.DoVector(probeTemplate, 1024);
    }

   private:
    // Registers that only hold what's written.
    static constexpr unsigned miscCount = 0x1000 / 4;
    uint32_t miscRegs[miscCount];
    std::vector<uint8_t> memory;
    bool started = false, booting = false;
    uint64_t bootAt = 0;
    uint32_t regAddress = 0, memAddress = 0, memControl = 0, endianControl = 0, irqMask = 0xffff, irqStatus = 0;
    uint32_t ecpuControl = 1, eepromControl = 0, eepromAddress = 0, phyAddress = 0, phyData = 0, phyControl = 0;
    uint32_t mailboxCommand = 0, mailboxInfo = 0, halfLatch = 0;
    uint64_t memWrites = 0;
    uint8_t phy[256];
    uint8_t stationMac[6];

    // The firmware's view of the host's layout, from the queue and memory settings.
    uint32_t rxRingStart = 0, rxNext = 0, txRingStart = 0, txNext = 0, blockSize = 0, rxPool = 0, rxBlocks = 0;
    std::vector<uint8_t> rxBlockUsed;
    uint32_t channel = 0;
    bool rxOn = false, txOn = false, joined = false;
    uint8_t bssid[6] = {};
    bool scanning = false, scanActive = false;
    uint32_t scanChannel = 0, scanLeft = 0, scanChannelBefore = 0;
    uint64_t scanStepAt = 0, scanDwell = 0;
    // Bytes between the RX buffer header and the 802.11 frame: the PHY header when the RX config asks for it
    // (ASSUMPTION "ACX100 PHY header": 4 bytes, left zero; the driver skips them).
    uint32_t rxHeaderExtra = 0;
    std::map<uint16_t, std::vector<uint8_t>> ies;
    std::vector<uint8_t> probeTemplate;

    uint32_t reg(uint32_t offset, uint64_t now);
    void setReg(uint32_t offset, uint32_t value, uint64_t now);
    uint32_t mem32(uint32_t address) const;
    void setMem32(uint32_t address, uint32_t value);
    uint16_t mem16(uint32_t address) const;
    void setMem16(uint32_t address, uint16_t value);
    uint8_t mem8(uint32_t address) const { return memory[address % memoryBytes]; }
    void setMem8(uint32_t address, uint8_t value) { memory[address % memoryBytes] = value; }
    void memRead(uint32_t address, uint8_t* out, size_t n) const;
    void memWrite(uint32_t address, const uint8_t* in, size_t n);
    void command(uint64_t now);
    void configure(uint16_t type, const std::vector<uint8_t>& data);
    std::vector<uint8_t> interrogate(uint16_t type, uint16_t length);
    void transmit(uint64_t now);
    void reclaimRx();
    void startScan(const std::vector<uint8_t>& params, uint64_t now);
    void scanStep(uint64_t now);
    void endScan();
    void log(const char* format, ...);
    std::vector<uint8_t> flattenSettings() const;
    bool unflattenSettings(const std::vector<uint8_t>& flat);
};

#endif
