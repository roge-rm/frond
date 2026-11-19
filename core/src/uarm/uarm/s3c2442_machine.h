#ifndef FROND_S3C2442_MACHINE_H
#define FROND_S3C2442_MACHINE_H

// An S3C2442 machine on the ARM920T CPU path the Zodiac's i.MX1 uses, run on the interpreter (Exact). It
// has the SoC blocks in s3c2442_blocks.h and s3c2442_nand.h, SDRAM in bank 6 and the steppingstone, and
// stops with a refusal that names the address and pc at the first access to anything it doesn't model.
// The board (the HP iPAQ rx1950) only sets options: pin levels, crystal, RAM size and the chip ID.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "memory_buffer.h"
#include "s3c2442_blocks.h"
#include "s3c2442_nand.h"

struct ArmCpu;
struct ArmMem;
struct ArmRam;

struct S3c2442Options {
    uint32_t crystal = 16934400;
    uint32_t ramBytes = 32u << 20;
    // ASSUMPTION "rx1950 chip ID": GSTATUS1 reads the S3C2442's 0x32440aaa.
    uint32_t chipId = 0x32440aaa;
    // GSTATUS2 after a power-on reset.
    uint32_t resetCause = 1;
    S3cRtc::Time rtcStart{6, 1, 1, 7, 12, 0, 0};
    // Board pin levels per port, A to J. Every pin reads high unless set.
    uint32_t pinLevels[S3cGpio::ports] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
    // nGCS5 (0x28000000, the debug board on a development unit) is an empty bus: writes are dropped and
    // reads give all ones. Off, any access there is refused.
    bool debugBoardEmpty = true;
    // nGCS4 (the ACX100 Wi-Fi MAC) as an empty bus until the chip is modelled: writes are dropped and reads
    // give all ones. Off stops the run at the first access.
    bool wifiBusEmpty = true;
};

enum class S3c2442Stop : uint8_t {
    running,
    budget,         // the instruction budget ran out
    refused,        // an access the machine doesn't model
    watchdogReset,  // the watchdog ran out with its reset on
    powerOff,       // unused: CLKCON POWER_OFF puts the machine to sleep (asleep())
    idleNoWake,     // idle with no interrupt that can come
    dataCache,      // a cacheable access to something that isn't memory
    undefined,      // the CPU took the undefined instruction exception
};

struct S3c2442StopInfo {
    S3c2442Stop stop = S3c2442Stop::running;
    uint32_t pc = 0, address = 0, value = 0;
    uint8_t size = 0;
    bool write = false;
    std::string text;
};

class S3c2442Machine {
   public:
    // nullptr with the reason in error when the machine can't be made.
    static std::unique_ptr<S3c2442Machine> create(const S3c2442Options& options, std::string& error);
    ~S3c2442Machine();

    // Copies bytes into SDRAM at a physical address. False outside it.
    bool loadRam(uint32_t physical, const uint8_t* data, size_t size);
    // A CPU reset at pc: SVC mode, interrupts off, MMU and caches off.
    void start(uint32_t pc);
    // Runs until the machine stops or about instructions more have run, idle CPU cycles counted as
    // instructions. Returns why it stopped, budget when it can go on.
    S3c2442Stop run(uint64_t instructions);
    const S3c2442StopInfo& stopInfo() const { return stopped; }
    // One line on why the machine stopped.
    std::string stopText() const;

    uint64_t retired() const { return retiredCount; }
    // The Fast engine (Dynarmic blocks, the lean path and the interpreter where they refuse) instead of the
    // interpreter alone. Both run the same instructions with the same timing. False if it can't be had (no
    // Dynarmic in this build).
    bool setFastEngine(bool fast);
    bool fastEngine() const { return fastOn; }
    // Fast: instructions run in translated blocks, on the lean path and in the interpreter.
    uint64_t jitRetired() const { return jitRetiredCount; }
    uint64_t leanRetired() const { return leanRetiredCount; }
    uint64_t stepRetired() const { return stepRetiredCount; }
    // VFP instructions and Windows CE breakpoints the guest ran, which take the undefined instruction
    // exception without a stop.
    uint64_t vfpProbeCount() const { return vfpProbes; }
    uint64_t breakpointCount() const { return breakpoints; }
    // Accesses to the empty Wi-Fi bus on nGCS4.
    uint64_t wifiAccesses = 0;
    // Guest time in CPU clock cycles, idle time included, and in nanoseconds.
    uint64_t cycles() const { return cycleCount; }
    uint64_t idleCycles() const { return idleCount; }
    uint64_t nanoseconds() const { return elapsedNs; }
    uint32_t pc() const;
    ArmCpu* cpu() { return cpuCore; }
    // Keeps the pc of the last n instructions, for a look at how a run stopped. Runs one instruction at a
    // time while on, so it's slow.
    void keepHistory(size_t n) { history.assign(n, 0); historyNext = 0; }
    // Oldest first.
    std::vector<uint32_t> recentPcs() const;

    // Called after each instruction while history is kept: pc of that instruction. For tools that watch
    // memory.
    std::function<void(uint32_t)> stepHook;
    const uint8_t* sdram() const { return ramBuffer; }

    // Sleep mode: CLKCON POWER_OFF stops the CPU and every clock but the RTC's. A wake source (EINT0-3, EINT4-15
    // that EINTMASK lets through, the RTC alarm) ends it with the wake-up reset: the blocks outside the
    // alive part go back to their reset values, GSTATUS2 reads OFFRST, the CPU resets and resetLoader runs.
    // run() goes on while asleep, the budget counting sleep time as idle cycles at the clock it slept at.
    bool asleep() const { return sleeping; }
    uint64_t sleepCount() const { return sleeps; }
    uint64_t wakeCount() const { return wakes; }
    // What ended the last sleep: SRCPND bits (EINT0-3, RTC alarm) with EINTPEND's EINT4-15.
    uint32_t lastWakeSources() const { return lastWake; }
    // The board's stand-in for the steppingstone loader at a wake from sleep: puts the boot code in SDRAM
    // and calls start(). Unset, the CPU starts at 0 (the steppingstone).
    std::function<void()> resetLoader;
    // The backlight, PWM timer 0 on TOUT0 (GPB0): its duty, 0 to 1000, from TCMPB0 against TCNTB0 and the
    // inverter bit; with GPB0 a plain output, 0 or 1000 from its level. -1 while asleep.
    int backlightLevel() const;

    // The reset button (nRESET): every block but the RTC to its reset state, then resetLoader as at power on.
    void reset();

    // Faster speed: the CPU runs num / den times its clock, with the timers, RTC and guest time as at 1 / 1.
    void setClockMultiplier(uint32_t num, uint32_t den) {
        multiplierNum = num ? num : 1;
        multiplierDen = den ? den : 1;
    }

    // The machine between runs, for sessions: the CPU with its MMU, TLB, caches and FCSE PID, SDRAM, the
    // steppingstone, every block's registers, the sleep state, the board's pin levels and the time counts.
    // The NAND's pages aren't in it, as their owner keeps them. A stopped machine isn't saved. loadState takes
    // a state made by a machine with the same crystal, SDRAM size and chip ID; false, with why, leaves this
    // machine as it was.
    bool saveState(std::vector<uint8_t>& out, std::string& why);
    bool loadState(const uint8_t* data, size_t size, std::string& why);
    static constexpr uint32_t stateVersion = 1;

    // Host input between runs: a board pin's level (keys, card detect, AC), and the stylus on the touch
    // panel in raw ADC units. Either can raise an interrupt for the next run.
    void setPin(S3cGpio::Port port, unsigned bit, bool level);
    void setStylus(bool down, uint16_t x, uint16_t y);
    // The screen as the LCD controller scans it out of SDRAM (S3cLcd::frame).
    bool screen(std::vector<uint32_t>& pixels, unsigned& width, unsigned& height, std::string& why) const;

    // Bytes each UART sends: channel, byte.
    std::function<void(unsigned, uint8_t)> uartSink;
    // Every SFR access, for traces: pc, physical address, size, write, value.
    std::function<void(uint32_t, uint32_t, unsigned, bool, uint32_t)> mmioTrace;

    S3cIntc intc;
    S3cClock clock;
    S3cMemoryController memory;
    S3cPwm pwm;
    S3cWatchdog watchdog;
    S3cRtc rtc;
    S3cSdi sdi;
    S3cUsbDevice usbDevice;
    S3cAdc adc;
    S3cDma dma;
    S3cIis iis;
    S3cIic iic;
    S3cGpio gpio;
    S3cUart uarts[3] = {S3cUart(0), S3cUart(1), S3cUart(2)};
    S3cLcd lcd;
    S3cNand nand;
    uint8_t steppingstone[4096];

   private:
    explicit S3c2442Machine(const S3c2442Options& options);
    static bool access(void* context, uint32_t address, uint_fast8_t size, bool write, void* data);
    static bool cacheCheck(void* context, uint32_t address, uint32_t size, bool write);
    bool mmio(uint32_t address, unsigned size, bool write, uint32_t& value);
    bool refuse(uint32_t address, unsigned size, bool write, uint32_t value, const std::string& text);
    void advance(uint64_t cpuCycles);
    // The CPU's clock times the Faster multiplier: CPU cycles a second of guest time.
    uint64_t cpuHz() const { return clock.cpuClock() * multiplierNum / multiplierDen; }
    uint32_t multiplierNum = 1, multiplierDen = 1;
    // Applies the time of the instructions the running interpreter call has finished (batchAdvanced of them
    // already), so an SFR access sees the time at its instruction.
    void catchUp();
    uint32_t step(uint64_t n);
    void runFast(uint64_t n);
    static uint8_t* leanPage(void* context, uint32_t pa, bool write, bool* counted);
    static bool leanRead(void* context, uint32_t pa, uint32_t size, uint32_t pc, uint32_t ran, uint32_t left, bool single,
                         uint32_t* value, uint32_t* cost);
    static bool leanWrite(void* context, uint32_t pa, uint32_t size, uint32_t value, uint32_t ran, uint32_t left,
                          uint32_t* cost);
    static bool leanGuard(uint32_t instruction, const uint32_t* registers, uint32_t cpsr, uint32_t spsr);
    // CPU cycles to the next event; with wakeOnly, only events that can wake an idle CPU: interrupts
    // INTMSK (and INTSUBMSK, LCDINTMSK) let through, and a watchdog reset.
    uint64_t cyclesToEvent(bool wakeOnly = false) const;
    // Sleep: the sources that wake it, pending now; time asleep, which moves only the RTC.
    uint32_t wakeSources() const;
    void sleepAdvance(uint64_t cpuCycles);
    void wake();
    void resetBlocks();
    // sfrWrite: the change comes from an SFR write, so a new IRQ waits irqWriteLatency instructions.
    void updateLines(bool sfrWrite = false);

    S3c2442Options options;
    S3c2442StopInfo stopped;
    ArmCpu* cpuCore = nullptr;
    ArmMem* mem = nullptr;
    ArmRam* ram[4] = {};
    uint8_t* ramBuffer = nullptr;
    MemoryBuffer ramMemory{};
    static void undefinedTaken(void* context, uint32_t pc, uint32_t instr);
    // The first undefined instruction since the last check that isn't the guest's own, to stop on.
    bool undefinedPending = false;
    uint32_t undefinedStopPc = 0, undefinedStopInstr = 0;
    uint64_t vfpProbes = 0, breakpoints = 0;
    bool irqLine = false, fiqLine = false;
    // An IRQ an SFR write raised, held from the CPU until retiredCount reaches irqHeldUntil.
    bool irqHeld = false;
    uint64_t irqHeldUntil = 0;
    uint64_t retiredCount = 0, cycleCount = 0, idleCount = 0, elapsedNs = 0;
    // Remainders of each clock domain in CPU cycles times the domain's rate.
    uint64_t pclkRest = 0, hclkRest = 0, rtcRest = 0, nsRest = 0;
    bool debugWriteSeen = false;
    bool sleeping = false;
    uint64_t sleeps = 0, wakes = 0;
    uint32_t lastWake = 0;
    uint64_t batchAdvanced = 0;
    uint32_t leanAdvanced = 0;  // of the running lean call, by leanRead
    bool sfrAccessed = false;  // since the Fast span started
    bool fastOn = false;
    uint64_t jitRetiredCount = 0, leanRetiredCount = 0, stepRetiredCount = 0;
    std::vector<uint32_t> history;
    size_t historyNext = 0;
};

#endif
