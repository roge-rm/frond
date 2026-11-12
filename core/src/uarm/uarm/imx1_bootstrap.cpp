#include "imx1_bootstrap.h"
#include "zodiac_ymf768_core.h"

#include <cstring>
#include <cassert>
#include <algorithm>
#include <limits>

void Imx1Bootstrap::advanceCpuCycles(uint32_t cycles, uint32_t mode, bool assumeSettled) {
    SourceChange change(*this);
    assert(cpuClockModeSupported(mode, assumeSettled));
    if (mode != 3) {
        const auto system = cgm.systemPllActiveRatio();
        if (!advanceReference(uint64_t(cycles) * cgm.busDivider() * system.denominator, system.numerator)) return;
        advanceSystemOutput(uint64_t(cycles) * cgm.busDivider());
        settleSystemStop(cycles);
        return;
    }
    const auto mcu = cgm.mcuPllTargetRatio();
    // A 32-bit charge times denominator (<= 16384) and PRESC (<= 2) fits uint64_t.
    uint64_t units = uint64_t(cycles) * mcu.denominator * cgm.mcuPrescaler();
    if (!referenceChargeSupported(units, mcu.numerator)) { referenceOverflow = true; return; }
    asynchronousUsed = true;
    cgm.qualifyMcuOutput();
    if (!advanceReference(units, mcu.numerator)) return;
    if (const auto pending = cgm.systemRestartRemaining()) {
        const auto elapsed = std::min(units, pending);
        units -= elapsed;
        if (cgm.advanceSystemRestart(elapsed)) systemPllPhase = 0;
        // LF=0 holds the output high. Timer and peripheral divider phases are retained.
    }
    const auto system = cgm.systemPllActiveRatio();
    const uint64_t denominator = mcu.numerator * system.denominator;
    while (units) {
        // Bound multiplication for every legal PLL field and maximum CPU charge on 32-bit hosts.
        const uint64_t chunk = std::min(units,
            (std::numeric_limits<uint64_t>::max() - systemPllPhase) / system.numerator);
        const uint64_t clocks = chunk * system.numerator + systemPllPhase;
        cgm.advanceSystemWriteFraction(chunk * system.numerator, denominator);
        advanceSystemOutput(clocks / denominator, false);
        systemPllPhase = clocks % denominator;
        units -= chunk;
    }
}

// The same sums as advanceCpuCycles and advanceSystemOutput, down to the PERCLK1 cycles a timer gets.
bool Imx1Bootstrap::timerCountAfter(uint32_t address, uint32_t size, uint64_t cycles, uint32_t mode, uint32_t& value,
                                     uint64_t hclk) const {
    if (size != 4 || (address != 0x00202010 && address != 0x00203010)) return false;
    if (cgm.systemRestartRemaining() || cycles > UINT32_MAX || hclk > UINT32_MAX || (hclk && systemStopped())) return false;
    uint64_t systemClocks = cycles * cgm.busDivider();
    if (mode == 3) {
        const auto mcu = cgm.mcuPllTargetRatio();
        const auto system = cgm.systemPllActiveRatio();
        const uint64_t units = cycles * mcu.denominator * cgm.mcuPrescaler();
        if (units > (std::numeric_limits<uint64_t>::max() - systemPllPhase) / system.numerator) return false;
        systemClocks = (units * system.numerator + systemPllPhase) / (mcu.numerator * system.denominator);
    }
    // advanceHclkCycles: each HCLK cycle is busDivider system clocks.
    systemClocks += hclk * cgm.busDivider();
    auto timer = address < 0x00203000 ? timer1 : timer2;
    timer.advance((systemClocks + perclk1Phase) / cgm.perclk1Divider());
    return timer.access(0x10, 4, false, value, false) == Imx1TimerAccess::ok;
}

void Imx1Bootstrap::advanceHclkCycles(uint32_t cycles) {
    SourceChange change(*this);
    assert(!cgm.systemRestartRemaining());
    if (systemStopped()) {
        // Only CLK32 runs: the reference time, the RTC, the watchdog and the PLL lock count.
        if (!advanceReference(cycles, 1)) return;
        if (systemStop == stopWaking && !cgm.mcuWakePending()) {
            systemStop = stopRunning;
            systemPllPhase = 0;
        }
        return;
    }
    const auto period = busReferencePeriod();
    if (!advanceReference(uint64_t(cycles) * period.numerator, period.denominator)) return;
    advanceSystemOutput(uint64_t(cycles) * cgm.busDivider());
    settleSystemStop(cycles);
}

uint64_t Imx1Bootstrap::idleQuietHclk() const {
    // The shutdown count is looked at after every charge.
    if (systemStopArmed()) return 1;
    const auto period = busReferencePeriod();
    const uint64_t numerator = period.numerator, denominator = period.denominator;
    const uint64_t bus = cgm.busDivider();
    uint64_t quiet = cgm.idleQuietSteps(numerator, denominator);
    quiet = std::min(quiet, rtc.idleQuietSteps(numerator, denominator));
    quiet = std::min(quiet, watchdog.idleQuietSteps(numerator, denominator));
    if (systemStopped()) {
        if (pwm.usesReference()) quiet = std::min(quiet, pwm.idleQuietReferenceSteps(numerator, denominator));
        return quiet ? quiet : 1;
    }
    if (pwm.usesReference()) quiet = std::min(quiet, pwm.idleQuietReferenceSteps(numerator, denominator));
    else if (pwm.enabled())
        quiet = std::min(quiet, imx1StepsUntilDivided(pwm.idleQuietCycles(), cgm.perclk1Divider(), perclk1Phase, bus));
    quiet = std::min(quiet, i2c.idleQuietHclk());
    if (const auto cycles = asp.idleQuietPerclk2())
        quiet = std::min(quiet, imx1StepsUntilDivided(cycles, cgm.perclk2Divider(), perclk2Phase, bus));
    if (sysctrl.dmaClockEnabled() && dmaResetClocks) quiet = std::min<uint64_t>(quiet, dmaResetClocks);
    if (memoryDmaCounting())
        for (unsigned n = 0; n < 10; ++n)
            if (memoryDma.active & (1u << n)) quiet = std::min<uint64_t>(quiet, memoryDma.remaining[n]);
    quiet = std::min(quiet, pcmIdleQuietHclk());
    if (ymfEnabled) {
        const auto pll = cgm.systemPllActiveRatio();
        const uint64_t clocks = ymf.clocksUntilChange(pll.numerator, pll.denominator);
        if (clocks != UINT64_MAX) quiet = std::min<uint64_t>(quiet, (clocks + bus - 1) / bus);
    }
    if (const auto cycles = mmcsd.idleQuietPerclk2())
        quiet = std::min(quiet, imx1StepsUntilDivided(cycles, cgm.perclk2Divider(), perclk2Phase, bus));
    for (const auto* timer : {&timer1, &timer2})
        if (const auto cycles = timer->idleQuietCycles())
            quiet = std::min(quiet, imx1StepsUntilDivided(cycles, cgm.perclk1Divider(), perclk1Phase, bus));
    for (const auto* uart : {&uart1, &uart2})
        if (const auto cycles = uart->idleQuietCycles())
            quiet = std::min(quiet, imx1StepsUntilDivided(cycles, cgm.perclk1Divider(), perclk1Phase, bus));
    return quiet ? quiet : 1;
}

// HCLK periods up to and including the one where the PCM DMA can reach the end of its buffer,
// the only thing the PCM transport does that a waiting CPU can see. advancePcm gives the same
// state for one call as for the same clocks in single steps, so the words and beats before that
// need no stop. The count is a lower bound: a beat takes two HCLK, and the eight word FIFO only
// takes the last one after the serial side has sent the words that don't fit.
uint64_t Imx1Bootstrap::pcmIdleQuietHclk() const {
    const bool dma = sysctrl.dmaClockEnabled() && !dmaResetClocks && (dmaControl & 1) && dmaPcmActive &&
                     dmaChannels[10][4] == 16 && (ssi1Words[0] & 0x100) && !(ssi1Words[6] & 0x10) &&
                     (ssi1Words[1] & 0x200);
    if (!dma) return UINT64_MAX;
    const uint32_t length = dmaChannels[10][2];
    if (dmaPcmCount >= length || ssi1TxCount > 8) return 1;
    const uint64_t beats = (uint64_t(length) - dmaPcmCount + 1) / 2;
    const uint64_t beatPeriod = cgm.busDivider() * 2;
    uint64_t clocks = (beats - 1) * beatPeriod + beatPeriod - dmaPcmPhase % beatPeriod;
    const bool serial = (ssi1Words[0] & 0x100) && (ssi1Words[1] & 0x60) == 0x60 && ssi1Words[3] == 0x6100;
    const uint64_t room = 8 - ssi1TxCount;
    if (serial && (ssi1Words[0] & 0x200) && beats > room) {
        const uint64_t wordPeriod = uint64_t(cgm.perclk3Divider()) * 64;
        clocks = std::max(clocks, (beats - room - 1) * wordPeriod + wordPeriod - ssi1Phase % wordPeriod);
    }
    return (clocks + cgm.busDivider() - 1) / cgm.busDivider();
}

uint64_t Imx1Bootstrap::finishSystemPllBusStall() {
    SourceChange change(*this);
    const auto pending = cgm.systemRestartRemaining();
    if (pending) {
        if (!advanceReference(pending, cgm.mcuPllTargetRatio().numerator)) return 0;
        cgm.advanceSystemRestart(pending);
        systemPllPhase = 0;
    }
    return pending;
}

void Imx1Bootstrap::advanceSystemOutput(uint64_t systemClocks, bool wholeWritePeriods) {
    cgm.advanceSystemPllCycles(systemClocks, wholeWritePeriods);
    const bool ymfLine = ymfEnabled && ymf.interrupt();
    uint64_t ymfSeen = 0;
    advancePcm(systemClocks, ymfSeen);
    if (ymfEnabled) {
        ymfCatchUp(ymfSeen, systemClocks);
        if (ymf.interrupt() != ymfLine) ymfDriveLine();
    }
    const uint64_t busClocks = systemClocks + bclkPhase;
    i2c.advance(busClocks / cgm.busDivider());
    pwm.advance(busClocks / cgm.busDivider());
    if (memoryDmaCounting()) advanceMemoryDma(busClocks / cgm.busDivider());
    if (sysctrl.dmaClockEnabled())
        dmaResetClocks -= uint32_t(std::min<uint64_t>(dmaResetClocks, busClocks / cgm.busDivider()));
    bclkPhase = busClocks % cgm.busDivider();
    const uint64_t contrastClocks = systemClocks + perclk2Phase;
    if (lcdcContrastOnly && (rmcr & 2) && (pwmr & 0x100))
        contrastPhase = (contrastPhase + contrastClocks / cgm.perclk2Divider()) & 255;
    mmcsd.advancePerclk2(contrastClocks / cgm.perclk2Divider());
    if (mmcsd.dataWaiting()) cardDmaService();
    asp.advance(contrastClocks / cgm.perclk2Divider());
    perclk2Phase = contrastClocks % cgm.perclk2Divider();
    const uint64_t clocks = systemClocks + perclk1Phase;
    const uint32_t divider = cgm.perclk1Divider();
    timer1.advance(clocks / divider);
    timer2.advance(clocks / divider);
    pwm.advancePerclk1(clocks / divider);
    uart1.advance(clocks / divider);
    uart2.advance(clocks / divider);
    perclk1Phase = clocks % divider;
}

void Imx1Bootstrap::setInterruptInputs(Imx1Aitc& controller, const Imx1I2c& state, const Imx1Rtc& clock,
                                        const Imx1Timer& first, const Imx1Timer& second, const Imx1Asp& analog, const Imx1Pwm& wave, const Imx1Gpio& pins) const {
    uint64_t asserted, known;
    const bool own = &state == &i2c && &clock == &rtc && &first == &timer1 && &second == &timer2 &&
                     &analog == &asp && &wave == &pwm;
    if (own && runCache.sourcesValid) {
        asserted = runCache.asserted;
        known = runCache.known;
    } else {
        sourceInputs(state, clock, first, second, analog, wave, asserted, known);
        if (own && !runCache.changing && !runCache.uartShared) {
            runCache.asserted = asserted;
            runCache.known = known;
            runCache.sourcesValid = true;
        }
    }
    constexpr unsigned sources[] = {11, 12, 13, 62};
    for (unsigned port = 0; port < 4; ++port) {
        const auto output = pins.interruptOutput(port);
        const auto bit = uint64_t(1) << sources[port];
        if (output != Imx1GpioLine::unknown) known |= bit;
        if (output == Imx1GpioLine::asserted) asserted |= bit;
    }
    controller.setInputs(asserted, known);
}

void Imx1Bootstrap::sourceInputs(const Imx1I2c& state, const Imx1Rtc& clock, const Imx1Timer& first, const Imx1Timer& second,
                                 const Imx1Asp& analog, const Imx1Pwm& wave, uint64_t& asserted, uint64_t& known) const {
    known = sourceKnown(analog);
    asserted = (wave.interruptOutput() ? Imx1Pwm::interruptSource : 0) | analog.assertedInterrupts() | (state.interruptOutput() ? uint64_t(1) << 39 : 0) | clock.assertedInterrupts() |
               (first.interruptOutput() ? uint64_t(1) << 59 : 0) |
               (second.interruptOutput() ? uint64_t(1) << 58 : 0);
    if (dmaControl && (dmaStatus & ~dmaMask)) asserted |= uint64_t(1) << 61;
    if (mmcsd.interruptOutput()) asserted |= uint64_t(1) << 35;
    // 6.2: PFERR, RTS, DTR, UARTC, TX and RX from 19 for UART2 and from 25 for UART1, which has no DTR.
    static constexpr unsigned outputs[]{Imx1Uart::errorOutput, Imx1Uart::rtsOutput, Imx1Uart::dtrOutput,
                                        Imx1Uart::controlOutput, Imx1Uart::txOutput, Imx1Uart::rxOutput};
    for (unsigned unit = 0; unit < 2; ++unit) {
        const unsigned first = unit ? 19 : 25;
        const uint32_t lines = (unit ? uart2 : uart1).interruptOutputs();
        for (unsigned line = 0; line < 6; ++line) {
            if (!unit && line == 2) continue;
            if (lines & outputs[line]) asserted |= uint64_t(1) << (first + line);
        }
    }
}

uint64_t Imx1Bootstrap::sourceKnown(const Imx1Asp& analog) const {
    uint64_t known = (uint64_t(1) << 39) | (analog.inactiveInterrupts() | analog.assertedInterrupts()) | Imx1Pwm::interruptSource | Imx1Rtc::interruptSources | Imx1Watchdog::interruptSource |
            (uint64_t(1) << 59) | (uint64_t(1) << 58);
    // Only the qualified SSI TX channel can complete; error producers remain disabled.
    known |= (uint64_t(1) << 60) | (uint64_t(1) << 61);
    // SSI interrupt enables remain guarded; DMA requests go directly to the DMAC.
    known |= uint64_t(0xf) << 42;
    known |= Imx1Bta::interruptSources;
    if (mmcsd.interruptKnown()) known |= uint64_t(1) << 35;
    // UART2's six lines from 19, UART1's from 25 without DTR (27), as sourceInputs sets them.
    known |= (uint64_t(0x3f) << 19) | (uint64_t(0x3b) << 25);
    // Modules with no model here, so no register of theirs was ever written (an access stops the machine)
    // and each still has its reset state, which the manual gives with every interrupt disabled or masked:
    // SSI2 36 to 38: STCR and SRCR reset to 0000, TIE and RIE clear (30.3.8, 30.3.9, tables 30-11 and 30-13).
    // SPI2 40 and SPI1 41: INTREG resets to 0000, all eight enables clear (18.3.4, table 18-7).
    // USBD 47 to 52: USB_EPn_MASK resets to 01ff, every endpoint interrupt masked (28.3.12, table 28-17).
    // USBD 53: USB_MASK resets to 800000ff, every interrupt masked (28.3.8).
    // UART3 0 to 4 and 54: UCR1 to UCR4 reset with every interrupt enable clear (27.7.3 to 27.7.6,
    // tables 27-16, 27-17, 27-19 and 27-20, which cover UART 1, 2 and 3).
    known |= resetStateSourcesLow;
    return known;
}

uint64_t Imx1Bootstrap::gpioKnown(const Imx1Gpio& pins) {
    constexpr unsigned sources[] = {11, 12, 13, 62};
    uint64_t known = 0;
    for (unsigned port = 0; port < 4; ++port)
        if (pins.interruptOutput(port) != Imx1GpioLine::unknown) known |= uint64_t(1) << sources[port];
    return known;
}

Imx1BootstrapAccess Imx1Bootstrap::connectedGate(const Imx1Gpio& pins) const {
    return aitc.interruptInputsKnownWith(sourceKnown(asp) | gpioKnown(pins)) ? Imx1BootstrapAccess::ok :
                                                                             Imx1BootstrapAccess::externalInput;
}

bool Imx1Bootstrap::pcmDmaSourceValid(uint32_t address, uint32_t bytes) const {
    return bytes && !(bytes & 3) && !(address & 3) && cacheMemoryRange(address, bytes, false);
}

// MC9328MX1RM 20.5.1 and 11: a card read takes BUFFER_ACCESS (FIFO mode, 16-bit) to linear RAM, a write
// linear RAM to BUFFER_ACCESS, with the request enabled and no repeat. Only RAM a plain access reaches.
bool Imx1Bootstrap::cardDmaConfigurationValid(unsigned number, const uint32_t* channel) const {
    if (number == 10 || !mmcsd.card() || channel[4] != 13 || !channel[2]) return false;
    const uint32_t control = channel[3];
    if ((control & 0xe) != 8) return false;
    const uint32_t sourceMode = (control >> 10) & 3, destinationMode = (control >> 12) & 3;
    const uint32_t sourceSize = (control >> 4) & 3, destinationSize = (control >> 6) & 3;
    if (sourceMode == 2 && destinationMode == 0 && sourceSize == 2 && channel[0] == 0x00214038)
        return cacheMemoryRange(channel[1], channel[2], true);
    if (sourceMode == 0 && destinationMode == 2 && destinationSize == 2 && channel[1] == 0x00214038)
        return cacheMemoryRange(channel[0], channel[2], false);
    return false;
}

// Moves the card's waiting data through each enabled card channel in the matching direction, as much
// as its count has left, at once. A channel that reaches its count sets its DISR bit and stops.
void Imx1Bootstrap::cardDmaService() {
    SourceChange change(*this);
    for (unsigned number = 0; number < 10 && mmcsd.dataWaiting(); ++number) {
        if (!(cardDma.active & (1u << number))) continue;
        auto& channel = dmaChannels[number];
        const bool toCard = channel[1] == 0x00214038;
        if (toCard != mmcsd.dataWrite()) continue;
        const uint32_t base = toCard ? channel[0] : channel[1];
        while (cardDma.moved[number] < channel[2] && mmcsd.dataWaiting()) {
            const uint32_t address = base + cardDma.moved[number];
            const uint32_t inPage = 0x1000 - (address & 0xfff);
            const uint32_t size = std::min(inPage, channel[2] - cardDma.moved[number]);
            uint8_t* page = plainPage(address & ~0xfffu, !toCard);
            if (!page) break;
            const uint32_t moved = mmcsd.moveData(page + (address & 0xfff), size);
            if (moved && !toCard && dmaStoreHook) dmaStoreHook(dmaStoreContext, address & ~0xfffu);
            cardDma.moved[number] += moved;
            if (moved < size) break;
        }
        if (cardDma.moved[number] == channel[2]) {
            dmaStatus |= 1u << number;
            cardDma.active &= ~(1u << number);
        }
    }
}

// MC9328MX1RM 13.3 and 13.4: with REN clear a channel starts on CEN and moves CNTR bytes from SAR to DAR in
// bursts of BLR bytes (0 is 64), reading a burst at the source port size and writing it at the destination's,
// with BUCR HCLK periods between bursts, then sets its DISR bit (interrupt source 61 unless DIMR masks it).
// Here the data moves at once and the DISR bit waits for the time the bursts take: an SDRAM or SRAM burst
// three HCLK periods to open and one a beat, a graphics memory beat what one CS5 access waits. clocks gets
// that time. Only linear transfers with no repeat; the rest stays guarded.
Imx1BootstrapAccess Imx1Bootstrap::memoryDmaPlan(unsigned number, const uint32_t* channel, uint32_t& clocks) const {
    const uint32_t control = channel[3];
    if (!sysctrl.dmaClockEnabled()) return Imx1BootstrapAccess::sideEffect;
    // FRC, RPT, REN, MSEL, MDIR, SMOD and DMOD clear; port sizes 32, 8 or 16 bits.
    if (number >= 10 || (control & 0x3f0e) || ((control >> 4) & 3) == 3 || ((control >> 6) & 3) == 3)
        return Imx1BootstrapAccess::sideEffect;
    static constexpr uint32_t sizes[]{4, 1, 2, 0};
    const uint32_t sourceSize = sizes[(control >> 4) & 3], destinationSize = sizes[(control >> 6) & 3];
    const uint32_t count = channel[2];
    if (!count || count % sourceSize || count % destinationSize) return Imx1BootstrapAccess::sideEffect;
    uint64_t beatClocks[2] = {}, openClocks[2] = {};
    for (unsigned end = 0; end < 2; ++end) {
        const bool write = end == 1;
        const uint32_t base = channel[end], size = write ? destinationSize : sourceSize;
        if (cacheMemoryRange(base, count, write)) {
            if (write && pcmDmaReads(base, count)) return Imx1BootstrapAccess::memoryUnavailable;
            beatClocks[end] = 1;
            openClocks[end] = 3;
            continue;
        }
        if (!dmaExternal || uint64_t(base) + count > 0x100000000ull || !dmaExternal->owns(base) ||
            !dmaExternal->owns(base + count - 1))
            return Imx1BootstrapAccess::memoryUnavailable;
        for (uint32_t offset = 0; offset < count; offset += size) {
            const auto status = dmaExternal->check(base + offset, size, write);
            if (status != Imx1BootstrapAccess::ok) return status;
        }
        uint32_t perBeat = 0;
        if (!dmaExternal->clocks(perBeat)) return Imx1BootstrapAccess::sideEffect;
        beatClocks[end] = perBeat;
    }
    const uint32_t burst = channel[5] ? channel[5] : 64;
    const uint64_t bursts = (uint64_t(count) + burst - 1) / burst;
    const uint64_t total = uint64_t(count / sourceSize) * beatClocks[0] + uint64_t(count / destinationSize) * beatClocks[1] +
                           bursts * (openClocks[0] + openClocks[1]) + (bursts - 1) * (channel[6] & 0xffff);
    if (!total || total > UINT32_MAX) return Imx1BootstrapAccess::sideEffect;
    clocks = uint32_t(total);
    return Imx1BootstrapAccess::ok;
}

// Moves a planned memory-to-memory transfer a burst at a time, so overlapping ends copy as the DMA would.
void Imx1Bootstrap::memoryDmaMove(unsigned number) {
    const auto* channel = dmaChannels[number];
    static constexpr uint32_t sizes[]{4, 1, 2, 0};
    const uint32_t sourceSize = sizes[(channel[3] >> 4) & 3], destinationSize = sizes[(channel[3] >> 6) & 3];
    const uint32_t count = channel[2], burst = channel[5] ? channel[5] : 64;
    const bool sourceRam = cacheMemoryRange(channel[0], count, false);
    const bool destinationRam = cacheMemoryRange(channel[1], count, true);
    uint8_t data[64];
    uint32_t storedPage = 1;
    for (uint32_t done = 0; done < count;) {
        // A burst holds whole beats at both ends.
        uint32_t bytes = std::min(burst, count - done);
        bytes -= bytes % std::max(sourceSize, destinationSize);
        if (!bytes) bytes = std::max(sourceSize, destinationSize);
        for (uint32_t offset = 0; offset < bytes; offset += sourceSize) {
            const uint32_t address = channel[0] + done + offset;
            uint32_t value = 0;
            if (sourceRam) std::memcpy(&value, plainPage(address & ~0xfffu, false) + (address & 0xfff), sourceSize);
            else dmaExternal->move(address, sourceSize, false, value);
            std::memcpy(data + offset, &value, sourceSize);
        }
        for (uint32_t offset = 0; offset < bytes; offset += destinationSize) {
            const uint32_t address = channel[1] + done + offset;
            uint32_t value = 0;
            std::memcpy(&value, data + offset, destinationSize);
            if (!destinationRam) {
                dmaExternal->move(address, destinationSize, true, value);
                continue;
            }
            std::memcpy(plainPage(address & ~0xfffu, true) + (address & 0xfff), &value, destinationSize);
            if ((address & ~0xfffu) != storedPage) {
                storedPage = address & ~0xfffu;
                if (dmaStoreHook) dmaStoreHook(dmaStoreContext, storedPage);
            }
        }
        done += bytes;
    }
}

// A memory-to-memory channel whose time is up sets its DISR bit and stops.
void Imx1Bootstrap::advanceMemoryDma(uint64_t hclk) {
    SourceChange change(*this);
    for (unsigned n = 0; n < 10; ++n) {
        if (!(memoryDma.active & (1u << n))) continue;
        if (memoryDma.remaining[n] > hclk) {
            memoryDma.remaining[n] -= uint32_t(hclk);
            continue;
        }
        memoryDma.remaining[n] = 0;
        memoryDma.active &= ~(1u << n);
        dmaStatus |= 1u << n;
    }
}

bool Imx1Bootstrap::pcmDmaConfigurationValid(const uint32_t* channel) const {
    return (channel[3] & ~5u) == 0x2088 && channel[1] == 0x00218000 &&
           channel[4] == 16 && channel[5] == 8 && channel[6] == 0 &&
           pcmDmaSourceValid(channel[0], channel[2]);
}

void Imx1Bootstrap::pcmDmaBeat() {
    auto& channel = dmaChannels[10];
    // Linear 32-bit memory source is unpacked little endian into 16-bit FIFO writes.
    if (!(dmaPcmCount & 3)) {
        const bool valid = instructionRead(dmaPcmSource + dmaPcmCount, 4, dmaPcmPacked);
        assert(valid);
        (void)valid;
    }
    ssi1TxFifo[ssi1TxCount++] = uint16_t(dmaPcmPacked >> ((dmaPcmCount & 2) * 8));
    if (ssi1StatusRead) ssi1Underrun = ssi1StatusRead = false;
    dmaPcmCount += 2;
    dmaPcmBurst -= 2;
    if (dmaPcmCount == channel[2]) {
        dmaStatus |= 1u << 10;
        if (channel[3] & 4) {
            dmaPcmCount = 0;
            dmaPcmSource = channel[0];
            dmaMask &= ~(1u << 10);
        } else dmaPcmActive = false;
    }
}

void Imx1Bootstrap::pcmWordBoundary() {
    if (ssi1Shifting) {
        ssi1LastTransmitted = ssi1ShiftWord;
        ++ssi1Transmitted;
        ssi1Shifting = false;
        if (pcmHook) pcmHook(pcmContext, ymfEnabled ? ymf.mix(ssi1ShiftWord, ssi1Slot) : ssi1ShiftWord, ssi1Slot);
    }
    if (ssi1Words[0] & 0x200) {
        if (ssi1TxCount) {
            ssi1ShiftWord = ssi1TxFifo[0];
            --ssi1TxCount;
            std::move(ssi1TxFifo + 1, ssi1TxFifo + 1 + ssi1TxCount, ssi1TxFifo);
        } else ssi1Underrun = true;
        // An underrun retransmits the previous shift-register value.
        ssi1Shifting = true;
    }
    ssi1Slot ^= 1;
}

void Imx1Bootstrap::pcmFrameRate(uint64_t& numerator, uint64_t& denominator) const {
    numerator = denominator = 0;
    const bool serial = (ssi1Words[0] & 0x100) && (ssi1Words[1] & 0x60) == 0x60 && ssi1Words[3] == 0x6100;
    const auto pll = cgm.systemPllActiveRatio();
    if (!serial || !pll.numerator || !pll.denominator) return;
    // System PLL = CLK32 (32768 Hz) * ratio, one frame per 128 PERCLK3 clocks.
    numerator = pll.numerator * 256;
    denominator = uint64_t(pll.denominator) * cgm.perclk3Divider();
}

// The YMF768 runs on guest time; SSI1 words take its output at the clock they finish.
void Imx1Bootstrap::ymfCatchUp(uint64_t& seen, uint64_t upTo) {
    if (!ymfEnabled || upTo <= seen) return;
    const auto pll = cgm.systemPllActiveRatio();
    ymf.advance(upTo - seen, pll.numerator, pll.denominator);
    seen = upTo;
}

void Imx1Bootstrap::advancePcm(uint64_t clocks, uint64_t& ymfSeen) {
    const uint64_t total = clocks;
    // Internal continuous clocks keep their phase with TE clear; SSI_EN resets the phase.
    const uint32_t wordPeriod = cgm.perclk3Divider() * 64;
    const uint32_t beatPeriod = cgm.busDivider() * 2;
    ssi1Phase %= wordPeriod;
    dmaPcmPhase %= beatPeriod;
    while (clocks) {
        const bool serial = (ssi1Words[0] & 0x100) &&
                            (ssi1Words[1] & 0x60) == 0x60 && ssi1Words[3] == 0x6100;
        const bool dma = sysctrl.dmaClockEnabled() && !dmaResetClocks &&
                         (dmaControl & 1) && dmaPcmActive &&
                         dmaChannels[10][4] == 16 &&
                         (ssi1Words[0] & 0x100) && !(ssi1Words[6] & 0x10) &&
                         (ssi1Words[1] & 0x200);
        if (dma && !dmaPcmBurst && 8 - ssi1TxCount >= (ssi1Words[5] & 15)) {
            dmaPcmBurst = std::min(8u, dmaChannels[10][2] - dmaPcmCount);
            dmaPcmPhase = 0;
        }
        const bool beat = dma && dmaPcmBurst && ssi1TxCount < 8;
        if (!serial && !beat) break;
        if (serial && !beat && !(ssi1Words[0] & 0x200) && !ssi1Shifting) {
            const uint64_t elapsed = clocks + ssi1Phase;
            ssi1Slot ^= (elapsed / wordPeriod) & 1;
            ssi1Phase = elapsed % wordPeriod;
            break;
        }
        uint64_t step = clocks;
        if (serial) step = std::min<uint64_t>(step, wordPeriod - ssi1Phase);
        if (beat) step = std::min<uint64_t>(step, beatPeriod - dmaPcmPhase);
        clocks -= step;
        if (serial) {
            ssi1Phase += uint32_t(step);
            if (ssi1Phase == wordPeriod) {
                ssi1Phase = 0;
                ymfCatchUp(ymfSeen, total - clocks);
                pcmWordBoundary();
            }
        }
        if (beat) {
            dmaPcmPhase += uint32_t(step);
            if (dmaPcmPhase == beatPeriod) { dmaPcmPhase = 0; pcmDmaBeat(); }
        }
    }
}
Imx1BootstrapAccess Imx1Bootstrap::interruptGate(const Imx1I2c& state, const Imx1Rtc& clock,
                                        const Imx1Timer& first, const Imx1Timer& second) const {
    if (cpuInterruptsConnected) return connectedGate(gpio);
    auto next = aitc;
    setInterruptInputs(next, state, clock, first, second, asp, pwm, gpio);
    return interruptGate(next);
}
Imx1BootstrapAccess Imx1Bootstrap::interruptGate(const Imx1Aitc& controller) const {
    if (!cpuInterruptsConnected &&
        (controller.irq() == Imx1AitcLine::asserted || controller.fiq() == Imx1AitcLine::asserted))
        return Imx1BootstrapAccess::sideEffect;
    if (!controller.interruptInputsKnown()) return Imx1BootstrapAccess::externalInput;
    return Imx1BootstrapAccess::ok;
}
Imx1BootstrapAccess Imx1Bootstrap::updateGpioInput(unsigned port, uint32_t value, uint32_t known) {
    auto next = gpio;
    const auto status = next.updateInput(port, value, known);
    if (status == Imx1GpioAccess::unmapped) return Imx1BootstrapAccess::unmapped;
    if (status != Imx1GpioAccess::ok) return Imx1BootstrapAccess::sideEffect;
    auto controller = aitc;
    setInterruptInputs(controller, i2c, rtc, timer1, timer2, asp, pwm, next);
    const auto gate = interruptGate(controller);
    if (gate != Imx1BootstrapAccess::ok) return gate;
    gpio = next;
    runCache.driveSettled = false;
    return Imx1BootstrapAccess::ok;
}
Imx1BootstrapAccess Imx1Bootstrap::replaceAspPenInputs(const Imx1AspPenInputs& inputs, bool commit) {
    SourceChange change(*this);
    auto next = asp;
    if (next.replacePenInputs(inputs) != Imx1AspAccess::ok) return Imx1BootstrapAccess::externalInput;
    auto controller = aitc;
    setInterruptInputs(controller, i2c, rtc, timer1, timer2, next, pwm, gpio);
    const auto gate = interruptGate(controller);
    if (gate != Imx1BootstrapAccess::ok) return gate;
    if (commit) asp = next;
    return Imx1BootstrapAccess::ok;
}

bool Imx1Bootstrap::aspPenWakePossible(const Imx1AspPenInputs& inputs) const {
    auto next = asp;
    if (next.replacePenInputs(inputs) != Imx1AspAccess::ok) return false;
    auto controller = aitc;
    setInterruptInputs(controller, i2c, rtc, timer1, timer2, next, pwm, gpio);
    return controller.irq() == Imx1AitcLine::asserted || controller.fiq() == Imx1AitcLine::asserted;
}

Imx1BootstrapAccess Imx1Bootstrap::transitionGpioInputs(const uint32_t (&values)[4], const uint32_t (&masks)[4], bool commit) {
    auto next = gpio;
    for (unsigned port = 0; port < 4; ++port) {
        if (!masks[port]) {
            if (values[port]) return Imx1BootstrapAccess::sideEffect;
            continue;
        }
        if (next.transitionInput(port, values[port], masks[port]) != Imx1GpioAccess::ok)
            return Imx1BootstrapAccess::sideEffect;
    }
    auto controller = aitc;
    setInterruptInputs(controller, i2c, rtc, timer1, timer2, asp, pwm, next);
    const auto gate = interruptGate(controller);
    if (gate != Imx1BootstrapAccess::ok) return gate;
    if (commit) {
        gpio = next;
        runCache.driveSettled = false;
    }
    return Imx1BootstrapAccess::ok;
}
Imx1BootstrapAccess Imx1Bootstrap::driveGpioInput(unsigned port, uint32_t value, uint32_t known,
                                                  bool commit, bool baseline) {
    // The same levels again give the same pins, so only the gate is left to look at.
    const auto& c = runCache;
    if (!baseline && c.driveSettled && port == c.drivePort && value == c.driveValue && known == c.driveKnown) {
        if (cpuInterruptsConnected) return connectedGate(gpio);
        auto controller = aitc;
        setInterruptInputs(controller, i2c, rtc, timer1, timer2, asp, pwm, gpio);
        return interruptGate(controller);
    }
    auto next = gpio;
    const auto status = next.driveInput(port, value, known, baseline);
    if (status == Imx1GpioAccess::unmapped) return Imx1BootstrapAccess::unmapped;
    if (status != Imx1GpioAccess::ok) return Imx1BootstrapAccess::sideEffect;
    Imx1BootstrapAccess gate;
    if (cpuInterruptsConnected) {
        gate = connectedGate(next);
    } else {
        auto controller = aitc;
        setInterruptInputs(controller, i2c, rtc, timer1, timer2, asp, pwm, next);
        gate = interruptGate(controller);
    }
    if (gate != Imx1BootstrapAccess::ok) return gate;
    if (commit) {
        gpio = next;
        runCache.driveSettled = !baseline;
        runCache.otherValid = false;
        runCache.drivePort = uint8_t(port);
        runCache.driveValue = value;
        runCache.driveKnown = known;
    }
    return Imx1BootstrapAccess::ok;
}
bool Imx1Bootstrap::gpioDriveAccepted(unsigned port, uint32_t value, uint32_t known) const {
    const auto& c = runCache;
    if (!cpuInterruptsConnected || !c.driveSettled || port != c.drivePort || value != c.driveValue ||
        known != c.driveKnown) return false;
    return connectedGate(gpio) == Imx1BootstrapAccess::ok;
}
bool Imx1Bootstrap::gpioDriveBothAccepted(unsigned port, uint32_t known) const {
    auto& c = runCache;
    if (!cpuInterruptsConnected || !c.driveSettled || port != c.drivePort || known != c.driveKnown ||
        !known || (known & (known - 1)) || (c.driveValue & ~known)) return false;
    if (!c.otherValid) {
        auto next = gpio;
        bool accepted = next.driveInput(port, c.driveValue ^ known, known, false) == Imx1GpioAccess::ok;
        for (unsigned n = 0; accepted && n < 4; ++n)
            accepted = next.interruptOutput(n) != Imx1GpioLine::unknown || gpio.interruptOutput(n) == Imx1GpioLine::unknown;
        c.otherAccepted = accepted;
        c.otherValid = true;
    }
    if (!c.otherAccepted) return false;
    // The other sources' inputs are the same at both levels, so this gate covers both.
    return connectedGate(gpio) == Imx1BootstrapAccess::ok;
}
Imx1Bootstrap::InterruptLines Imx1Bootstrap::interruptLines() const {
    auto current = aitc;
    setInterruptInputs(current, i2c, rtc, timer1, timer2, asp, pwm, gpio);
    return {current.irq(), current.fiq()};
}

Imx1BootstrapAccess Imx1Bootstrap::clockStatus() const {
    if (referenceOverflow || ymfLineRefused) return Imx1BootstrapAccess::sideEffect;
    if (i2c.clockStatus() == Imx1I2cAccess::externalInput) return Imx1BootstrapAccess::externalInput;
    if (i2c.clockStatus() != Imx1I2cAccess::ok) return Imx1BootstrapAccess::sideEffect;
    if (watchdog.possibleTimeout()) return Imx1BootstrapAccess::possibleWatchdogTimeout;
    return interruptGate(i2c, rtc, timer1, timer2);
}

Imx1AitcPending Imx1Bootstrap::interruptPending() const {
    auto current = aitc;
    setInterruptInputs(current, i2c, rtc, timer1, timer2, asp, pwm, gpio);
    return current.pendingState();
}

void Imx1Bootstrap::setRamMiB(uint32_t size) {
    sdram[0].bytes = size == 32 ? 0x02000000 : size == 128 ? 0x04000000 : 0;
    sdram[1].bytes = size == 128 ? 0x04000000 : 0;
}

bool Imx1Bootstrap::instructionWord(uint32_t address, uint32_t& value) const {
    return instructionRead(address, 4, value);
}

bool Imx1Bootstrap::cacheMemoryRange(uint32_t address, uint32_t size, bool /*write*/) const {
    const uint64_t end = uint64_t(address) + size;
    if (!size || end > 0x100000000ull) return false;
    // The SRAM read extension is deliberately excluded from cache backing.
    if (address >= 0x00300000 && end <= 0x00320000) return true;
    if (address < 0x08000000 || end > 0x10000000) return false;
    const auto& s = sdram[(address - 0x08000000) >> 26];
    const uint32_t offset = address & 0x03ffffff;
    return s.bytes && (s.control & 0xf0000000) == 0x80000000 && s.modeSet &&
           size <= 0x04000000 - offset && offset < s.bytes && size <= s.bytes - offset;
}

bool Imx1Bootstrap::instructionRead(uint32_t address, uint32_t size, uint32_t& value) const {
    if ((size != 2 && size != 4) || (address & (size - 1))) return false;
    value = 0;
    if (address >= 0x00300000 && address < 0x00400000) {
        if (size != 4 && address >= 0x00320000) return false;
        const uint32_t offset = address < 0x00320000 ? address - 0x00300000 : 0x1fffc;
        std::memcpy(&value, sram.data() + offset, size);
        return true;
    }
    if (address >= 0x08000000 && address < 0x10000000) {
        const auto& s = sdram[(address - 0x08000000) >> 26];
        const uint32_t offset = address & 0x03ffffff;
        if (!s.bytes || (s.control & 0xf0000000) != 0x80000000 || !s.modeSet ||
            offset >= s.bytes || s.bytes - offset < size) return false;
        if (s.ram.empty()) value = 0;
        else std::memcpy(&value, s.ram.data() + offset, size);
        return true;
    }
    return false;
}

const char* Imx1Bootstrap::registerName(uint32_t address) {
    if (address >= 0x00209080 && address < 0x00209340 && !(address & 3)) {
        static constexpr const char* names[]{"DMA.SAR", "DMA.DAR", "DMA.CNTR", "DMA.CCR",
                                             "DMA.RSSR", "DMA.BLR", "DMA.RTOR_BUCR"};
        const auto word = ((address - 0x00209080) & 0x3f) / 4;
        if (word < 7) return names[word];
    }
    if (address >= 0x00300000 && address < 0x00400000) return "SRAM";
    if (const auto* name = Imx1Rtc::registerName(address)) return name;
    if (const auto* name = Imx1I2c::registerName(address)) return name;
    if (const auto* name = Imx1Asp::registerName(address)) return name;
    if (const auto* name = Imx1Reset::registerName(address)) return name;
    if (const auto* name = Imx1Pwm::registerName(address)) return name;
    if (const auto* name = Imx1Watchdog::registerName(address)) return name;
    if (const auto* name = Imx1Aitc::registerName(address)) return name;
    if (const auto* name = Imx1Cgm::registerName(address)) return name;
    if (const auto* name = Imx1Gpio::registerName(address)) return name;
    if (const auto* name = Imx1Eim::registerName(address)) return name;
    if (const auto* name = Imx1Timer::registerName(address)) return name;
    if (const auto* name = Imx1Uart::registerName(address)) return name;
    if (const auto* name = Imx1Sysctrl::registerName(address)) return name;
    if (const auto* name = Imx1Mmcsd::registerName(address)) return name;
    switch (address & ~3u) {
        case 0x00216110: return "BTA.WU_CONTROL";
        case 0x00216118: return "BTA.CLK_CONTROL";
        case 0x0020502c: return "LCDC.PWMR";
        case 0x00205034: return "LCDC.RMCR";
        case 0x00209000: return "DMA.DCR";
        case 0x00209004: return "DMA.DISR";
        case 0x00209008: return "DMA.DIMR";
        case 0x0020900c: return "DMA.DBTOSR";
        case 0x00209010: return "DMA.DRTOSR";
        case 0x00209014: return "DMA.DSESR";
        case 0x00209018: return "DMA.DBOSR";
        case 0x0020901c: return "DMA.DBTOCR";
        case 0x00218000: return "SSI1.STX";
        case 0x00218008: return "SSI1.SCSR";
        case 0x0021800c: return "SSI1.STCR";
        case 0x00218010: return "SSI1.SRCR";
        case 0x00218014: return "SSI1.STCCR";
        case 0x00218018: return "SSI1.SRCCR";
        case 0x00218020: return "SSI1.SFCSR";
        case 0x00218028: return "SSI1.SOR";
        case 0x00221000: return "SDRAMC.SDCTL0";
        case 0x00221004: return "SDRAMC.SDCTL1";
        default: return nullptr;
    }
}

Imx1BootstrapAccess Imx1Bootstrap::access(uint32_t address, uint32_t size, bool write, uint32_t& value, uint32_t cpuClockMode) {
    SourceChange change(*this);
    if (address >= 0x00218000 && address < 0x0021802c) {
        if (size != 4 || (address & 3)) return Imx1BootstrapAccess::width;
        if (address == 0x00218000) {
            // Readback of the write-side port is not qualified by this subset.
            if (!write) return Imx1BootstrapAccess::sideEffect;
            if (value & ~0xffffu) return Imx1BootstrapAccess::reservedBits;
            if (!(ssi1Words[0] & 0x100) || (ssi1Words[6] & 0x10))
                return Imx1BootstrapAccess::sequence;
            if (ssi1StatusRead) ssi1Underrun = ssi1StatusRead = false;
            const uint32_t capacity = (ssi1Words[1] & 0x80) ? 8 : 1;
            // MC9328MX1RM 30.3.1: writes to a full FIFO are ignored.
            if (ssi1TxCount < capacity) ssi1TxFifo[ssi1TxCount++] = uint16_t(value);
            return Imx1BootstrapAccess::ok;
        }
        constexpr uint32_t offsets[] = {8, 0xc, 0x10, 0x14, 0x18, 0x20, 0x28};
        unsigned index = 0;
        while (index < 7 && offsets[index] != address - 0x00218000) ++index;
        if (index == 7) return Imx1BootstrapAccess::unmapped;
        if (write) {
            const uint32_t masks[] = {0xffff, 0x7ff, 0x7ff, 0xffff, 0xffff, 0xffff, 0x71};
            if (value & ~masks[index]) return Imx1BootstrapAccess::reservedBits;
            uint32_t next = value;
            if (index == 0) {
                if ((value & 0x400) || (value & 0x6000) == 0x6000) return Imx1BootstrapAccess::sideEffect;
                next &= 0xff00;
                if ((ssi1Words[0] & 0x100) && ssi1Shifting && (next & 0x100) &&
                    ((next ^ ssi1Words[0]) & ~0x200u)) return Imx1BootstrapAccess::sideEffect;
                if (value & 0x200) {
                    if ((ssi1Words[1] & 0x60) != 0x60) return Imx1BootstrapAccess::externalInput;
                    if ((value & 0x9900) != 0x9900 || (value & 0x6000) ||
                        (ssi1Words[1] & ~0x200u) != 0xe8 || ssi1Words[3] != 0x6100 ||
                        (ssi1Words[6] & 0x10))
                        return Imx1BootstrapAccess::sideEffect;
                }
            } else if (index == 1 || index == 2) {
                if (value & (index == 1 ? 0x100 : 0x300)) return Imx1BootstrapAccess::sideEffect;
                if (index == 1 && (value & 0x200) && (value & ~0x200u) != 0xe8)
                    return Imx1BootstrapAccess::sideEffect;
                if (index == 1 && (value & 0x200) && (ssi1Words[5] & 15) < 4)
                    return Imx1BootstrapAccess::sideEffect;
                if (index == 1 && (ssi1Shifting || (ssi1Words[0] & 0x200)) &&
                    ((value ^ ssi1Words[1]) & ~0x200u)) return Imx1BootstrapAccess::sideEffect;
                if (index == 1 && ssi1TxCount && ((value ^ ssi1Words[1]) & 0x80))
                    return Imx1BootstrapAccess::sideEffect;
                if (index == 2 && (value & 0x80) && !(ssi1Words[5] & 0xf0))
                    return Imx1BootstrapAccess::sideEffect;
            } else if (index == 5) {
                const uint32_t tx = value & 15, rx = (value >> 4) & 15;
                if (!tx || tx > 8 || rx > 8) return Imx1BootstrapAccess::sideEffect;
                // The native zero RX watermark is retained only while its FIFO is disabled.
                if (!rx && (ssi1Words[2] & 0x80)) return Imx1BootstrapAccess::sideEffect;
                next &= 0xff;
            }
            if (index == 3 && (ssi1Shifting || (ssi1Words[0] & 0x200)) && value != ssi1Words[3])
                return Imx1BootstrapAccess::sideEffect;
            if (index == 6 && (value & 0x10) && (ssi1Shifting || (ssi1Words[0] & 0x200)))
                return Imx1BootstrapAccess::sideEffect;
            if (index == 5 && (ssi1Words[1] & 0x200) && (value & 15) < 4)
                return Imx1BootstrapAccess::sideEffect;
            if ((index == 0 && !(next & 0x100)) || (index == 6 && (next & 0x10))) {
                std::fill(std::begin(ssi1TxFifo), std::end(ssi1TxFifo), 0);
                ssi1TxCount = 0;
                if (index == 0) {
                    ssi1Shifting = false;
                    ssi1Phase = ssi1Slot = 0;
                    ssi1Underrun = ssi1StatusRead = false;
                }
            }
            ssi1Words[index] = next;
        } else {
            value = ssi1Words[index];
            if (index == 0) {
                ssi1StatusRead = true;
                if (ssi1Underrun) value |= 0x10;
                if ((ssi1Words[0] & 0x200) && !ssi1Slot) value |= 8;
                if (!ssi1TxCount) value |= 0x40;
                if (!(value & 0x100) || 8 - ssi1TxCount >= (ssi1Words[5] & 15)) value |= 1;
            }
            if (index == 5) value |= ssi1TxCount << 8;
        }
        return Imx1BootstrapAccess::ok;
    }
    if ((address & ~3u) == 0x0020502c) {
        if (!lcdcContrastOnly) return Imx1BootstrapAccess::unmapped;
        if (size != 4 || (address & 3)) return Imx1BootstrapAccess::width;
        if (write) {
            if (value & ~0x01ff87ffu) return Imx1BootstrapAccess::reservedBits;
            if ((value & 0x600) == 0x600) return Imx1BootstrapAccess::reservedBits;
            // Only the LCD-clock contrast source is independent of unmodelled panel timing.
            if ((value & 0x100) && (value & 0x600) != 0x400) return Imx1BootstrapAccess::sideEffect;
            // With the LCDC on only CC_EN may go off, the way the ROM's sound power-down does it
            // (2009f978). The manual puts no condition on the bit, and the counter stops with it.
            if ((rmcr & 2) && value != pwmr && value != (pwmr & ~0x100u)) return Imx1BootstrapAccess::sequence;
            pwmr = value;
        } else value = pwmr;
        return Imx1BootstrapAccess::ok;
    }
    if (address >= 0x00209000 && address < 0x00209020) {
        if (size != 4 || (address & 3)) return Imx1BootstrapAccess::width;
        const auto offset = address - 0x00209000;
        if (offset == 0) {
            if (write) {
                if (value & ~3u) return Imx1BootstrapAccess::reservedBits;
                if (value == 3) return Imx1BootstrapAccess::sideEffect;
                if (value & 2) {
                    // DRST clears owned global state and holds reset for three HCLKs.
                    dmaControl = 0; dmaMask = 0x7ff; dmaBurstTimeout = 0;
                    dmaResetClocks = 3;
                    dmaPcmActive = false; dmaStatus = dmaPcmCount = dmaPcmBurst = dmaPcmPhase = 0;
                    for (auto& channel : dmaChannels) for (auto& word : channel) word = 0;
                    memoryDma = {};
                } else dmaControl = value;
            } else value = dmaControl;
            return Imx1BootstrapAccess::ok;
        }
        if (write) {
            const uint32_t mask = offset == 0x1c ? 0xffff : 0x7ff;
            if (value & ~mask) return Imx1BootstrapAccess::reservedBits;
            if (offset == 8) dmaMask = value;
            else if (offset == 0x1c) dmaBurstTimeout = value;
            // Channel 10's bit only clears while it is enabled; the card channels' always do.
            else if (offset == 4) dmaStatus &= ~(value & ((dmaChannels[10][3] & 1) ? 0x7ffu : 0x3ffu));
        } else value = offset == 8 ? dmaMask : offset == 0x1c ? dmaBurstTimeout : offset == 4 ? dmaStatus : 0;
        return Imx1BootstrapAccess::ok;
    }
    if (address >= 0x00209080 && address < 0x00209340) {
        if (size != 4 || (address & 3)) return Imx1BootstrapAccess::width;
        const auto offset = address - 0x00209080;
        const auto word = (offset & 0x3f) / 4;
        if (word >= 7) return Imx1BootstrapAccess::unmapped;
        auto& channel = dmaChannels[offset / 0x40];
        uint32_t memoryClocks = 0;
        if (write) {
            static constexpr uint32_t masks[]{0xffffffff,0xffffffff,0xffffff,0x3fff,0x1f,0x3f,0xffff};
            if (value & ~masks[word]) return Imx1BootstrapAccess::reservedBits;
            if (word == 3) {
                if (value & 2) return Imx1BootstrapAccess::sideEffect;
                if (value & 1) {
                    auto next = std::vector<uint32_t>(channel, channel + 7);
                    next[3] = value;
                    const unsigned number = offset / 0x40;
                    if (dmaResetClocks || !(dmaControl & 1)) return Imx1BootstrapAccess::sideEffect;
                    // A memory transfer under way can't start again, and neither kind can start over the other.
                    if (number != 10 && ((memoryDma.active & (1u << number)) ||
                                         (!(value & 8) && (cardDma.active & (1u << number)))))
                        return Imx1BootstrapAccess::sideEffect;
                    if (number != 10 && !(value & 8)) {
                        const auto planned = memoryDmaPlan(number, next.data(), memoryClocks);
                        if (planned != Imx1BootstrapAccess::ok) return planned;
                    } else if (number == 10 ? !pcmDmaConfigurationValid(next.data()) : !cardDmaConfigurationValid(number, next.data()))
                        return number != 10 && !mmcsd.card() ? Imx1BootstrapAccess::sideEffect :
                               number != 10 && next[4] == 13 ? Imx1BootstrapAccess::memoryUnavailable :
                               Imx1BootstrapAccess::sideEffect;
                }
                const auto source = (value >> 10) & 3, destination = (value >> 12) & 3;
                if (((value >> 4) & 3) == 3 || ((value >> 6) & 3) == 3 ||
                    destination == 3 || source == 3)
                    return Imx1BootstrapAccess::sideEffect;
            }
            if (offset / 0x40 == 10 && (channel[3] & 1)) {
                if (word == 0 && !pcmDmaSourceValid(value & ~3u, channel[2]))
                    return Imx1BootstrapAccess::memoryUnavailable;
                if (word != 0 && word != 3 && !(word == 4 && value == 0) && value != channel[word])
                    return Imx1BootstrapAccess::sideEffect;
            }
            if (word == 3 && offset / 0x40 == 10) {
                if (!(value & 1)) { dmaPcmActive = false; dmaPcmBurst = dmaPcmPhase = 0; }
                else if (!(channel[3] & 1)) {
                    dmaPcmActive = true; dmaPcmCount = dmaPcmBurst = dmaPcmPhase = 0;
                    dmaPcmSource = channel[0];
                }
            }
            const unsigned number = offset / 0x40;
            if (number != 10 && ((cardDma.active | memoryDma.active) & (1u << number)) && word != 3)
                return Imx1BootstrapAccess::sideEffect;
            // The selected probe is little endian; both DMA address registers are word aligned.
            channel[word] = word < 2 ? value & ~3u : value;
            if (number != 10 && word == 3 && memoryClocks) {
                // Started with the plan checked above.
                memoryDmaMove(number);
                memoryDma.active |= 1u << number;
                memoryDma.remaining[number] = memoryClocks;
            } else if (number != 10 && word == 3) {
                // Clearing CEN abandons a memory-to-memory transfer: its data has moved, its DISR bit never sets.
                if (!(value & 1)) {
                    cardDma.active &= ~(1u << number);
                    memoryDma.active &= ~(1u << number);
                } else if (!(cardDma.active & (1u << number))) {
                    cardDma.active |= 1u << number;
                    cardDma.moved[number] = 0;
                    cardDmaService();
                }
            }
        } else value = channel[word];
        return Imx1BootstrapAccess::ok;
    }
    if (Imx1Uart::registerName(address)) {
        auto& uart = address < 0x00207000 ? uart1 : uart2;
        const auto before = uart;
        uint32_t preview = value;
        switch (uart.access(address & 0xfff, size, write, preview)) {
            case Imx1UartAccess::ok: break;
            case Imx1UartAccess::unmapped: return Imx1BootstrapAccess::unmapped;
            case Imx1UartAccess::width: return Imx1BootstrapAccess::width;
            case Imx1UartAccess::reservedBits: return Imx1BootstrapAccess::reservedBits;
            case Imx1UartAccess::sideEffect: return Imx1BootstrapAccess::sideEffect;
            case Imx1UartAccess::externalInput: return Imx1BootstrapAccess::externalInput;
        }
        // A read of URXD changes the flags too.
        auto controller = aitc;
        setInterruptInputs(controller, i2c, rtc, timer1, timer2, asp, pwm, gpio);
        const auto gate = interruptGate(controller);
        if (gate != Imx1BootstrapAccess::ok) { uart = before; return gate; }
        value = preview;
        return Imx1BootstrapAccess::ok;
    }
    // MC9328MX1RM 3.1.2: 128 KiB SRAM, then word reads repeat the last word.
    if (address >= 0x00300000 && address < 0x00400000) {
        const uint32_t offset = address - 0x00300000;
        if (size != 1 && size != 2 && size != 4) return Imx1BootstrapAccess::width;
        if (offset >= sram.size()) {
            if (write) return Imx1BootstrapAccess::sideEffect;
            if (size != 4 || (address & 3)) return Imx1BootstrapAccess::width;
            std::memcpy(&value, sram.data() + sram.size() - 4, 4);
        } else {
            if (size > sram.size() - offset) return Imx1BootstrapAccess::width;
            if (write) std::memcpy(sram.data() + offset, &value, size);
            else { value = 0; std::memcpy(&value, sram.data() + offset, size); }
        }
        return Imx1BootstrapAccess::ok;
    }
    // Table 3-1 fixes each CSD window at 64 MiB. Installed capacity is an explicit input.
    if (address >= 0x08000000 && address < 0x10000000)
        return sdramAccess((address - 0x08000000) >> 26, address & 0x03ffffff, size, write, value);
    if (Imx1Aitc::registerName(address)) {
        auto next = aitc;
        setInterruptInputs(next, i2c, rtc, timer1, timer2, asp, pwm, gpio);
        const auto status = next.access(address, size, write, value);
        switch (status) {
            case Imx1AitcAccess::unmapped: return Imx1BootstrapAccess::unmapped;
            case Imx1AitcAccess::width: return Imx1BootstrapAccess::width;
            case Imx1AitcAccess::reservedBits: return Imx1BootstrapAccess::reservedBits;
            case Imx1AitcAccess::externalInput: return Imx1BootstrapAccess::externalInput;
            case Imx1AitcAccess::ok: break;
        }
        if (write) {
            const auto gate = interruptGate(next);
            if (gate != Imx1BootstrapAccess::ok) return gate;
            aitc = next;
        }
        return Imx1BootstrapAccess::ok;
    }
    if (Imx1Cgm::registerName(address)) {
        // Later programming requires the explicit frequency-preserving MCU policy.
        if (write && asynchronousUsed && !mcuRetuneScenario &&
            (address == 0x0021b004 || (address == 0x0021b000 && (value & 0x00200000))))
            return Imx1BootstrapAccess::sideEffect;
        // Holding System PLL output high requires the independent asynchronous MCU clock.
        if (write && address == 0x0021b000 && (value & 0x00400000) && cpuClockMode != 3)
            return Imx1BootstrapAccess::sideEffect;
        // MPEN alone is only cleared by a CPU on HCLK. With SPEN cleared too it is stop mode, which an
        // asynchronous CPU enters at once, as its FCLK goes with the MCU PLL (table 12-3).
        if (write && address == 0x0021b000 && (!(value & 1) || bool(value & 1) != cgm.mcuEnabled()) &&
            ((cpuClockMode != 0 && (value & 2)) || !cgm.mcuPowerQualified())) return Imx1BootstrapAccess::sideEffect;
        // The shutdown count is not restarted or called off by a write (table 12-5 has an interrupt only).
        if (write && systemStop != stopRunning) return Imx1BootstrapAccess::sideEffect;
        auto next = cgm;
        const auto result = next.access(address, size, write, value);
        if (result == Imx1CgmAccess::ok && write && !next.systemPllEnabled()) {
            // HCLK stops under whatever is still running on it. The ROM's sleep path turns these off first.
            if (timer1.enabled() || timer2.enabled() || uart1.busy() || uart2.busy() || i2c.active() ||
                asp.converting() || pwm.resetting() || mmcsd.clockRunning() || dmaPcmBurst ||
                (dmaPcmActive && (ssi1Words[0] & 0x100)) || ssi1Shifting)
                return Imx1BootstrapAccess::sideEffect;
            systemStop = stopArmed;
            systemStopHclk = 16;
            systemStopTick = elapsedReference.ticks + next.systemShutdownEdges();
            cgm = next;
            return Imx1BootstrapAccess::ok;
        }
        if (result == Imx1CgmAccess::ok && write) {
            const auto referenceDenominator = cpuClockMode == 3 ?
                next.mcuPllTargetRatio().numerator : next.systemPllActiveRatio().numerator;
            if (!referenceRatioSupported(referenceDenominator) ||
                !next.mcuReferenceSupported(next.mcuPllTargetRatio().numerator) ||
                !next.mcuReferenceSupported(next.systemPllActiveRatio().numerator))
                return Imx1BootstrapAccess::sideEffect;
            if (cpuClockMode == 3 &&
                (!asynchronousRatioSupported(next, next.systemPllActiveRatio()) ||
                 !asynchronousRatioSupported(next, next.systemPllTargetRatio())))
                return Imx1BootstrapAccess::sideEffect;
            const bool busChanged = next.busDivider() != cgm.busDivider();
            // DMA beats use HCLK; the independent SSI serial clock uses PERCLK3. Channel 10 only
            // takes beats on SSI1's request, so with SSI_EN clear it can stay set up (ROM 20084608).
            // With TDMAE clear (STCR bit 9, 30.3.8) SSI1 asks for no new burst, which is how the display off
            // code leaves it before it slows HCLK (ROM 200cefd0, then 200845c4). A burst under way still blocks.
            const bool pcmRequests = (ssi1Words[0] & 0x100) && (ssi1Words[1] & 0x200);
            if (busChanged && (ssi1Words[0] & 0x100) && ((dmaPcmActive && pcmRequests) || dmaPcmBurst))
                return Imx1BootstrapAccess::sideEffect;
            // Only the SSI1 bit clock uses PERCLK3. With SSI_EN clear nothing shifts and no DMA beat
            // is taken, so the divider can change with channel 10 still set up (ROM 2009f9f0).
            if (next.perclk3Divider() != cgm.perclk3Divider() && (ssi1Shifting || (ssi1Words[0] & 0x100)))
                return Imx1BootstrapAccess::sideEffect;
            const bool perclk2Changed = next.perclk2Divider() != cgm.perclk2Divider();
            if (perclk2Changed && mmcsd.clockRunning())
                return Imx1BootstrapAccess::sideEffect;
            const bool peripheralChanged = next.perclk1Divider() != cgm.perclk1Divider();
            if (cpuClockMode == 3 && peripheralChanged) return Imx1BootstrapAccess::sideEffect;
            if (peripheralChanged && (timer1.enabled() || timer2.enabled()))
                return Imx1BootstrapAccess::sideEffect;
            // The ASP runs on PERCLK2 (table 12-6), so only that divider matters to a conversion. The ROM's
            // idle code drops HCLK to /16 with the pen down and a conversion under way (20084608).
            if (perclk2Changed && asp.converting()) return Imx1BootstrapAccess::sideEffect;
            if (peripheralChanged && pwm.usesPerclk1())
                return Imx1BootstrapAccess::sideEffect;
            // A character, a software reset or an idle count under way is timed in PERCLK1.
            if (peripheralChanged && (uart1.busy() || uart2.busy())) return Imx1BootstrapAccess::sideEffect;
            if (busChanged && pwm.resetting()) return Imx1BootstrapAccess::sideEffect;
            if (busChanged && i2c.active()) return Imx1BootstrapAccess::sideEffect;
            // Idle divider changes start a functional phase at the write boundary.
            // CPU mode, PRESC and BCLK do not rephase the independent peripheral clocks.
            if (busChanged) bclkPhase = 0;
            if (peripheralChanged) perclk1Phase = 0;
            if (perclk2Changed) perclk2Phase = 0;
            cgm = next;
        }
        switch (result) {
            case Imx1CgmAccess::ok: return Imx1BootstrapAccess::ok;
            case Imx1CgmAccess::unmapped: return Imx1BootstrapAccess::unmapped;
            case Imx1CgmAccess::width: return Imx1BootstrapAccess::width;
            case Imx1CgmAccess::reservedBits: return Imx1BootstrapAccess::reservedBits;
            case Imx1CgmAccess::sideEffect: return Imx1BootstrapAccess::sideEffect;
            case Imx1CgmAccess::externalInput: return Imx1BootstrapAccess::externalInput;
        }
    }
    if (Imx1Mmcsd::registerName(address)) {
        switch (mmcsd.access(address, size, write, value)) {
            case Imx1MmcsdAccess::ok: return Imx1BootstrapAccess::ok;
            case Imx1MmcsdAccess::unmapped: return Imx1BootstrapAccess::unmapped;
            case Imx1MmcsdAccess::width: return Imx1BootstrapAccess::width;
            case Imx1MmcsdAccess::reservedBits: return Imx1BootstrapAccess::reservedBits;
            case Imx1MmcsdAccess::sideEffect: return Imx1BootstrapAccess::sideEffect;
            case Imx1MmcsdAccess::externalInput: return Imx1BootstrapAccess::externalInput;
            case Imx1MmcsdAccess::sequence: return Imx1BootstrapAccess::sequence;
        }
    }
    if (Imx1Rtc::registerName(address)) {
        auto next = rtc;
        uint32_t preview = value;
        const auto result = next.access(address, size, write, preview);
        switch (result) {
            case Imx1RtcAccess::unmapped: return Imx1BootstrapAccess::unmapped;
            case Imx1RtcAccess::width: return Imx1BootstrapAccess::width;
            case Imx1RtcAccess::reservedBits: return Imx1BootstrapAccess::reservedBits;
            case Imx1RtcAccess::sideEffect: return Imx1BootstrapAccess::sideEffect;
            case Imx1RtcAccess::externalInput: return Imx1BootstrapAccess::externalInput;
            case Imx1RtcAccess::ok: break;
        }
        if (write) {
            if (watchdog.enabled() && !rtc.preservesWatchdogClock(next, address))
                return Imx1BootstrapAccess::sideEffect;
            const auto gate = interruptGate(i2c, next, timer1, timer2);
            if (gate != Imx1BootstrapAccess::ok) return gate;
        }
        rtc = next;
        value = preview;
        return Imx1BootstrapAccess::ok;
    }
    if (Imx1I2c::registerName(address)) {
        // Validate without emitting bus events, then perform the admitted access once.
        auto next = i2c;
        next.setObserver(nullptr, nullptr);
        next.setBus(nullptr);
        uint32_t preview = value;
        auto result = next.access(address, size, write, preview);
        if (result == Imx1I2cAccess::ok) {
            if (!i2c.active() && next.active() && !gpio.i2cPinsSelected())
                return Imx1BootstrapAccess::sideEffect;
            const auto gate = interruptGate(next, rtc, timer1, timer2);
            if (gate != Imx1BootstrapAccess::ok) return gate;
            result = i2c.access(address, size, write, value);
        }
        switch (result) {
            case Imx1I2cAccess::ok: return Imx1BootstrapAccess::ok;
            case Imx1I2cAccess::unmapped: return Imx1BootstrapAccess::unmapped;
            case Imx1I2cAccess::width: return Imx1BootstrapAccess::width;
            case Imx1I2cAccess::reservedBits: return Imx1BootstrapAccess::reservedBits;
            case Imx1I2cAccess::sideEffect: return Imx1BootstrapAccess::sideEffect;
            case Imx1I2cAccess::externalInput: return Imx1BootstrapAccess::externalInput;
        }
    }
    if (Imx1Watchdog::registerName(address)) {
        auto next = watchdog;
        uint32_t preview = value;
        const auto result = next.access(address, size, write, preview);
        if (result == Imx1WatchdogAccess::ok) {
            if (next.enabled() && !rtc.watchdogClockBoundSupported()) return Imx1BootstrapAccess::sideEffect;
            watchdog = next;
            value = preview;
        }
        switch (result) {
            case Imx1WatchdogAccess::ok: return Imx1BootstrapAccess::ok;
            case Imx1WatchdogAccess::unmapped: return Imx1BootstrapAccess::unmapped;
            case Imx1WatchdogAccess::width: return Imx1BootstrapAccess::width;
            case Imx1WatchdogAccess::reservedBits: return Imx1BootstrapAccess::reservedBits;
            case Imx1WatchdogAccess::sideEffect: return Imx1BootstrapAccess::sideEffect;
            case Imx1WatchdogAccess::resetRequest: return Imx1BootstrapAccess::resetRequest;
            case Imx1WatchdogAccess::possibleTimeout: return Imx1BootstrapAccess::possibleWatchdogTimeout;
        }
    }
    if (Imx1Pwm::registerName(address)) {
        auto next = pwm;
        uint32_t preview = value;
        const auto result = next.access(address, size, write, preview);
        if (result == Imx1PwmAccess::ok) {
            auto controller = aitc;
            setInterruptInputs(controller, i2c, rtc, timer1, timer2, asp, next, gpio);
            const auto gate = interruptGate(controller);
            if (gate != Imx1BootstrapAccess::ok) return gate;
            pwm = next;
            value = preview;
        }
        switch (result) {
            case Imx1PwmAccess::ok: return Imx1BootstrapAccess::ok;
            case Imx1PwmAccess::unmapped: return Imx1BootstrapAccess::unmapped;
            case Imx1PwmAccess::width: return Imx1BootstrapAccess::width;
            case Imx1PwmAccess::sideEffect: return Imx1BootstrapAccess::sideEffect;
            case Imx1PwmAccess::externalInput: return Imx1BootstrapAccess::externalInput;
        }
    }
    if (Imx1Asp::registerName(address)) {
        auto next = asp;
        uint32_t preview = value;
        const auto status = next.access(address, size, write, preview);
        if (status == Imx1AspAccess::ok) {
            auto controller = aitc;
            setInterruptInputs(controller, i2c, rtc, timer1, timer2, next, pwm, gpio);
            const auto gate = interruptGate(controller);
            if (gate != Imx1BootstrapAccess::ok) return gate;
            asp = next;
            value = preview;
        }
        switch (status) {
            case Imx1AspAccess::ok: return Imx1BootstrapAccess::ok;
            case Imx1AspAccess::unmapped: return Imx1BootstrapAccess::unmapped;
            case Imx1AspAccess::width: return Imx1BootstrapAccess::width;
            case Imx1AspAccess::reservedBits: return Imx1BootstrapAccess::reservedBits;
            case Imx1AspAccess::sideEffect: return Imx1BootstrapAccess::sideEffect;
            case Imx1AspAccess::externalInput: return Imx1BootstrapAccess::externalInput;
        }
    }
    if (Imx1Reset::registerName(address)) {
        switch (reset.access(address, size, write, value)) {
            case Imx1ResetAccess::ok: return Imx1BootstrapAccess::ok;
            case Imx1ResetAccess::width: return Imx1BootstrapAccess::width;
            case Imx1ResetAccess::readOnly: return Imx1BootstrapAccess::sideEffect;
        }
    }
    if (Imx1Bta::contains(address)) {
        switch (bta.access(address, size, write, value, sysctrl.btaClockEnabled())) {
            case Imx1BtaAccess::ok: return Imx1BootstrapAccess::ok;
            case Imx1BtaAccess::unmapped: return Imx1BootstrapAccess::unmapped;
            case Imx1BtaAccess::width: return Imx1BootstrapAccess::width;
            case Imx1BtaAccess::reservedBits: return Imx1BootstrapAccess::reservedBits;
            case Imx1BtaAccess::sideEffect: return Imx1BootstrapAccess::sideEffect;
        }
    }
    if (Imx1Sysctrl::registerName(address)) {
        switch (sysctrl.access(address, size, write, value)) {
            case Imx1SysctrlAccess::ok: return Imx1BootstrapAccess::ok;
            case Imx1SysctrlAccess::unmapped: return Imx1BootstrapAccess::unmapped;
            case Imx1SysctrlAccess::width: return Imx1BootstrapAccess::width;
            case Imx1SysctrlAccess::reservedBits: return Imx1BootstrapAccess::reservedBits;
            case Imx1SysctrlAccess::sideEffect: return Imx1BootstrapAccess::sideEffect;
            case Imx1SysctrlAccess::externalInput: return Imx1BootstrapAccess::externalInput;
        }
    }
    if (Imx1Gpio::registerName(address)) {
        auto next = gpio;
        uint32_t preview = value;
        const auto status = next.access(address, size, write, preview);
        if (status == Imx1GpioAccess::ok) {
            if (i2c.active() && !next.i2cPinsSelected()) return Imx1BootstrapAccess::sideEffect;
            if (timer2.outputIsolationRequired() && !next.timer2OutputIsolated())
                return Imx1BootstrapAccess::sideEffect;
            auto controller = aitc;
            setInterruptInputs(controller, i2c, rtc, timer1, timer2, asp, pwm, next);
            const auto gate = interruptGate(controller);
            if (gate != Imx1BootstrapAccess::ok) return gate;
            gpio = next;
            // A read only marks the port accessed, which a drive doesn't look at.
            if (write) runCache.driveSettled = false;
            value = preview;
        }
        switch (status) {
            case Imx1GpioAccess::ok: return Imx1BootstrapAccess::ok;
            case Imx1GpioAccess::unmapped: return Imx1BootstrapAccess::unmapped;
            case Imx1GpioAccess::width: return Imx1BootstrapAccess::width;
            case Imx1GpioAccess::reservedBits: return Imx1BootstrapAccess::reservedBits;
            case Imx1GpioAccess::sideEffect: return Imx1BootstrapAccess::sideEffect;
            case Imx1GpioAccess::externalInput: return Imx1BootstrapAccess::externalInput;
        }
    }
    if (Imx1Timer::registerName(address)) {
        auto& timer = address < 0x00203000 ? timer1 : timer2;
        auto next = timer;
        uint32_t preview = value;
        const auto result = next.access(address & 0xfff, size, write, preview,
                                        address >= 0x00203000 && gpio.timer2OutputIsolated());
        switch (result) {
            case Imx1TimerAccess::ok: break;
            case Imx1TimerAccess::unmapped: return Imx1BootstrapAccess::unmapped;
            case Imx1TimerAccess::width: return Imx1BootstrapAccess::width;
            case Imx1TimerAccess::reservedBits: return Imx1BootstrapAccess::reservedBits;
            case Imx1TimerAccess::sideEffect: return Imx1BootstrapAccess::sideEffect;
            case Imx1TimerAccess::externalInput: return Imx1BootstrapAccess::externalInput;
        }
        if (write) {
            const auto gate = address < 0x00203000 ? interruptGate(i2c, rtc, next, timer2) :
                                                   interruptGate(i2c, rtc, timer1, next);
            if (gate != Imx1BootstrapAccess::ok) return gate;
        }
        timer = next;
        value = preview;
        return Imx1BootstrapAccess::ok;
    }
    if (Imx1Eim::registerName(address)) {
        switch (eim.access(address, size, write, value)) {
            case Imx1EimAccess::ok: return Imx1BootstrapAccess::ok;
            case Imx1EimAccess::unmapped: return Imx1BootstrapAccess::unmapped;
            case Imx1EimAccess::width: return Imx1BootstrapAccess::width;
            case Imx1EimAccess::reservedBits: return Imx1BootstrapAccess::reservedBits;
            case Imx1EimAccess::sideEffect: return Imx1BootstrapAccess::sideEffect;
            case Imx1EimAccess::externalInput: return Imx1BootstrapAccess::externalInput;
        }
    }
    if (!registerName(address)) return Imx1BootstrapAccess::unmapped;
    if (size != 4 || (address & 3)) return Imx1BootstrapAccess::width;
    uint32_t* reg = nullptr;
    uint32_t mask = 0xffffffff;
    switch (address) {
        // Contrast-only mode retains enable state without attaching an internal panel or DMA client.
        case 0x00205034:
            reg = &rmcr; mask = 3;
            if (write && (value & mask)) {
                if (!lcdcContrastOnly || (value & 1)) return Imx1BootstrapAccess::sideEffect;
                if ((pwmr & 0x700) != 0x500) return Imx1BootstrapAccess::sequence;
            }
            break;
        case 0x00221000: return sdramControl(0, write, value);
        case 0x00221004: return sdramControl(1, write, value);
    }
    if (write) {
        if (value & ~mask) return Imx1BootstrapAccess::reservedBits;
        *reg = value;
    } else value = *reg;
    return Imx1BootstrapAccess::ok;
}

Imx1BootstrapAccess Imx1Bootstrap::sdramControl(unsigned bank, bool write, uint32_t& value) {
    auto& s = sdram[bank];
    if (!write) {
        if (bank == 1 && !s.controlKnown) return Imx1BootstrapAccess::externalInput;
        value = s.control;
        return Imx1BootstrapAccess::ok;
    }
    if (dmaPcmActive && value != s.control) return Imx1BootstrapAccess::sideEffect;
    // Table 24-6. The measured sequence uses 32-bit, non-interleaved, CAS-2 SDRAM.
    constexpr uint32_t mask = 0xfb3bf377;
    if (value & ~mask) return Imx1BootstrapAccess::reservedBits;
    const uint32_t mode = (value >> 28) & 7;
    const uint32_t geometry = value & 0x03ffffff;
    const uint32_t base = geometry & ~0x0000f000u;
    if (!(value & 0x80000000) || (value & 0x08000000) || mode > 3 ||
        (base != 0x01120200 && base != 0x02120200)) return Imx1BootstrapAccess::sideEffect;
    if (!s.bytes) return Imx1BootstrapAccess::memoryUnavailable;
    if (((value >> 24) & 3) != (s.bytes == 0x02000000 ? 1u : 2u))
        return Imx1BootstrapAccess::sideEffect;
    const uint32_t normal = base | (s.bytes == 0x02000000 ? 0x9000 : 0xd000);
    if ((mode && geometry != base) || (!mode && geometry != normal))
        return Imx1BootstrapAccess::sideEffect;
    if (mode == 0 && (!s.precharged || s.refreshes < 8 || !s.modeSet))
        return Imx1BootstrapAccess::sequence;
    if (mode == 1) { s.precharged = false; s.refreshes = 0; s.modeSet = false; }
    if (mode == 2 && !s.precharged) return Imx1BootstrapAccess::sequence;
    if (mode == 3 && (s.refreshes < 8 || !s.precharged)) return Imx1BootstrapAccess::sequence;
    s.control = value;
    s.controlKnown = true;
    return Imx1BootstrapAccess::ok;
}

Imx1BootstrapAccess Imx1Bootstrap::sdramAccess(unsigned bank, uint32_t offset, uint32_t size,
                                             bool write, uint32_t& value) {
    auto& s = sdram[bank];
    if (!s.bytes) return Imx1BootstrapAccess::memoryUnavailable;
    if (!(s.control & 0x80000000)) return Imx1BootstrapAccess::sequence;
    const uint32_t mode = (s.control >> 28) & 7;
    if (mode) {
        if (size != 4 || (offset & 3)) return Imx1BootstrapAccess::width;
        // Sections 24.5.3/4/5 and 24.7.3: these accesses issue commands, without RAM data.
        if (mode == 1 && offset == 0x00200000) s.precharged = true;
        else if (mode == 2 && offset == 0 && s.precharged) ++s.refreshes;
        else if (mode == 3 && offset == 0x00111800 && s.refreshes >= 8) s.modeSet = true;
        else return Imx1BootstrapAccess::sequence;
        if (!write) value = 0;  // Command-read data is unspecified, diagnostic value only.
        return Imx1BootstrapAccess::command;
    }
    if (!s.modeSet) return Imx1BootstrapAccess::sequence;
    if ((size != 1 && size != 2 && size != 4) || (offset & (size - 1))) return Imx1BootstrapAccess::width;
    if (offset >= s.bytes || size > s.bytes - offset) return Imx1BootstrapAccess::memoryUnavailable;
    if (s.ram.empty()) s.ram.resize(s.bytes);  // Diagnostic power-on contents are zero.
    if (write) std::memcpy(s.ram.data() + offset, &value, size);
    else { value = 0; std::memcpy(&value, s.ram.data() + offset, size); }
    return Imx1BootstrapAccess::ok;
}

const char* imx1BootstrapAccessName(Imx1BootstrapAccess access) {
    switch (access) {
        case Imx1BootstrapAccess::ok: return "ok";
        case Imx1BootstrapAccess::busTimeout: return "external-bus-timeout";
        case Imx1BootstrapAccess::command: return "sdram-command-no-data";
        case Imx1BootstrapAccess::unmapped: return "unmapped-register";
        case Imx1BootstrapAccess::width: return "unsupported-width";
        case Imx1BootstrapAccess::reservedBits: return "reserved-bits";
        case Imx1BootstrapAccess::sideEffect: return "unsupported-side-effect";
        case Imx1BootstrapAccess::resetRequest: return "unsupported-reset-request";
        case Imx1BootstrapAccess::possibleWatchdogTimeout: return "possible-watchdog-timeout";
        case Imx1BootstrapAccess::externalInput: return "external-input-unavailable";
        case Imx1BootstrapAccess::memoryUnavailable: return "external-memory-unavailable";
        case Imx1BootstrapAccess::sequence: return "unsupported-sdram-sequence";
    }
    return "unknown";
}

void Imx1Bootstrap::emitDiagnosticMemory(const ArmDiagnosticSink& sink) const {
    const uint32_t internal[] = {0x00300000,uint32_t(sram.size()),uint32_t(sram.size()),0,1};
    sink.emit("sram-map",internal,5);
    if (sink.bytes) sink.bytes(sink.context,"sram",sram.data(),sram.size());
    for (unsigned i=0;i<2;++i) {
        const auto& s=sdram[i];
        const uint32_t map[] = {0x08000000+i*0x04000000,s.bytes,uint32_t(s.ram.size()),s.control,uint32_t(s.modeSet)};
        sink.emit(i ? "sdram1-map" : "sdram0-map",map,5);
        if (sink.bytes) sink.bytes(sink.context,i ? "sdram1" : "sdram0",s.ram.data(),s.ram.size());
    }
}
