#ifndef FROND_S3C2442_BLOCKS_H
#define FROND_S3C2442_BLOCKS_H

// The Samsung S3C2442's on-chip blocks, from the S3C2440/S3C2442 user's manual. Each block takes word
// register accesses at its own offsets and answers unknown, which the machine turns into a refusal that
// names the address and pc. No block touches the CPU: interrupts go through S3cIntc and time comes in as
// clock ticks from the machine.

#include <cstdint>
#include <deque>
#include <functional>

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
    const uint32_t crystal;
};

// SDRAM and bank control at 0x48000000 (manual chapter 5). The SDRAM itself needs no setup here, so the
// registers are only kept.
class S3cMemoryController {
   public:
    void reset();
    S3cAccess read(uint32_t offset, uint32_t& value);
    S3cAccess write(uint32_t offset, uint32_t value);
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

   private:
    uint64_t divisor() const;
};

// Real time clock at 0x57000000 (manual chapter 17), driven by the 32768 Hz crystal.
class S3cRtc {
   public:
    struct Time { uint8_t year, month, date, day, hour, minute, second; };
    // The time in BCD registers form, year 0-99 from 2000, day of week 1-7.
    void reset(const Time& start);
    S3cAccess read(uint32_t offset, uint32_t& value);
    S3cAccess write(uint32_t offset, uint32_t value);
    void advance(uint64_t ticks32k, S3cIntc& intc);
    // 32 kHz ticks until the next tick interrupt (tick) or alarm check (alarmCheck), UINT64_MAX with none due.
    uint64_t ticksToEvent(bool tick = true, bool alarmCheck = true) const;

    uint32_t rtccon, ticnt, rtcalm, rtcrst, alarm[6], bcd[7];
    uint64_t secondPhase, tickPhase;

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

    uint32_t ulcon, ucon, ufcon, umcon, ubrdiv;
    std::deque<uint8_t> rx;
    unsigned channel;

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

    uint32_t lcdcon[5], saddr[3], redlut, greenlut, bluelut, dithmode, tpal, lcdintpnd, lcdsrcpnd, lcdintmsk, tconsel;
    uint32_t palette[256];
    // HCLK ticks into the current frame.
    uint64_t phase;

   private:
    void interrupts(S3cIntc& intc);
};

#endif
