#ifndef FROND_S3C2442_BLOCKS_H
#define FROND_S3C2442_BLOCKS_H

// The Samsung S3C2442's on-chip blocks, from the S3C2440/S3C2442 user's manual. Each block takes word
// register accesses at its own offsets and answers unknown, which the machine turns into a refusal that
// names the address and pc. No block touches the CPU: interrupts go through S3cIntc and time comes in as
// clock ticks from the machine.

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "../../common/UsbPacketResult.h"

// unknown is a register the block doesn't have, unsupported a known one set to something not modelled.
enum class S3cAccess : uint8_t { ok, unknown, unsupported };

// Interrupt sources, the bit numbers of SRCPND (manual 14.3).
namespace s3cIrq {
    constexpr unsigned eint0 = 0, eint1 = 1, eint2 = 2, eint3 = 3, eint4to7 = 4, eint8to23 = 5, cam = 6,
                       battFault = 7, tick = 8, wdtAc97 = 9, timer0 = 10, timer4 = 14, uart2 = 15, lcd = 16,
                       dma0 = 17, sdi = 21, spi0 = 22, uart1 = 23, nand = 24, usbd = 25, usbh = 26, iic = 27,
                       uart0 = 28, spi1 = 29, rtc = 30, adc = 31;
}
// Sub-sources, the bit numbers of SUBSRCPND.
namespace s3cSubIrq {
    constexpr unsigned rxd0 = 0, txd0 = 1, err0 = 2, tc = 9, adc = 10, camC = 11, camP = 12, wdt = 13, ac97 = 14;
}

// Interrupt controller at 0x4A000000 (manual chapter 14).
class S3cIntc {
   public:
    void reset();
    S3cAccess read(uint32_t offset, uint32_t& value);
    S3cAccess write(uint32_t offset, uint32_t value);
    // An edge request: the source's SRCPND bit is set once.
    void pulse(unsigned source);
    // A level request: SRCPND stays set while it's on, even after a clear.
    void level(unsigned source, bool on);
    void subPulse(unsigned sub);
    void subLevel(unsigned sub, bool on);
    bool irq() const { return intpnd != 0; }
    bool fiq() const { return fiqLine; }
    uint32_t pending() const { return srcpnd; }

    uint32_t srcpnd, intmod, intmsk, priority, intpnd, intoffset, subsrcpnd, intsubmsk;
    // The saved state's fields (machine_state_archive.h).
    template <class A> void state(A& a) {
        a.Do32(srcpnd).Do32(intmod).Do32(intmsk).Do32(priority).Do32(intpnd).Do32(intoffset).Do32(subsrcpnd);
        a.Do32(intsubmsk).Do32(levels).Do32(subLevels).DoBool(fiqLine);
    }

   private:
    void update();
    int arbitrate(uint32_t requests);
    uint32_t levels = 0, subLevels = 0;
    bool fiqLine = false;
};

// Clock and power management at 0x4C000000 (manual chapter 7). Crystal in Hz.
class S3cClock {
   public:
    explicit S3cClock(uint32_t crystal = 16934400) : crystal(crystal) { reset(); }
    void reset();
    S3cAccess read(uint32_t offset, uint32_t& value);
    S3cAccess write(uint32_t offset, uint32_t value);
    uint64_t mpll() const;
    uint64_t upll() const;
    // FCLK, the CPU's clock: HCLK while DVS is on.
    uint64_t cpuClock() const;
    uint64_t fclk() const;
    uint64_t hclk() const;
    uint64_t pclk() const;
    bool dvs() const { return camdivn & 0x1000; }
    // CLKCON's IDLE bit, the CPU's clock held until an interrupt. Cleared by the wake.
    bool idle() const { return clkcon & 4; }
    void wake() { clkcon &= ~4u; }
    // CLKCON's POWER_OFF bit, sleep mode.
    bool powerOff() const { return clkcon & 8; }
    // Called after any write that changes a clock rate.
    std::function<void()> changed;

    uint32_t locktime, mpllcon, upllcon, clkcon, clkslow, clkdivn, camdivn;
    template <class A> void state(A& a) {
        a.Do32(locktime).Do32(mpllcon).Do32(upllcon).Do32(clkcon).Do32(clkslow).Do32(clkdivn).Do32(camdivn);
    }
    const uint32_t crystal;
};

// SDRAM and bank control at 0x48000000 (manual chapter 5). The SDRAM itself needs no setup here, so the
// registers are only kept.
class S3cMemoryController {
   public:
    void reset();
    S3cAccess read(uint32_t offset, uint32_t& value);
    S3cAccess write(uint32_t offset, uint32_t value);
    template <class A> void state(A& a) { a.DoBuffer32(regs, 13); }
    uint32_t regs[13];
};

// The five PWM timers at 0x51000000 (manual chapter 10), counted in PCLK ticks.
class S3cPwm {
   public:
    void reset();
    S3cAccess read(uint32_t offset, uint32_t& value);
    S3cAccess write(uint32_t offset, uint32_t value);
    void advance(uint64_t pclkTicks, S3cIntc& intc);
    // PCLK ticks until the next interrupt of a timer in timers (bit n timer n), UINT64_MAX with none due.
    uint64_t ticksToEvent(uint32_t timers = 0x1f) const;
    // The timer's count, TCNTO.
    uint32_t count(unsigned timer) const { return timers[timer].count; }

    uint32_t tcfg0, tcfg1, tcon;
    struct Timer {
        uint32_t buffer, compare, count, compareLatch;
        uint64_t phase;
        bool running;
    } timers[5];
    template <class A> void state(A& a) {
        a.Do32(tcfg0).Do32(tcfg1).Do32(tcon);
        for (auto& t : timers)
            a.Do32(t.buffer).Do32(t.compare).Do32(t.count).Do32(t.compareLatch).Do64(t.phase).DoBool(t.running);
    }

   private:
    uint64_t divisor(unsigned timer) const;
    bool autoReload(unsigned timer) const;
    void writeTcon(uint32_t value);
};

// Watchdog at 0x53000000 (manual chapter 18), in PCLK ticks.
class S3cWatchdog {
   public:
    void reset();
    S3cAccess read(uint32_t offset, uint32_t& value);
    S3cAccess write(uint32_t offset, uint32_t value);
    void advance(uint64_t pclkTicks, S3cIntc& intc);
    // PCLK ticks until the count runs out with the reset on, or with the interrupt on when interrupts is
    // set. UINT64_MAX when it can't run out (stopped, or WTCNT and WTDAT both 0).
    uint64_t ticksToEvent(bool interrupts = true) const;
    // Set when the count ran out with the reset output on; the machine stops on it.
    bool resetRequested = false;

    uint32_t wtcon, wtdat, wtcnt;
    uint64_t phase;
    template <class A> void state(A& a) { a.DoBool(resetRequested).Do32(wtcon).Do32(wtdat).Do32(wtcnt).Do64(phase); }

   private:
    uint64_t divisor() const;
};

// The card in the SD slot, as the SD host sees it (the machine wraps the uARM card, vSD.h). command gives the
// card's response in reply, MSB first as on the wire (17 bytes for a 136-bit one): 0 for none, 48 or 136
// bits, and busy set for an R1b. The block calls are whole data blocks; false is a card error.
struct S3cSdCard {
    virtual ~S3cSdCard() = default;
    virtual unsigned command(uint8_t index, uint32_t argument, uint8_t reply[17], bool& busy) = 0;
    virtual bool readBlock(uint8_t* data, uint32_t size) = 0;
    virtual bool writeBlock(const uint8_t* data, uint32_t size) = 0;
};

// SD host interface at 0x5a000000 (manual chapter 19), S3C2440 register layout (SDIIMSK at 0x3c, SDIDAT at
// 0x40). A command runs at once when CmdStart is written: no card or no answer times out (CmdTout) when a
// response is wanted, and a response sets RspFin and the SDIRSP registers (RSP0 bits 127:96 or 39:8). A data
// transfer is set by SDIDCON: it starts at once, or after the next command with RACMD, TARSP or BACMD. Data
// moves a block at a time through the card: the FIFO shows what's left of the block, up to 64 bytes, to the
// CPU at SDIDAT and to the DMA (dmaWants, fifoRead, fifoWrite), and the last byte of the last block sets
// DatFin. ASSUMPTION "SDI transfers take no time": the card answers and the FIFO fills or drains at once,
// with no clock or data timer. ASSUMPTION "SDI start without DTST": a transfer starts with DatMode set
// whether DTST is set or not, as the WM5 driver's polled path leaves it clear. INT_SDI is a level from the
// status bits SDIIMSK lets through (irq()).
class S3cSdi {
public:
    void reset();
    S3cAccess read(uint32_t offset, unsigned size, uint32_t& value);
    S3cAccess write(uint32_t offset, uint32_t value, unsigned size);
    // The card, or nullptr for an empty slot. Not saved; the owner puts it back before a load.
    S3cSdCard* card = nullptr;
    // The card's supply went off: an unfinished transfer stops.
    void cardRemoved();
    bool irq() const;
    // The DMA's share of SDIDAT: whether a unit of bytes can move now, and the moves.
    bool dmaWants(unsigned bytes) const;
    uint32_t fifoRead(unsigned bytes);
    void fifoWrite(unsigned bytes, uint32_t value);
    uint64_t commands = 0, blocksRead = 0, blocksWritten = 0, cardErrors = 0;
    template <class A> void state(A& a) { a.Do64(commands).DoBuffer32(regs, 17); }
    // The transfer under way, kept in the machine's card section.
    template <class A> void runState(A& a) {
        a.Do64(blocksRead).Do64(blocksWritten).Do64(cardErrors);
        a.DoBool(armed).DoBool(active).DoBool(loaded).Do32(blocksLeft).Do32(blockSize).Do32(position);
        a.DoBuffer(block, sizeof block);
        a.Require(blockSize <= sizeof block && position <= blockSize, "SDI block");
    }
    void clearRun() {
        armed = active = loaded = false;
        blocksLeft = blockSize = position = 0;
        blocksRead = blocksWritten = cardErrors = 0;
    }

private:
    uint32_t regs[17];  // SDICON to SDIDAT, by word
    bool armed = false, active = false, loaded = false;
    uint32_t blocksLeft = 0, blockSize = 0, position = 0;
    uint8_t block[4096];
    void command(uint32_t value);
    void start();
    void finish();
    void loadBlock();
    bool receiving() const;
    uint32_t available() const;
    uint32_t fifoStatus() const;
    uint32_t pop(unsigned bytes);
    void push(unsigned bytes, uint32_t value);
};

// USB device controller at 0x52000000 (manual chapter 13): five endpoints, EP0 for control with a 16-byte
// FIFO and EP1-4 as bulk IN or OUT (IN_CSR2 MODE_IN) with 64-byte FIFOs, driven by the CPU through the
// FIFO registers. The host side (UsbHostBridge through SocS3C2442) hands it whole packets: setup, out and in
// return Ack, Nak or Stall as the endpoint's CSR bits say, and raise the endpoint interrupt like the real one.
// Registers are bytes at word addresses. The endpoint DMA registers (0x200 to 0x26c) keep what's written;
// a transfer with DMA_MODE_EN set isn't modelled (ASSUMPTION "USB endpoint DMA unused": WM5's
// sc2440usbfn.dll was seen moving every packet through the FIFO registers, see the R8 notes).
class S3cUsbDevice {
public:
    void reset();
    S3cAccess read(uint32_t offset, uint32_t& value);
    S3cAccess write(uint32_t offset, uint32_t value);
    // The interrupt line to INT_USBD: an enabled endpoint or bus interrupt is pending.
    bool irq() const;
    // After a guest access at offset: whether it can have changed what the host sees. Writes but FIFO data, a
    // FIFO write that left IN_PKT_RDY set (AUTO_SET), a FIFO read with OUT_PKT_RDY clear (AUTO_CLR or no packet).
    bool hostVisible(uint32_t offset, bool write) const;

    // Host side. A bus reset puts the address and endpoints back and raises the reset interrupt.
    void busReset();
    UsbPacketResult setup(const uint8_t* data);
    UsbPacketResult out(unsigned ep, const uint8_t* data, unsigned size);
    // take false only peeks (size of the packet ready).
    UsbPacketResult in(unsigned ep, uint8_t* data, unsigned* size, bool take);
    bool outReady(unsigned ep) const;
    // The address the device answers to: FUNC_ADDR once the status stage that set it is over.
    uint8_t address() const { return activeAddress; }
    // Counts of host packets taken and given, for tests and traces.
    uint64_t setups{0}, outs{0}, ins{0};

    // The register file 0x140 to 0x1fc, as states before R8 saved it.
    template <class A> void state(A& a) { a.DoBuffer32(regs, 0x30); }
    // Everything else (endpoint CSRs, FIFOs, the control stage, the active address), its own state section.
    template <class A> void runState(A& a) {
        a.DoBuffer(maxp, 5).DoBuffer(inCsr1, 5).DoBuffer(inCsr2, 5).DoBuffer(outCsr1, 5).DoBuffer(outCsr2, 5);
        for (auto& f : fifo) a.DoVector(f, 256);
        a.DoBuffer32(dmaRegs, 0x1c).Do8(activeAddress).Do8(ep0Csr).DoBool(controlOpen).DoBool(addressPending).DoBool(setupIn);
    }
    // States saved before R8: endpoints as after a reset, keeping the register file.
    void clearRun();

    uint32_t regs[0x30];  // 0x140 to 0x1fc, by word; the indexed registers live in the arrays below
    uint8_t maxp[5]{}, inCsr1[5]{}, inCsr2[5]{}, outCsr1[5]{}, outCsr2[5]{};
    uint8_t ep0Csr{0};
    std::vector<uint8_t> fifo[5];
    uint32_t dmaRegs[0x1c]{};  // 0x200 to 0x26c
    uint8_t activeAddress{0};
    // A control transfer is between its SETUP and its status stage.
    bool controlOpen{false}, addressPending{false}, setupIn{false};

private:
    uint32_t& reg(uint32_t offset) { return regs[(offset - 0x140) / 4]; }
    uint32_t reg(uint32_t offset) const { return regs[(offset - 0x140) / 4]; }
    void endpointInterrupt(unsigned ep) { reg(0x148) |= 1u << ep; }
    void statusDone();
    unsigned index() const { return reg(0x178) & 7; }
    unsigned packetSize(unsigned ep) const;
};

// ADC and touch screen at 0x58000000 (manual chapter 16), counted in PCLK ticks. A conversion takes
// ADCDLY ticks in the touch modes (separate X or Y, auto) and then 5 ADC clocks, then sets ECFLG, the
// data registers and the ADC sub-interrupt (INT_ADC_S). In wait-for-interrupt mode (XY_PST 3) the touch
// sub-interrupt (INT_TC) comes when the stylus goes the way UD_SEN waits for, with ADCUPDN's history bit.
// The stylus comes from the host as a position in the panel's raw ADC units (setStylus).
// Channels 0-7 read channel[]: mid-scale after reset; the rx1950's battery inputs (AIN0-3) come from
// rx1950_power.h.
class S3cAdc {
public:
    void reset();
    S3cAccess read(uint32_t offset, uint32_t& value);
    S3cAccess write(uint32_t offset, uint32_t value, S3cIntc& intc);
    void advance(uint64_t pclkTicks, S3cIntc& intc);
    // PCLK ticks until the conversion ends, UINT64_MAX with none running.
    uint64_t ticksToEvent() const;
    // The stylus down at raw position x, y (10 bits each), or up.
    void setStylus(bool down, uint16_t x, uint16_t y, S3cIntc& intc);
    bool stylusDown() const { return penDown; }

    uint16_t channel[8];  // 10-bit inputs AIN0 to AIN7
    uint64_t conversions = 0, touchInterrupts = 0;
    uint32_t channelsRead = 0;  // a bit per channel converted in normal mode, for traces
    int firstChannel = -1;      // the first channel converted in normal mode
    uint32_t adccon, adctsc, adcdly, adcdat0, adcdat1, adcupdn;
    template <class A> void state(A& a) {
        a.DoBuffer16(channel, 8).Do64(conversions).Do64(touchInterrupts).Do32(channelsRead);
        uint32_t first = uint32_t(firstChannel);
        a.Do32(first);
        firstChannel = int(first);
        a.Do32(adccon).Do32(adctsc).Do32(adcdly).Do32(adcdat0).Do32(adcdat1).Do32(adcupdn).Do64(remaining);
        a.DoBool(penDown).DoBool(touchCondition).Do16(penX).Do16(penY);
    }

private:
    void start();
    void finish(S3cIntc& intc);
    void checkTouch(S3cIntc& intc);
    uint64_t remaining = 0;  // PCLK ticks left in the conversion, 0 with none running
    bool penDown = false, touchCondition = false;
    uint16_t penX = 0, penY = 0;
};

// DMA at 0x4b000000 (manual chapter 8), four channels of 0x40. Turning a channel on loads its current source,
// destination and count from the initial registers; each hardware request then moves one unit (four with
// TSZ burst) through bus, counts down and at 0 raises the channel's interrupt (INT) and reloads from the
// initial registers (auto reload) or turns the channel off (RELOAD set). Requests come from the machine for
// the IIS (I2SSDO on channel 2 source 0, I2SSDI on channel 1 source 2 and channel 2 source 1) and the SDI
// (channel 0 source 2, channel 2 source 2, channel 3 source 1); the other sources never ask. A software
// trigger is refused (S3cAccess::unsupported).
enum class S3cDmaRequest : uint8_t { iisOut, iisIn, sdi };
class S3cDma {
public:
    void reset();
    S3cAccess read(uint32_t offset, uint32_t& value);
    S3cAccess write(uint32_t offset, uint32_t value);
    // The channel on and set to hardware requests from the source, -1 with none.
    int channelFor(S3cDmaRequest request) const;
    // One service of a hardware request on channel ch.
    void service(unsigned ch, S3cIntc& intc);
    // Units left in the channel's current count, 0 when it's off.
    uint32_t remaining(unsigned ch) const { return on(ch) ? current[ch].count : 0; }
    // Bytes a unit moves (DSZ), 0 for the reserved size.
    uint32_t unitBytes(unsigned ch) const;
    // Bytes one count (an atomic transfer) moves: the unit, or four of them with TSZ set.
    uint32_t transferBytes(unsigned ch) const { return unitBytes(ch) * ((channel[ch].dcon & (1u << 28)) ? 4 : 1); }
    bool interruptAtEnd(unsigned ch) const { return channel[ch].dcon & (1u << 29); }
    bool on(unsigned ch) const { return channel[ch].dmasktrig & 2; }
    // A unit's move: physical address, size in bytes, write, the value. False where there's nothing there,
    // and the unit is dropped (counted in busErrors).
    std::function<bool(uint32_t, unsigned, bool, uint32_t&)> bus;
    uint64_t units = 0, busErrors = 0;
    uint32_t lastBusError = 0;
    template <class A> void state(A& a) {
        for (auto& c : channel) a.Do32(c.disrc).Do32(c.disrcc).Do32(c.didst).Do32(c.didstc).Do32(c.dcon).Do32(c.dmasktrig);
    }
    // The current registers and counts, kept in the machine's audio section.
    template <class A> void runState(A& a) {
        for (auto& c : current) a.Do32(c.src).Do32(c.dst).Do32(c.count);
        a.Do64(units).Do64(busErrors).Do32(lastBusError);
    }
    // For a state saved without them: no channel mid-run.
    void clearRun() {
        for (auto& c : current) c = {};
        units = busErrors = 0;
        lastBusError = 0;
    }

private:
    struct Channel { uint32_t disrc, disrcc, didst, didstc, dcon, dmasktrig; } channel[4];
    struct Current { uint32_t src, dst, count; } current[4];
    void load(unsigned ch);
};

// IIS at 0x55000000 (manual chapter 21) with its two 32-entry 16-bit FIFOs. The machine calls frame() at the
// sample rate (frameDivider() master clocks a frame): a transmitting interface takes the left and right
// samples from the transmit FIFO (silence and an underrun when it's short), a receiving one puts the codec's
// two samples in the receive FIFO (dropped and an overrun when it's full). The CPU reaches the FIFOs through
// IISFIFO, the DMA through the same address; the machine asks for DMA while the FIFOs want it (txWantsDma,
// rxWantsDma).
class S3cIis {
public:
    static constexpr unsigned depth = 32;
    void reset();
    S3cAccess read(uint32_t offset, uint32_t& value);
    S3cAccess write(uint32_t offset, uint32_t value);
    // A halfword in or out of IISFIFO (the CPU's or the DMA's access).
    void push(uint16_t value);
    uint16_t pop();
    bool enabled() const { return iiscon & 1; }
    bool transmitting() const { return enabled() && (iismod & 0x80) && !(iiscon & 8); }
    bool receiving() const { return enabled() && (iismod & 0x40) && !(iiscon & 4); }
    bool txDmaOn() const { return (iiscon & 0x20) && (iisfcon & 0x8000); }
    bool rxDmaOn() const { return (iiscon & 0x10) && (iisfcon & 0x4000); }
    bool txWantsDma() const { return (iiscon & 0x20) && (iisfcon & 0x8000) && (iisfcon & 0x2000) && txCount < depth; }
    bool rxWantsDma() const { return (iiscon & 0x10) && (iisfcon & 0x4000) && (iisfcon & 0x1000) && rxCount > 0; }
    // IISMOD MPLLin (true) or PCLK as the master clock.
    bool mpllIn() const { return iismod & 0x200; }
    // Master clocks a frame: 384 or 256 (IISMOD bit 2).
    uint32_t frameDivider() const { return (iismod & 4) ? 384 : 256; }
    // One frame: out gets the transmitted samples when transmitting (false otherwise), in the received ones.
    bool frame(int16_t out[2], const int16_t in[2]);
    uint64_t framesSent = 0, framesReceived = 0, underruns = 0, overruns = 0;
    template <class A> void state(A& a) { a.Do32(iiscon).Do32(iismod).Do32(iispsr).Do32(iisfcon); }
    template <class A> void runState(A& a) {
        a.DoBuffer16(tx, depth).DoBuffer16(rx, depth).Do32(txHead).Do32(txCount).Do32(rxHead).Do32(rxCount);
        a.Do64(framesSent).Do64(framesReceived).Do64(underruns).Do64(overruns);
    }
    void clearRun() {
        txHead = txCount = rxHead = rxCount = 0;
        framesSent = framesReceived = underruns = overruns = 0;
    }

private:
    uint32_t iiscon, iismod, iispsr, iisfcon;
    uint16_t tx[depth], rx[depth];
    uint32_t txHead, txCount, rxHead, rxCount;
};

// IIC at 0x54000000 (manual chapter 20) as master. A start or a byte finishes at once with the interrupt
// pending bit set. The board's devices answer through device: start gets the address byte, a byte sent goes
// to write, a byte read comes from read, and stop ends the transfer. ASSUMPTION "IIC devices ACK": every byte
// is acknowledged, with or without a device there.
struct S3cIicDevice {
    virtual ~S3cIicDevice() = default;
    virtual void start(uint8_t addressByte) = 0;
    virtual void write(uint8_t byte) = 0;
    virtual uint8_t read() = 0;
    virtual void stop() = 0;
};
class S3cIic {
public:
    void reset();
    S3cAccess read(uint32_t offset, uint32_t& value);
    S3cAccess write(uint32_t offset, uint32_t value);
    S3cIicDevice* device = nullptr;
    uint64_t bytes = 0;
    template <class A> void state(A& a) { a.Do64(bytes).Do32(iiccon).Do32(iicstat).Do32(iicadd).Do32(iicds).Do32(iiclc); }

private:
    uint32_t iiccon, iicstat, iicadd, iicds, iiclc;
};

// Real time clock at 0x57000000 (manual chapter 17), driven by the 32768 Hz crystal.
class S3cRtc {
   public:
    struct Time { uint8_t year, month, date, day, hour, minute, second; };
    // The start time: decimal fields the registers get in BCD, day of week 1-7, and year the raw BCDYEAR
    // byte, which the board's OS reads its own way (see rx1950RtcStart).
    void reset(const Time& start);
    S3cAccess read(uint32_t offset, uint32_t& value);
    S3cAccess write(uint32_t offset, uint32_t value);
    void advance(uint64_t ticks32k, S3cIntc& intc);
    // 32 kHz ticks until the next tick interrupt (tick) or alarm check (alarmCheck), UINT64_MAX with none due.
    uint64_t ticksToEvent(bool tick = true, bool alarmCheck = true) const;

    uint32_t rtccon, ticnt, rtcalm, rtcrst, alarm[6], bcd[7];
    uint64_t secondPhase, tickPhase;
    template <class A> void state(A& a) {
        a.Do32(rtccon).Do32(ticnt).Do32(rtcalm).Do32(rtcrst).DoBuffer32(alarm, 6).DoBuffer32(bcd, 7);
        a.Do64(secondPhase).Do64(tickPhase);
    }

   private:
    void secondPassed(S3cIntc& intc);
};

// GPIO ports A to J with the external interrupts, MISCCR and GSTATUS at 0x56000000 (manual chapter 9).
// Input levels come from the board; every pin reads high unless set.
class S3cGpio {
   public:
    enum Port : unsigned { a, b, c, d, e, f, g, h, j, ports };
    // gstatus1 is the chip ID, gstatus2 the reset cause.
    void reset(uint32_t chipId, uint32_t resetCause);
    S3cAccess read(uint32_t offset, uint32_t& value);
    S3cAccess write(uint32_t offset, uint32_t value, S3cIntc& intc);
    // Sets the board's level on the pins in mask, then takes any EINT edge or level it makes.
    void setInputs(Port port, uint32_t mask, uint32_t levels, S3cIntc& intc);
    // A pin's level as the port reads it.
    bool pin(Port port, unsigned bit) const;
    static uint32_t pinMask(Port port);

    uint32_t con[ports], dat[ports], up[ports], input[ports];
    uint32_t misccr, dclkcon, extint[3], eintflt[4], eintmask, eintpend, gstatus[5], dsc[2], mslcon;
    template <class A> void state(A& a) {
        a.DoBuffer32(con, ports).DoBuffer32(dat, ports).DoBuffer32(up, ports).DoBuffer32(input, ports);
        a.Do32(misccr).Do32(dclkcon).DoBuffer32(extint, 3).DoBuffer32(eintflt, 4).Do32(eintmask).Do32(eintpend);
        a.DoBuffer32(gstatus, 5).DoBuffer32(dsc, 2).Do32(mslcon).Do32(lastEintLevels);
    }

   private:
    // Re-checks every EINT pin against the last levels seen, for edges and held levels.
    void scanEints(S3cIntc& intc);
    bool eintPin(unsigned eint, bool& level) const;
    uint32_t lastEintLevels = 0;
};

// One UART at 0x50000000 + 0x4000 * channel (manual chapter 11). Sent bytes go straight to the sink,
// so the transmitter always reads empty.
class S3cUart {
   public:
    explicit S3cUart(unsigned channel = 0) : channel(channel) { reset(); }
    void reset();
    S3cAccess read(uint32_t offset, uint32_t& value, S3cIntc& intc);
    S3cAccess write(uint32_t offset, uint32_t value, S3cIntc& intc);
    // A byte from the far end.
    void receive(uint8_t byte, S3cIntc& intc);
    std::function<void(unsigned channel, uint8_t byte)> sink;
    // The nCTS input as asserted, from the far end (host side, not saved; asserted with nothing attached). setCts
    // sets UMSTAT's delta CTS (bit 2) on a change, which a UMSTAT read clears. ASSUMPTION "no UART modem interrupt":
    // the S3C2440/2442 UART has no modem status interrupt (only RXD, TXD and ERR sub-sources, no UMCON enable for
    // one), so a CTS change raises nothing; the driver polls UMSTAT.
    bool cts = true;
    bool deltaCts = false;
    void setCts(bool on) {
        if (on != cts) deltaCts = true;
        cts = on;
    }
    // Infra-red mode (ULCON bit 6), receiver and transmitter on (UCON modes not 0).
    bool infrared() const { return ulcon & 0x40; }
    bool receiverOn() const { return ucon & 3; }
    bool transmitterOn() const { return ucon & 0xc; }
    // The FIFO's 64 bytes with UFCON bit 0 set, else the one holding register.
    bool rxRoom() const { return rx.size() < (ufcon & 1 ? 64u : 1u); }

    uint32_t ulcon, ucon, ufcon, umcon, ubrdiv;
    std::deque<uint8_t> rx;
    unsigned channel;
    template <class A> void state(A& a) {
        a.Do32(ulcon).Do32(ucon).Do32(ufcon).Do32(umcon).Do32(ubrdiv);
        std::vector<uint8_t> bytes(rx.begin(), rx.end());
        a.DoVector(bytes, 4096);
        rx.assign(bytes.begin(), bytes.end());
    }

   private:
    void interrupts(S3cIntc& intc, bool sent);
};

// The LCD controller at 0x4D000000 (manual chapter 15), as far as R1 needs it: the registers, the line
// counter and the frame sync interrupt the OAL's DVS switch runs on. No picture yet.
class S3cLcd {
   public:
    void reset();
    S3cAccess read(uint32_t offset, uint32_t& value);
    S3cAccess write(uint32_t offset, uint32_t value, S3cIntc& intc);
    // HCLK ticks.
    void advance(uint64_t hclkTicks, S3cIntc& intc);
    // HCLK ticks to the next frame sync; with interruptOnly, UINT64_MAX when LCDINTMSK masks it.
    uint64_t ticksToEvent(bool interruptOnly = false) const;
    bool enabled() const { return lcdcon[0] & 1; }
    // HCLK ticks in one line and lines in one frame, from the timing registers.
    uint64_t lineTicks() const;
    uint32_t frameLines() const;
    // The picture the controller scans out, one 0xRRGGBB per pixel, row by row. Width and height come
    // from HOZVAL and LINEVAL, the frame from LCDSADDR1 and the line stride from LCDSADDR3 (PAGEWIDTH plus
    // OFFSIZE), and the pixel format from BPPMODE, FRM565, BPP24BL, BSWP and HWSWP. TFT 16 and 24 bpp only.
    // memory reads n bytes at a physical address, false where there's no memory. False, with why, when
    // the controller is off or set to something not decoded.
    bool frame(const std::function<bool(uint32_t, void*, size_t)>& memory, std::vector<uint32_t>& pixels,
               unsigned& width, unsigned& height, std::string& why) const;
    // The frame's physical start, from LCDSADDR1.
    uint32_t frameAddress() const { return (saddr[0] & 0x3fffffff) << 1; }

    uint32_t lcdcon[5], saddr[3], redlut, greenlut, bluelut, dithmode, tpal, lcdintpnd, lcdsrcpnd, lcdintmsk, tconsel;
    uint32_t palette[256];
    // HCLK ticks into the current frame.
    uint64_t phase;
    template <class A> void state(A& a) {
        a.DoBuffer32(lcdcon, 5).DoBuffer32(saddr, 3).Do32(redlut).Do32(greenlut).Do32(bluelut).Do32(dithmode);
        a.Do32(tpal).Do32(lcdintpnd).Do32(lcdsrcpnd).Do32(lcdintmsk).Do32(tconsel).DoBuffer32(palette, 256).Do64(phase);
    }

   private:
    void interrupts(S3cIntc& intc);
};

#endif
