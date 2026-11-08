#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "imx1_boot_probe.h"

namespace {
using A = Imx1BootstrapAccess;
constexpr uint32_t rtc = 0x00204000, wdog = 0x00201000, aitc = 0x00223000;
uint32_t read(Imx1Bootstrap& b, uint32_t address) {
    uint32_t value = 0xdeadbeef;
    assert(b.access(address, 4, false, value) == A::ok);
    return value;
}
void write(Imx1Bootstrap& b, uint32_t address, uint32_t value, A expected = A::ok) {
    const auto old = value;
    const auto result = b.access(address, 4, true, value);
    if (result != expected) std::fprintf(stderr, "%08x=%08x: %s expected %s\n", address, old,
        imx1BootstrapAccessName(result), imx1BootstrapAccessName(expected));
    assert(result == expected && value == old);
}
void start(Imx1Bootstrap& b, bool connected = true) {
    assert(b.setRtcInitialTime({1, 23, 59, 59}));
    if (connected) b.connectCpuInterrupts();
    write(b, rtc + 16, 0xe0);
    write(b, wdog, 0x7f02); write(b, wdog, 0x7f03);
}
void liveWritesAndDeadline() {
    Imx1Bootstrap a; start(a);
    a.advanceFastBusCycles(124); // Fractional CLK32 phase before the live writes.
    auto b = a;
    write(a, rtc + 24, 0xff3e);
    write(a, rtc + 8, 0x0102); write(a, rtc + 12, 3); write(a, rtc + 36, 2);
    a.advanceFastBusCycles(32768 * 250 - 124);
    b.advanceFastBusCycles(127);
    b.advanceFastBusCycles(32768 * 250 - 251);
    assert(read(a, rtc) == 0 && read(a, rtc + 4) == 0 && read(a, rtc + 32) == 2);
    for (unsigned offset : {0u, 4u, 16u, 32u}) assert(read(a, rtc + offset) == read(b, rtc + offset));
    assert(read(a, rtc + 20) == 0xff3a); // Midnight rollover, no alarm match.
    write(a, rtc + 24, 0); assert(read(a, rtc + 20) == 0xff3a);
    write(a, rtc + 20, 0xff3a); assert(read(a, rtc + 20) == 0);
    write(a, rtc + 24, 8); // Whole native day-mask store with the watchdog still active.
    write(a, rtc + 16, 0xe0);
    for (uint32_t control : {0x80u, 0xa0u, 0u, 0xe1u})
        write(a, rtc + 16, control, A::sideEffect);
    for (unsigned offset : {0u, 4u, 32u}) write(a, rtc + offset, read(a, rtc + offset), A::sideEffect);
    write(a, rtc + 24, 0x88, A::sideEffect); assert(read(a, rtc + 24) == 8);
    write(a, rtc + 24, 0x48, A::reservedBits); assert(read(a, rtc + 24) == 8);
    const uint32_t remaining = (126 * 16384 - 32768) * 250;
    a.advanceFastBusCycles(remaining - 1); b.advanceFastBusCycles(remaining - 1);
    assert(a.clockStatus() == A::ok && b.clockStatus() == A::ok);
    a.advanceFastBusCycles(1); b.advanceFastBusCycles(1);
    assert(a.clockStatus() == A::possibleWatchdogTimeout && b.clockStatus() == A::possibleWatchdogTimeout);
    // An admitted RTC write must not extend an almost-expired bound or the service prefix.
    Imx1Bootstrap near; start(near);
    near.advanceFastBusCycles(126 * 16384 * 250 - 1);
    write(near, wdog + 4, 0x5555); write(near, rtc + 24, 8);
    write(near, wdog + 4, 0xaaaa);
    near.advanceFastBusCycles(126 * 16384 * 250 - 1); assert(near.clockStatus() == A::ok);
    write(near, rtc + 20, 8); near.advanceFastBusCycles(1);
    assert(near.clockStatus() == A::possibleWatchdogTimeout);
    Imx1Bootstrap unknown;
    write(unknown, wdog, 0x7f02); write(unknown, wdog, 0x7f03);
    write(unknown, rtc + 24, 8, A::externalInput); assert(read(unknown, rtc + 24) == 0);
    std::puts("PASS: live RTC masks/status/alarm preserve RTC phase, future watchdog deadline and service sequence");
}
void pendingAndAtomicity() {
    for (bool fiq : {false, true}) {
        Imx1Bootstrap b; start(b);
        if (fiq) write(b, aitc + 0x1c, 1u << 17);
        write(b, rtc + 24, 8); write(b, rtc + 16, 0xe0); write(b, aitc + 8, 17);
        b.advanceFastBusCycles(32768 * 250);
        assert(read(b, rtc + 20) == 8);
        assert((fiq ? b.interruptLines().fiq : b.interruptLines().irq) == Imx1AitcLine::asserted);
        write(b, rtc + 24, 0); assert(read(b, rtc + 20) == 8);
        assert(b.interruptLines().irq == Imx1AitcLine::clear && b.interruptLines().fiq == Imx1AitcLine::clear);
        write(b, rtc + 24, 8);
        const auto pending = b.interruptPending();
        assert((fiq ? pending.fast : pending.normal) == (uint64_t(1) << 17));
        write(b, rtc + 20, 8); assert(read(b, rtc + 20) == 0);
        assert(b.interruptLines().irq == Imx1AitcLine::clear && b.interruptLines().fiq == Imx1AitcLine::clear);
    }
    Imx1Bootstrap alarm; start(alarm);
    write(alarm, rtc + 8, 0); write(alarm, rtc + 12, 0); write(alarm, rtc + 36, 2);
    write(alarm, rtc + 24, 12); write(alarm, rtc + 16, 0xe0); write(alarm, aitc + 8, 17);
    alarm.advanceFastBusCycles(32768 * 250);
    assert(read(alarm, rtc + 20) == 12 && alarm.interruptLines().irq == Imx1AitcLine::asserted);
    write(alarm, rtc + 20, 8); assert(read(alarm, rtc + 20) == 4);
    write(alarm, rtc + 24, 8); assert(alarm.interruptLines().irq == Imx1AitcLine::clear);
    write(alarm, rtc + 20, 4); assert(read(alarm, rtc + 20) == 0);
    Imx1Bootstrap guarded; start(guarded, false);
    write(guarded, aitc + 8, 17); write(guarded, rtc + 24, 8);
    guarded.advanceFastBusCycles(32768 * 250);
    write(guarded, rtc + 24, 0); assert(read(guarded, rtc + 20) == 8);
    write(guarded, rtc + 24, 8, A::sideEffect);
    assert(read(guarded, rtc + 24) == 0 && read(guarded, rtc + 20) == 8);
    // A masked unknown AITC source becomes eligible only in the prospective write.
    Imx1Bootstrap unknown; start(unknown);
    write(unknown, aitc + 4, 30); write(unknown, aitc + 8, 0);
    write(unknown, rtc + 24, 8);
    write(unknown, aitc + 4, 31, A::externalInput);
    assert(read(unknown, aitc + 4) == 30 && read(unknown, rtc + 24) == 8);
    std::puts("PASS: DAY IRQ17/FIQ17, masking, retained pending, W1C and transactional RTC/AITC guards");
}
struct Trace { std::vector<Imx1ProbeAccess> bus; std::vector<Imx1ProbeInterrupt> irq; };
void load(std::vector<uint32_t>& w, unsigned reg, uint32_t value) {
    w.push_back(0xe59f0000 | reg << 12); w.push_back(0xea000000); w.push_back(value);
}
void store(std::vector<uint32_t>& w, uint32_t offset, uint32_t value) {
    load(w, 1, value); w.push_back(0xe5801000 | offset);
}
Imx1ProbeResult probe(const std::vector<uint32_t>& w, Trace& t, bool fast) {
    Imx1ProbeOptions o{}; o.bootstrapPeripherals = true; o.rtcInitialTimeKnown = true;
    o.busObserver = [](void* p, const Imx1ProbeAccess& a) { static_cast<Trace*>(p)->bus.push_back(a); };
    o.busContext = &t;
    o.interruptObserver = [](void* p, const Imx1ProbeInterrupt& a) { static_cast<Trace*>(p)->irq.push_back(a); };
    o.interruptContext = &t;
    return imx1BootProbe(reinterpret_cast<const uint8_t*>(w.data()), w.size() * 4,
        Imx1ProbeEntry::resetAlias, 200000, nullptr, nullptr, fast, &o);
}
void cpuDelivery() {
    for (bool fiq : {false, true}) for (bool thumb : {false, true}) {
        std::vector<uint32_t> w(0x100 / 4, 0xe1a00000);
        w[0] = 0xea00003e; w[6] = 0xea0000f8; w[7] = 0xea0000f7;
        load(w, 0, rtc); store(w, 16, 0xe0);
        load(w, 0, wdog); store(w, 0, 0x7f02); store(w, 0, 0x7f03);
        load(w, 0, rtc); store(w, 24, 8); store(w, 16, 0xe0);
        load(w, 0, aitc); store(w, 8, 17);
        load(w, 0, aitc); if (fiq) store(w, 0x1c, 1u << 18); store(w, 8, 18);
        load(w, 0, rtc); store(w, 24, 0x8000);
        w.push_back(0xe3a07000); w.push_back(0xe321f013);
        load(w, 5, 20000);
        if (thumb) {
            load(w, 2, uint32_t(w.size() * 4 + 17)); w.push_back(0xe12fff12);
            w.push_back(0xd1fd3d01); // SUBS r5,#1; BNE to SUBS.
            w.push_back(0xdefe46c0);
        } else {
            w.push_back(0xe2555001); w.push_back(0x1afffffd); w.push_back(0xe7f000f0);
        }
        w.resize(0x400 / 4, 0xe1a00000);
        load(w, 9, rtc); load(w, 10, 0x8000);
        w.push_back(0xe5993014); w.push_back(0xe589a014); // Read pending and W1C.
        w.push_back(0xe3a0a000); w.push_back(0xe589a018); // Disable future samples.
        w.push_back(0xe5994014); w.push_back(0xe2877001); w.push_back(0xe25ef004);
        Trace a; const auto exact = probe(w, a, false);
        assert(exact.stop == Imx1ProbeStop::unsupportedInstruction && exact.irqEntries == !fiq && exact.fiqEntries == fiq);
        assert(exact.registers[7] == 1 && exact.registers[3] == 0x8000 && exact.registers[4] == 0);
        assert(a.irq.size() == 1 && a.irq[0].fiq == fiq && a.irq[0].link == a.irq[0].interruptedPc + 4);
        assert(bool(exact.cpsr & 0x20) == thumb);
#if FROND_TEST_FAST
        Trace b; const auto fast = probe(w, b, true);
        assert(exact.stop == fast.stop && exact.trace.step == fast.trace.step && exact.cpsr == fast.cpsr);
        assert(exact.spsr == fast.spsr && exact.irqEntries == fast.irqEntries && exact.fiqEntries == fast.fiqEntries);
        assert(!std::memcmp(exact.registers, fast.registers, sizeof(exact.registers)));
        assert(a.bus.size() == b.bus.size() && a.irq.size() == b.irq.size());
        for (size_t n = 0; n < a.bus.size(); ++n) {
            const auto& x = a.bus[n]; const auto& y = b.bus[n];
            assert(x.step == y.step && x.pc == y.pc && x.address == y.address && x.value == y.value &&
                x.write == y.write && x.status == y.status);
        }
        assert(a.irq[0].step == b.irq[0].step && a.irq[0].interruptedPc == b.irq[0].interruptedPc);
        assert(fast.jitBlocks && fast.jitInstructions);
#endif
    }
    std::puts("PASS: active-watchdog RTC sampling IRQ/FIQ, ARM/Thumb return and W1C in Exact/Fast");
}
}
int main() { liveWritesAndDeadline(); pendingAndAtomicity(); cpuDelivery(); }
