#ifndef FROND_IMX1_BOOTSTRAP_H
#define FROND_IMX1_BOOTSTRAP_H

#include "arm_diagnostic.h"
#include <cstdint>
#include <vector>

#include "imx1_aitc.h"
#include "imx1_cgm.h"
#include "imx1_gpio.h"
#include "imx1_eim.h"
#include "imx1_timer.h"
#include "imx1_uart.h"
#include "imx1_sysctrl.h"
#include "zodiac_ymf768.h"
#include "imx1_bta.h"
#include "imx1_reset.h"
#include "imx1_mmcsd.h"
#include "imx1_i2c.h"
#include "imx1_asp.h"
#include "imx1_pwm.h"
#include "imx1_watchdog.h"
#include "imx1_rtc.h"
#include "imx1_reference_time.h"

enum class Imx1BootstrapAccess { ok, command, unmapped, width, reservedBits, sideEffect, externalInput, memoryUnavailable, sequence, resetRequest, possibleWatchdogTimeout, busTimeout };

struct Imx1PcmState {
    uint32_t remaining = 0, source = 0, pendingSource = 0, fifoWords = 0, status = 0;
    uint32_t control = 0, transmitControl = 0, clockControl = 0;
    uint64_t transmittedWords = 0;
    bool dmaActive = false;
};

// What a memory-to-memory DMA reaches outside SRAM and SDRAM: the graphics memory on CS5. check looks at one
// beat with nothing changed; move makes it. clocks is the HCLK periods one CS5 beat takes; false where the bus
// can't take one now. The DMA is a bus master, so CPU mode and permissions play no part.
class Imx1DmaExternal {
   public:
    virtual ~Imx1DmaExternal() = default;
    virtual bool owns(uint32_t address) const = 0;
    virtual Imx1BootstrapAccess check(uint32_t address, uint32_t size, bool write) const = 0;
    virtual void move(uint32_t address, uint32_t size, bool write, uint32_t& value) = 0;
    virtual bool clocks(uint32_t& perBeat) const = 0;
};

// Startup memory/register subset. External pin samples require an explicit input source.
class Imx1Bootstrap {
   public:
    explicit Imx1Bootstrap(bool aspPx1HighFromReset = false, bool aspUSampleKnown = false, uint16_t aspUSample = 0)
        : asp(aspPx1HighFromReset, aspUSampleKnown, aspUSample) {}
    // Atomic execution-thread contact/sample replacement, with AITC preview.
    Imx1BootstrapAccess replaceAspPenInputs(const Imx1AspPenInputs& inputs, bool commit = true);
    bool aspPenWakePossible(const Imx1AspPenInputs& inputs) const;
    const Imx1AspPenInputs& aspPenInputs() const { return asp.currentPenInputs(); }
    void setMmcsdEmptySlot(bool enabled) { SourceChange change(*this); mmcsd.setEmptySlot(enabled); }
    // The card in the MMCSD slot, or null. Card detect is a GPIO input the caller drives. The card already
    // in, or a card a loaded state's card section resumes (stateCard), keeps its transfer and DMA. Returns
    // true when it does.
    bool attachMmcsdCard(VSD* card) {
        SourceChange change(*this);
        if (mmcsd.attachCard(card)) return true;
        cardDma = {};
        return false;
    }
    VSD* mmcsdCard() const { return mmcsd.card(); }
    // The card section: saved only while a card is in, so a state with the slot empty keeps the BOOT layout.
    static constexpr uint32_t cardStateVersion = Imx1Mmcsd::cardStateVersion;
    template <class A> void stateCard(A& a) {
        SourceChange change(*this);
        mmcsd.stateCard(a);
        a.Do32(cardDma.active).DoBuffer32(cardDma.moved, 11);
        a.Require(!(cardDma.active & ~0x3ffu), "card dma channel");
        for (unsigned n = 0; n < 10 && a.ok(); ++n)
            a.Require(!(cardDma.active & (1u << n)) || ((dmaChannels[n][3] & 1) && cardDma.moved[n] <= dmaChannels[n][2]),
                      "card dma count");
    }
    // The memory DMA section: saved only while a memory-to-memory transfer is under way, so other states keep
    // the BOOT layout.
    static constexpr uint32_t memoryDmaStateVersion = 1;
    bool memoryDmaBusy() const { return memoryDma.active != 0; }
    template <class A> void stateMemoryDma(A& a) {
        SourceChange change(*this);
        a.Do32(memoryDma.active).DoBuffer32(memoryDma.remaining, 11);
        a.Require(!(memoryDma.active & ~0x3ffu) && !(memoryDma.active & cardDma.active), "memory dma channel");
        for (unsigned n = 0; n < 10 && a.ok(); ++n)
            a.Require(!(memoryDma.active & (1u << n)) ||
                      ((dmaChannels[n][3] & 9) == 1 && memoryDma.remaining[n]), "memory dma count");
    }
    // Graphics memory for memory-to-memory DMA, or none. Not saved.
    void setDmaExternal(Imx1DmaExternal* external) { dmaExternal = external; }
    // Called with the physical address of each 4 KiB page a DMA transfer writes, so decoded code from it goes.
    void setDmaStoreHook(void (*hook)(void*, uint32_t), void* context) { dmaStoreHook = hook; dmaStoreContext = context; }
    // Called with each 16-bit word SSI1 finishes sending and its slot (0 the frame sync slot, then 1), in
    // HCLK order, so Exact and Fast give the same words. An underrun's repeated word counts too. Not saved.
    void setPcmOutput(void (*hook)(void*, uint16_t, unsigned), void* context) { pcmHook = hook; pcmContext = context; }
    // Moves the PCM output to another board, as a state load replaces this one.
    void takePcmOutput(const Imx1Bootstrap& old) { pcmHook = old.pcmHook; pcmContext = old.pcmContext; }
    // The Zodiac's YMF768 on CS4, off unless enableYmf768 is called. Its FM output is added to SSI1's words on
    // their way to the PCM output, and its interrupt drives GPIO B15 (low while asserted). Saved in its own
    // section (stateYmf768), so a state saved before it existed loads with the chip reset.
    // driveLine false leaves GPIO B15 where it is, for a state saved without the chip.
    void enableYmf768(bool driveLine = true) {
        ymfEnabled = true;
        if (driveLine) ymfDriveLine();
    }
    bool ymf768Enabled() const { return ymfEnabled; }
    // One CPU byte access to 0x15000000 (index) or 0x15000001 (data).
    Imx1BootstrapAccess ymf768Access(uint32_t address, uint32_t size, bool write, uint32_t& value) {
        if (address != 0x15000000 && address != 0x15000001) return Imx1BootstrapAccess::unmapped;
        if (size != 1) return Imx1BootstrapAccess::width;
        SourceChange change(*this);
        if (write) ymf.write(address & 1, uint8_t(value));
        else value = ymf.read(address & 1);
        if (!ymfDriveLine()) return Imx1BootstrapAccess::sideEffect;
        return Imx1BootstrapAccess::ok;
    }
    static constexpr uint32_t ymf768StateVersion = Imx1Ymf768::stateVersion;
    template <class A> void stateYmf768(A& a) { ymf.state(a); }
    const Imx1Ymf768& ymf768() const { return ymf; }
    // The SSI1 frame rate in frames a second as numerator / denominator, 0 when the serial side isn't
    // running in the qualified two-slot internal-clock set-up: the System PLL over PERCLK3 and 128.
    void pcmFrameRate(uint64_t& numerator, uint64_t& denominator) const;
    // The PWM duty last written as high clocks of a period, false while it is off or no sample was written.
    bool pwmDuty(uint32_t& high, uint32_t& period) const { return pwm.writtenDuty(high, period); }
    // A GPIO port's register by index (0 DDIR, 1 OCR1, 7 DR, 8 GIUS, 9 SSR, 14 GPR, ...).
    uint32_t gpioRegister(unsigned port, unsigned index) const { return gpio.registerValue(port, index); }
    void setLcdcContrastOnly(bool enabled) { lcdcContrastOnly = enabled; }
    uint32_t lcdcContrastPhase() const { return contrastPhase; }
    bool setRtcInitialTime(Imx1RtcTime time) { SourceChange change(*this); return rtc.initialise(time); }
    void setI2cScenario(bool enabled) { SourceChange change(*this); i2c.setSingleMasterScenario(enabled); }
    void setI2cBus(Imx1I2cBus* bus) { SourceChange change(*this); i2c.setBus(bus); }
    void setI2cObserver(Imx1I2cObserver observer, void* context) { SourceChange change(*this); i2c.setObserver(observer, context); }
    const Imx1I2c& i2cState() const { return i2c; }
    const Imx1Eim& eimState() const { return eim; }
    // Where an infrared or serial peer attaches. unit is 1 or 2. The UARTs can change behind the board's
    // back from then on, so their interrupt inputs are no longer kept between calls.
    Imx1Uart& uart(unsigned unit) {
        runCache.uartShared = true;
        runCache.sourcesValid = false;
        return unit == 1 ? uart1 : uart2;
    }
    const Imx1Uart& uartState(unsigned unit) const { return unit == 1 ? uart1 : uart2; }
    uint32_t dmaResetRemainingHclk() const { return dmaResetClocks; }
    uint32_t pcmDmaRemaining() const { return dmaPcmActive ? dmaChannels[10][2] - dmaPcmCount : 0; }
    uint64_t pcmTransmittedWords() const { return ssi1Transmitted; }
    uint16_t pcmLastTransmittedWord() const { return ssi1LastTransmitted; }
    Imx1PcmState pcmState() const {
        return {pcmDmaRemaining(), dmaPcmSource, dmaChannels[10][0], ssi1TxCount, dmaStatus,
                ssi1Words[0], ssi1Words[1], ssi1Words[3], ssi1Transmitted, dmaPcmActive};
    }
    // True when the PCM DMA can still read these RAM bytes, so a write to them has to come at its own time.
    bool pcmDmaReads(uint32_t address, uint32_t size) const {
        if (!dmaPcmActive) return false;
        const uint64_t end = uint64_t(address) + size, length = dmaChannels[10][2];
        return (address < dmaPcmSource + length && end > dmaPcmSource) ||
               (address < dmaChannels[10][0] + length && end > dmaChannels[10][0]);
    }
    // Host address of the 4 KiB page at pa when every access to it is a plain memory access with no side
    // effect: SRAM, or SDRAM in normal mode. Writes also need the page clear of the PCM DMA. Null otherwise.
    // For the Fast engine; an SDRAM bank that was never touched gets its zero contents here.
    uint8_t* plainPage(uint32_t pa, bool write) {
        if (write && pcmDmaReads(pa, 0x1000)) return nullptr;
        if (pa >= 0x00300000 && pa < 0x00320000) return sram.data() + (pa - 0x00300000);
        if (pa < 0x08000000 || pa >= 0x10000000) return nullptr;
        auto& s = sdram[(pa - 0x08000000) >> 26];
        const uint32_t offset = pa & 0x03fff000;
        if (!s.bytes || (s.control & 0xf0000000) != 0x80000000 || !s.modeSet || offset >= s.bytes) return nullptr;
        if (s.ram.empty()) s.ram.resize(s.bytes);
        return s.ram.data() + offset;
    }
    // What a read of timer 1 or 2's counter gives after cycles more CPU cycles in this clock mode, then hclk
    // HCLK cycles (bus wait clocks, charged after the CPU cycles), with nothing charged. Only for cycles in which
    // no timer reaches its compare value. False for any other access, or when the sum is too big to work out in
    // one piece.
    bool timerCountAfter(uint32_t address, uint32_t size, uint64_t cycles, uint32_t mode, uint32_t& value,
                         uint64_t hclk = 0) const;
    static bool timerCountAddress(uint32_t address) { return address == 0x00202010 || address == 0x00203010; }
    // Registers whose reads change nothing access commits but the GPIO port's accessed mark, and read the same
    // anywhere in a span where no interrupt line or input changes: the ASP status, GPIO and AITC.
    // Also CGM CSCR, the timers' control, prescaler, compare, capture and status (the count has its own path) and DMA
    // DISR and DIMR: reads that change nothing but a timer's status-read flag, which a charge in between leaves as is.
    // And the RTC's registers: reads only mark it used, and the span ends at its next second or sampling period.
    static bool quietReadAddress(uint32_t address) {
        const uint32_t timer = address & ~0x1000u;
        return address == 0x0021501c || Imx1Gpio::registerName(address) || Imx1Aitc::registerName(address) ||
               address == 0x0021b000 || (timer >= 0x00202000 && timer <= 0x00202014 && timer != 0x00202010 && !(timer & 3)) ||
               address == 0x00209004 || address == 0x00209008 || (Imx1Rtc::registerName(address) && !(address & 3));
    }
    Imx1BootstrapAccess clockStatus() const;
    const Imx1ReferenceTime& referenceTime() const { return elapsedReference; }
    bool referenceChargeSupported(uint64_t n, uint64_t d) const {
        auto next = elapsedReference; return next.add(n, d);
    }
    bool gpioOutputPattern(unsigned port, uint32_t inputs, uint32_t high, uint32_t low) const {
        return gpio.outputPattern(port, inputs, high, low);
    }
    bool cs5PinsSelected(bool acknowledge) const { return gpio.cs5PinsSelected(acknowledge); }
    // Stop mode (MC9328MX1RM 12.4.3): with SPEN and MPEN clear both PLLs are off and only CLK32 runs,
    // so the CPU and every module on HCLK or a PERCLK stand still. The System PLL goes on until the
    // CLK32 edges of SD_CNT have passed, 16 HCLK at least (table 12-5), and an interrupt before that
    // keeps it on. An asserted IRQ or FIQ turns both PLLs on again (table 12-3), locked 300 us at most
    // later (12.4.2), taken as the nine CLK32 periods the MCU PLL wake already uses.
    bool systemStopArmed() const { return systemStop == stopArmed; }
    bool systemStopped() const { return systemStop >= stopStopped; }
    // Whether the CPU's clock is off: stopped, or on FCLK (clock mode 3) with the MCU PLL off or still
    // locking after a wake. Only stop mode leaves a CPU on FCLK without the MCU PLL.
    bool cpuClockHeld(uint32_t cpuClockMode) const {
        return systemStopped() || (cpuClockMode == 3 && (systemStopArmed() || cgm.mcuWakePending()));
    }
    // The step a waiting CPU's time passes in: one HCLK period, or one CLK32 period in stop mode.
    Imx1PllRatio busReferencePeriod() const {
        if (systemStopped()) return {1, 1};
        const auto rate = cgm.systemPllActiveRatio();
        return {uint64_t(rate.denominator) * cgm.busDivider(), uint32_t(rate.numerator)};
    }
    // Elapsed HCLK periods while the CPU waits, independently of retired instructions.
    void advanceHclkCycles(uint32_t cycles);
    // HCLK periods that can pass in one advance before a peripheral changes state.
    // 1 while a model is busy in a way that isn't worked out ahead.
    uint64_t idleQuietHclk() const;
    Imx1AitcPending interruptPending() const;
    bool mcuWakePending() const { return cgm.mcuWakePending(); }
    bool mcuEnabled() const { return cgm.mcuEnabled(); }
    bool wakeMcuFromInterrupt() {
        const auto lines = interruptLines();
        const bool asserted = lines.irq == Imx1AitcLine::asserted || lines.fiq == Imx1AitcLine::asserted;
        if (!asserted) return false;
        if (systemStop == stopStopped) { cgm.wakePlls(); systemStop = stopWaking; }
        else if (systemStop == stopArmed) { cgm.wakePlls(); systemStop = stopRunning; }
        else cgm.wakeMcuPll();
        return true;
    }
    bool gpioWakePossible(unsigned port, uint32_t value, uint32_t known) const {
        auto pins = gpio;
        if (pins.driveInput(port, value, known) != Imx1GpioAccess::ok) return false;
        auto controller = aitc;
        setInterruptInputs(controller, i2c, rtc, timer1, timer2, asp, pwm, pins);
        return controller.irq() == Imx1AitcLine::asserted || controller.fiq() == Imx1AitcLine::asserted;
    }
    bool idleWakePossible() const {
        // Stopped, time goes on in CLK32 periods whatever can interrupt, so the RTC keeps the time.
        if (systemStop != stopRunning) return true;
        auto candidate = aitc;
        uint64_t sources = rtc.futureInterrupts() | asp.futureInterrupts() |
            (mmcsd.canInterrupt() ? uint64_t(1) << 35 : 0) |
            (timer1.canInterrupt() ? uint64_t(1) << 59 : 0) |
            (timer2.canInterrupt() ? uint64_t(1) << 58 : 0) |
            (uart1.busy() ? uint64_t(0x3f) << 25 : 0) | (uart2.busy() ? uint64_t(0x3f) << 19 : 0) |
            ((pcmDmaEndPossible() || memoryDmaEndPossible()) ? uint64_t(1) << 61 : 0);
        candidate.setInputs(sources, ~uint64_t(0));
        return watchdog.enabled() || candidate.irq() == Imx1AitcLine::asserted ||
               candidate.fiq() == Imx1AitcLine::asserted || ymfWakePossible();
    }
    // The YMF768's sequencer will assert its line and B15 low would reach the CPU through GPIO and AITC.
    bool ymfWakePossible() const {
        if (!ymfEnabled || ymf.interrupt()) return false;
        const auto pll = cgm.systemPllActiveRatio();
        if (ymf.clocksUntilChange(pll.numerator, pll.denominator) == UINT64_MAX) return false;
        auto pins = gpio;
        if (pins.transitionInput(1, 0, 0x8000) != Imx1GpioAccess::ok) return false;
        auto controller = aitc;
        setInterruptInputs(controller, i2c, rtc, timer1, timer2, asp, pwm, pins);
        return controller.irq() == Imx1AitcLine::asserted || controller.fiq() == Imx1AitcLine::asserted;
    }
    struct InterruptLines { Imx1AitcLine irq, fiq; };
    InterruptLines interruptLines() const;
    void connectCpuInterrupts() { cpuInterruptsConnected = true; }
    void setMcuRetuneScenario(bool enabled) { mcuRetuneScenario = enabled; cgm.setMcuRetuneScenario(enabled); }
    void setSystemPllScenario(bool enabled) { cgm.setSystemPllScenario(enabled); }
    bool setGpioInput(unsigned port, uint32_t value) { runCache.driveSettled = false; return gpio.setInput(port, value); }
    Imx1BootstrapAccess updateGpioInput(unsigned port, uint32_t value, uint32_t known);
    bool setGpioInputMasked(unsigned port, uint32_t value, uint32_t mask) {
        runCache.driveSettled = false;
        return gpio.setInputMasked(port, value, mask);
    }
    bool reserveGpioInput(unsigned port, uint32_t mask) { runCache.driveSettled = false; return gpio.reserveInput(port, mask); }
    // Preview permits a peripheral and its GPIO/AITC effects to commit together.
    Imx1BootstrapAccess driveGpioInput(unsigned port, uint32_t value, uint32_t known,
                                      bool commit = true, bool baseline = false);
    // True when driving port with these levels again leaves the pins as they are, the CPU's interrupt
    // lines are connected and the AITC gate passes now. A drive of the same known lane with either level
    // is then accepted too: only that lane's pad changes, no status bit can become unknown, and the gate
    // depends only on which inputs are known.
    bool gpioDriveAccepted(unsigned port, uint32_t value, uint32_t known) const;
    // True when a drive of the single pin in known is accepted at both levels: the last committed drive was on
    // that pin and is accepted again now, and the other level would leave every GPIO interrupt output as known.
    bool gpioDriveBothAccepted(unsigned port, uint32_t known) const;
    // The known part of sourceInputs, and of the GPIO ports' AITC inputs.
    uint64_t sourceKnown(const Imx1Asp& analog) const;
    static uint64_t gpioKnown(const Imx1Gpio& pins);
    // interruptGate with the CPU's lines connected, where only which inputs are known matters.
    Imx1BootstrapAccess connectedGate(const Imx1Gpio& pins) const;
    // True when the last committed drive was this one, so driving it again changes nothing.
    bool gpioDriveSettled(unsigned port, uint32_t value, uint32_t known) const {
        const auto& c = runCache;
        return c.driveSettled && port == c.drivePort && value == c.driveValue && known == c.driveKnown;
    }
    // One atomic external event, including all GPIO pending effects and the AITC gate.
    Imx1BootstrapAccess transitionGpioInputs(const uint32_t (&values)[4], const uint32_t (&masks)[4], bool commit = true);
    bool setGpioDInput(uint32_t value) { return setGpioInput(3, value); }
    void setRamMiB(uint32_t size);
    // FastBus CPU cycles are BCLK cycles; both BCLK and PERCLK1 derive from System PLL.
    void advanceFastBusCycles(uint32_t cycles) { advanceCpuCycles(cycles, 0, false); }
    bool cpuClockModeSupported(uint32_t mode, bool assumeSettled) const {
        if (mode == 0) return !cgm.systemRestartRemaining() &&
            referenceRatioSupported(cgm.systemPllActiveRatio().numerator);
        if (mode != 3 || !cgm.mcuClockReady(assumeSettled)) return false;
        return asynchronousRatioSupported(cgm.systemPllActiveRatio()) &&
            referenceRatioSupported(cgm.mcuPllTargetRatio().numerator);
    }
    bool asynchronousRatioSupported(Imx1PllRatio system) const {
        return asynchronousRatioSupported(cgm, system);
    }
    static bool asynchronousRatioSupported(const Imx1Cgm& clocks, Imx1PllRatio system) {
        const auto pll = clocks.mcuPllTargetRatio();
        // DDI0151C 5.4 requires asynchronous FCLK strictly greater than BCLK.
        return pll.numerator * system.denominator * clocks.busDivider() >
               system.numerator * pll.denominator * clocks.mcuPrescaler();
    }
    Imx1PllRatio cpuReferencePeriod(uint32_t mode) const {
        const auto rate = mode == 3 ? cgm.mcuPllTargetRatio() : cgm.systemPllActiveRatio();
        return {uint64_t(rate.denominator) * (mode == 3 ? cgm.mcuPrescaler() : cgm.busDivider()),
                uint32_t(rate.numerator)};
    }
    // Finish a held-high System PLL interval before the next architectural bus access.
    // Returns elapsed independent reference units, without retiring CPU instructions.
    uint64_t finishSystemPllBusStall();
    bool systemPllBusStallPending() const { return cgm.systemRestartRemaining() != 0; }
    // Charge with the post-instruction mode, preserving System PLL and PERCLK1 fractions.
    void advanceCpuCycles(uint32_t cycles, uint32_t mode, bool assumeSettled);
    Imx1BootstrapAccess access(uint32_t address, uint32_t size, bool write, uint32_t& value, uint32_t cpuClockMode = 0);
    // Instruction reads cannot issue SDRAM commands or consume peripheral state.
    bool instructionRead(uint32_t address, uint32_t size, uint32_t& value) const;
    bool cacheMemoryRange(uint32_t address, uint32_t size, bool write) const;
    void emitDiagnosticMemory(const ArmDiagnosticSink&) const;
    bool instructionWord(uint32_t address, uint32_t& value) const;
    static const char* registerName(uint32_t address);
    // Writes that can change what plainPage gives: the DMA controller (the PCM DMA buffer) and the SDRAM
    // controller (bank modes).
    static bool plainPagesWrite(uint32_t address) {
        return (address & 0xfffff000) == 0x00209000 || (address & 0xfffff000) == 0x00221000;
    }
    static bool isRamAddress(uint32_t address) {
        return (address >= 0x00300000 && address < 0x00400000) ||
               (address >= 0x08000000 && address < 0x10000000);
    }
    // Registers, clocks and every model, without the memories below.
    static constexpr uint32_t stateVersion = 5;
    template <class A> void state(A& a) {
        SourceChange change(*this);
        runCache.driveSettled = false;
        a.Do64(elapsedReference.ticks).Do64(elapsedReference.numerator).Do64(elapsedReference.denominator);
        a.Require(elapsedReference.valid(), "reference time");
        a.DoBool(referenceOverflow).FixedBool(cpuInterruptsConnected);
        rtc.state(a); i2c.state(a); asp.state(a); pwm.state(a); watchdog.state(a);
        a.Do32(bclkPhase);
        aitc.state(a); cgm.state(a); gpio.state(a); eim.state(a); timer1.state(a); timer2.state(a);
        sysctrl.state(a); mmcsd.state(a);
        a.Do32(perclk1Phase).Do64(systemPllPhase).DoBool(asynchronousUsed).FixedBool(mcuRetuneScenario);
        a.Do32(rmcr).Do32(pwmr).FixedBool(lcdcContrastOnly).Do32(contrastPhase).Do32(perclk2Phase);
        uart1.state(a); uart2.state(a);
        a.DoBuffer32(ssi1Words, 7).DoBuffer16(ssi1TxFifo, 8).Do32(ssi1TxCount).Do32(ssi1Phase).Do32(ssi1Slot);
        a.Do16(ssi1ShiftWord).Do16(ssi1LastTransmitted).Do64(ssi1Transmitted);
        a.DoBool(ssi1Shifting).DoBool(ssi1Underrun).DoBool(ssi1StatusRead);
        a.Require(ssi1TxCount <= 8, "ssi fifo");
        a.Do32(dmaControl).Do32(dmaMask).Do32(dmaBurstTimeout).Do32(dmaResetClocks);
        for (auto& channel : dmaChannels) a.DoBuffer32(channel, 7);
        a.Do32(dmaStatus).Do32(dmaPcmCount).Do32(dmaPcmSource).Do32(dmaPcmBurst).Do32(dmaPcmPhase).Do32(dmaPcmPacked);
        a.DoBool(dmaPcmActive);
        for (auto& s : sdram)
            a.Do32(s.control).Fixed32(s.bytes).DoBool(s.controlKnown).DoBool(s.precharged).DoBool(s.modeSet).Do32(s.refreshes);
        reset.state(a); bta.state(a);
        a.Do32(systemStop).Do32(systemStopHclk).Do64(systemStopTick);
        a.Require(systemStop <= stopWaking, "stop mode");
    }
    // A hash of SRAM and both SDRAM banks, carried on from h, for tests.
    uint64_t memoryHash(uint64_t h) const {
        const auto add = [&](const std::vector<uint8_t>& bytes) {
            for (uint8_t b : bytes) h = (h ^ b) * 0x100000001b3ull;
            h = (h ^ bytes.size()) * 0x100000001b3ull;
        };
        add(sram);
        for (const auto& s : sdram) add(s.ram);
        return h;
    }
    template <class A> void stateSram(A& a) {
        const size_t bytes = sram.size();
        a.DoVector(sram, bytes).Require(sram.size() == bytes, "sram size");
    }
    // An SDRAM bank is empty until its first write.
    template <class A> void stateSdram(A& a, unsigned bank) {
        auto& s = sdram[bank];
        a.DoVector(s.ram, s.bytes).Require(s.ram.empty() || s.ram.size() == s.bytes, "sdram size");
    }
    // What the last hardware reset was, as RSR reports it. A machine is made by power-on.
    void setResetSource(Imx1ResetSource source) { reset.set(source); }
    // Makes this board, in its power-on state, the other one after a reset through RESET_IN. That
    // reset leaves the clock controller alone (MC9328MX1RM table 6-1), the SDRAM controller and
    // the memory it holds in self-refresh (24.6.3.1) and the RTC's time (23.2.1 to 23.2.3), and RSR
    // reads EXR after it (table 6-3).
    // ASSUMPTION "SDRAM kept in powerdown": with refresh never enabled (SREFR 0) the controller uses
    // powerdown instead (24.6.3.3), which keeps the power on and promises nothing for the data. The
    // memory is kept in that case too.
    // ASSUMPTION "Reset restarts stopped clocks": the manual is silent on RESET_IN in stop mode. Taken
    // here: a reset during the shutdown count, stopped or locking again turns both PLLs on (SPEN and
    // MPEN set) and they are locked by the time the boot runs, as after a wake that has finished.
    // Without it the boot's first clock write is refused and the machine cannot start.
    void takeExternalReset(Imx1Bootstrap& other) {
        SourceChange change(*this);
        takeMemory(other);
        cgm = other.cgm;
        if (other.systemStop != stopRunning) cgm.restartAfterStop();
        for (unsigned bank = 0; bank < 2; ++bank) {
            auto& to = sdram[bank];
            const auto& from = other.sdram[bank];
            if (to.bytes != from.bytes) continue;
            to.control = from.control; to.controlKnown = from.controlKnown;
            to.precharged = from.precharged; to.modeSet = from.modeSet; to.refreshes = from.refreshes;
        }
        rtc.keepTime(other.rtc);
        reset.set(Imx1ResetSource::external);
    }
    Imx1ResetSource resetSource() const { return reset.last(); }
    // Takes the SRAM and SDRAM bytes of another board of the same geometry, leaving it with these.
    void takeMemory(Imx1Bootstrap& other) {
        sram.swap(other.sram);
        for (unsigned bank = 0; bank < 2; ++bank)
            if (sdram[bank].bytes == other.sdram[bank].bytes) sdram[bank].ram.swap(other.sdram[bank].ram);
    }

   private:
    bool referenceRatioSupported(uint64_t denominator) const {
        return elapsedReference.supports(denominator) && cgm.mcuReferenceSupported(denominator) && rtc.referenceRatioSupported(denominator) && watchdog.referenceRatioSupported(denominator) &&
               pwm.referenceRatioSupported(denominator);
    }
    Imx1ReferenceTime elapsedReference;
    bool referenceOverflow = false;
    bool advanceReference(uint64_t numerator, uint64_t denominator) {
        if (!elapsedReference.add(numerator, denominator)) { referenceOverflow = true; return false; }
        cgm.advanceMcuReference(numerator, denominator);
        rtc.advanceReference(numerator, denominator);
        watchdog.advanceReference(numerator, denominator);
        pwm.advanceReference(numerator, denominator);
        return true;
    }
    // AITC sources of modules that have no model and stay in reset: UART3, SSI2, SPI2, SPI1 and USBD.
    static constexpr uint64_t resetStateSourcesLow = 0x1f | (uint64_t(7) << 36) | (uint64_t(3) << 40) | (uint64_t(0xff) << 47);
    void setInterruptInputs(Imx1Aitc& controller, const Imx1I2c& state, const Imx1Rtc& clock,
                            const Imx1Timer& first, const Imx1Timer& second, const Imx1Asp& analog, const Imx1Pwm& wave, const Imx1Gpio& pins) const;
    // The asserted and known AITC inputs of every source but the GPIO ports.
    void sourceInputs(const Imx1I2c& state, const Imx1Rtc& clock, const Imx1Timer& first, const Imx1Timer& second,
                      const Imx1Asp& analog, const Imx1Pwm& wave, uint64_t& asserted, uint64_t& known) const;
    Imx1BootstrapAccess interruptGate(const Imx1I2c& state, const Imx1Rtc& clock,
                            const Imx1Timer& first, const Imx1Timer& second) const;
    Imx1BootstrapAccess interruptGate(const Imx1Aitc& controller) const;
    bool cpuInterruptsConnected = false;
    // Stop mode: running, SPEN cleared with the shutdown count going, stopped, and locking again.
    static constexpr uint32_t stopRunning = 0, stopArmed = 1, stopStopped = 2, stopWaking = 3;
    uint32_t systemStop = stopRunning, systemStopHclk = 0;
    // The CLK32 edge the shutdown waits for, as a whole reference tick.
    uint64_t systemStopTick = 0;
    void settleSystemStop(uint64_t hclk) {
        if (systemStop != stopArmed) return;
        systemStopHclk -= uint32_t(std::min<uint64_t>(hclk, systemStopHclk));
        if (!systemStopHclk && elapsedReference.ticks >= systemStopTick) systemStop = stopStopped;
    }
    Imx1Rtc rtc;
    Imx1I2c i2c;
    Imx1Asp asp;
    Imx1Pwm pwm;
    Imx1Watchdog watchdog;
    uint32_t bclkPhase = 0;
    Imx1Aitc aitc;
    Imx1Cgm cgm;
    Imx1Gpio gpio;
    Imx1Eim eim;
    Imx1Timer timer1, timer2;
    Imx1Sysctrl sysctrl;
    Imx1Bta bta;
    Imx1Mmcsd mmcsd;
    uint32_t perclk1Phase = 0;
    uint64_t systemPllPhase = 0;
    void advanceSystemOutput(uint64_t clocks, bool wholeWritePeriods = true);
    bool asynchronousUsed = false, mcuRetuneScenario = false;
    std::vector<uint8_t> sram = std::vector<uint8_t>(0x20000);
    uint32_t rmcr = 0, pwmr = 0;
    bool lcdcContrastOnly = false;
    uint32_t contrastPhase = 0, perclk2Phase = 0;
    Imx1Uart uart1{false}, uart2{true};
    // ymfSeen: the clocks of this charge the YMF768 has been moved on by, brought up to each word's end.
    void advancePcm(uint64_t systemClocks, uint64_t& ymfSeen);
    void ymfCatchUp(uint64_t& seen, uint64_t upTo);
    // The MMCSD card's data DMA: a channel other than 10 on request 13 (SDHC) between BUFFER_ACCESS and RAM.
    bool cardDmaConfigurationValid(unsigned number, const uint32_t* channel) const;
    void cardDmaService();
    bool pcmDmaSourceValid(uint32_t address, uint32_t bytes) const;
    bool pcmDmaConfigurationValid(const uint32_t* channel) const;
    void pcmDmaBeat();
    uint64_t pcmIdleQuietHclk() const;
    // Memory-to-memory transfers (REN clear): linear SRAM, SDRAM or graphics memory at both ends. Starting one
    // plans it (every beat checked, the HCLK periods until its DISR bit sets), then moves the data.
    Imx1BootstrapAccess memoryDmaPlan(unsigned number, const uint32_t* channel, uint32_t& clocks) const;
    void memoryDmaMove(unsigned number);
    bool memoryDmaCounting() const { return memoryDma.active && sysctrl.dmaClockEnabled() && !dmaResetClocks && (dmaControl & 1); }
    void advanceMemoryDma(uint64_t hclk);
    // A counting memory-to-memory transfer whose DISR bit will raise the DMA interrupt (source 61).
    bool memoryDmaEndPossible() const { return memoryDmaCounting() && (memoryDma.active & ~dmaMask & 0x3ff); }
    // Channel 10 is taking beats and the end of its buffer raises the DMA interrupt (source 61).
    bool pcmDmaEndPossible() const {
        return pcmIdleQuietHclk() != UINT64_MAX && ((dmaChannels[10][3] & 4) || !(dmaMask & (1u << 10)));
    }
    void pcmWordBoundary();
    // Qualified synchronous network transport, without an attached DAC.
    uint32_t ssi1Words[7] = {0, 0, 0, 0, 0, 0x81, 0};
    uint16_t ssi1TxFifo[8] = {};
    uint32_t ssi1TxCount = 0;
    uint32_t ssi1Phase = 0, ssi1Slot = 0;
    uint16_t ssi1ShiftWord = 0, ssi1LastTransmitted = 0;
    uint64_t ssi1Transmitted = 0;
    bool ssi1Shifting = false, ssi1Underrun = false, ssi1StatusRead = false;
    uint32_t dmaControl = 0, dmaMask = 0x7ff, dmaBurstTimeout = 0;
    uint32_t dmaResetClocks = 0;
    uint32_t dmaChannels[11][7] = {};
    uint32_t dmaStatus = 0, dmaPcmCount = 0, dmaPcmSource = 0;
    uint32_t dmaPcmBurst = 0, dmaPcmPhase = 0, dmaPcmPacked = 0;
    bool dmaPcmActive = false;
    struct Sdram {
        uint32_t control = 0x01000300, bytes = 0;
        bool controlKnown = false;
        bool precharged = false, modeSet = false;
        uint32_t refreshes = 0;
        std::vector<uint8_t> ram;
    } sdram[2];
    Imx1Reset reset;
    Imx1BootstrapAccess sdramControl(unsigned bank, bool write, uint32_t& value);
    Imx1BootstrapAccess sdramAccess(unsigned bank, uint32_t offset, uint32_t size, bool write, uint32_t& value);

    // Not saved and not part of the state layout. sourceInputs of the board's own models, kept from one
    // call to the next until a model can have changed, and the last GPIO drive that left the pins as a
    // second one with the same levels would (driveGpioInput gives the same pins when run twice).
    struct RunCache {
        uint64_t asserted = 0, known = 0;
        uint32_t driveValue = 0, driveKnown = 0;
        // Above zero while a method that can change a source runs; nothing is kept then.
        uint32_t changing = 0;
        uint8_t drivePort = 0;
        bool sourcesValid = false, driveSettled = false, uartShared = false;
        // Whether the other level of the settled drive keeps the GPIO outputs known. Worked out from the GPIO
        // alone, so it holds until the next committed drive.
        bool otherValid = false, otherAccepted = false;
        RunCache() = default;
        RunCache(const RunCache&) = default;
        // Once uart() has handed out a reference the flag stays, whatever is assigned onto the board.
        RunCache& operator=(const RunCache& other) {
            asserted = other.asserted; known = other.known;
            driveValue = other.driveValue; driveKnown = other.driveKnown;
            changing = other.changing; drivePort = other.drivePort;
            sourcesValid = other.sourcesValid; driveSettled = other.driveSettled;
            otherValid = other.otherValid; otherAccepted = other.otherAccepted;
            uartShared = uartShared || other.uartShared;
            return *this;
        }
    };
    mutable RunCache runCache;
    // In the card section (stateCard): the card DMA channels taking requests and the bytes each has moved.
    // Not saved: the store hook and the PCM output.
    struct CardDma {
        uint32_t active = 0;
        uint32_t moved[11] = {};
    } cardDma;
    void (*pcmHook)(void*, uint16_t, unsigned) = nullptr;
    void* pcmContext = nullptr;
    // In the YMF768 section (stateYmf768). ymfEnabled is set by the machine, not saved. ymfLineRefused is set
    // when B15 can't follow the chip's line; clockStatus then stops the run, so it isn't saved either.
    Imx1Ymf768 ymf;
    bool ymfEnabled = false, ymfLineRefused = false;
    // GPIO B15 follows the chip's interrupt line, low while asserted. The board options fix port B before the
    // run, so a change is a transition; a board without fixed port B inputs gets B15 fixed here before its
    // first access. Anything else (an interrupt on a port B output, say) refuses the change.
    bool ymfDriveLine() {
        const uint32_t level = ymf.interrupt() ? 0 : 0x8000;
        if ((gpio.fixedInputs(1) & 0x8000) && (gpio.inputSample(1) & 0x8000) == level) return true;
        runCache.driveSettled = false;
        if (gpio.transitionInput(1, level, 0x8000) == Imx1GpioAccess::ok || gpio.setInputMasked(1, level, 0x8000))
            return true;
        ymfLineRefused = true;
        return false;
    }
    void (*dmaStoreHook)(void*, uint32_t) = nullptr;
    void* dmaStoreContext = nullptr;
    // In the memory DMA section (stateMemoryDma): the memory-to-memory channels under way and the HCLK periods
    // each has left (10 unused: channel 10 is the PCM). Not saved: the external memory.
    struct MemoryDma {
        uint32_t active = 0;
        uint32_t remaining[11] = {};
    } memoryDma;
    Imx1DmaExternal* dmaExternal = nullptr;
    // Every method that can change a source other than the GPIO ports holds one of these while it runs.
    struct SourceChange {
        const Imx1Bootstrap& board;
        explicit SourceChange(const Imx1Bootstrap& board) : board(board) {
            board.runCache.sourcesValid = false;
            ++board.runCache.changing;
        }
        ~SourceChange() {
            --board.runCache.changing;
            board.runCache.sourcesValid = false;
        }
        SourceChange(const SourceChange&) = delete;
        SourceChange& operator=(const SourceChange&) = delete;
    };

   public:
    static constexpr size_t unsavedBytes = sizeof(RunCache) + sizeof(CardDma) + sizeof(MemoryDma) + 5 * sizeof(void*) +
                                           Imx1Mmcsd::unsavedBytes + sizeof(Imx1Ymf768) + sizeof(uint64_t);
};

#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1Bootstrap) - Imx1Bootstrap::unsavedBytes == 2344, "update state() and stateVersion");
#endif

const char* imx1BootstrapAccessName(Imx1BootstrapAccess access);

#endif
