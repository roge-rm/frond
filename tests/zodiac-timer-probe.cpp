#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "imx1_boot_probe.h"

namespace {
struct Trace { std::vector<Imx1ProbeAccess> bus; };
void access(void* context, const Imx1ProbeAccess& entry) {
    static_cast<Trace*>(context)->bus.push_back(entry);
}
void load(std::vector<uint32_t>& words, unsigned reg, uint32_t value) {
    words.push_back(0xe59f0000 | reg << 12);
    words.push_back(0xea000000);
    words.push_back(value);
}
Imx1ProbeResult run(const std::vector<uint32_t>& words, Trace& trace, bool fast = false, bool assumeSettled = false, bool assumeSystem = false, uint32_t steps = 30000) {
    Imx1ProbeOptions options{};
    options.bootstrapPeripherals = true;
    options.rtcInitialTimeKnown = true;
    options.assumeMpllSettled = assumeSettled;
    options.assumeSpllSettled = assumeSystem;
    options.busObserver = access;
    options.busContext = &trace;
    return imx1BootProbe(reinterpret_cast<const uint8_t*>(words.data()), words.size() * 4,
                          Imx1ProbeEntry::physicalFlash, steps, nullptr, nullptr, fast, &options);
}
}  // namespace

int main() {
    std::vector<uint32_t> words;
    load(words, 0, 0x00202000);
    words.push_back(0xe3a0101f); words.push_back(0xe5801004);
    load(words, 1, 0x103); words.push_back(0xe5801000);
    for (unsigned i = 0; i < 70; ++i) words.push_back(0xe1a03003);
    words.push_back(0xe5902010); words.push_back(0xe3520004); words.push_back(0x3afffffc);
    words.push_back(0xe7f000f0);
    Trace exact;
    const auto a = run(words, exact);
    assert(a.stop == Imx1ProbeStop::unsupportedInstruction && a.trace.instruction == 0xe7f000f0);
    uint32_t enabledAt = 0, reads = 0;
    for (const auto& entry : exact.bus) {
        assert(entry.status == Imx1BootstrapAccess::ok);
        if (entry.write && entry.address == 0x00202000) enabledAt = entry.step;
        if (!entry.write && entry.address == 0x00202010) {
            assert(entry.value == (entry.step - enabledAt) / 32);
            ++reads;
        }
    }
    assert(reads > 1 && a.registers[2] == 4);
#if FROND_TEST_FAST
    Trace fast;
    const auto b = run(words, fast, true);
    assert(a.stop == b.stop && a.trace.step == b.trace.step && a.cpsr == b.cpsr);
    assert(!std::memcmp(a.registers, b.registers, sizeof(a.registers)));
    assert(exact.bus.size() == fast.bus.size());
    for (size_t i = 0; i < exact.bus.size(); ++i) {
        const auto& x = exact.bus[i]; const auto& y = fast.bus[i];
        assert(x.step == y.step && x.pc == y.pc && x.address == y.address &&
               x.value == y.value && x.write == y.write && x.status == y.status);
    }
    assert(b.jitBlocks && b.jitInstructions);
#endif
    // A clock-mode change must stop before accepting an unqualified time ratio.
    words.clear(); load(words, 1, 0x40000078); words.push_back(0xee011f10);
    Trace mode;
    const auto change = run(words, mode);
    assert(change.stop == Imx1ProbeStop::unsupportedCp15 && change.trace.instruction == 0xee011f10);
    // A failed condition must leave FastBus timing available.
    words[3] = 0x0e011f10; words.push_back(0xe7f000f0);
    Trace skipped;
    const auto skip = run(words, skipped);
    assert(skip.stop == Imx1ProbeStop::unsupportedInstruction && skip.trace.instruction == 0xe7f000f0);
    for (uint32_t unsupportedMode : {0x40000078u, 0x80000078u}) {
        words.clear(); load(words, 1, unsupportedMode); words.push_back(0xee011f10);
        Trace invalid;
        assert(run(words, invalid, false, true).stop == Imx1ProbeStop::unsupportedCp15);
    }
    words.clear(); load(words, 0, 0x0021b000); load(words, 1, 0x57008003);
    words.push_back(0xe5801000); load(words, 1, 0xc0000078); words.push_back(0xee011f10);
    Trace equalFrequency;
    const auto rejected = run(words, equalFrequency, false, true);
    assert(rejected.stop == Imx1ProbeStop::unsupportedCp15 && rejected.trace.instruction == 0xee011f10);
    assert(rejected.registers[15] == rejected.trace.pc && rejected.cpsr == rejected.trace.cpsr);
    words.clear(); load(words, 1, 0xc0000078); words.push_back(0xee011f10);
    words.push_back(0xee114f10); words.push_back(0xe7f000f0);
    Trace resetClock;
    const auto knownReset = run(words, resetClock);
    assert(knownReset.stop == Imx1ProbeStop::unsupportedInstruction && knownReset.registers[4] == 0xc0000078);
    // Program, restart and hand off with a running Timer 1. CP15 readback uses real state.
    words.clear();
    load(words, 0, 0x0021b000); load(words, 1, 0x57000003); words.push_back(0xe5801000);
    load(words, 1, 0x03e717bf); words.push_back(0xe5801004);
    load(words, 1, 0xc0000078); words.push_back(0xee011f10);
    Trace staged;
    assert(run(words, staged, false, true).stop == Imx1ProbeStop::unsupportedCp15);
    words.pop_back();
    load(words, 2, 0x57200003); words.push_back(0xe5802000);
    words.push_back(0xee011f10);
    Trace pending;
    assert(run(words, pending, false, true).stop == Imx1ProbeStop::unsupportedCp15);
    words.pop_back();
    for (unsigned i = 0; i < 6000; ++i) words.push_back(0xe1a03003);
    load(words, 0, 0x00202000); words.push_back(0xe3a02002);
    load(words, 3, 0x0021b020); words.push_back(0xe5832000);
    words.push_back(0xe3a0201f); words.push_back(0xe5802004);
    load(words, 2, 0x103);
    const auto enable = words.size(); words.push_back(0xe5802000);
    words.push_back(0x0e011f10); // Failed EQ leaves FastBus unchanged.
    const auto async = words.size(); words.push_back(0xee011f10);
    words.push_back(0xee114f10); // MRC p15,0,r4,c1,c0,0.
    load(words, 9, 0x0021b810);
    words.push_back(0xe599a000); // Read SCM.GCCR in asynchronous mode.
    words.push_back(0xe3caa001); // Disable USB clock input.
    words.push_back(0xe589a000);
    words.push_back(0xe599b000);
    for (unsigned i = 0; i < 1000; ++i) words.push_back(0xe1a03003);
    load(words, 1, 0x78);
    const auto fastMode = words.size(); words.push_back(0xee011f10);
    words.push_back(0xee115f10);
    for (unsigned i = 0; i < 13; ++i) words.push_back(0xe1a03003);
    load(words, 1, 0xc0000078);
    const auto asyncAgain = words.size(); words.push_back(0xee011f10);
    for (unsigned i = 0; i < 173; ++i) words.push_back(0xe1a03003);
    const auto readCounter = words.size(); words.push_back(0xe5906010);
    words.push_back(0xe3a07020); words.push_back(0xe2577001); words.push_back(0x1afffffd);
    words.push_back(0xe5908010); words.push_back(0xe7f000f0);
    Trace defaultClock;
    const auto oldStop = run(words, defaultClock);
    assert(oldStop.stop == Imx1ProbeStop::unsupportedCp15 && oldStop.trace.pc == 0x10000000 + async * 4);
    Trace asyncExact;
    const auto nominal = run(words, asyncExact, false, true);
    assert(nominal.stop == Imx1ProbeStop::unsupportedInstruction && nominal.trace.instruction == 0xe7f000f0);
    assert(nominal.registers[4] == 0xc0000078 && nominal.registers[5] == 0x78);
    assert(nominal.registers[10] == 0x2e && nominal.registers[11] == 0x2e);
    // Literal loads branch over data words; charge each handoff with its selected mode.
    uint64_t integerCycles = 0, asyncCycles = 0;
    // All indices here follow setup; load() contributes two executed instructions for three words.
    integerCycles = async - enable + (asyncAgain - fastMode - 1);
    asyncCycles = (fastMode - async - 2) + (readCounter - asyncAgain);
    // Each interval with load() skips one literal word.
    uint32_t dividerStep = 0, enabledStep = 0;
    for (const auto& entry : asyncExact.bus) {
        if (entry.write && entry.address == 0x0021b020) dividerStep = entry.step;
        if (entry.write && entry.address == 0x00202000) enabledStep = entry.step;
    }
    const auto initialPeripheralPhase = (enabledStep - dividerStep) % 3;
    const auto expectedCounter = (initialPeripheralPhase + integerCycles + asyncCycles * 375000 / 762752) / 96;
    assert(nominal.registers[6] == expectedCounter);
    assert(nominal.registers[7] == 0 && nominal.registers[8] >= nominal.registers[6]);
#if FROND_TEST_FAST
    Trace asyncFast;
    const auto nominalFast = run(words, asyncFast, true, true);
    assert(nominal.stop == nominalFast.stop && nominal.trace.step == nominalFast.trace.step);
    assert(nominal.cpsr == nominalFast.cpsr && nominal.spsr == nominalFast.spsr);
    assert(!std::memcmp(nominal.registers, nominalFast.registers, sizeof(nominal.registers)));
    assert(asyncExact.bus.size() == asyncFast.bus.size());
    for (size_t i = 0; i < asyncExact.bus.size(); ++i) {
        const auto& x = asyncExact.bus[i]; const auto& y = asyncFast.bus[i];
        assert(x.step == y.step && x.pc == y.pc && x.address == y.address &&
               x.value == y.value && x.write == y.write && x.status == y.status);
    }
    assert(nominalFast.jitBlocks && nominalFast.jitInstructions);
#endif
    // System PLL restart uses independent MCU elapsed time while Timer 1 output is held high.
    words.resize(async + 1);
    load(words, 9, 0x0021b000);
    load(words, 1, 0x123638ad); words.push_back(0xe589100c);
    words.push_back(0xe599a010);
    load(words, 1, 0x57400003); words.push_back(0xe5891000);
    words.push_back(0xe599b010);
    words.push_back(0xe5906010);
    words.push_back(0xe5992000); words.push_back(0xe3120501); words.push_back(0x1afffffc);
    words.push_back(0xe5907010); words.push_back(0xe5998010);
    for (unsigned i = 0; i < 300; ++i) words.push_back(0xe1a03003);
    words.push_back(0xe590c010); words.push_back(0xe7f000f0);
    Trace strictSystem;
    const auto systemStop = run(words, strictSystem, false, true);
    assert(systemStop.stop == Imx1ProbeStop::unsupportedPeripheral && systemStop.address == 0x0021b00c);
    Trace systemExact;
    const auto system = run(words, systemExact, false, true, true);
    assert(system.stop == Imx1ProbeStop::unsupportedInstruction && system.trace.instruction == 0xe7f000f0);
    assert(system.registers[10] == 0x8000 && system.registers[11] == 0x8000 && system.registers[8] == 0x8000);
    assert(system.registers[7] >= system.registers[6] && system.registers[7] <= system.registers[6] + 1);
    assert(system.registers[12] > system.registers[7]);
#if FROND_TEST_FAST
    Trace systemFastTrace;
    const auto systemFast = run(words, systemFastTrace, true, true, true);
    assert(system.stop == systemFast.stop && system.trace.step == systemFast.trace.step);
    assert(system.cpsr == systemFast.cpsr && system.spsr == systemFast.spsr);
    assert(!std::memcmp(system.registers, systemFast.registers, sizeof(system.registers)));
    assert(systemExact.bus.size() == systemFastTrace.bus.size());
    for (size_t i = 0; i < systemExact.bus.size(); ++i) {
        const auto& x = systemExact.bus[i]; const auto& y = systemFastTrace.bus[i];
        assert(x.step == y.step && x.pc == y.pc && x.address == y.address &&
               x.value == y.value && x.write == y.write && x.status == y.status);
    }
    assert(systemFast.jitBlocks && systemFast.jitInstructions);
#endif
    // A compare request remains pending while CPSR I masks CPU delivery.
    words.clear();
    load(words, 0, 0x0021b000); load(words, 1, 0x57000003); words.push_back(0xe5801000);
    words.push_back(0xe3a01000); words.push_back(0xe5801020);
    load(words, 0, 0x00223000); words.push_back(0xe3a0103a); words.push_back(0xe5801008);
    load(words, 0, 0x00203000); words.push_back(0xe3a0100a); words.push_back(0xe5801008);
    words.push_back(0xe3a01013); words.push_back(0xe5801000);
    const auto afterEnable = words.size();
    for (unsigned i = 0; i < 20; ++i) words.push_back(0xe1a03003);
    const auto maskedStopPc = 0x10000000 + words.size() * 4;
    words.push_back(0xe7f000f0);
    Trace irqTrace;
    const auto irq = run(words, irqTrace);
    assert(irq.stop == Imx1ProbeStop::unsupportedInstruction && !irq.elapsedInterruptBoundary);
    assert(irq.peripheralStatus == Imx1BootstrapAccess::ok && !irq.irqEntries && !irq.fiqEntries);
    assert(irq.trace.pc == maskedStopPc && irq.trace.pc > 0x10000000 + (afterEnable + 8) * 4);
    assert(irq.registers[15] == irq.trace.pc && (irq.cpsr & 31) == 0x13);
    assert(irq.address == 0 && irq.size == 0 && irq.value == 0);
    assert(irq.interruptPending.normal == uint64_t(1) << 58 && irq.interruptPending.fast == 0);
    assert(irq.interruptPending.unknownNormal == 0 && irq.interruptPending.unknownFast == 0);
    for (const auto& entry : irqTrace.bus)
        assert(entry.address != 0x00217010 && entry.step < irq.trace.step);
#if FROND_TEST_FAST
    Trace irqFastTrace;
    const auto irqFast = run(words, irqFastTrace, true);
    assert(!irqFast.elapsedInterruptBoundary && irqFast.trace.step == irq.trace.step && !irqFast.irqEntries);
    assert(irqFast.interruptPending.normal == irq.interruptPending.normal);
    assert(irqFast.interruptPending.fast == 0 && irqFast.busAccesses == irq.busAccesses);
    assert(irqFastTrace.bus.size() == irqTrace.bus.size());
    assert(!std::memcmp(irqFast.registers, irq.registers, sizeof(irq.registers)));
#endif
    // A sampling event during the pre-fetch SPLL stall remains pending while I is set.
    words.clear();
    load(words, 0, 0x00223000); words.push_back(0xe3a01012); words.push_back(0xe5801008);
    load(words, 0, 0x00204000); load(words, 1, 0x8000); words.push_back(0xe5801018);
    for (unsigned i = 0; i < 15200; ++i) words.push_back(0xe1a03003);
    load(words, 0, 0x0021b000); load(words, 1, 0x57000003); words.push_back(0xe5801000);
    load(words, 1, 0x03e717bf); words.push_back(0xe5801004);
    load(words, 1, 0x57200003); words.push_back(0xe5801000);
    for (unsigned i = 0; i < 6000; ++i) words.push_back(0xe1a03003);
    load(words, 1, 0xc0000078); words.push_back(0xee011f10);
    load(words, 1, 0x123638ad); words.push_back(0xe580100c);
    load(words, 1, 0x57400003);
    const auto restartPc = 0x10000000 + words.size() * 4;
    words.push_back(0xe5801000);
    words.push_back(0xe3a07055);
    words.push_back(0xe7f000f0);
    Trace stallTrace;
    const auto stalled = run(words, stallTrace, false, true, true);
    assert(stalled.stop == Imx1ProbeStop::unsupportedInstruction && !stalled.elapsedInterruptBoundary);
    assert(stalled.trace.pc == restartPc + 8 && stalled.registers[15] == restartPc + 8);
    assert(stalled.registers[7] == 0x55 && stalled.cpsr == stalled.trace.cpsr && !stalled.irqEntries);
    assert(stalled.address == 0 && stalled.size == 0 && stalled.value == 0);
    assert(stalled.interruptPending.normal == uint64_t(1) << 18 && stalled.interruptPending.fast == 0);
    assert(stallTrace.bus.back().pc == restartPc && stallTrace.bus.back().value == 0x57400003);
    assert(!stalled.possibleWatchdogTimeoutBoundary);
    // Possible watchdog expiry is a sticky uncertainty boundary, with no invented IRQ/reset.
    words.clear(); load(words, 0, 0x00201000); load(words, 1, 0x0202);
    words.push_back(0xe5801000); words.push_back(0xe2811001); words.push_back(0xe5801000);
    const auto loopPc = 0x10000000 + words.size() * 4;
    words.push_back(0xeafffffe);
    Trace watchdogTrace;
    const auto watchdog = run(words, watchdogTrace, false, false, false, 4200000);
    assert(watchdog.stop == Imx1ProbeStop::unsupportedPeripheral && watchdog.possibleWatchdogTimeoutBoundary);
    assert(watchdog.peripheralStatus == Imx1BootstrapAccess::possibleWatchdogTimeout);
    assert(!watchdog.elapsedInterruptBoundary && watchdog.interruptPending.normal == 0);
    assert(watchdog.address == 0 && watchdog.size == 0 && watchdog.value == 0);
    assert(watchdog.trace.pc == loopPc && watchdog.registers[15] == loopPc && (watchdog.cpsr & 31) == 0x13);
    assert(watchdogTrace.bus.size() == 2 && watchdogTrace.bus.back().value == 0x0203);
    std::puts("PASS: Timer 1 advances between instructions, polling sees elapsed cycles and clock modes stay explicit");
}
