#include "s3c2442_machine.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CPU.h"
#include "RAM.h"
#include "arm920_state.h"
#include "machine_state_archive.h"
#include "mem.h"
#include "memory_buffer.h"
#include "pace_patch.h"
#include "patch_dispatch.h"
#include "system_state.h"

namespace {
    constexpr uint32_t sdramBase = 0x30000000, bank6Size = 0x08000000;
    // Most instructions one interpreter call runs, so timers and interrupts are seen this often.
    constexpr uint32_t batch = 64;
    // Most instructions one Fast span runs between looks at the clocks; events end spans before this.
    constexpr uint32_t fastSpan = 1u << 22;
    // ASSUMPTION "IRQ after an SFR write": an interrupt that an SFR write raises (an unmask with a request
    // pending) reaches the CPU at least this many instructions after the write, for the store through the
    // bus and the interrupt controller's synchronisers; Frond holds it to the end of the batch the write is
    // in. ser2410 unmasks TXD0 and sets its own transmit flag in the next instruction, and its interrupt
    // thread sees nothing to do if the IRQ comes in between.
    constexpr uint64_t irqWriteLatency = 2;

    // SFR blocks the machine doesn't model yet, by base, for the refusal text.
    struct Unmodelled { uint32_t base; const char* name; };
    constexpr Unmodelled unmodelled[] = {
        {0x49000000, "USB host"}, {0x4f000000, "camera interface"},
        {0x59000000, "SPI"}, {0x5b000000, "AC97"},
    };

    uint64_t mulDiv(uint64_t a, uint64_t b, uint64_t c, uint64_t& rest) {
        const __uint128_t total = __uint128_t(a) * b + rest;
        rest = uint64_t(total % c);
        return uint64_t(total / c);
    }
}

std::unique_ptr<S3c2442Machine> S3c2442Machine::create(const S3c2442Options& options, std::string& error) {
    if (options.ramBytes == 0 || bank6Size % options.ramBytes || options.ramBytes % MEMORY_BUFFER_GRANULARITY) {
        error = "SDRAM size must divide bank 6 (128 MB)";
        return nullptr;
    }
    if (!cpuSetFastEngine(false)) {
        error = "the interpreter could not be picked";
        return nullptr;
    }
    std::unique_ptr<S3c2442Machine> m(new S3c2442Machine(options));
    m->mem = memInit();
    // uARM has no teardown for the bus, RAM regions or CPU, which live as long as the process, as with the
    // other machines; the destructor gives back the SDRAM, the only large part.
    MemoryBuffer& buffer = m->ramMemory;
    if (!memoryBufferAllocate(&buffer, options.ramBytes)) {
        error = "SDRAM could not be allocated";
        return nullptr;
    }
    m->ramBuffer = buffer.buffer;
    // The SDRAM repeats through bank 6, as its upper address lines aren't wired.
    for (uint32_t i = 0; i < bank6Size / options.ramBytes && i < 4; ++i) {
        m->ram[i] = ramInit(m->mem, nullptr, sdramBase + i * options.ramBytes, options.ramBytes, &buffer, i == 0);
        ramSetNoFramebuffer(m->ram[i]);
    }
    const uint32_t ramEnd = sdramBase + std::min<uint32_t>(4, bank6Size / options.ramBytes) * options.ramBytes;
    if (!memRegionAdd(m->mem, 0, sdramBase, access, m.get()) ||
        !memRegionAdd(m->mem, ramEnd, 0xffffffff - ramEnd, access, m.get())) {
        error = "the SFR space could not be mapped";
        return nullptr;
    }
    auto* patches = initPatchDispatch();
    m->cpuCore = cpuInit(0, m->mem, ARM_MEMORY_SYSTEM_MMU, false, false, -1, 0, 0, patches, new PacePatch{},
                         createSystemState(), ArmCpuModel::arm920t);
    if (!m->cpuCore) {
        error = "the ARM920T could not be made";
        return nullptr;
    }
    patchDispatchSetCpu(patches, m->cpuCore);
    cpuArm920EnableWait(m->cpuCore);
    cpuArm920SetMemoryCheck(m->cpuCore, cacheCheck, m.get());
    cpuSetUndefinedHook(m->cpuCore, undefinedTaken, m.get());
    return m;
}

S3c2442Machine::S3c2442Machine(const S3c2442Options& options) : clock(options.crystal), options(options) {
    intc.reset();
    clock.reset();
    memory.reset();
    pwm.reset();
    watchdog.reset();
    rtc.reset(options.rtcStart);
    sdi.reset();
    usbDevice.reset();
    adc.reset();
    dma.reset();
    iis.reset();
    iic.reset();
    gpio.reset(options.chipId, options.resetCause);
    for (unsigned p = 0; p < S3cGpio::ports; ++p) gpio.input[p] = options.pinLevels[p] & S3cGpio::pinMask(S3cGpio::Port(p));
    for (auto& u : uarts) {
        u.reset();
        u.sink = [this](unsigned channel, uint8_t byte) { if (uartSink) uartSink(channel, byte); };
    }
    lcd.reset();
    nand.reset();
    std::memset(steppingstone, 0, sizeof steppingstone);
}

S3c2442Machine::~S3c2442Machine() {
    // Nothing runs on the CPU after this, and its RAM regions point into the buffer.
    if (ramMemory.buffer) memoryBufferRelease(&ramMemory);
}

bool S3c2442Machine::loadRam(uint32_t physical, const uint8_t* data, size_t size) {
    if (physical < sdramBase || uint64_t(physical) + size > uint64_t(sdramBase) + options.ramBytes) return false;
    std::memcpy(ramBuffer + (physical - sdramBase), data, size);
    return true;
}

void S3c2442Machine::start(uint32_t pc) {
    cpuReset(cpuCore, pc);
    stopped = {};
    undefinedPending = false;
}

// An undefined instruction stops the run, as the CPU also takes one for forms it doesn't model. Two are
// the guest's own and go to its handler, as on the real ARM920T: VFP (coprocessors 10 and 11), which the
// kernel probes for and the chip doesn't have, and Windows CE's breakpoint 0xe6000010 (DebugBreak), which
// the kernel takes as a breakpoint exception. The first other one in a batch is the one reported.
void S3c2442Machine::undefinedTaken(void* context, uint32_t pc, uint32_t instr) {
    auto& m = *static_cast<S3c2442Machine*>(context);
    const bool coprocessor = ((instr >> 24) & 0xf) == 0xe || ((instr >> 25) & 7) == 6;
    const bool vfpProbe = instr && coprocessor && ((instr >> 9) & 7) == 5;
    if (vfpProbe) { ++m.vfpProbes; return; }
    if (instr == 0xe6000010) { ++m.breakpoints; return; }
    if (m.undefinedPending) return;
    m.undefinedPending = true;
    m.undefinedStopPc = pc;
    m.undefinedStopInstr = instr;
}

uint32_t S3c2442Machine::pc() const { return cpuGetRegisters(cpuCore)[15]; }

bool S3c2442Machine::refuse(uint32_t address, unsigned size, bool write, uint32_t value, const std::string& text) {
    if (stopped.stop == S3c2442Stop::running || stopped.stop == S3c2442Stop::budget) {
        stopped.stop = S3c2442Stop::refused;
        stopped.pc = cpuGetCurInstrPC(cpuCore);
        stopped.address = address;
        stopped.size = uint8_t(size);
        stopped.write = write;
        stopped.value = value;
        stopped.text = text;
    }
    return false;
}

std::string S3c2442Machine::stopText() const {
    char line[256];
    switch (stopped.stop) {
        case S3c2442Stop::running: return "running";
        case S3c2442Stop::budget: return "instruction budget reached";
        case S3c2442Stop::refused:
            std::snprintf(line, sizeof line, "refused %u-byte %s at %08x%s, pc %08x: ", stopped.size,
                          stopped.write ? "write" : "read", stopped.address,
                          stopped.write ? (" of " + [&] { char v[16]; std::snprintf(v, sizeof v, "%08x", stopped.value); return std::string(v); }()).c_str() : "",
                          stopped.pc);
            return line + stopped.text;
        case S3c2442Stop::watchdogReset: return "the watchdog ran out with its reset on (resets aren't modelled yet)";
        case S3c2442Stop::powerOff: return "CLKCON POWER_OFF set";
        case S3c2442Stop::idleNoWake: return "idle with no interrupt that can come";
        case S3c2442Stop::undefined:
            std::snprintf(line, sizeof line, "undefined instruction %08x at %08x (or a form the CPU doesn't model)",
                          stopped.value, stopped.pc);
            return line;
        case S3c2442Stop::dataCache:
            std::snprintf(line, sizeof line, "cacheable access to %08x, which isn't memory, pc %08x", stopped.address, stopped.pc);
            return line;
    }
    return "?";
}

// What the ARM920T's data cache may fill from or write back to: SDRAM and the steppingstone (an SRAM),
// never an SFR.
bool S3c2442Machine::cacheCheck(void* context, uint32_t address, uint32_t size, bool write) {
    auto& m = *static_cast<S3c2442Machine*>(context);
    if (address < 0x08000000) return (address & (sizeof m.steppingstone - 1)) + size <= sizeof m.steppingstone;
    return memIsBufferRange(m.mem, address, size, write);
}

bool S3c2442Machine::access(void* context, uint32_t address, uint_fast8_t size, bool write, void* data) {
    auto& m = *static_cast<S3c2442Machine*>(context);
    // The steppingstone, the 4 KB SRAM at 0 when booting from NAND. ASSUMPTION "steppingstone mirrored in
    // bank 0": it repeats through nGCS0's 128 MB, as the bank decodes only its low 12 address bits. A
    // driver in device.exe reads a pointer at 0x001ffffc.
    if (address < 0x08000000) {
        const uint32_t at = address & (sizeof m.steppingstone - 1);
        if (at + size > sizeof m.steppingstone) return m.refuse(address, size, write, 0, "steppingstone overrun");
        if (write) std::memcpy(m.steppingstone + at, data, size);
        else std::memcpy(data, m.steppingstone + at, size);
        return true;
    }
    if (size != 1 && size != 2 && size != 4)
        return m.refuse(address, size, write, 0, "SFR access wider than a word");
    if (address & (size - 1)) return m.refuse(address, size, write, 0, "unaligned SFR access");
    uint32_t value = 0;
    if (write) std::memcpy(&value, data, size);
    if (!m.mmio(address, size, write, value)) return false;
    if (!write) std::memcpy(data, &value, size);
    if (m.mmioTrace) m.mmioTrace(cpuGetCurInstrPC(m.cpuCore), address, size, write, value);
    return true;
}

// One SFR access. A read of less than a word reads the word and takes its part, except where the
// register's own width matters (the NAND data port and the UART receive buffer). A write of less than a
// word only goes to the registers that are bytes: UART transmit, the RTC and the NAND ports.
bool S3c2442Machine::mmio(uint32_t address, unsigned size, bool write, uint32_t& value) {
    const uint32_t base = address & 0xff000000, offset = address & 0x00ffffff;
    const unsigned shift = (address & 3) * 8;
    const uint32_t word = offset & ~3u;
    S3cAccess result = S3cAccess::unknown;
    const char* name = nullptr;
    const char* detail = nullptr;
    bool byteRegister = false;
    uint32_t got = 0;

    auto readWord = [&](auto&& reader) {
        result = reader(word, got);
        value = size == 4 ? got : (got >> shift) & ((1u << (8 * size)) - 1);
    };
    // The SFRs see the time at the instruction doing the access, and the run goes on from the instruction
    // after it, as the access may have moved the next event. Both engines run the same way: Fast's
    // translated code never makes SFR accesses, the interpreter does them (S3c2442Machine::run).
    catchUp();
    cpuSetSlowPath(cpuCore, SLOW_PATH_REASON_RESCHEDULE);
    sfrAccessed = true;

    if ((address & 0xf8000000) == 0x28000000) {
        // nGCS5, the debug board on a development unit (LEDs at 0x28000000, a chip eboot probes at
        // 0x2a000e60). Nothing is there on a retail one. ASSUMPTION "nGCS5 empty bus": reads give 0xff,
        // which eboot's probe takes as no board.
        if (!options.debugBoardEmpty)
            return refuse(address, size, write, value, "nGCS5 debug board access");
        if (write) debugWriteSeen = true;
        else value = size == 4 ? ~0u : (1u << (8 * size)) - 1;
        return true;
    }
    if ((address & 0xf8000000) == 0x20000000) {
        // nGCS4, the TI ACX100 Wi-Fi MAC, which isn't modelled yet. ASSUMPTION "Wi-Fi empty bus": reads give
        // all ones and writes are dropped, so tiacxwln finds no chip.
        if (!options.wifiBusEmpty) return refuse(address, size, write, value, "nGCS4 Wi-Fi access");
        ++wifiAccesses;
        if (!write) value = size == 4 ? ~0u : (1u << (8 * size)) - 1;
        return true;
    }
    switch (base) {
        case 0x48000000:
            name = "memory controller";
            if (write) result = size == 4 ? memory.write(offset, value) : S3cAccess::unsupported;
            else readWord([&](uint32_t o, uint32_t& v) { return memory.read(o, v); });
            break;
        case 0x4a000000:
            name = "interrupt controller";
            if (write) result = size == 4 ? intc.write(offset, value) : S3cAccess::unsupported;
            else readWord([&](uint32_t o, uint32_t& v) { return intc.read(o, v); });
            break;
        case 0x4c000000:
            name = "clock and power";
            if (write) result = size == 4 ? clock.write(offset, value) : S3cAccess::unsupported;
            else readWord([&](uint32_t o, uint32_t& v) { return clock.read(o, v); });
            if (write && result == S3cAccess::ok && clock.powerOff()) {
                // Sleep mode from the next instruction: the interpreter call ends here (RESCHEDULE above).
                sleeping = true;
                ++sleeps;
            }
            break;
        case 0x4d000000:
            name = "LCD controller";
            if (write) result = size == 4 ? lcd.write(offset, value, intc) : S3cAccess::unsupported;
            else readWord([&](uint32_t o, uint32_t& v) { return lcd.read(o, v); });
            if (result == S3cAccess::unsupported && write && offset == 0x5c) detail = "LCD FIFO interrupt unmasked (not modelled)";
            break;
        case 0x4e000000:
            name = "NAND controller";
            byteRegister = true;
            if (write) result = (address & 3) ? S3cAccess::unsupported : nand.write(offset, size, value, intc);
            else if (word == 0x10) result = nand.read(offset, size, value);
            else readWord([&](uint32_t o, uint32_t& v) { return nand.read(o, 4, v); });
            if (result == S3cAccess::unsupported) detail = nand.refusal;
            break;
        case 0x50000000: {
            if (offset >= 0xc000) break;
            auto& uart = uarts[offset >> 14];
            const uint32_t o = offset & 0x3fff, w = o & ~3u;
            name = "UART";
            if (write) {
                if (size == 4 || w == 0x20) result = uart.write(w, value, intc);
                else result = S3cAccess::unsupported;
            } else if (w == 0x24) {
                result = uart.read(w, got, intc);
                value = got & 0xff;
            } else {
                result = uart.read(w, got, intc);
                value = size == 4 ? got : (got >> shift) & ((1u << (8 * size)) - 1);
            }
            break;
        }
        case 0x51000000:
            name = "PWM timers";
            if (write) result = size == 4 ? pwm.write(offset, value) : S3cAccess::unsupported;
            else readWord([&](uint32_t o, uint32_t& v) { return pwm.read(o, v); });
            break;
        case 0x53000000:
            name = "watchdog";
            if (write) result = size == 4 ? watchdog.write(offset, value) : S3cAccess::unsupported;
            else readWord([&](uint32_t o, uint32_t& v) { return watchdog.read(o, v); });
            break;
        case 0x56000000:
            name = "GPIO";
            if (write) result = size == 4 ? gpio.write(offset, value, intc) : S3cAccess::unsupported;
            else readWord([&](uint32_t o, uint32_t& v) { return gpio.read(o, v); });
            break;
        case 0x52000000:
            name = "USB device";
            if (write) result = size == 4 ? usbDevice.write(offset, value) : S3cAccess::unsupported;
            else readWord([&](uint32_t o, uint32_t& v) { return usbDevice.read(o, v); });
            break;
        case 0x58000000:
            name = "ADC and touch screen";
            if (write) result = size == 4 ? adc.write(offset, value, intc) : S3cAccess::unsupported;
            else readWord([&](uint32_t o, uint32_t& v) { return adc.read(o, v); });
            break;
        case 0x4b000000:
            name = "DMA";
            if (write) {
                result = size == 4 ? dma.write(offset, value) : S3cAccess::unsupported;
                if (result == S3cAccess::unsupported && size == 4) detail = "software-triggered DMA isn't modelled yet";
            } else readWord([&](uint32_t o, uint32_t& v) { return dma.read(o, v); });
            break;
        case 0x54000000:
            name = "IIC";
            if (write) result = size == 4 ? iic.write(offset, value) : S3cAccess::unsupported;
            else readWord([&](uint32_t o, uint32_t& v) { return iic.read(o, v); });
            break;
        case 0x55000000:
            name = "IIS";
            if (write) result = size == 4 ? iis.write(offset, value) : S3cAccess::unsupported;
            else readWord([&](uint32_t o, uint32_t& v) { return iis.read(o, v); });
            break;
        case 0x5a000000:
            name = "SD interface";
            if (write) result = size == 4 ? sdi.write(offset, value) : S3cAccess::unsupported;
            else readWord([&](uint32_t o, uint32_t& v) { return sdi.read(o, v); });
            break;
        case 0x57000000:
            name = "RTC";
            byteRegister = true;
            if (write) result = (address & 3) ? S3cAccess::unsupported : rtc.write(offset, value & (size == 4 ? ~0u : (1u << (8 * size)) - 1));
            else readWord([&](uint32_t o, uint32_t& v) { return rtc.read(o, v); });
            break;
    }
    if (result == S3cAccess::ok) {
        // Reads change levels too, such as URXH draining the receive FIFO.
        updateLines(write);
        return true;
    }
    std::string text;
    if (!name) {
        for (const auto& u : unmodelled)
            if (u.base == base) name = u.name;
        text = name ? std::string(name) + " isn't modelled yet" : "nothing is mapped there in the machine";
    } else if (result == S3cAccess::unsupported && !detail && write && size != 4 && !byteRegister) {
        text = std::string(name) + ": byte or halfword write to a word register";
    } else {
        text = std::string(name) + (result == S3cAccess::unknown ? ": unknown register" : ": ") + (detail ? detail : "");
        if (result == S3cAccess::unsupported && !detail) text += "value or mode not modelled";
    }
    return refuse(address, size, write, value, text);
}

void S3c2442Machine::setPin(S3cGpio::Port port, unsigned bit, bool level) {
    gpio.setInputs(port, 1u << bit, level ? 1u << bit : 0, intc);
    updateLines();
}

void S3c2442Machine::setStylus(bool down, uint16_t x, uint16_t y) {
    adc.setStylus(down, x, y, intc);
    updateLines();
}

bool S3c2442Machine::screen(std::vector<uint32_t>& pixels, unsigned& width, unsigned& height, std::string& why) const {
    auto memory = [&](uint32_t pa, void* out, size_t n) {
        if (pa < sdramBase || uint64_t(pa) + n > uint64_t(sdramBase) + options.ramBytes) return false;
        std::memcpy(out, ramBuffer + (pa - sdramBase), n);
        return true;
    };
    return lcd.frame(memory, pixels, width, height, why);
}

void S3c2442Machine::catchUp() {
    // The instructions the interpreter call has finished; cpuCyclesInProgress counts the one running too.
    const uint32_t inProgress = cpuCyclesInProgress(cpuCore);
    const uint64_t done = inProgress ? inProgress - 1 : 0;
    if (done > batchAdvanced) {
        advance(done - batchAdvanced);
        batchAdvanced = done;
    }
}

void S3c2442Machine::updateLines(bool sfrWrite) {
    const bool irq = intc.irq(), fiq = intc.fiq();
    if (!irq) irqHeld = false;
    else if (!irqLine && sfrWrite && !irqHeld) {
        irqHeld = true;
        irqHeldUntil = retiredCount + cpuCyclesInProgress(cpuCore) + irqWriteLatency;
    }
    if (irq != irqLine && !irqHeld) { cpuIrq(cpuCore, false, irq); irqLine = irq; }
    if (fiq != fiqLine) { cpuIrq(cpuCore, true, fiq); fiqLine = fiq; }
}

// Moves every clock domain on by CPU cycles at the CPU clock as it is now.
void S3c2442Machine::advance(uint64_t cpuCycles) {
    const uint64_t cpuHz = this->cpuHz();
    if (!cpuHz || !cpuCycles) return;
    cycleCount += cpuCycles;
    elapsedNs += mulDiv(cpuCycles, 1000000000, cpuHz, nsRest);
    const uint64_t pclk = mulDiv(cpuCycles, clock.pclk(), cpuHz, pclkRest);
    const uint64_t hclk = mulDiv(cpuCycles, clock.hclk(), cpuHz, hclkRest);
    const uint64_t ticks32k = mulDiv(cpuCycles, 32768, cpuHz, rtcRest);
    if (pclk) {
        pwm.advance(pclk, intc);
        watchdog.advance(pclk, intc);
        adc.advance(pclk, intc);
    }
    if (hclk) lcd.advance(hclk, intc);
    if (ticks32k) rtc.advance(ticks32k, intc);
    if (watchdog.resetRequested && stopped.stop == S3c2442Stop::running) {
        stopped.stop = S3c2442Stop::watchdogReset;
        stopped.pc = pc();
    }
    updateLines();
}

// CPU cycles until the next timer, watchdog, frame or RTC event, UINT64_MAX with none due.
uint64_t S3c2442Machine::cyclesToEvent(bool wakeOnly) const {
    const uint64_t cpuHz = this->cpuHz();
    uint64_t best = UINT64_MAX;
    auto consider = [&](uint64_t ticks, uint64_t hz, uint64_t rest) {
        if (ticks == UINT64_MAX || !hz) return;
        // Cycles c with (c * hz + rest) / cpuHz >= ticks.
        const __uint128_t need = __uint128_t(ticks) * cpuHz;
        const uint64_t cycles = need <= rest ? 1 : uint64_t((need - rest + hz - 1) / hz);
        best = std::min(best, std::max<uint64_t>(cycles, 1));
    };
    if (!wakeOnly) {
        consider(std::min({pwm.ticksToEvent(), watchdog.ticksToEvent(), adc.ticksToEvent()}), clock.pclk(), pclkRest);
        consider(lcd.ticksToEvent(), clock.hclk(), hclkRest);
        consider(rtc.ticksToEvent(), 32768, rtcRest);
        return best;
    }
    auto open = [&](unsigned source) { return !(intc.intmsk & (1u << source)); };
    const uint32_t timers = (~intc.intmsk >> s3cIrq::timer0) & 0x1f;
    const bool wdtInterrupt = open(s3cIrq::wdtAc97) && !(intc.intsubmsk & (1u << s3cSubIrq::wdt));
    consider(std::min(pwm.ticksToEvent(timers), watchdog.ticksToEvent(wdtInterrupt)), clock.pclk(), pclkRest);
    if (open(s3cIrq::lcd)) consider(lcd.ticksToEvent(true), clock.hclk(), hclkRest);
    if (open(s3cIrq::adc) && !(intc.intsubmsk & (1u << s3cSubIrq::adc))) consider(adc.ticksToEvent(), clock.pclk(), pclkRest);
    consider(rtc.ticksToEvent(open(s3cIrq::tick), open(s3cIrq::rtc)), 32768, rtcRest);
    return best;
}

// EINT0-3 and the RTC alarm by their SRCPND bits, EINT4-15 by EINTPEND where EINTMASK lets them through.
uint32_t S3c2442Machine::wakeSources() const {
    return (intc.srcpnd & (0xfu | (1u << s3cIrq::rtc))) | (gpio.eintpend & ~gpio.eintmask & 0xfff0);
}

// Asleep the CPU's clock is off; the cycles are the budget's measure of time at the rate it slept at.
void S3c2442Machine::sleepAdvance(uint64_t cpuCycles) {
    const uint64_t cpuHz = this->cpuHz();
    if (!cpuHz || !cpuCycles) return;
    cycleCount += cpuCycles;
    idleCount += cpuCycles;
    elapsedNs += mulDiv(cpuCycles, 1000000000, cpuHz, nsRest);
    const uint64_t ticks32k = mulDiv(cpuCycles, 32768, cpuHz, rtcRest);
    if (ticks32k) rtc.advance(ticks32k, intc);
}

// The wake-up reset (manual 7, "Power management"). The RTC, the GPIO block with EINTPEND and GSTATUS3-4
// are in the alive part and keep their state; ASSUMPTION "GPIO kept in sleep": the port registers too (the
// rx1950's OAL sets its own sleep pin table and restores the ones it saved). SDRAM held itself in self refresh, so it's kept; the data cache is cleaned to it
// first, as the OAL cleans it before sleeping. Every other block is reset. The sources that woke it stay
// pending for the OAL to read.
void S3c2442Machine::wake() {
    const uint32_t sources = intc.srcpnd & (0xfu | (1u << s3cIrq::rtc));
    lastWake = wakeSources();
    sleeping = false;
    ++wakes;
    cpuArm920LeanReset(cpuCore);
    if (fastOn) {
        // The loader writes SDRAM behind the CPU's back, so translated code goes.
        setFastEngine(false);
        cpuArm920DropJit(cpuCore);
        setFastEngine(true);
    }
    intc.reset();
    clock.reset();
    memory.reset();
    pwm.reset();
    watchdog.reset();
    sdi.reset();
    usbDevice.reset();
    uint16_t inputs[8];  // the board's inputs stay, as does the stylus
    std::memcpy(inputs, adc.channel, sizeof inputs);
    adc.reset();
    std::memcpy(adc.channel, inputs, sizeof inputs);
    dma.reset();
    iis.reset();
    iic.reset();
    for (auto& u : uarts) u.reset();
    lcd.reset();
    nand.reset();
    gpio.gstatus[2] = 2;  // OFFRST
    for (unsigned n = 0; n < 32; ++n)
        if (sources & (1u << n)) intc.pulse(n);
    const uint32_t live = gpio.eintpend & ~gpio.eintmask;
    intc.level(s3cIrq::eint4to7, live & 0xf0);
    intc.level(s3cIrq::eint8to23, live & 0xffff00);
    irqHeld = false;
    batchAdvanced = 0;
    if (irqLine) cpuIrq(cpuCore, false, false);
    if (fiqLine) cpuIrq(cpuCore, true, false);
    irqLine = fiqLine = false;
    if (resetLoader) resetLoader();
    else start(0);
    updateLines();
}

int S3c2442Machine::backlightLevel() const {
    if (sleeping) return -1;
    const unsigned mode = gpio.con[S3cGpio::b] & 3;
    if (mode == 1) return (gpio.dat[S3cGpio::b] & 1) ? 1000 : 0;
    if (mode != 2) return -1;
    const auto& t = pwm.timers[0];
    if (!(pwm.tcon & 1) || !t.buffer) return (pwm.tcon & 4) ? 1000 : 0;
    // TOUT0 is high from TCMPB0 down to 0 of each TCNTB0 + 1 period; TCON bit 2 inverts it. With TCMPB0
    // above TCNTB0 the count never meets the compare, so TOUT0 stays at its idle level.
    uint64_t duty = t.compare > t.buffer ? 0 : uint64_t(t.compare + 1) * 1000 / (uint64_t(t.buffer) + 1);
    if (pwm.tcon & 4) duty = 1000 - duty;
    return int(duty);
}

S3c2442Stop S3c2442Machine::run(uint64_t instructions) {
    if (stopped.stop == S3c2442Stop::budget) stopped.stop = S3c2442Stop::running;
    if (stopped.stop != S3c2442Stop::running) return stopped.stop;
    const uint64_t target = retiredCount + idleCount + instructions;
    while (stopped.stop == S3c2442Stop::running) {
        const uint64_t used = retiredCount + idleCount;
        if (sleeping) {
            if (wakeSources()) {
                wake();
                continue;
            }
            if (used >= target) { stopped.stop = S3c2442Stop::budget; break; }
            // Only the RTC alarm can come by itself; pins come from outside between runs.
            uint64_t skip = target - used;
            const uint64_t ticks = rtc.ticksToEvent(false, true);
            if (ticks != UINT64_MAX && cpuHz()) {
                const __uint128_t need = __uint128_t(ticks) * cpuHz();
                const uint64_t cycles = need <= rtcRest ? 1 : uint64_t((need - rtcRest + 32767) / 32768);
                skip = std::min(skip, std::max<uint64_t>(cycles, 1));
            }
            sleepAdvance(skip);
            continue;
        }
        if (used >= target) { stopped.stop = S3c2442Stop::budget; break; }
        // IDLE in CLKCON or the ARM920T's wait for interrupt: time goes on to the next event.
        if (clock.idle() || cpuArm920Waiting(cpuCore)) {
            if (intc.irq() || intc.fiq()) {
                clock.wake();
                cpuArm920EndWait(cpuCore);
                continue;
            }
            // Masked sources still count while idle, but only an unmasked one can end it. External
            // interrupts (EINT, UART receive) come from outside between runs.
            uint64_t skip = cyclesToEvent(true);
            if (skip == UINT64_MAX) {
                stopped.stop = S3c2442Stop::idleNoWake;
                stopped.pc = pc();
                break;
            }
            skip = std::min(skip, target - used);
            idleCount += skip;
            advance(skip);
            continue;
        }
        const bool fast = fastOn && history.empty();
        uint64_t n = history.empty() ? std::min<uint64_t>({fast ? fastSpan : batch, cyclesToEvent(), target - used}) : 1;
        if (irqHeld) {
            if (retiredCount >= irqHeldUntil) {
                irqHeld = false;
                updateLines();
            } else {
                n = std::min(n, irqHeldUntil - retiredCount);
            }
        }
        if (fast) {
            runFast(n);
            continue;
        }
        const uint32_t stepPc = pc();
        if (!history.empty()) {
            history[historyNext] = stepPc;
            historyNext = (historyNext + 1) % history.size();
        }
        step(n);
        if (!history.empty() && stepHook) stepHook(stepPc);
    }
    return stopped.stop;
}

// One interpreter call of up to n instructions (it ends early at an SFR access, an interrupt or another slow
// path), then their time and the stops it may have hit.
uint32_t S3c2442Machine::step(uint64_t n) {
    batchAdvanced = 0;
    const uint32_t ran = cpuCycle<ARM_MEMORY_SYSTEM_ARM920T, false>(cpuCore, uint32_t(std::max<uint64_t>(n, 1)));
    retiredCount += ran;
    const uint64_t total = ran ? ran : 1;
    advance(total - std::min(total, batchAdvanced));
    batchAdvanced = 0;
    const auto guard = cpuArm920DataGuard(cpuCore);
    if (guard.active && stopped.stop == S3c2442Stop::running) {
        stopped.stop = S3c2442Stop::dataCache;
        stopped.address = guard.address;
        stopped.size = uint8_t(guard.size);
        stopped.write = guard.write;
        stopped.pc = cpuGetCurInstrPC(cpuCore);
    }
    if (undefinedPending && stopped.stop == S3c2442Stop::running) {
        stopped.stop = S3c2442Stop::undefined;
        stopped.pc = undefinedStopPc;
        stopped.value = undefinedStopInstr;
    }
    undefinedPending = false;
    return ran;
}

// Fast: n instructions with no event due, as Exact runs them. Translated blocks run first, then the lean path,
// then the interpreter for one instruction where both refuse (SFR accesses, CP15 writes, interrupts, code not
// compiled yet). Neither makes SFR accesses, so time for what they ran is applied after them; an SFR access in
// the interpreter ends the span, so the next one is worked out from the events as they are then.
void S3c2442Machine::runFast(uint64_t n) {
    uint64_t left = n;
    const uint32_t slack = cpuArm920JitMaxUnchecked(cpuCore);
    uint32_t leanCount = UINT32_MAX;
    bool blPending = false;
    sfrAccessed = false;
    while (left && stopped.stop == S3c2442Stop::running && !clock.idle() && !cpuArm920Waiting(cpuCore)) {
        // A run passes its clock budget by a block at most (slack), and a Thumb BL pair is one clock but two
        // instructions, so it gets half of what's left less a block.
        if (leanCount == UINT32_MAX && !blPending && left / 2 > slack && cpuArm920JitCanRun(cpuCore)) {
            int stop = arm920JitExit;
            uint32_t instructions = 0;
            const uint32_t budget = uint32_t(std::min<uint64_t>(left / 2 - slack + 1, UINT32_MAX / 2));
            cpuArm920RunJit(cpuCore, budget, &stop, budget, &instructions);
            if (instructions) {
                retiredCount += instructions;
                jitRetiredCount += instructions;
                advance(instructions);
                left -= std::min<uint64_t>(left, instructions);
                if (stop == arm920JitBudget || stop == arm920JitHalt) continue;
            }
            // Code not compiled yet, a run that keeps stopping straight away or one stopped at a read the lean
            // path takes (WTCNT) goes to the lean path a while.
            leanCount = stop == arm920JitCold || instructions < 4 ? 32 : stop == arm920JitExit ? 16 : 1;
            continue;
        }
        if (!blPending) {
            uint32_t blNext = ~0u, clocks = 0;
            uint64_t bus = 0;
            const uint32_t count = uint32_t(std::min<uint64_t>(left, leanCount));
            leanCount = UINT32_MAX;
            leanAdvanced = 0;
            const uint32_t ran = cpuArm920RunLeanFor(cpuCore, count, count, &blNext, &bus, &clocks);
            if (ran) {
                retiredCount += ran;
                leanRetiredCount += ran;
                advance(ran - std::min(ran, leanAdvanced));
                left -= std::min<uint64_t>(left, ran);
                blPending = blNext != ~0u;
                if (ran == count) continue;
            }
            if (!left || blPending) continue;
        }
        // A BL suffix after a lean prefix, or an instruction neither the JIT nor the lean path runs.
        blPending = false;
        const uint32_t ran = step(1);
        stepRetiredCount += ran;
        left -= std::min<uint64_t>(left, ran ? ran : 1);
        if (sfrAccessed) break;
    }
}

bool S3c2442Machine::setFastEngine(bool fast) {
    if (fast == fastOn) return true;
    cpuArm920SetLean(cpuCore, fast ? leanPage : nullptr, fast ? leanGuard : nullptr, this);
    cpuArm920SetLeanRead(cpuCore, fast ? leanRead : nullptr);
    cpuArm920SetLeanWrite(cpuCore, fast ? leanWrite : nullptr);
    if (fast && !cpuArm920SetJit(cpuCore, true)) {
        cpuArm920SetLean(cpuCore, nullptr, nullptr, this);
        return false;
    }
    if (!fast) cpuArm920SetJit(cpuCore, false);
    // Fast keeps no cache contents: dirty data goes to memory first.
    cpuArm920LeanReset(cpuCore);
    fastOn = fast;
    return true;
}

// SDRAM (bank 6, repeating) and the steppingstone (bank 0, repeating): plain memory with no time and no side
// effect, which the Fast engine reads and writes from host memory.
uint8_t* S3c2442Machine::leanPage(void* context, uint32_t pa, bool, bool* counted) {
    auto& m = *static_cast<S3c2442Machine*>(context);
    *counted = true;
    if (pa < 0x08000000) return m.steppingstone;
    // The RAM regions create() maps: up to 4 copies of the SDRAM.
    const uint32_t mapped = std::min<uint32_t>(4, bank6Size / m.options.ramBytes) * m.options.ramBytes;
    if (pa >= sdramBase && pa - sdramBase < mapped) return m.ramBuffer + ((pa - sdramBase) & (m.options.ramBytes - 1) & ~0xfffu);
    return nullptr;
}

// SFR accesses Fast takes without ending its run, as they change nothing but the block's own state: WTCNT
// reads, which the OS's delay loops poll, and the NAND controller, which has no timing and raises no interrupt.
// For WTCNT the machine's time is first brought up to the reading instruction, as the interpreter does
// (mmio); with no event due in the span that changes nothing else. Translated code leaves WTCNT to the lean
// path, as its ran counts clocks, not instructions.
bool S3c2442Machine::leanRead(void* context, uint32_t pa, uint32_t size, uint32_t pc, uint32_t ran, uint32_t, bool,
                              uint32_t* value, uint32_t* cost) {
    auto& m = *static_cast<S3c2442Machine*>(context);
    *cost = 0;
    if ((pa & 0xffffffc0) == 0x4e000000) {
        // As mmio: the data port at its own width, the other registers as a part of the word.
        uint32_t got = 0;
        if ((pa & ~3u) == 0x4e000010) {
            if (m.nand.read(0x10, size, got) != S3cAccess::ok) return false;
            *value = got;
        } else {
            if (m.nand.read(pa & 0x3c, 4, got) != S3cAccess::ok) return false;
            *value = size == 4 ? got : (got >> ((pa & 3) * 8)) & ((1u << (8 * size)) - 1);
        }
    } else if (pa == 0x53000008 && size == 4 && !cpuArm920LeanInJit(m.cpuCore)) {
        if (ran > m.leanAdvanced) {
            m.advance(ran - m.leanAdvanced);
            m.leanAdvanced = ran;
        }
        if (m.watchdog.read(8, *value) != S3cAccess::ok) return false;
    } else {
        return false;
    }
    if (m.mmioTrace) m.mmioTrace(pc, pa, size, false, *value);
    return true;
}

bool S3c2442Machine::leanWrite(void* context, uint32_t pa, uint32_t size, uint32_t value, uint32_t, uint32_t,
                               uint32_t* cost) {
    auto& m = *static_cast<S3c2442Machine*>(context);
    *cost = 0;
    if ((pa & 0xffffffc0) != 0x4e000000 || (pa & 3) || m.nand.write(pa & 0x3c, size, value, m.intc) != S3cAccess::ok)
        return false;
    if (m.mmioTrace) m.mmioTrace(cpuGetCurInstrPC(m.cpuCore), pa, size, true, value);
    return true;
}

// Every instruction class the lean path asks about runs there as in the interpreter.
bool S3c2442Machine::leanGuard(uint32_t, const uint32_t*, uint32_t, uint32_t) { return true; }

std::vector<uint32_t> S3c2442Machine::recentPcs() const {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < history.size(); ++i) out.push_back(history[(historyNext + i) % history.size()]);
    return out;
}

// Machine state. The container and visitors are machine_state_archive.h's; the sections are the CPU's (as the
// Zodiac's machine keeps them), the blocks' state() lists and the machine's own counts below.
namespace {
    constexpr uint32_t s3cStateRevision = 1;
    // Sections: identity, CPU, its instruction and data caches, TLB, memory, blocks, machine.
    constexpr uint32_t secIdentity = stateFourcc("S3CI"), secCpu = stateFourcc("CPU "), secIcache = stateFourcc("ICAC"),
                       secDcache = stateFourcc("DCAC"), secTlb = stateFourcc("TLB "), secMemory = stateFourcc("SDRM"),
                       secBlocks = stateFourcc("BLKS"), secMachine = stateFourcc("MACH");
    constexpr uint64_t s3cLayout = stateLayoutMix(stateLayoutMix(0xcbf29ce484222325ull, 0x53334332), S3c2442Machine::stateVersion);
}

template <class A, class M> static void blockStates(A& a, M& m) {
    m.intc.state(a);
    m.clock.state(a);
    m.memory.state(a);
    m.pwm.state(a);
    m.watchdog.state(a);
    m.rtc.state(a);
    m.sdi.state(a);
    m.usbDevice.state(a);
    m.adc.state(a);
    m.dma.state(a);
    m.iis.state(a);
    m.iic.state(a);
    m.gpio.state(a);
    for (auto& u : m.uarts) u.state(a);
    m.lcd.state(a);
    m.nand.state(a);
}

// The blocks as one copy, so a load that fails part way leaves the machine's own alone.
struct S3cBlockCopy {
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
    S3cUart uarts[3];
    S3cLcd lcd;
    S3cNand nand;
    explicit S3cBlockCopy(const S3c2442Machine& m)
        : intc(m.intc), clock(m.clock), memory(m.memory), pwm(m.pwm), watchdog(m.watchdog), rtc(m.rtc), sdi(m.sdi),
          usbDevice(m.usbDevice), adc(m.adc), dma(m.dma), iis(m.iis), iic(m.iic), gpio(m.gpio),
          uarts{m.uarts[0], m.uarts[1], m.uarts[2]}, lcd(m.lcd) {}
};

template <class A> static void machineCounts(A& a, uint64_t* counts, size_t n, bool* flags, size_t f, uint32_t& lastWake) {
    for (size_t i = 0; i < n; ++i) a.Do64(counts[i]);
    for (size_t i = 0; i < f; ++i) a.DoBool(flags[i]);
    a.Do32(lastWake);
}

bool S3c2442Machine::saveState(std::vector<uint8_t>& out, std::string& why) {
    if (stopped.stop != S3c2442Stop::running && stopped.stop != S3c2442Stop::budget) {
        why = "the machine has stopped";
        return false;
    }
    auto cpu = std::make_unique<Arm920SavedState>();
    if (!cpuArm920CaptureState(cpuCore, *cpu)) {
        why = "the CPU isn't between instructions";
        return false;
    }
    // A loaded machine starts without translated code, so this one carries on without it too.
    cpuDropTranslatedCode(cpuCore);
    StateContainerWriter file(out, s3cStateRevision, s3cLayout, 0);
    {
        auto a = file.begin(secIdentity, stateVersion);
        uint32_t crystal = options.crystal, ram = options.ramBytes, chip = options.chipId;
        a.Fixed32(crystal).Fixed32(ram).Fixed32(chip);
        file.end();
    }
    { auto a = file.begin(secCpu, Arm920SavedState::stateVersion); cpu->stateCpu(a); file.end(); }
    { auto a = file.begin(secIcache, Arm920SavedState::stateVersion); cpu->stateIcache(a); file.end(); }
    { auto a = file.begin(secDcache, Arm920SavedState::stateVersion); cpu->stateDcache(a); file.end(); }
    { auto a = file.begin(secTlb, Arm920SavedState::stateVersion); cpu->stateTlb(a); file.end(); }
    {
        auto a = file.begin(secMemory, stateVersion);
        a.DoBuffer(ramBuffer, options.ramBytes).DoBuffer(steppingstone, sizeof steppingstone);
        file.end();
    }
    { auto a = file.begin(secBlocks, stateVersion); blockStates(a, *this); file.end(); }
    {
        auto a = file.begin(secMachine, stateVersion);
        uint64_t counts[] = {retiredCount, cycleCount, idleCount, elapsedNs, pclkRest, hclkRest, rtcRest, nsRest,
                             irqHeldUntil, sleeps, wakes, vfpProbes, breakpoints, wifiAccesses};
        bool flags[] = {irqLine, fiqLine, irqHeld, sleeping, debugWriteSeen};
        machineCounts(a, counts, std::size(counts), flags, std::size(flags), lastWake);
        file.end();
    }
    file.finish();
    return true;
}

bool S3c2442Machine::loadState(const uint8_t* data, size_t size, std::string& why) {
    StateContainerReader file;
    if (file.open(data, size) != StateContainerError::ok) {
        why = "not a saved S3C2442 machine, or a damaged one";
        return false;
    }
    if (file.coreRevision != s3cStateRevision || file.layoutFingerprint != s3cLayout) {
        why = "saved by another version of Frond's S3C2442 machine";
        return false;
    }
    auto open = [&](uint32_t fourcc, uint32_t version, StateReader& reader) {
        const auto* section = file.find(fourcc);
        if (!section || section->version != version) return false;
        reader = file.reader(*section);
        return true;
    };
    auto done = [&](StateReader& a, const char* what) {
        if (a.complete()) return true;
        why = std::string("the saved ") + what + (a.fixedMismatch() ? " was made for another machine" : " doesn't fit");
        return false;
    };
    StateReader a(nullptr, 0);
    if (!open(secIdentity, stateVersion, a)) { why = "not a saved S3C2442 machine"; return false; }
    uint32_t crystal = options.crystal, ram = options.ramBytes, chip = options.chipId;
    a.Fixed32(crystal).Fixed32(ram).Fixed32(chip);
    if (!done(a, "machine")) return false;

    auto cpu = std::make_unique<Arm920SavedState>();
    if (!cpuArm920CaptureState(cpuCore, *cpu)) { why = "the CPU isn't between instructions"; return false; }
    struct Part { uint32_t fourcc; void (Arm920SavedState::*list)(StateReader&); const char* name; };
    const Part parts[] = {{secCpu, &Arm920SavedState::stateCpu<StateReader>, "CPU"},
                          {secIcache, &Arm920SavedState::stateIcache<StateReader>, "instruction cache"},
                          {secDcache, &Arm920SavedState::stateDcache<StateReader>, "data cache"},
                          {secTlb, &Arm920SavedState::stateTlb<StateReader>, "TLB"}};
    for (const auto& part : parts) {
        if (!open(part.fourcc, Arm920SavedState::stateVersion, a)) { why = std::string("no saved ") + part.name; return false; }
        ((*cpu).*part.list)(a);
        if (!done(a, part.name)) return false;
    }
    if (!cpuArm920StateFits(cpuCore, *cpu)) { why = "the saved CPU doesn't fit this one"; return false; }

    if (!open(secMemory, stateVersion, a)) { why = "no saved memory"; return false; }
    std::vector<uint8_t> sdram(options.ramBytes), stone(sizeof steppingstone);
    a.DoBuffer(sdram.data(), sdram.size()).DoBuffer(stone.data(), stone.size());
    if (!done(a, "memory")) return false;

    S3cBlockCopy blocks(*this);
    if (!open(secBlocks, stateVersion, a)) { why = "no saved blocks"; return false; }
    blockStates(a, blocks);
    if (!done(a, "blocks")) return false;

    if (!open(secMachine, stateVersion, a)) { why = "no saved machine counts"; return false; }
    uint64_t counts[14] = {};
    bool flags[5] = {};
    uint32_t wake = 0;
    machineCounts(a, counts, std::size(counts), flags, std::size(flags), wake);
    if (!done(a, "machine counts")) return false;

    // Nothing from here on can fail. No cpuIrq call, so the restored lines make no new edge.
    cpuArm920ApplyState(cpuCore, *cpu);
    std::memcpy(ramBuffer, sdram.data(), sdram.size());
    std::memcpy(steppingstone, stone.data(), stone.size());
    intc = blocks.intc;
    clock.locktime = blocks.clock.locktime; clock.mpllcon = blocks.clock.mpllcon; clock.upllcon = blocks.clock.upllcon;
    clock.clkcon = blocks.clock.clkcon; clock.clkslow = blocks.clock.clkslow; clock.clkdivn = blocks.clock.clkdivn;
    clock.camdivn = blocks.clock.camdivn;
    memory = blocks.memory;
    pwm = blocks.pwm;
    watchdog = blocks.watchdog;
    rtc = blocks.rtc;
    sdi = blocks.sdi;
    usbDevice = blocks.usbDevice;
    adc = blocks.adc;
    dma = blocks.dma;
    iis = blocks.iis;
    iic = blocks.iic;
    gpio = blocks.gpio;
    for (unsigned c = 0; c < 3; ++c) {
        auto sink = uarts[c].sink;
        uarts[c] = blocks.uarts[c];
        uarts[c].sink = sink;
    }
    lcd = blocks.lcd;
    // The registers only: the pages, their store and the dirty marks stay this machine's.
    {
        std::vector<uint8_t> bytes;
        StateWriter w(bytes);
        blocks.nand.state(w);
        StateReader r(bytes.data(), bytes.size());
        nand.state(r);
    }
    uint64_t* targets[] = {&retiredCount, &cycleCount, &idleCount, &elapsedNs, &pclkRest, &hclkRest, &rtcRest, &nsRest,
                           &irqHeldUntil, &sleeps, &wakes, &vfpProbes, &breakpoints, &wifiAccesses};
    for (size_t i = 0; i < std::size(targets); ++i) *targets[i] = counts[i];
    irqLine = flags[0];
    fiqLine = flags[1];
    irqHeld = flags[2];
    sleeping = flags[3];
    debugWriteSeen = flags[4];
    lastWake = wake;
    stopped = {};
    undefinedPending = false;
    batchAdvanced = 0;
    leanAdvanced = 0;
    // After the CPU state, so the JIT takes its mapping and permission context from it. Fast starts with its
    // caches cleaned and emptied, over the loaded memory.
    cpuDropTranslatedCode(cpuCore);
    if (fastOn) cpuArm920LeanReset(cpuCore);
    if (clock.changed) clock.changed();
    return true;
}
