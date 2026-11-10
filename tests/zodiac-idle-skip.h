#ifndef FROND_TEST_ZODIAC_IDLE_SKIP_H
#define FROND_TEST_ZODIAC_IDLE_SKIP_H
// Included by zodiac-machine.cpp after its guest helpers. Runs idle-heavy guests with the
// stepped loop and with the skip, and compares every result, status and observer event.
#include <array>
#include <chrono>
#include "machine_state_archive.h"
#include "zodiac-graphics-setup.h"

namespace idle_skip {
using A = Imx1BootstrapAccess;
struct Log {
    std::vector<uint64_t> words;
    void add(std::initializer_list<uint64_t> values) { words.insert(words.end(), values); }
    void time(const Imx1ReferenceTime& t) { add({t.ticks, t.numerator, t.denominator}); }
    void status(const Imx1MachineCheapStatus& s) {
        add({uint64_t(s.status), uint64_t(s.stop), s.retired, s.work, s.idleHclk, s.cs5WaitClocks, s.pc});
        time(s.time);
    }
    void run(const Imx1MachineRun& r) {
        const auto& x = r.result;
        add({uint64_t(r.status), r.retiredInstructions, uint64_t(x.stop), x.trace.step, x.trace.pc, x.trace.instruction,
             x.trace.cpsr, x.trace.operand, x.trace.instructionSize, x.address, x.value, x.size, x.cpsr, x.spsr,
             x.jitBlocks, x.jitInstructions, x.busAccesses, x.cs5WaitClocks, x.idleHclk, x.graphicsFrameWidth,
             x.graphicsFrameHeight, x.graphicsFrameHash, x.graphicsLcdEnabled, x.irqEntries, x.fiqEntries,
             uint64_t(x.peripheralStatus), x.gpioEventApplied, x.elapsedInterruptBoundary,
             x.possibleWatchdogTimeoutBoundary, x.interruptPending.normal, x.interruptPending.fast,
             x.interruptPending.unknownNormal, x.interruptPending.unknownFast, x.pcm.remaining, x.pcm.transmittedWords});
        for (auto value : x.registers) add({value});
    }
    void bounded(const Imx1MachineBoundedRun& r) {
        status(r.total);
        add({uint64_t(r.reason), r.retiredDelta, r.workDelta, r.workOvershoot, r.timeOvershoot.ticks,
             uint64_t(r.timeOvershoot.numerator), uint64_t(r.timeOvershoot.numerator >> 64),
             uint64_t(r.timeOvershoot.denominator), uint64_t(r.timeOvershoot.denominator >> 64)});
        time(r.elapsed);
    }
};
struct Sink {
    static void word(void* p, const char* name, const uint32_t* data, uint32_t count) {
        auto& log = *static_cast<Log*>(p);
        for (const char* n = name; *n; ++n) log.add({uint8_t(*n)});
        // RAM is hashed by the byte sink; rows are short.
        for (uint32_t i = 0; i < count; ++i) log.add({data[i]});
    }
    static void bytes(void* p, const char*, const uint8_t* data, uint32_t count) {
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t i = 0; i < count; ++i) hash = (hash ^ data[i]) * 1099511628211ull;
        static_cast<Log*>(p)->add({count, hash});
    }
    static void counters(void* p, const char*, const uint64_t* data, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) static_cast<Log*>(p)->add({data[i]});
    }
};
enum class Script { plain, slices, bounded, gpio };
struct Outcome { Log log; uint64_t idle = 0, advances = 0, irqs = 0; double ms = 0; Imx1ProbeStop stop{}; A status{}; };

inline Outcome execute(const std::vector<uint32_t>& words, Imx1ProbeOptions base, bool stepped, bool fast, Script script) {
    Outcome out;
    machine_test::Capture capture;
    base.idleStepped = stepped;
    capture.original = base;
    const auto options = capture.options();
    auto* m = imx1MachineCreate(reinterpret_cast<const uint8_t*>(words.data()), words.size() * 4,
        Imx1ProbeEntry::resetAlias, fast, options, machine_test::Capture::trace, &capture);
    assert(m);
    const auto begin = std::chrono::steady_clock::now();
    const auto slice = [&](uint64_t idleLimit) {
        const auto run = imx1MachineRunSlice(m, 100000, idleLimit);
        out.log.run(run); out.log.status(imx1MachineGetStatus(m)); out.log.add({capture.hash, capture.count});
        return run.status;
    };
    if (script == Script::slices) {
        // Cumulative idle caps that land in the middle of a wait.
        for (uint64_t limit : {uint64_t(1), uint64_t(2), uint64_t(3), uint64_t(1000), uint64_t(77777),
                               uint64_t(300001), uint64_t(1234567)})
            if (slice(limit) == Imx1MachineStatus::terminal) break;
    } else if (script == Script::bounded) {
        // Work and deadline bounds of many sizes, most of them inside a wait.
        constexpr uint64_t work[] = {10000000, 1, 7, 4096, 99991, 1000003};
        for (unsigned i = 0; i < 4000; ++i) {
            const auto before = imx1MachineGetStatus(m);
            if (before.status == Imx1MachineStatus::terminal) break;
            Imx1ReferenceTime deadline{UINT64_MAX, 0, 1};
            if (i % 2) { deadline = before.time; assert(deadline.add(uint64_t(i % 7 + 1) * 12345677, 1000003)); }
            const auto run = imx1MachineRunBounded(m, {50000, deadline, work[i % 6]});
            assert(run.reason != Imx1MachineYield::invalidBounds);
            out.log.bounded(run); out.log.add({capture.hash, capture.count});
        }
    } else if (script == Script::gpio) {
        // Stop inside the first wait, then let a pin wake the CPU before the timer does.
        assert(slice(150000) == Imx1MachineStatus::yielded);
        const uint32_t values[4]{}, masks[4]{0x80, 0, 0, 0};
        assert(imx1MachineReplaceGpioInputs(m, values, masks) == A::ok);
    }
    for (unsigned i = 0; i < 64 && slice(UINT64_MAX) != Imx1MachineStatus::terminal; ++i) {}
    out.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    const auto last = imx1MachineInspect(m);
    assert(last.status == Imx1MachineStatus::terminal);
    out.log.run(last);
    imx1MachineEmitDiagnostic(m, {{&out.log, Sink::word, Sink::bytes}, Sink::counters, &out.log});
    out.log.add({capture.hash, capture.count});
    out.idle = last.result.idleHclk; out.irqs = last.result.irqEntries;
    out.advances = imx1MachineIdleAdvances(m);
    out.stop = last.result.stop; out.status = last.result.peripheralStatus;
    imx1MachineStop(m);
    return out;
}
// Runs one guest stepped and skipped and returns the skipped outcome.
inline Outcome compare(const char* name, const std::vector<uint32_t>& words, const Imx1ProbeOptions& options,
                       bool fast, Script script, bool timing = false) {
    const auto stepped = execute(words, options, true, fast, script);
    const auto skipped = execute(words, options, false, fast, script);
    if (stepped.log.words != skipped.log.words) {
        size_t at = 0;
        while (at < stepped.log.words.size() && at < skipped.log.words.size() &&
               stepped.log.words[at] == skipped.log.words[at]) ++at;
        std::fprintf(stderr, "%s: stepped and skipped differ at log word %zu of %zu/%zu\n", name, at,
                     stepped.log.words.size(), skipped.log.words.size());
        assert(false);
    }
    assert(stepped.idle == skipped.idle && skipped.idle);
    // The stepped loop passes once per clock. The skip has to engage: at most a quarter as many passes.
    if (stepped.advances != stepped.idle || skipped.advances * 4 > skipped.idle) {
        std::fprintf(stderr, "%s: %llu idle HCLK in %llu stepped and %llu skipped passes\n", name,
                     (unsigned long long)skipped.idle, (unsigned long long)stepped.advances, (unsigned long long)skipped.advances);
        assert(false);
    }
    if (timing)
        std::printf("TIMING: %s, %llu idle HCLK: stepped %.1f ms, skipped %.2f ms, %.0f times faster\n", name,
                    (unsigned long long)skipped.idle, stepped.ms, skipped.ms, stepped.ms / (skipped.ms > 0.001 ? skipped.ms : 0.001));
    return skipped;
}

// Timer 2, RTC sampling and a GPIO A pin 7 falling edge wake a loop of waits. The handler acknowledges all three.
inline std::vector<uint32_t> wakeGuest(uint32_t compare, uint32_t rtcEnable, uint32_t waits) {
    auto words = setup();
    load(words, 0, 0x0021c000); store(words, 0, 0x20, 0x80); store(words, 0, 0x28, 1u << 14); store(words, 0, 0x30, 0x80);
    load(words, 0, 0x00223000); store(words, 0, 8, 11);
    if (compare) store(words, 0, 8, 58);
    if (rtcEnable) store(words, 0, 8, 18);
    if (compare) { load(words, 0, 0x00203000); store(words, 0, 8, compare); store(words, 0, 0, 0x13); }
    if (rtcEnable) { load(words, 0, 0x00204000); store(words, 0, 0x18, rtcEnable); }
    words.push_back(0xe3a07000); words.push_back(0xe321f013);
    words.push_back(0xee070f90);
    words.push_back(0xe3570000 | waits); words.push_back(0xbafffffc);
    // Read back the counters the wait advanced.
    load(words, 0, 0x00203000); words.push_back(0xe5904010);
    load(words, 0, 0x00204000); words.push_back(0xe5905004); words.push_back(0xe5908000);
    words.push_back(0xe3a06055); words.push_back(0xe7f000f0);
    assert(words.size() < 0x400 / 4);
    words.resize(0x400 / 4, 0xe1a00000);
    load(words, 9, 0x00203000); words.push_back(0xe599b014); words.push_back(0xe3a0a000); words.push_back(0xe589a014);
    load(words, 9, 0x00204000); load(words, 11, 0xffbf); words.push_back(0xe589b014);
    load(words, 9, 0x0021c000); words.push_back(0xe599b034); words.push_back(0xe589b034);
    words.push_back(0xe2877001); words.push_back(0xe25ef004);
    return words;
}
inline Imx1ProbeOptions wakeOptions() {
    Imx1ProbeOptions o; o.bootstrapPeripherals = true; o.rtcInitialTimeKnown = true; o.rtcInitialTime = {1, 12, 34, 56};
    o.gpioAInputKnown = true; o.gpioAInput = 0x80;
    return o;
}
// The sound setup the original ROM leaves running: SSI1 sending and DMA channel 10 refilling it
// from a repeating buffer. Timer 2 wakes a loop of waits, and so does the end of the buffer when
// its interrupt is enabled. The handler acknowledges both.
inline std::vector<uint32_t> pcmGuest(uint32_t compare, uint32_t waits, uint32_t bytes, uint32_t pcdr, bool dmaInterrupt) {
    auto words = setup();
    if (pcdr) { load(words, 0, 0x0021b000); store(words, 0, 0x20, pcdr); }
    load(words, 0, 0x00300000); store(words, 0, 0, 0x12340001); store(words, 0, 4, 0x12350003);
    load(words, 0, 0x00209000); store(words, 0, 0, 1); store(words, 0, 8, 0x3ff);
    load(words, 0, 0x00218000); store(words, 0, 8, 0x9900); store(words, 0, 0x14, 0x6100); store(words, 0, 0x20, 4);
    store(words, 0, 0xc, 0xe8); store(words, 0, 0, 0x101); store(words, 0, 0, 0x102);
    load(words, 0, 0x00209300); store(words, 0, 0, 0x00300000); store(words, 0, 4, 0x00218000); store(words, 0, 8, bytes);
    store(words, 0, 0xc, 0x208c); store(words, 0, 0x10, 16); store(words, 0, 0x14, 8); store(words, 0, 0xc, 0x208d);
    load(words, 0, 0x00218000); store(words, 0, 0xc, 0x2e8); store(words, 0, 8, 0x9b00);
    load(words, 0, 0x00223000); store(words, 0, 8, 58);
    if (dmaInterrupt) store(words, 0, 8, 61);
    load(words, 0, 0x00203000); store(words, 0, 8, compare); store(words, 0, 0, 0x13);
    words.push_back(0xe3a07000); words.push_back(0xe321f013);
    words.push_back(0xee070f90);
    words.push_back(0xe3570000 | waits); words.push_back(0xbafffffc);
    // Read back what the wait moved: DMA status, the count left and the FIFO level.
    load(words, 0, 0x00209000); words.push_back(0xe5904004);
    load(words, 0, 0x00218000); words.push_back(0xe5905020);
    load(words, 0, 0x00203000); words.push_back(0xe5908010);
    words.push_back(0xe3a06055); words.push_back(0xe7f000f0);
    assert(words.size() < 0x400 / 4);
    words.resize(0x400 / 4, 0xe1a00000);
    load(words, 9, 0x00203000); words.push_back(0xe599b014); words.push_back(0xe3a0a000); words.push_back(0xe589a014);
    load(words, 9, 0x00209000); load(words, 11, 0x400); words.push_back(0xe589b004);
    words.push_back(0xe2877001); words.push_back(0xe25ef004);
    return words;
}
// The MCU PLL is off during each of two timed waits, so every wake also runs the nine CLK32 restart.
inline std::vector<uint32_t> mcuOffGuest(uint32_t compare) {
    constexpr uint32_t cgm = 0x21b000;
    std::vector<uint32_t> v(64, 0xe1a00000);
    const auto literal = [&](unsigned reg, uint32_t n) {
        v.push_back(0xe59f0000 | reg << 12); v.push_back(0xea000000); v.push_back(n);
    };
    v[0] = 0xea00003e; v[6] = 0xea000018; v[7] = 0xea000017;
    unsigned h = 32;
    v[h++] = 0xe5945000; v[h++] = 0xe3a05000; v[h++] = 0xe5845000; v[h++] = 0xe5825000; v[h++] = 0xe25ef004;
    literal(0, cgm); literal(1, 0x57000003); v.push_back(0xe5801000);
    literal(1, 0x03e717bf); v.push_back(0xe5801004);
    literal(1, 0x57200003); v.push_back(0xe5801000);
    v.push_back(0xe5902000); v.push_back(0xe3120602); v.push_back(0x1afffffc);
    literal(1, 0xc0000078); v.push_back(0xee011f10);
    for (unsigned i = 0; i < 60; ++i) v.push_back(0xe1a03003);
    literal(1, 0x78); v.push_back(0xee011f10);
    literal(4, 0x203014);
    literal(2, 0x223000); literal(1, 0x04000000); v.push_back(0xe5821010);
    literal(2, 0x203000); literal(1, compare); v.push_back(0xe5821008);
    literal(1, 0x113); v.push_back(0xe5821000);
    literal(1, 0x57003c03); v.push_back(0xe5801000);
    literal(1, 0x57003c02); v.push_back(0xe5801000);
    v.push_back(0xe321f013);
    v.push_back(0xee070f90);
    v.push_back(0xe5945000); v.push_back(0xe3a05000); v.push_back(0xe5845000);
    v.push_back(0xe5825000); literal(5, 0x113); v.push_back(0xe5825000);
    literal(1, 0x57003c02); v.push_back(0xe5801000);
    v.push_back(0xee070f90);
    literal(1, 0xc0000078); v.push_back(0xee011f10);
    literal(1, 0x57000003); v.push_back(0xe5801000);
    v.push_back(0xe3a06055); v.push_back(0xe7f000f0);
    return v;
}
// The watchdog's shortest timeout bound ends a wait that nothing else can wake.
inline std::vector<uint32_t> watchdogGuest() {
    auto words = setup();
    // HCLK at a sixteenth of the System PLL keeps the stepped run short.
    load(words, 0, 0x0021b000); store(words, 0, 0, 0x57003c03);
    load(words, 0, 0x00201000); store(words, 0, 0, 0x200); store(words, 0, 0, 0x203);
    words.push_back(0xe321f013); words.push_back(0xee070f90);
    words.push_back(0xe3a06055); words.push_back(0xe7f000f0);
    return words;
}

inline void gstore(Imx1Cs5Configuration& state, const Imx1Eim& eim, uint32_t pc, uint32_t offset, uint32_t value) {
    assert(state.access(eim, pc, 0x16010000 + offset, 4, true, value) == A::ok);
}
inline void transaction(Imx1Cs5Configuration& state, const Imx1Eim& eim, bool begin) {
    gstore(state, eim, 0x200aeb68, 0x4d8, begin ? 0x78 : 0x7b);
}
// The 3x2 display of the display test, enabled and on programmed graphics clocks.
inline Imx1Cs5Configuration clockedDisplay(Imx1Eim& eim) {
    uint32_t value = 0x3e00;
    assert(eim.access(0x00220028, 4, true, value) == Imx1EimAccess::ok);
    value = 0x11111e01;
    assert(eim.access(0x0022002c, 4, true, value) == Imx1EimAccess::ok);
    Imx1Cs5Configuration state(Imx1GraphicsProfile::virtual5744Compatible8MiB);
    configureGraphicsInterface(state, eim);
    configureGraphicsController(state, eim);
    assert(state.access(eim, 0x200acb98, 0x160113e8, 4, false, value) == A::ok);
    for (auto op : {std::array<uint32_t,3>{0x200acba8,0x13e8,0x400},
            {0x200acbb4,0x144,0x2440},{0x200acbc4,0x10a8,0x1fff1fff},{0x200acbd0,0x4dc,0xc0000},
            {0x200b8e80,0x214,0x10150000},{0x200b8e88,0x220,0},{0x200b8f04,8,0x04150000},
            {0x200b8f10,0x230,0x80000000},{0x200b8f20,0x210,0x0800000d}})
        gstore(state, eim, op[0], op[1], op[2]);
    gstore(state, eim, 0x200b99c4, 0x4bc, 0xffff1fff);
    gstore(state, eim, 0x200b9a14, 0x4b8, 0x01ff0008);
    transaction(state, eim, true);
    gstore(state, eim, 0x200ad720, 0x418, 0x800000);
    gstore(state, eim, 0x200ad72c, 0x41c, 8);
    gstore(state, eim, 0x200ad738, 0x414, 6);
    transaction(state, eim, false);
    state.advanceCpuCycles(32);
    transaction(state, eim, true);
    gstore(state, eim, 0x200ad65c, 0x42c, 0x40001);
    gstore(state, eim, 0x200ad668, 0x430, 0x30001);
    for (auto op : {std::array<uint32_t,3>{0x200ad2e0,0x410,3},
            {0x200ad404,0x540,0x80604020},{0x200ad410,0x544,0xffdfbf9f},{0x200ad418,0x548,0},
            {0x200ad420,0x578,0x80604020},{0x200ad428,0x58c,0xffdfbf9f},{0x200ad430,0x590,0},
            {0x200ad438,0x594,0x80604020},{0x200ad440,0x598,0xffdfbf9f},{0x200ad448,0x59c,0},
            {0x200ad528,0x538,0x800000},{0x200ad3b8,0x524,0x102aa}}) gstore(state, eim, op[0], op[1], op[2]);
    gstore(state, eim, 0x200ad2ec, 0x414, 0x1800406);
    gstore(state, eim, 0x200ad2f8, 0x420, 0x60008);
    gstore(state, eim, 0x200ad304, 0x424, 0x40001);
    gstore(state, eim, 0x200ad310, 0x428, 0x30001);
    transaction(state, eim, false);
    state.advanceCpuCycles(32);
    transaction(state, eim, true);
    gstore(state, eim, 0x200ad8b4, 0x414, 0x1801c06);
    transaction(state, eim, false);
    state.advanceCpuCycles(32);
    assert(state.displayState().lcdEnabled());
    gstore(state, eim, 0x200c5f20, 0x194, 0x1545);
    uint32_t aperture = 0;
    assert(state.access(eim, 0x200c1118, 0x16000010, 1, false, aperture) == A::ok && aperture == 1);
    assert(state.access(eim, 0x200c111c, 0x16000010, 1, true, aperture) == A::ok);
    gstore(state, eim, 0x200c45e4, 0x80, 0x3f);
    gstore(state, eim, 0x200c4920, 0x90, 0x20);
    assert(state.clockState().programmed() && state.displayState().lcdEnabled());
    return state;
}
inline std::array<uint64_t, 12> shown(const Imx1Cs5Configuration& s) {
    const auto& d = s.displayState();
    return {d.frameNumber(), d.verticalBlankEvents(), d.revision(), d.lcdEnabled(), d.latchPending(),
            d.clockAcknowledgements(), s.interruptState().pendingValue(), s.interruptState().knownMask(),
            uint64_t(s.interruptState().interruptOutput()), s.engineState().completed(), s.engineState().consumer(),
            s.clockState().count()};
}
// Every graphics field a state holds, the time, pixel and count fractions among them.
inline std::vector<uint8_t> saved(Imx1Cs5Configuration s) {
    std::vector<uint8_t> bytes;
    StateWriter writer(bytes);
    s.state(writer);
    return bytes;
}
// Graphics alone: a stepped twin shows that nothing visible changes before the reported step,
// and that one advance over the quiet steps plus the last step leaves the same state.
inline void graphics() {
    Imx1Eim eim;
    auto stepped = clockedDisplay(eim), skipped = stepped;
    uint64_t total = 0, advances = 0, longest = 0;
    for (auto period : {Imx1PllRatio{16, 46875}, Imx1PllRatio{48, 46875}, Imx1PllRatio{7, 3001}}) {
        for (unsigned round = 0; round < 3000; ++round) {
            if (round == 1000) {
                // A latch that waits for the frame boundary, on both.
                for (auto* s : {&stepped, &skipped}) {
                    transaction(*s, eim, true);
                    gstore(*s, eim, 0x200ae9a8, 0x414, 0x1801c16);
                    gstore(*s, eim, 0x200ae9b8, 0x418, 0x800000);
                    gstore(*s, eim, 0x200ae9c4, 0x41c, 4);
                    transaction(*s, eim, false);
                }
            }
            const auto steps = std::min<uint64_t>(skipped.idleQuietSteps(period.numerator, period.denominator), 100000);
            assert(steps && steps == std::min<uint64_t>(stepped.idleQuietSteps(period.numerator, period.denominator), 100000));
            const auto before = shown(stepped);
            for (uint64_t i = 0; i < steps; ++i) {
                if (i) assert(shown(stepped) == before);
                stepped.advanceReference(period.numerator, period.denominator);
            }
            if (steps > 1) skipped.advanceReference((steps - 1) * period.numerator, period.denominator);
            skipped.advanceReference(period.numerator, period.denominator);
            assert(shown(stepped) == shown(skipped));
            assert(stepped.displayState().scanPhase() == skipped.displayState().scanPhase());
            assert(saved(stepped) == saved(skipped));
            total += steps; ++advances; longest = std::max(longest, steps);
        }
    }
    assert(stepped.displayState().frameNumber() > 1000 && stepped.displayState().verticalBlankEvents() > 1000 && longest > 1);
    std::printf("PASS: graphics idle skip matches %llu stepped clocks in %llu advances (longest %llu), frames %llu\n",
        (unsigned long long)total, (unsigned long long)advances, (unsigned long long)longest,
        (unsigned long long)stepped.displayState().frameNumber());
}

inline void write(Imx1Bootstrap& b, uint32_t address, uint32_t value) {
    assert(b.access(address, 4, true, value) == A::ok);
}
inline uint32_t read(Imx1Bootstrap b, uint32_t address) {
    uint32_t value = 0; assert(b.access(address, 4, false, value) == A::ok); return value;
}
inline std::array<uint64_t, 8> shown(const Imx1Bootstrap& b) {
    const auto lines = b.interruptLines(); const auto pending = b.interruptPending();
    return {uint64_t(lines.irq), uint64_t(lines.fiq), uint64_t(b.clockStatus()), b.idleWakePossible(),
            pending.normal, pending.fast, pending.unknownNormal, pending.unknownFast};
}
// Peripherals alone, the same way: both timers, RTC, PWM on either clock and the watchdog up to its bound.
inline void peripherals() {
    for (uint32_t pwm : {0x10u, 0x8010u}) {
        Imx1Bootstrap stepped;
        stepped.connectCpuInterrupts();
        assert(stepped.setRtcInitialTime({1, 12, 34, 56}));
        write(stepped, 0x0021b000, 0x57003c03);
        write(stepped, 0x00223008, 58); write(stepped, 0x00223008, 17); write(stepped, 0x00223008, 18);
        write(stepped, 0x00202004, 7); write(stepped, 0x00202000, 0x103);
        write(stepped, 0x00203008, 5000); write(stepped, 0x00203000, 0x13);
        write(stepped, 0x00204018, 0x1010);
        write(stepped, 0x00208000, pwm); write(stepped, 0x00208008, 1000);
        write(stepped, 0x00201000, 0x200); write(stepped, 0x00201000, 0x203);
        auto skipped = stepped;
        uint64_t total = 0, advances = 0, longest = 0;
        while (stepped.clockStatus() == A::ok) {
            const auto steps = std::min<uint64_t>(skipped.idleQuietHclk(), 1u << 30);
            assert(steps && steps == std::min<uint64_t>(stepped.idleQuietHclk(), 1u << 30));
            const auto before = shown(stepped);
            for (uint64_t i = 0; i < steps; ++i) {
                if (i) assert(shown(stepped) == before);
                stepped.advanceHclkCycles(1);
            }
            if (steps > 1) skipped.advanceHclkCycles(uint32_t(steps - 1));
            skipped.advanceHclkCycles(1);
            assert(shown(stepped) == shown(skipped) && stepped.referenceTime().compare(skipped.referenceTime()) == 0 &&
                   stepped.referenceTime().denominator == skipped.referenceTime().denominator);
            for (uint32_t address : {0x00202010u, 0x00203010u, 0x00203014u, 0x00204000u, 0x00204004u, 0x00204014u,
                                     0x00208000u, 0x0020800cu})
                assert(read(stepped, address) == read(skipped, address));
            // Acknowledge like a handler would, so the sources keep firing.
            for (auto* b : {&stepped, &skipped}) {
                uint32_t value = 0;
                if (b->clockStatus() != A::ok) continue;
                assert(b->access(0x00203014, 4, false, value) == A::ok); write(*b, 0x00203014, 0);
                write(*b, 0x00204014, 0xffbf);
                assert(b->access(0x00208000, 4, false, value) == A::ok);
            }
            total += steps; ++advances; longest = std::max(longest, steps);
        }
        assert(stepped.clockStatus() == A::possibleWatchdogTimeout && skipped.clockStatus() == A::possibleWatchdogTimeout);
        assert(total > 3000000 && longest > 1000);
        std::printf("PASS: peripheral idle skip matches %llu stepped clocks in %llu advances (longest %llu) up to the watchdog bound\n",
            (unsigned long long)total, (unsigned long long)advances, (unsigned long long)longest);
    }
}

// Every peripheral field a state holds.
inline std::vector<uint8_t> saved(Imx1Bootstrap b) {
    std::vector<uint8_t> bytes;
    StateWriter writer(bytes);
    b.state(writer);
    return bytes;
}
// The PCM transport the same way: SSI1 sending with DMA channel 10 refilling it, once and repeating,
// beside timer 2. The whole saved state has to match after each advance.
inline void pcm() {
    for (bool repeat : {false, true}) for (uint32_t pcdr : {0u, 0x00100000u}) for (uint32_t bytes : {16u, 4096u}) {
        Imx1Bootstrap stepped;
        stepped.connectCpuInterrupts();
        write(stepped, 0x0021b000, 0x57000003); write(stepped, 0x0021b020, pcdr);
        write(stepped, 0x00223008, 58); write(stepped, 0x00223008, 61);
        write(stepped, 0x00203008, 70001); write(stepped, 0x00203000, 0x13);
        write(stepped, 0x00209000, 1); write(stepped, 0x00209008, 0x3ff);
        write(stepped, 0x00218008, 0x9900); write(stepped, 0x00218014, 0x6100); write(stepped, 0x00218020, 4);
        write(stepped, 0x0021800c, 0xe8);
        for (unsigned i = 0; i < 8; ++i) write(stepped, 0x00218000, 0x100 + i);
        for (unsigned i = 0; i < bytes / 4; ++i) write(stepped, 0x00300000 + i * 4, 0x12340001 + i * 0x10002);
        write(stepped, 0x00209300, 0x00300000); write(stepped, 0x00209304, 0x00218000); write(stepped, 0x00209308, bytes);
        write(stepped, 0x0020930c, repeat ? 0x208c : 0x2088);
        write(stepped, 0x00209310, 16); write(stepped, 0x00209314, 8);
        write(stepped, 0x0020930c, repeat ? 0x208d : 0x2089);
        write(stepped, 0x0021800c, 0x2e8); write(stepped, 0x00218008, 0x9b00);
        auto skipped = stepped;
        uint64_t total = 0, advances = 0, longest = 0, ends = 0;
        while (total < 5000000) {
            const auto steps = std::min<uint64_t>(skipped.idleQuietHclk(), 1u << 30);
            assert(steps && steps == std::min<uint64_t>(stepped.idleQuietHclk(), 1u << 30));
            const auto before = shown(stepped);
            for (uint64_t i = 0; i < steps; ++i) {
                if (i) assert(shown(stepped) == before);
                stepped.advanceHclkCycles(1);
            }
            if (steps > 1) skipped.advanceHclkCycles(uint32_t(steps - 1));
            skipped.advanceHclkCycles(1);
            assert(shown(stepped) == shown(skipped) && saved(stepped) == saved(skipped));
            assert(stepped.pcmTransmittedWords() == skipped.pcmTransmittedWords() &&
                   stepped.pcmLastTransmittedWord() == skipped.pcmLastTransmittedWord());
            if (read(stepped, 0x00209004) & 0x400) ++ends;
            for (auto* b : {&stepped, &skipped}) {
                uint32_t value = 0;
                assert(b->access(0x00203014, 4, false, value) == A::ok); write(*b, 0x00203014, 0);
                write(*b, 0x00209004, 0x400);
            }
            total += steps; ++advances; longest = std::max(longest, steps);
        }
        if (stepped.pcmTransmittedWords() <= 1000 || ends < (repeat ? 2u : 1u) || advances * (repeat && bytes == 16 ? 8 : 1000) > total) {
            std::fprintf(stderr, "PCM idle skip, repeat %d, PCDR %x, %u bytes: %llu clocks in %llu advances (longest %llu), %llu buffer ends, %llu words sent\n",
                repeat, pcdr, bytes, (unsigned long long)total, (unsigned long long)advances, (unsigned long long)longest,
                (unsigned long long)ends, (unsigned long long)stepped.pcmTransmittedWords());
            assert(false);
        }
        std::printf("PASS: PCM idle skip matches %llu stepped clocks in %llu advances (longest %llu), %llu buffer ends, %llu words sent\n",
            (unsigned long long)total, (unsigned long long)advances, (unsigned long long)longest, (unsigned long long)ends,
            (unsigned long long)stepped.pcmTransmittedWords());
    }
}

inline void run() {
    graphics();
    peripherals();
    pcm();
    const auto options = wakeOptions();
    for (bool fast : {false, true}) {
        const auto timer = wakeGuest(100000, 0, 4);
        for (auto script : {Script::plain, Script::slices, Script::bounded, Script::gpio}) {
            const auto out = compare("timer", timer, options, fast, script);
            assert(out.stop == Imx1ProbeStop::unsupportedInstruction && out.irqs == 4);
        }
    }
    const auto rtc = compare("rtc", wakeGuest(0, 0x4000, 3), options, false, Script::plain, true);
    assert(rtc.stop == Imx1ProbeStop::unsupportedInstruction && rtc.irqs == 3 && rtc.idle > 750000);
    for (auto script : {Script::plain, Script::slices, Script::bounded, Script::gpio}) {
        const auto both = compare("timer and rtc", wakeGuest(100000, 0x8000, 8), options, false, script);
        assert(both.stop == Imx1ProbeStop::unsupportedInstruction && both.irqs == 8);
    }
    Imx1ProbeOptions settled; settled.bootstrapPeripherals = true; settled.assumeMpllSettled = true;
    settled.virtualMpllRetune = true; settled.assumeSpllSettled = true;
    for (auto script : {Script::plain, Script::slices, Script::bounded}) {
        const auto off = compare("mcu pll off", mcuOffGuest(150000), settled, false, script);
        assert(off.stop == Imx1ProbeStop::unsupportedInstruction && off.irqs == 2 && off.idle > 200000);
    }
    Imx1ProbeOptions plain; plain.bootstrapPeripherals = true;
    for (bool fast : {false, true}) for (bool dmaInterrupt : {false, true}) for (uint32_t pcdr : {0u, 0x00100000u})
    for (auto script : {Script::plain, Script::slices, Script::bounded}) {
        if (fast && script != Script::plain) continue;
        const auto sound = compare("pcm dma", pcmGuest(300000, 6, 4096, pcdr, dmaInterrupt), plain, fast, script,
                                   !fast && script == Script::plain);
        assert(sound.stop == Imx1ProbeStop::unsupportedInstruction && sound.irqs >= 6 && sound.idle > 500000);
        // Passes are a few per timer wake and buffer end, never one per word.
        assert(sound.advances * 500 < sound.idle);
    }
    const auto watchdog = compare("watchdog bound", watchdogGuest(), plain, false, Script::plain, true);
    assert(watchdog.stop == Imx1ProbeStop::unsupportedPeripheral && watchdog.status == A::possibleWatchdogTimeout);
    std::puts("PASS: idle skip and stepped idle agree on results, status, time, observers and diagnostics for timer, RTC, GPIO, MCU restart, PCM DMA and watchdog waits, with idle, work and deadline bounds inside a wait");
}
}
#endif
