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

#include "acx100.h"
#include "audio_input.h"
#include "memory_buffer.h"
#include "s3c2442_blocks.h"
#include "s3c2442_nand.h"
#include "uda1380.h"
#include "wifi_access_point.h"

struct ArmCpu;
struct ArmMem;
struct ArmRam;
struct VSD;

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
    // nGCS4 (the ACX100 Wi-Fi MAC) while the chip is unpowered or held in reset: writes are dropped and reads
    // give all ones. Off stops the run at the first such access.
    bool wifiBusEmpty = true;
};

enum class S3c2442Stop : uint8_t {
    running,
    budget,         // the instruction budget ran out
    refused,        // an access the machine doesn't model
    watchdogReset,  // unused: the watchdog's reset restarts the machine (watchdogResets())
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
    // Accesses on nGCS4, the Wi-Fi chip's bus, powered or not.
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
    // Power lost and back (a clean boot after the host wiped the user store): SDRAM cleared, every block but
    // the RTC to its reset state, GSTATUS2 PWRST, then resetLoader. The RTC and the board's pin inputs are kept.
    void powerOnReset();
    // Watchdog resets so far: as nRESET with GSTATUS2 WDTRST, between instructions in run(). Not saved.
    uint64_t watchdogResets() const { return watchdogResetCount; }

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

    // Sound. The IIS runs at the sample rate audioRate() gives (frames a second, numerator / denominator, 0 / 1
    // while it's off), moving its FIFOs through DMA and the UDA1380. Each frame it sends goes to pcmSink after
    // the codec's volume, with the board's gain on it as 16.16 (audioGain()), in guest time order.
    std::function<void(int16_t left, int16_t right, uint32_t gain16)> pcmSink;
    void audioRate(uint64_t& numerator, uint64_t& denominator) const;
    // The codec's output reaching the speaker or the headphones: 65536 or 0. The speaker needs its amplifier
    // (GPA1 driven high), the headphones (in, GPG12 low) need the codec's headphone driver; either needs the
    // codec powered (GPJ0) and its DAC playing.
    uint32_t audioGain() const;
    // The host's microphone for the codec's ADC, pulled one sample a received frame; silence without one.
    const AudioInputSource* microphone = nullptr;
    // Fences nest; while any is held the microphone isn't pulled and the demand reads fenced.
    void fenceMicrophone(bool on);
    // Closes the host source and lets go of it; a load or reset, a sleep or a new source starts disarmed.
    void disarmMicrophone();
    unsigned microphoneFences = 0;
    // What the guest asks of the microphone now: on while the IIS receives and the codec's ADC is on.
    AudioInputDemand microphoneDemand() const;
    // Headphones in the jack: GPG12 (EINT20) low while they're in.
    void setHeadphones(bool in) { setPin(S3cGpio::g, 12, !in); }
    bool headphonesIn() const { return !gpio.pin(S3cGpio::g, 12); }

    // The SD card in the slot (the uARM card on the card store), or nullptr for none. The board's card detect
    // and write protect pins are the owner's (setPin). ASSUMPTION "rx1950 SD power on GPJ1": the card is
    // powered unless GPJ1 is an output driven low (the SD driver sets GPJDAT bit 1 when a card goes in and
    // clears it when it comes out); the change to off resets the card.
    void setSdCard(VSD* card);
    VSD* sdCard() const { return sdVsd; }
    bool sdPowered() const;
    // The card section of the last loadState: whether the state had one, whether a card was in, its id and
    // its saved state (vsdMachineLoad). The owner puts the card back, or takes it out, after the load.
    struct SavedCard {
        bool section = false, in = false;
        std::string id;
        std::vector<uint8_t> card;
    } loadedCard;
    // The id saveState keeps with the card, for the owner to tell the same card at a load.
    std::string sdCardId;

    // Wi-Fi: the ACX100 on nGCS4 (acx100.h) and the access point its radio reaches (wifi_access_point.h).
    // ASSUMPTION "rx1950 Wi-Fi power": the chip is powered while GPC8 and GPC9 are outputs driven high, and held
    // in reset while GPA14 is an output driven low; losing either resets it. Its interrupt pulls GPG8 (EINT16) low:
    // the OAL sets EINT16 to low level. The access point's Ethernet frames go to
    // wifiEthernetOut; the owner feeds frames from the network to wifiEthernetIn.
    Acx100 wifi;
    WifiAccessPoint accessPoint;
    std::function<void(const uint8_t*, size_t)> wifiEthernetOut;
    void wifiEthernetIn(const uint8_t* frame, size_t size);
    // After the owner changes the GPIO registers directly.
    void wifiPinsChanged() { wifiPins(); }
    // Whether the access point is on the air (the desktop's Virtual Wi-Fi network mode). Turning it off
    // forgets the station.
    void setAccessPointOn(bool on);
    bool accessPointOn = true;
    bool wifiPowered() const;
    bool wifiLive() const;
    // False after loading a state saved before the machine kept the Wi-Fi chip (no "ACXW" section).
    bool stateHadWifi = true;

    // USB device. ASSUMPTION "rx1950 VBUS on GPG5": the cable's VBUS reaches GPG5 (EINT13) inverted, low while a
    // cable is in (Linux's rx1950 board file has vbus_pin_inverted). ASSUMPTION "rx1950 D+ pull-up on GPJ5": the
    // device shows itself to the host while GPJDAT bit 5 is set. sc2440usbfn.dll sets it when it starts the
    // controller (Linux's pullup_pin is GPJ5 too), but GPJCON reads 0 in Frond's sessions (the loader's GPJCON isn't
    // known), so the latch decides whatever GPJCON says. The host sees it (usbListening) while
    // VBUS is on, the pull-up is on and CLKCON gives the controller its clock. usbHostChanged after each host
    // packet updates INT_USBD; usbNotify runs after a guest access that can change what the host sees: writes, and
    // FIFO accesses that leave a packet for the host or take the last byte of one (host side, not saved).
    void setUsbVbus(bool on);
    bool usbVbus() const { return usbVbusOn; }
    bool usbListening() const;
    // Times usbListening went on since the machine was made, for the host to see a reconnect.
    uint32_t usbEnableCount() const { return usbEnables; }
    void usbHostChanged();
    std::function<void()> usbNotify;
    // Host work on guest time (bytes from a serial or infrared host, the USB host's ticks): run calls hostService
    // between instructions once nanoseconds() reaches hostNextNs, and busy batches and idle skips both end there, so
    // the work lands at the same instruction whatever the run slices are. UINT64_MAX for none. hostService sets it
    // again. hostUartNextNs is the owner's per-UART schedule (guest ns of the next look for a host byte) and
    // hostNetNextNs its next network poll. All are saved in their own state section while any is set, so a
    // checkpoint restore goes on at the same points; a state without it loads them as UINT64_MAX. Host work goes on
    // while asleep too.
    std::function<void()> hostService;
    uint64_t hostNextNs = UINT64_MAX;
    uint64_t hostUartNextNs[3]{UINT64_MAX, UINT64_MAX, UINT64_MAX};
    uint64_t hostNetNextNs = UINT64_MAX;
    // Runs right after a watchdog reset, at the instruction where it happens, for the board's inputs and host peers
    // (host side, not saved).
    std::function<void()> watchdogResetHook;

    // A byte from a UART's far end (host serial or infrared), and the UART's bit rate from UBRDIV and its clock.
    void uartReceive(unsigned channel, uint8_t byte);
    uint32_t uartBaud(unsigned channel) const;

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
    Uda1380 codec;
    // False after loading a state saved before the machine kept its sound (no "AUDI" section).
    bool stateHadSound = true;
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
    void watchdogReset();
    // IIS frames: each sends, receives and keeps the FIFOs topped up through DMA.
    void audioFrames(uint64_t frames);
    void pumpAudioDma();
    // CPU cycles until a DMA interrupt the IIS's frames bring, UINT64_MAX with none; wakeOnly as cyclesToEvent.
    uint64_t audioCyclesToEvent(bool wakeOnly) const;
    uint64_t iisMasterHz() const;
    bool codecPowered() const;
    // After a GPIO write: the codec's reset pin (GPD0, high resets it).
    void codecPins();
    bool codecResetHigh = false;
    uint64_t iisRest = 0;
    // After an SDI or DMA access: SDI DMA requests served, INT_SDI's level.
    void sdiService();
    // After a GPIO write: the card's supply (GPJ1).
    void sdPowerPins();
    // After a GPIO or clock write: whether the host sees the USB device, counting each time it shows up.
    void usbPins();
    bool usbVbusOn = false, usbListeningWas = false;
    uint32_t usbEnables = 0;
    // The Wi-Fi chip's power and reset pins after a GPIO change; its events and interrupt pin.
    void wifiPins();
    void wifiService();
    bool wifiLiveWas = false, wifiBusTrace = false;
    uint64_t wifiNext = UINT64_MAX;
    VSD* sdVsd = nullptr;
    std::unique_ptr<S3cSdCard> sdAdapter;
    bool sdPowerWas = true;
    void hardReset(uint32_t gstatus2);
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
    uint64_t sleeps = 0, wakes = 0, watchdogResetCount = 0;
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
