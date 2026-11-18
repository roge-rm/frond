#include "s3c2442_machine.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CPU.h"
#include "RAM.h"
#include "mem.h"
#include "memory_buffer.h"
#include "pace_patch.h"
#include "patch_dispatch.h"
#include "system_state.h"

namespace {
    constexpr uint32_t sdramBase = 0x30000000, bank6Size = 0x08000000;
    // Most instructions one interpreter call runs, so timers and interrupts are seen this often.
    constexpr uint32_t batch = 64;

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
    undefinedSeen = cpuUndefinedTaken(cpuCore, nullptr, nullptr);
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
        case S3c2442Stop::powerOff: return "CLKCON POWER_OFF set, sleep mode isn't modelled yet";
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
                stopped.stop = S3c2442Stop::powerOff;
                stopped.pc = cpuGetCurInstrPC(cpuCore);
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
            if (write) result = size == 4 ? adc.write(offset, value) : S3cAccess::unsupported;
            else readWord([&](uint32_t o, uint32_t& v) { return adc.read(o, v); });
            break;
        case 0x4b000000:
            name = "DMA";
            if (write) result = size == 4 ? dma.write(offset, value) : S3cAccess::unsupported;
            if (result == S3cAccess::unsupported && size == 4) detail = "software-triggered DMA isn't modelled yet";
            else readWord([&](uint32_t o, uint32_t& v) { return dma.read(o, v); });
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
        updateLines();
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

void S3c2442Machine::updateLines() {
    const bool irq = intc.irq(), fiq = intc.fiq();
    if (irq != irqLine) { cpuIrq(cpuCore, false, irq); irqLine = irq; }
    if (fiq != fiqLine) { cpuIrq(cpuCore, true, fiq); fiqLine = fiq; }
}

// Moves every clock domain on by CPU cycles at the CPU clock as it is now.
void S3c2442Machine::advance(uint64_t cpuCycles) {
    const uint64_t cpuHz = clock.cpuClock();
    if (!cpuHz || !cpuCycles) return;
    cycleCount += cpuCycles;
    elapsedNs += mulDiv(cpuCycles, 1000000000, cpuHz, nsRest);
    const uint64_t pclk = mulDiv(cpuCycles, clock.pclk(), cpuHz, pclkRest);
    const uint64_t hclk = mulDiv(cpuCycles, clock.hclk(), cpuHz, hclkRest);
    const uint64_t ticks32k = mulDiv(cpuCycles, 32768, cpuHz, rtcRest);
    if (pclk) {
        pwm.advance(pclk, intc);
        watchdog.advance(pclk, intc);
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
    const uint64_t cpuHz = clock.cpuClock();
    uint64_t best = UINT64_MAX;
    auto consider = [&](uint64_t ticks, uint64_t hz, uint64_t rest) {
        if (ticks == UINT64_MAX || !hz) return;
        // Cycles c with (c * hz + rest) / cpuHz >= ticks.
        const __uint128_t need = __uint128_t(ticks) * cpuHz;
        const uint64_t cycles = need <= rest ? 1 : uint64_t((need - rest + hz - 1) / hz);
        best = std::min(best, std::max<uint64_t>(cycles, 1));
    };
    if (!wakeOnly) {
        consider(std::min(pwm.ticksToEvent(), watchdog.ticksToEvent()), clock.pclk(), pclkRest);
        consider(lcd.ticksToEvent(), clock.hclk(), hclkRest);
        consider(rtc.ticksToEvent(), 32768, rtcRest);
        return best;
    }
    auto open = [&](unsigned source) { return !(intc.intmsk & (1u << source)); };
    const uint32_t timers = (~intc.intmsk >> s3cIrq::timer0) & 0x1f;
    const bool wdtInterrupt = open(s3cIrq::wdtAc97) && !(intc.intsubmsk & (1u << s3cSubIrq::wdt));
    consider(std::min(pwm.ticksToEvent(timers), watchdog.ticksToEvent(wdtInterrupt)), clock.pclk(), pclkRest);
    if (open(s3cIrq::lcd)) consider(lcd.ticksToEvent(true), clock.hclk(), hclkRest);
    consider(rtc.ticksToEvent(open(s3cIrq::tick), open(s3cIrq::rtc)), 32768, rtcRest);
    return best;
}

S3c2442Stop S3c2442Machine::run(uint64_t instructions) {
    if (stopped.stop == S3c2442Stop::budget) stopped.stop = S3c2442Stop::running;
    if (stopped.stop != S3c2442Stop::running) return stopped.stop;
    const uint64_t target = retiredCount + idleCount + instructions;
    while (stopped.stop == S3c2442Stop::running) {
        const uint64_t used = retiredCount + idleCount;
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
        const uint64_t n = history.empty() ? std::min<uint64_t>({batch, cyclesToEvent(), target - used}) : 1;
        const uint32_t stepPc = pc();
        if (!history.empty()) {
            history[historyNext] = stepPc;
            historyNext = (historyNext + 1) % history.size();
        }
        const uint32_t ran = cpuCycle<ARM_MEMORY_SYSTEM_ARM920T, false>(cpuCore, uint32_t(std::max<uint64_t>(n, 1)));
        if (!history.empty() && stepHook) stepHook(stepPc);
        retiredCount += ran;
        advance(ran ? ran : 1);
        const auto guard = cpuArm920DataGuard(cpuCore);
        if (guard.active && stopped.stop == S3c2442Stop::running) {
            stopped.stop = S3c2442Stop::dataCache;
            stopped.address = guard.address;
            stopped.size = uint8_t(guard.size);
            stopped.write = guard.write;
            stopped.pc = cpuGetCurInstrPC(cpuCore);
        }
        // An undefined instruction stops the run, as the CPU also takes one for forms it doesn't model. Two
        // are the guest's own and go to its handler, as on the real ARM920T: VFP (coprocessors 10 and 11),
        // which the kernel probes for and the chip doesn't have, and Windows CE's breakpoint 0xe6000010
        // (DebugBreak), which the kernel takes as a breakpoint exception.
        uint32_t undefinedPc = 0, undefinedInstr = 0;
        const uint64_t taken = cpuUndefinedTaken(cpuCore, &undefinedPc, &undefinedInstr);
        const bool coprocessor = ((undefinedInstr >> 24) & 0xf) == 0xe || ((undefinedInstr >> 25) & 7) == 6;
        const bool vfpProbe = undefinedInstr && coprocessor && ((undefinedInstr >> 9) & 7) == 5;
        if (taken > undefinedSeen && (vfpProbe || undefinedInstr == 0xe6000010)) {
            undefinedSeen = taken;
            ++(vfpProbe ? vfpProbes : breakpoints);
        }
        if (taken > undefinedSeen && stopped.stop == S3c2442Stop::running) {
            undefinedSeen = taken;
            stopped.stop = S3c2442Stop::undefined;
            stopped.pc = undefinedPc;
            stopped.value = undefinedInstr;
        }
    }
    return stopped.stop;
}

std::vector<uint32_t> S3c2442Machine::recentPcs() const {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < history.size(); ++i) out.push_back(history[(historyNext + i) % history.size()]);
    return out;
}
