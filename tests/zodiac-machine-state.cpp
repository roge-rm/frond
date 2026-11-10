// Zodiac machine save state: a loaded machine continues exactly like the one that saved.
// Board and CPU guests first, then CS4 and CS5 inputs and virtual graphics guests.
#include "zodiac-machine-parity.h"
#define imx1BootProbe machineParityProbe
#define main originalIrqMain
#include "zodiac-irq.cpp"
#undef main
#undef imx1BootProbe
#include <functional>
namespace {
using Bytes = std::vector<uint8_t>;
const Imx1ReferenceTime never{UINT64_MAX, 0, 1};
Imx1AspPenInputs pen(bool down) {
    Imx1AspPenInputs p;
    p.contactKnown = p.normalKnown = p.referenceKnown = true; p.contactDown = down;
    p.normal = {0x1100, 0x5500, 0xa900, 0x1234}; p.reference = {0x0180, 0x3280, 0x7100};
    return p;
}
struct Diagnostic {
    uint64_t hash = 0, words = 0;
    static void word(void* p, const char* name, const uint32_t* data, uint32_t count) {
        auto& d = *static_cast<Diagnostic*>(p);
        for (const char* n = name; *n; ++n) d.hash = d.hash * 131 + uint8_t(*n);
        for (uint32_t i = 0; i < count; ++i) { d.hash = d.hash * 131 + data[i]; ++d.words; }
    }
    static void bytes(void* p, const char*, const uint8_t* data, uint32_t count) {
        auto& d = *static_cast<Diagnostic*>(p);
        for (uint32_t i = 0; i < count; ++i) d.hash = d.hash * 131 + data[i];
    }
    static void counters(void* p, const char* name, const uint64_t* data, uint32_t count) {
        auto& d = *static_cast<Diagnostic*>(p);
        for (const char* n = name; *n; ++n) d.hash = d.hash * 131 + uint8_t(*n);
        for (uint32_t i = 0; i < count; ++i) { d.hash = d.hash * 131 + data[i]; ++d.words; }
    }
};
// One step of a run. A bounded call's deadline is the machine's own time plus deadlineTicks/deadlineDivisor.
struct Call {
    bool bounded = false;
    uint32_t retired = 0;
    uint64_t idle = 0, work = 0, deadlineTicks = 0, deadlineDivisor = 1;
    bool penDown = false;
    // A named control to press or let go, and a stick position, set before running.
    int control = -1; bool pressed = false;
    bool stick = false; int stickX = 0, stickY = 0;
    // Stop at this retired count instead of after `retired` more.
    uint64_t until = 0;
    // Take and acknowledge the pending frame before running.
    bool acquire = false;
};
Call slice(uint32_t retired, uint64_t idle) { Call c; c.retired = retired; c.idle = idle; return c; }
Call until(uint64_t retired, uint64_t idle = 0, bool acquire = false) {
    Call c; c.until = retired; c.idle = idle; c.acquire = acquire; return c;
}
// A frame as PeekFrame or AcquireFrame lends it.
struct Frame {
    bool present = false;
    uint32_t width = 0, height = 0, first = 0, last = 0;
    uint64_t hash = 0;
    void pixels(const uint32_t* data) {
        present = data != nullptr;
        hash = 1469598103934665603ull;
        for (uint32_t i = 0; data && i < width * height; ++i) hash = (hash ^ data[i]) * 1099511628211ull;
    }
    bool operator==(const Frame& o) const {
        return present == o.present && width == o.width && height == o.height && first == o.first && last == o.last && hash == o.hash;
    }
};
Call bounded(uint32_t retired, uint64_t work, uint64_t deadlineTicks = 0, uint64_t deadlineDivisor = 1) {
    Call c; c.bounded = true; c.retired = retired; c.work = work;
    c.deadlineTicks = deadlineTicks; c.deadlineDivisor = deadlineDivisor; return c;
}
struct Record {
    uint64_t hash = 0, count = 0, diagnostic = 0, diagnosticWords = 0;
    Imx1MachineRun run{};
    Imx1MachineCheapStatus cheap{};
    Imx1MachineBoundedRun boundedRun{};
    Frame acquired, shown;
    uint64_t idleAdvances = 0;
    Bytes save;
};
struct Guest {
    const char* name;
    std::vector<uint32_t> words;
    Imx1ProbeOptions options;
    bool ring = false;
    Imx1ProbeEntry entry = Imx1ProbeEntry::resetAlias;
};
struct Live {
    machine_test::Capture capture;
    Imx1Machine* machine = nullptr;
    Live(const Guest& g, bool fast) {
        capture.original = g.options;
        auto o = capture.options();
        static const ArmDiagnosticSink retain{};
        if (g.ring) o.diagnosticSnapshot = &retain;
        machine = imx1MachineCreate(reinterpret_cast<const uint8_t*>(g.words.data()), g.words.size() * 4,
                                    g.entry, fast, o, machine_test::Capture::trace, &capture);
        assert(machine);
    }
    ~Live() { imx1MachineStop(machine); }
    Bytes save() {
        Bytes out;
        const auto status = imx1MachineSaveState(machine, out);
        if (status != Imx1StateStatus::ok) std::fprintf(stderr, "save: %s\n", imx1StateStatusName(status));
        assert(status == Imx1StateStatus::ok && !out.empty());
        return out;
    }
    void load(const Bytes& state) {
        const auto check = imx1MachineCheckState(machine, state.data(), state.size());
        const auto info = imx1MachineLoadState(machine, state.data(), state.size());
        if (info.status != Imx1StateStatus::ok)
            std::fprintf(stderr, "load: %s section=%s detail=%s\n", imx1StateStatusName(info.status), info.section,
                         info.detail ? info.detail : "");
        assert(check.status == Imx1StateStatus::ok && info.status == Imx1StateStatus::ok);
        assert(!info.engineChanged && !info.coreRevisionDiffers && check.retired == info.retired);
    }
    Record run(const Call& c) {
        Record r;
        capture.hash = 1469598103934665603ull; capture.count = 0;
        if (c.acquire) {
            r.acquired.pixels(imx1MachineAcquireFrame(machine, r.acquired.width, r.acquired.height, r.acquired.first, r.acquired.last));
            imx1MachineAcknowledgeFrame(machine);
        }
        const uint64_t before = imx1MachineGetStatus(machine).retired;
        const uint32_t retired = c.until ? uint32_t(c.until > before ? c.until - before : 0) : c.retired;
        if (c.penDown) assert(imx1MachineReplacePenInputs(machine, pen(true)) == Imx1BootstrapAccess::ok);
        if (c.control >= 0)
            assert(imx1MachineSetZodiacControl(machine, Imx1ZodiacControl(c.control), c.pressed) == Imx1BootstrapAccess::ok);
        if (c.stick) assert(imx1MachineSetZodiacStick(machine, c.stickX, c.stickY) == Imx1BootstrapAccess::ok);
        if (c.bounded) {
            auto deadline = never;
            if (c.deadlineTicks) { deadline = imx1MachineGetStatus(machine).time; assert(deadline.add(c.deadlineTicks, c.deadlineDivisor)); }
            r.boundedRun = imx1MachineRunBounded(machine, {retired, deadline, c.work});
            assert(r.boundedRun.reason != Imx1MachineYield::invalidBounds);
        } else if (retired || c.idle) {
            imx1MachineRunSlice(machine, retired, c.idle);
        }
        r.shown.pixels(imx1MachinePeekFrame(machine, r.shown.width, r.shown.height));
        r.idleAdvances = imx1MachineIdleAdvances(machine);
        r.hash = capture.hash; r.count = capture.count;
        r.run = imx1MachineInspect(machine);
        r.cheap = imx1MachineGetStatus(machine);
        Diagnostic d;
        imx1MachineEmitDiagnostic(machine, {{&d, Diagnostic::word, Diagnostic::bytes}, Diagnostic::counters, &d});
        r.diagnostic = d.hash; r.diagnosticWords = d.words;
        r.save = save();
        return r;
    }
};
bool sameTime(const Imx1ReferenceTime& a, const Imx1ReferenceTime& b) {
    return a.ticks == b.ticks && a.numerator == b.numerator && a.denominator == b.denominator;
}
bool sameCheap(const Imx1MachineCheapStatus& a, const Imx1MachineCheapStatus& b) {
    return a.status == b.status && a.stop == b.stop && a.retired == b.retired && a.work == b.work &&
           a.idleHclk == b.idleHclk && a.cs5WaitClocks == b.cs5WaitClocks && sameTime(a.time, b.time) && a.pc == b.pc;
}
void match(const char* name, bool fast, size_t k, const char* how, const Record& a, const Record& b) {
    const bool events = a.hash == b.hash && a.count == b.count;
    const bool diagnostic = a.diagnostic == b.diagnostic && a.diagnosticWords == b.diagnosticWords;
    const bool bytes = a.save == b.save;
    const bool frames = a.acquired == b.acquired && a.shown == b.shown;
    if (!events || !diagnostic || !bytes || !frames) {
        size_t at = 0;
        while (at < a.save.size() && at < b.save.size() && a.save[at] == b.save[at]) ++at;
        std::fprintf(stderr, "%s %s point %zu (%s): events=%d diagnostic=%d bytes=%d frames=%d sizes %zu/%zu first difference %zu\n",
                     name, fast ? "Fast" : "Exact", k, how, events, diagnostic, bytes, frames, a.save.size(), b.save.size(), at);
    }
    assert(events);
    assert(a.run.status == b.run.status && a.run.retiredInstructions == b.run.retiredInstructions);
    machine_test::same(a.run.result, b.run.result);
    assert(a.run.result.interruptPending.normal == b.run.result.interruptPending.normal &&
           a.run.result.interruptPending.fast == b.run.result.interruptPending.fast);
    assert(sameCheap(a.cheap, b.cheap));
    assert(a.boundedRun.reason == b.boundedRun.reason && a.boundedRun.retiredDelta == b.boundedRun.retiredDelta &&
           a.boundedRun.workDelta == b.boundedRun.workDelta && sameTime(a.boundedRun.elapsed, b.boundedRun.elapsed) &&
           a.boundedRun.workOvershoot == b.boundedRun.workOvershoot && sameCheap(a.boundedRun.total, b.boundedRun.total));
    assert(diagnostic);
    assert(frames);
    assert(bytes);
}
// Runs the calls on A with a save after each, then continues every save on other machines.
// fresh picks the save points that also get a newly created machine, the rest reuse one.
std::vector<Record> roundTrip(const Guest& g, bool fast, const std::vector<Call>& calls,
                              const std::function<bool(const std::vector<Record>&, size_t)>& fresh) {
    std::vector<Record> records;
    {
        Live a(g, fast);
        Record start; start.save = a.save(); start.run = imx1MachineInspect(a.machine);
        assert(a.save() == start.save);
        records.push_back(std::move(start));
        for (const auto& c : calls) {
            records.push_back(a.run(c));
            if (records.back().run.status == Imx1MachineStatus::terminal) break;
        }
    }
    const size_t points = records.size() - 1;
    {
        Live b(g, fast);
        // Later points first, so each load replaces state from further along the run.
        for (size_t k = points; k-- > 0;) {
            b.load(records[k].save);
            assert(b.save() == records[k].save);
            match(g.name, fast, k, "reused", records[k + 1], b.run(calls[k]));
        }
        b.load(records[points].save);
        assert(b.save() == records[points].save);
    }
    for (size_t k = 0; k < points; ++k) {
        if (!fresh(records, k)) continue;
        Live b(g, fast);
        b.load(records[k].save);
        assert(b.save() == records[k].save);
        match(g.name, fast, k, "fresh", records[k + 1], b.run(calls[k]));
    }
    return records;
}
const auto always = [](const std::vector<Record>&, size_t) { return true; };

Guest timerGuest() {
    Guest g{"timer WFI", setup(), {}};
    auto& words = g.words;
    load(words, 0, 0x00223000); store(words, 0, 8, 58);
    load(words, 0, 0x00203000); store(words, 0, 8, 200); store(words, 0, 0, 0x13);
    words.push_back(0xe321f013); words.push_back(0xee070f90);
    words.push_back(0xe3a06055); words.push_back(0xe7f000f0);
    installTimerHandler(words);
    g.options.bootstrapPeripherals = true;
    return g;
}
void timerWfi(bool fast) {
    const auto g = timerGuest();
    auto r = roundTrip(g, fast, {slice(500, 1), slice(500, 57), slice(3, 10000), slice(500, 10000)}, always);
    assert(r.size() == 5 && r[1].run.result.stop == Imx1ProbeStop::idleBudget && r[1].run.result.idleHclk == 1);
    assert(r[2].run.result.stop == Imx1ProbeStop::idleBudget && r[2].run.result.idleHclk == 57);
    assert(r[1].run.retiredInstructions == r[2].run.retiredInstructions);
    assert(r[4].run.status == Imx1MachineStatus::terminal && r[4].run.result.registers[6] == 0x55 &&
           r[4].run.result.irqEntries == 1);
    if (fast) assert(r[4].run.result.jitBlocks && r[4].run.result.jitInstructions);
    // Bounded: work limits and a near deadline stop inside the wait, then it runs to the end.
    std::vector<Call> calls{bounded(500, 20), bounded(500, 40), bounded(500, 37), bounded(500, 100000, 1, 2000)};
    for (unsigned i = 0; i < 6; ++i) calls.push_back(bounded(500, 100000, i ? 0 : 2));
    r = roundTrip(g, fast, calls, [](const std::vector<Record>& all, size_t k) {
        return k < 5 && all[k].cheap.idleHclk > 0 ? true : k == 0;
    });
    bool inside = false, deadline = false;
    for (size_t k = 1; k + 1 < r.size(); ++k) {
        inside |= r[k].cheap.idleHclk > 0 && !r[k].boundedRun.retiredDelta && r[k].boundedRun.reason == Imx1MachineYield::work;
        deadline |= r[k].boundedRun.reason == Imx1MachineYield::deadline;
    }
    assert(inside && deadline);
    assert(r.back().run.status == Imx1MachineStatus::terminal && r.back().run.result.registers[6] == 0x55);
}
// UART2 sends two characters at the reset rate, 80 PERCLK1 cycles a bit, and the transmit complete
// interrupt (source 23) ends the wait. Saves before the first load, inside each character and after.
void uartWfi(bool fast) {
    Guest g{"UART WFI", setup(), {}};
    auto& words = g.words;
    load(words, 0, 0x00207000); store(words, 0, 0x80, 5); store(words, 0, 0x84, 0x4027);
    store(words, 0, 0x40, 0x41); store(words, 0, 0x40, 0x42); store(words, 0, 0x8c, 0x8048);
    load(words, 0, 0x00223000); store(words, 0, 8, 23);
    words.push_back(0xe321f013); words.push_back(0xee070f90);
    words.push_back(0xe3a06055); words.push_back(0xe7f000f0);
    words.resize(0x400 / 4, 0xe1a00000);
    // The handler switches TCEN off again.
    load(words, 9, 0x00207000); load(words, 10, 0x8040); words.push_back(0xe589a08c);
    words.push_back(0xe2877001); words.push_back(0xe25ef004);
    g.options.bootstrapPeripherals = true;
    auto r = roundTrip(g, fast, {slice(500, 1), slice(500, 300), slice(500, 700), slice(3, 100000), slice(500, 100000)}, always);
    assert(r.size() == 6 && r[1].run.result.stop == Imx1ProbeStop::idleBudget && r[3].run.result.stop == Imx1ProbeStop::idleBudget);
    assert(r[1].run.retiredInstructions == r[3].run.retiredInstructions && !r[3].run.result.irqEntries);
    assert(r[5].run.status == Imx1MachineStatus::terminal && r[5].run.result.registers[6] == 0x55 &&
           r[5].run.result.irqEntries == 1 && r[5].run.result.registers[7] == 1);
    // The two characters end 21 bit times after the wait began, less the instructions before it.
    assert(r[5].run.result.idleHclk > 1500 && r[5].run.result.idleHclk <= 1680);
}
void penWait(bool fast) {
    Guest g{"pen wait", setup(), {}, true};
    auto& words = g.words;
    load(words, 0, 0x00215000); store(words, 0, 0x18, 0x10);
    load(words, 0, 0x00223000); store(words, 0, 8, 46);
    words.push_back(0xe321f013);
    words.push_back(0xee070f90); words.push_back(0xe3a06055); words.push_back(0xe7f000f0);
    words.resize(0x400 / 4, 0xe1a00000);
    load(words, 0, 0x00215000); store(words, 0, 0x18, 0); store(words, 0, 0x1c, 0x40);
    words.push_back(0xe25ef004);
    g.options.bootstrapPeripherals = true; g.options.aspPenInputs = pen(false);
    g.options.gpioBInputKnown = true; g.options.gpioBInput = 0;
    g.options.gpioEventAfterInstructions = 1; g.options.gpioEventMasks[1] = 1; g.options.gpioEventValues[1] = 1;
    Call touch; touch.penDown = true;
    Call touchRun = slice(2, 10000); touchRun.penDown = true;
    // Saves: before the GPIO event, waiting, pen down but not yet run, inside the handler.
    auto r = roundTrip(g, fast, {slice(1, 0), slice(500, 10000), slice(1, 10000), touch, slice(2, 10000), slice(500, 10000)}, always);
    assert(r.size() == 7 && r[2].run.status == Imx1MachineStatus::waiting && r[3].run.status == Imx1MachineStatus::waiting);
    assert(r[2].run.retiredInstructions == r[4].run.retiredInstructions && r[5].run.result.irqEntries == 1);
    assert(r[6].run.status == Imx1MachineStatus::terminal && r[6].run.result.registers[6] == 0x55);
    r = roundTrip(g, fast, {bounded(500, 100000), touchRun, bounded(500, 100000)}, always);
    assert(r[1].boundedRun.reason == Imx1MachineYield::waiting && r.back().run.result.registers[6] == 0x55);
}
// A wait that Home (A7, falling edge, source 11) ends. Saves while waiting, with the stick moved, with
// Home down but not yet run, and after. The stick and the pin level have to come back from the save.
void controlsWait(bool fast) {
    Guest g{"controls wait", setup(), {}, true};
    auto& words = g.words;
    load(words, 0, 0x0021b000); store(words, 0, 0, 0x57000003); store(words, 0, 0x20, 0);
    load(words, 0, 0x0021c000); store(words, 0, 0x20, 0x80); store(words, 0, 0x28, 1u << 14); store(words, 0, 0x30, 0x80);
    load(words, 0, 0x00223000); store(words, 0, 8, 11);
    words.push_back(0xe321f013);
    words.push_back(0xee070f90); words.push_back(0xe3a06055); words.push_back(0xe7f000f0);
    words.resize(0x400 / 4, 0xe1a00000);
    load(words, 0, 0x0021c000); store(words, 0, 0x30, 0); store(words, 0, 0x34, 0x80);
    words.push_back(0xe25ef004);
    g.options.bootstrapPeripherals = true;
    g.options.graphicsProfile = Imx1GraphicsProfile::virtual5744Compatible8MiB; g.options.graphicsIrqA13 = true;
    g.options.graphicsRawAxesKnown = true; g.options.graphicsRawFirst = g.options.graphicsRawSecond = 512;
    g.options.gpioAInputMaskedKnown = true; g.options.gpioAInputMask = 0xffffdfff; g.options.gpioAInput = 0x780;
    g.options.gpioDInputKnown = true; g.options.gpioDInput = 0x7780;
    Call stick; stick.stick = true; stick.stickX = -300; stick.stickY = 511;
    Call function; function.control = int(Imx1ZodiacControl::function); function.pressed = true;
    Call home; home.control = int(Imx1ZodiacControl::home); home.pressed = true;
    Call up = slice(500, 10000); up.control = int(Imx1ZodiacControl::home);
    auto r = roundTrip(g, fast, {slice(500, 10000), stick, function, home, slice(500, 10000)}, always);
    assert(r.size() == 6 && r[1].run.status == Imx1MachineStatus::waiting && r[4].run.status == Imx1MachineStatus::waiting);
    assert(r[1].save != r[2].save && r[2].save != r[3].save && r[3].save != r[4].save);
    assert(r[1].run.retiredInstructions == r[4].run.retiredInstructions && !r[4].run.result.irqEntries);
    assert(r[5].run.status == Imx1MachineStatus::terminal && r[5].run.result.registers[6] == 0x55 &&
           r[5].run.result.irqEntries == 1);
    // Home let go again before the run: the latched edge still ends the wait.
    r = roundTrip(g, fast, {slice(500, 10000), home, up}, always);
    assert(r.back().run.status == Imx1MachineStatus::terminal && r.back().run.result.irqEntries == 1);
    // The rest position is what the options gave, so putting the stick back gives the first save again.
    Call rest; rest.stick = true;
    r = roundTrip(g, fast, {slice(500, 10000), stick, rest}, always);
    assert(r[1].save != r[2].save && r[1].save == r[3].save);
}
bool blPrefix(const Record& r) {
    return r.run.result.trace.instructionSize == 2 && (r.run.result.trace.instruction & 0xf800) == 0xf000;
}
void thumbBl(bool fast, bool ldmReturn, bool useBounded) {
    Guest g{ldmReturn ? "Thumb BL prefix, LDM return" : "Thumb BL prefix", setup(), {}};
    auto& words = g.words;
    load(words, 0, 0x00223000); store(words, 0, 8, 58);
    load(words, 0, 0x00203000); store(words, 0, 8, 6); store(words, 0, 0, 0x13);
    words.push_back(0xe321f013);
    const uint32_t thumbPc = words.size() * 4 + 16;
    load(words, 2, thumbPc | 1); words.push_back(0xe12fff12);
    words.push_back(0xf806f000); words.push_back(0xde00de00);
    words.push_back(0xde00f800); words.push_back(0xde00de00);
    words.push_back(0x47702655);
    installTimerHandler(words);
    if (ldmReturn) {
        words.back() = 0xe24ee004;
        load(words, 8, 0x00300000);
        words.push_back(0xe588e000);
        words.push_back(0xe8d88000);
    }
    g.options.bootstrapPeripherals = true; g.options.rtcInitialTimeKnown = true;
    std::vector<Call> calls(80, useBounded ? bounded(1, 100000) : slice(1, 10000));
    const auto r = roundTrip(g, fast, calls, [](const std::vector<Record>& all, size_t k) {
        return blPrefix(all[k]) || (k && blPrefix(all[k - 1]));
    });
    bool interrupted = false;
    for (size_t k = 1; k + 1 < r.size(); ++k)
        interrupted |= blPrefix(r[k]) && r[k].run.result.trace.pc == thumbPc &&
                       r[k + 1].run.result.irqEntries == r[k].run.result.irqEntries + 1;
    assert(interrupted);
    assert(r.back().run.status == Imx1MachineStatus::terminal && r.back().run.result.registers[6] == 0x55 &&
           r.back().run.result.trace.pc == thumbPc + 4 && r.back().run.result.irqEntries == 1);
}
void maskedIrq(bool fast) {
    Guest g{"masked IRQ", setup(), {}};
    auto& words = g.words;
    load(words, 0, 0x00223000); store(words, 0, 8, 58);
    load(words, 0, 0x00203000); store(words, 0, 8, 6); store(words, 0, 0, 0x13);
    for (unsigned i = 0; i < 24; ++i) words.push_back(0xe1a00000);
    words.push_back(0xe321f013);
    words.push_back(0xe3a06055); words.push_back(0xe7f000f0);
    installTimerHandler(words);
    g.options.bootstrapPeripherals = true;
    unsigned taken = 0;
    const auto r = roundTrip(g, fast, std::vector<Call>(80, slice(1, 10000)),
        [&taken](const std::vector<Record>& all, size_t k) {
            return all[k].run.result.interruptPending.normal && !all[k].run.result.irqEntries && taken++ % 8 == 0;
        });
    unsigned pending = 0;
    for (const auto& x : r) pending += x.run.result.interruptPending.normal && !x.run.result.irqEntries;
    assert(pending >= 8 && taken >= pending);
    assert(r.back().run.status == Imx1MachineStatus::terminal && r.back().run.result.registers[6] == 0x55 &&
           r.back().run.result.irqEntries == 1);
}
// A save doesn't depend on Inspect having been called. Bounded runs don't inspect, so one machine inspects
// before each save and the others never do, with the timer interrupt pending and masked.
void uninspectedSave(bool fast) {
    Guest g{"uninspected save", setup(), {}};
    auto& words = g.words;
    load(words, 0, 0x00223000); store(words, 0, 8, 58);
    load(words, 0, 0x00203000); store(words, 0, 8, 6); store(words, 0, 0, 0x13);
    for (unsigned i = 0; i < 24; ++i) words.push_back(0xe1a00000);
    words.push_back(0xe321f013);
    words.push_back(0xe3a06055); words.push_back(0xe7f000f0);
    installTimerHandler(words);
    g.options.bootstrapPeripherals = true;
    const Imx1MachineBounds step{1, never, 100000};
    std::vector<Bytes> inspected;
    std::vector<bool> pending;
    {
        Live a(g, fast);
        for (unsigned k = 0; k < 40; ++k) {
            imx1MachineRunBounded(a.machine, step);
            const auto run = imx1MachineInspect(a.machine);
            if (run.status == Imx1MachineStatus::terminal || run.status == Imx1MachineStatus::stopped) break;
            pending.push_back(run.result.interruptPending.normal && !run.result.irqEntries);
            inspected.push_back(a.save());
        }
    }
    unsigned first = 0, count = 0;
    for (size_t k = 0; k < pending.size(); ++k)
        if (pending[k] && !count++) first = unsigned(k);
    assert(count >= 8 && first > 0 && !pending[first - 1]);
    {
        // Saves at every step.
        Live b(g, fast);
        for (size_t k = 0; k < inspected.size(); ++k) {
            imx1MachineRunBounded(b.machine, step);
            assert(b.save() == inspected[k]);
        }
    }
    // The first save is with the interrupt pending, then it's loaded back and saved again.
    for (unsigned at : {first, first + 3}) {
        Live b(g, fast);
        for (unsigned k = 0; k <= at; ++k) imx1MachineRunBounded(b.machine, step);
        const auto saved = b.save();
        assert(saved == inspected[at]);
        b.load(saved);
        assert(b.save() == saved);
        imx1MachineRunBounded(b.machine, step);
        assert(b.save() == inspected[at + 1]);
    }
}
// Sections 0 (ROM, I-cache), 2 (registers), 3 and 5 (SRAM, cached) and 4 (SRAM, uncached alias).
void mmuCaches(bool fast, bool useBounded) {
    Guest g{"MMU and caches", setup(), {}};
    auto& words = g.words;
    load(words, 0, 0x00304000);
    store(words, 0, 0x00, 0x00000c1a); store(words, 0, 0x08, 0x00200c12); store(words, 0, 0x0c, 0x00300c1e);
    store(words, 0, 0x10, 0x00300c12); store(words, 0, 0x14, 0x00300c1e);
    words.push_back(0xee020f10);
    load(words, 1, 1); words.push_back(0xee031f10);
    load(words, 1, 0x107d); words.push_back(0xee011f10);
    load(words, 2, 0x00308004); words.push_back(0xe5923000);
    load(words, 4, 0xdeadbeef); words.push_back(0xe5824000);        // dirty D-cache line
    load(words, 5, 0x00508100); words.push_back(0xe5953000);        // TLB entry for section 5
    load(words, 6, 0x00404014);
    words.push_back(0xe3a07000); words.push_back(0xe5867000);       // the table drops section 5, the TLB keeps it
    words.push_back(0xe5958040);
    words.push_back(0xe5929000);
    load(words, 10, 0x00408004); words.push_back(0xe59ab000);       // memory still holds the old word
    load(words, 13, 0x00308000); words.push_back(0xee07df3a);       // clean the line by address
    words.push_back(0xe59ac000);
    words.push_back(0xe7f000f0);
    words.resize(0x800 / 4, 0xe1a00000);
    g.options.bootstrapPeripherals = true;
    std::vector<Call> calls(120, useBounded ? bounded(1, 100000) : slice(1, 10000));
    const auto r = roundTrip(g, fast, calls, [](const std::vector<Record>& all, size_t k) {
        const auto i = all[k].run.result.trace.instruction;
        return i == 0xee011f10 || i == 0xe5824000 || i == 0xe5867000 || i == 0xe5958040;
    });
    const auto& end = r.back().run;
    if (end.status != Imx1MachineStatus::terminal || end.result.trace.instruction != 0xe7f000f0)
        std::fprintf(stderr, "MMU guest stop=%s pc=%08x address=%08x\n", imx1ProbeStopName(end.result.stop),
                     end.result.trace.pc, end.result.address);
    assert(end.status == Imx1MachineStatus::terminal && end.result.trace.instruction == 0xe7f000f0);
    assert(end.result.registers[9] == 0xdeadbeef && end.result.registers[11] == 0 &&
           end.result.registers[12] == 0xdeadbeef);
}
void sdram(bool fast) {
    Guest g{"SDRAM", setup(), {}};
    auto& words = g.words;
    load(words, 0, 0x00221000);
    load(words, 1, 0x91120200); words.push_back(0xe5801000);
    load(words, 2, 0x08200000); words.push_back(0xe5923000);
    load(words, 1, 0xa1120200); words.push_back(0xe5801000);
    load(words, 2, 0x08000000);
    for (unsigned i = 0; i < 8; ++i) words.push_back(0xe5923000);
    load(words, 1, 0xb1120200); words.push_back(0xe5801000);
    load(words, 2, 0x08111800); words.push_back(0xe5923000);
    load(words, 1, 0x81129200); words.push_back(0xe5801000);
    load(words, 4, 0x08000000);
    load(words, 1, 0x12345678); words.push_back(0xe5841010);
    load(words, 1, 0x9abcdef0); words.push_back(0xe5841014);
    words.push_back(0xe5945010); words.push_back(0xe5946014);
    words.push_back(0xe7f000f0);
    g.options.bootstrapPeripherals = true; g.options.ramMiB = 32;
    // Saves in the middle of the refresh commands and after the first word is stored.
    const auto r = roundTrip(g, fast, {slice(22, 0), slice(22, 0), slice(3, 0), slice(100, 0)}, always);
    const auto& end = r.back().run;
    if (end.status != Imx1MachineStatus::terminal || end.result.registers[5] != 0x12345678)
        std::fprintf(stderr, "SDRAM guest stop=%s pc=%08x address=%08x status=%s\n", imx1ProbeStopName(end.result.stop),
                     end.result.trace.pc, end.result.address, imx1BootstrapAccessName(end.result.peripheralStatus));
    assert(end.status == Imx1MachineStatus::terminal && end.result.trace.instruction == 0xe7f000f0);
    assert(end.result.registers[5] == 0x12345678 && end.result.registers[6] == 0x9abcdef0);
}
// The power-on state loaded with imx1StateKeepMemory is a warm reset: the machine starts over and
// RAM keeps what the guest stored.
void keepMemory() {
    Guest g{"keep memory", setup(), {}};
    auto& words = g.words;
    // r8 = RSR, the SDRAM setup of the guest above, then r7 = [0x08000010] before the store there.
    load(words, 0, 0x0021b800); words.push_back(0xe5908000);
    load(words, 0, 0x00221000);
    load(words, 1, 0x91120200); words.push_back(0xe5801000);
    load(words, 2, 0x08200000); words.push_back(0xe5923000);
    load(words, 1, 0xa1120200); words.push_back(0xe5801000);
    load(words, 2, 0x08000000);
    for (unsigned i = 0; i < 8; ++i) words.push_back(0xe5923000);
    load(words, 1, 0xb1120200); words.push_back(0xe5801000);
    load(words, 2, 0x08111800); words.push_back(0xe5923000);
    load(words, 1, 0x81129200); words.push_back(0xe5801000);
    load(words, 4, 0x08000000);
    words.push_back(0xe5947010);
    load(words, 1, 0x12345678); words.push_back(0xe5841010);
    words.push_back(0xe5945010);
    words.push_back(0xe7f000f0);
    g.options.bootstrapPeripherals = true; g.options.ramMiB = 32;
    Live a(g, false);
    const Bytes powerOn = a.save();
    auto finish = [&] {
        imx1MachineRunSlice(a.machine, 1000, false);
        const auto run = imx1MachineInspect(a.machine);
        assert(run.status == Imx1MachineStatus::terminal && run.result.trace.instruction == 0xe7f000f0);
        assert(run.result.registers[5] == 0x12345678);
        return run.result.registers[7];
    };
    const uint32_t first = finish();
    assert(first != 0x12345678);
    // A plain load brings RAM back too.
    a.load(powerOn);
    assert(a.save() == powerOn && finish() == first);
    const auto source = [&] { return imx1MachineInspect(a.machine).result.registers[8]; };
    // The reset source register reads 0 after power-on (POR), and after a load that says nothing else.
    assert(source() == 0);
    const auto info = imx1MachineLoadState(a.machine, powerOn.data(), powerOn.size(), imx1StateKeepMemory);
    assert(info.status == Imx1StateStatus::ok && imx1MachineGetStatus(a.machine).retired == 0);
    assert(a.save() != powerOn);
    assert(finish() == 0x12345678 && source() == 0);
    // A reset through RESET_IN reads EXR, a state saved after it keeps that, and power-on clears it.
    assert(imx1MachineLoadState(a.machine, powerOn.data(), powerOn.size(), imx1StateExternalReset).status == Imx1StateStatus::ok);
    const Bytes afterReset = a.save();
    assert(afterReset != powerOn && finish() == 0x12345678 && source() == 1);
    a.load(powerOn);
    assert(finish() == first && source() == 0);
    a.load(afterReset);
    assert(finish() == 0x12345678 && source() == 1);
}
// A guest for the virtual graphics profile. Every bus access runs from the PC the graphics
// models expect for it, reached from a stub that loads the address, the value and the return.
struct GraphicsGuest {
    std::vector<uint32_t> words = std::vector<uint32_t>(0x100000 / 4);
    uint32_t cursor = 0x40000;
    // Instructions retired once everything placed so far has run.
    uint64_t retired = 1;
    GraphicsGuest() { branch(0, cursor); }
    void word(uint32_t at, uint32_t value) {
        assert(at / 4 < words.size() && (!words[at / 4] || words[at / 4] == value));
        words[at / 4] = value;
    }
    void branch(uint32_t at, uint32_t target) { word(at, 0xea000000 | (((target - at - 8) / 4) & 0xffffff)); }
    // Returns the retired count right after the access. A pause is 64 instructions for a latch or a delay.
    uint64_t op(uint32_t pc, uint32_t address, bool write, uint32_t value, unsigned size = 4, bool pause = false) {
        if (!pc) pc = write ? 0xff000 : 0xff010;
        word(cursor, 0xe59f0010); word(cursor + 4, 0xe59f6010); word(cursor + 8, 0xe59fe010);
        branch(cursor + 12, pc);
        words[cursor / 4 + 6] = address; words[cursor / 4 + 7] = value; words[cursor / 4 + 8] = 0x20000000 + cursor + 0x24;
        word(pc, size == 1 ? (write ? 0xe5c06000 : 0xe5d06000) : (write ? 0xe5806000 : 0xe5906000));
        word(pc + 4, 0xe12fff1e);
        cursor += 0x24;
        const uint64_t mark = retired + 5;
        retired += 6;
        if (pause) { for (unsigned i = 0; i < 64; ++i, cursor += 4) word(cursor, 0xe1a00000); retired += 64; }
        return mark;
    }
    uint64_t store(uint32_t address, uint32_t value) { return op(0, address, true, value); }
    uint64_t graphics(uint32_t pc, uint32_t offset, uint32_t value, bool pause = false) {
        return op(pc, 0x16010000 + offset, true, value, 4, pause);
    }
    // A counted loop of two instructions.
    void spin(uint32_t count) {
        word(cursor, 0xe59f2008); word(cursor + 4, 0xe2522001); word(cursor + 8, 0x1afffffd); word(cursor + 12, 0xea000000);
        words[cursor / 4 + 4] = count;
        cursor += 0x14;
        retired += 2 + uint64_t(count) * 2;
    }
    // Wait for interrupt. Returns the retired count while waiting.
    uint64_t wait() { word(cursor, 0xee070f90); cursor += 4; return ++retired; }
    void end() { word(cursor, 0xe7f000f0); cursor += 4; ++retired; }

    // The interface, controller and engine setup of zodiac-graphics-setup.h and the commands test.
    void setup() {
        store(0x0021b000, 0x57000003); store(0x0021b020, 0);
        store(0x00220028, 0x3e00); store(0x0022002c, 0x11111e01);
        op(0xbab4c, 0x16000010, false, 0, 1); op(0xbabf8, 0x1600001c, true, 0xe, 1);
        op(0xc50bc, 0x16010018, true, 0xc000000); op(0xc50dc, 0x16000014, true, 0x11, 1);
        graphics(0xc50f4, 0x188, 0x15ff1000); graphics(0xc5130, 0x190, 0xffff8000);
        graphics(0xb31b0, 0x1a4, 0x7800); graphics(0xb31bc, 0x180, 0x100007);
        struct External { uint32_t pc, offset, value; bool write; };
        for (auto e : {External{0xc4114, 0x1b8, 0, false}, {0xc4130, 0x1b0, 0, false}, {0xc413c, 0x1b0, 0xff0000, true},
                {0xc4144, 0x194, 0, false}, {0xc4150, 0x194, 0xff000000, true}, {0xc4158, 0x18c, 0, false},
                {0xc4164, 0x18c, 0x80000, true}, {0xc4178, 0x18c, 0x80000, false}, {0xc4188, 0x18c, 0x70080000, true},
                {0xc4190, 0x18c, 0x70080000, false}, {0xc4198, 0x18c, 0x70080001, true}, {0xc41c4, 0x1b0, 0xff0000, false},
                {0xc41d4, 0x1b0, 0, true}, {0xc41e8, 0x194, 0xff000000, false}, {0xc4220, 0x194, 0x15001545, true},
                {0xc4234, 0x18c, 0x70080001, false}, {0xc423c, 0x18c, 0x70000001, true}, {0xc4250, 0x198, 0, false},
                {0xc4264, 0x18c, 0x70000001, false}, {0xc42c0, 0x18c, 0x7000000d, true}, {0xc42d4, 0x18c, 0x7000000d, false},
                {0xc42e4, 0x18c, 0x7964000d, true}, {0xc42f8, 0x18c, 0x7964000d, false}, {0xc4304, 0x18c, 0x964000d, true},
                {0xc4318, 0x18c, 0x964000d, false}, {0xc4320, 0x18c, 0x964000d, true}, {0xc4334, 0x198, 0x1600021, true},
                {0xc434c, 0x198, 0x81600021, true}, {0xc4364, 0x198, 0x1600021, true}})
            op(e.pc, 0x16010000 + e.offset, e.write, e.value, 4, e.pc == 0xc4364);
        graphics(0xc6888, 0x19c, 0x7bbb7444);
        op(0xacb98, 0x160113e8, false, 0);
        for (auto e : {External{0xacba8, 0x13e8, 0x400, true}, {0xacbb4, 0x144, 0x2440, true}, {0xacbc4, 0x10a8, 0x1fff1fff, true},
                {0xacbd0, 0x4dc, 0xc0000, true}, {0xb8e80, 0x214, 0x10150000, true}, {0xb8e88, 0x220, 0, true},
                {0xb8f04, 8, 0x04150000, true}, {0xb8f10, 0x230, 0x80000000, true}, {0xb8f20, 0x210, 0x0800000d, true}})
            graphics(e.pc, e.offset, e.value);
    }
    // The 3x2 display of the machine frame test, two rows of pixels at the start of the external memory.
    void display() {
        store(0x16800000, 0x07e0f800); store(0x16800004, 0x0000001f); store(0x16800008, 0x0000ffff); store(0x1680000c, 0x0000ffe0);
        const auto transaction = [&](bool begin) { graphics(0xaeb68, 0x4d8, begin ? 0x78 : 0x7b, !begin); };
        transaction(true);
        graphics(0xad720, 0x418, 0x800000); graphics(0xad72c, 0x41c, 8); graphics(0xad738, 0x414, 6);
        transaction(false);
        transaction(true);
        struct Word { uint32_t pc, offset, value; };
        for (auto w : {Word{0xad65c, 0x42c, 0x40001}, {0xad668, 0x430, 0x30001}, {0xad2e0, 0x410, 3}, {0xad404, 0x540, 0x80604020},
                {0xad410, 0x544, 0xffdfbf9f}, {0xad418, 0x548, 0}, {0xad420, 0x578, 0x80604020}, {0xad428, 0x58c, 0xffdfbf9f},
                {0xad430, 0x590, 0}, {0xad438, 0x594, 0x80604020}, {0xad440, 0x598, 0xffdfbf9f}, {0xad448, 0x59c, 0},
                {0xad528, 0x538, 0x800000}, {0xad3b8, 0x524, 0x102aa}, {0xad2ec, 0x414, 0x1800406}, {0xad2f8, 0x420, 0x60008},
                {0xad304, 0x424, 0x40001}, {0xad310, 0x428, 0x30001}})
            graphics(w.pc, w.offset, w.value);
        transaction(false);
        transaction(true);
        graphics(0xad8b4, 0x414, 0x1801c06);
        transaction(false);
    }
    // Programmed graphics clocks: the display and the engine then run on guest time.
    void clocks() {
        graphics(0xc5f20, 0x194, 0x1545);
        // The aperture byte is read and written back by two instructions in a row.
        op(0xc1118, 0x16000010, false, 0, 1);
        words[0xc111c / 4] = 0xe5c06000; word(0xc1120, 0xe12fff1e);
        ++retired;
        graphics(0xc45e4, 0x80, 0x3f); graphics(0xc4920, 0x90, 0x20, true);
    }
    uint32_t ring = 0;
    // Puts a batch in the ring and submits it from the given producer PC. Returns the retired count after the submission.
    uint64_t submit(uint32_t pc, const std::vector<uint32_t>& batch) {
        for (auto value : batch) store(0x16150000 + 4 * ring++, value);
        return graphics(pc, 0x220, ring);
    }
    uint64_t busy() { return op(0xb8cb0, 0x16010140, false, 0); }
    // A white 96x64 fill over the display's memory, 6193 ticks of engine work.
    uint64_t fill() {
        const uint32_t regs[]{0x4a1, 0x41b, 0x41f, 0x472, 0x4b2, 0x4b1, 0x490, 0x491, 0x46f, 0x470, 0x401, 0x402, 0x46b, 0x46c, 0x41e,
            0x488, 0x484, 0x471, 0x423, 0x41d, 0x4b3, 0x30f, 0x42e, 0x42f, 0x430, 0x431, 0x432, 0x433, 0x48f, 0x4a0};
        const uint32_t values[]{0, 0x55e0b4df, 0xffff, 0x3f, 0xf00a00, 0x40030d04, 0xffffffff, 0, 0, 0x1e00140, 0x800000, 0x140,
            0x100000, 0x140, 0, 4, 0xf0f0f, 0x1fff1fff, 0x100000, 0, 0xffffffff, 0x20, 0, 0, 0, 0, 0, 0, 0xffffffff, 0};
        std::vector<uint32_t> batch;
        for (unsigned i = 0; i < 30; i += 2)
            for (auto value : {0x40000000 | regs[i] | (regs[i + 1] << 11), values[i], values[i + 1]}) batch.push_back(value);
        for (auto value : {0xc0011a00u, 0u, 0x00400060u}) batch.push_back(value);
        return submit(0xa9ed4, batch);
    }
    // Host pixels over the display's first row: 32x32 RGB565, or 64x32 packed 1bpp with the packed mono policy.
    uint64_t hostData(bool mono) {
        std::vector<uint32_t> batch{0x4025941b, mono ? 0x579804dfu : 0x5798b4dfu, 0x00cc0b00, 0x40200c70, 0x0022002c, 0x00800000, 0x402, 0x140};
        const uint32_t width = mono ? 64 : 32, height = 32, payload = mono ? width / 32 * height : width / 2 * height;
        for (auto value : {0xc0001900u | ((payload + 1) << 16), 0u, height << 16 | width}) batch.push_back(value);
        for (uint32_t i = 0; i < payload; ++i) batch.push_back(mono ? 0x5a3c0ff0u + i * 0x01010101u : 0x07e0f800u + i * 0x00210021u);
        return submit(0xac300, batch);
    }
};
Imx1ProbeOptions graphicsOptions(bool mono) {
    Imx1ProbeOptions o;
    o.bootstrapPeripherals = true;
    o.graphicsProfile = Imx1GraphicsProfile::virtual5744Compatible8MiB;
    o.graphicsVirtualPackedMono = mono;
    return o;
}
bool terminalAt(const Record& r, const GraphicsGuest& g) {
    return r.run.status == Imx1MachineStatus::terminal && r.run.result.stop == Imx1ProbeStop::unsupportedInstruction &&
           r.run.retiredInstructions == g.retired - 1;
}
// A fill and then host pixels, with the engine counting CPU cycles or, clocked, graphics time.
// Saves: frame pending, right after each submission, while each batch is busy, and after it.
void graphicsBatches(bool fast, bool mono, bool clocked) {
    GraphicsGuest g;
    g.setup(); g.display();
    const auto shown = g.retired;
    if (clocked) g.clocks();
    const auto programmed = g.retired;
    const auto filling = g.fill(), fillBusy = g.busy();
    g.spin(500);
    const auto fillSpinning = g.retired;
    g.spin(4000);
    g.busy();
    const auto filled = g.retired;
    const auto host = g.hostData(mono), hostBusy = g.busy();
    g.spin(2000);
    g.busy();
    const auto hosted = g.retired;
    g.op(0, 0x16800000, false, 0);
    g.end();
    const Guest guest{mono ? "graphics batches, packed mono" : clocked ? "graphics batches, clocked" : "graphics batches", g.words,
                      graphicsOptions(mono), false, Imx1ProbeEntry::linkedAlias};
    const std::vector<Call> calls{until(shown), until(programmed, 0, true), until(filling), until(fillBusy), until(fillSpinning),
        until(filled), until(host, 0, true), until(hostBusy), until(hosted), until(g.retired, 0, true)};
    const auto r = roundTrip(guest, fast, calls, [](const std::vector<Record>&, size_t k) {
        return k == 1 || k == 3 || k == 4 || k == 5 || k == 7 || k == 8;
    });
    assert(r.size() == 11);
    const size_t quiet = r[1].save.size();
    // The first frame is still pending in save 1: the next call takes it, on the loaded machines too.
    assert(r[1].shown.present && r[1].shown.width == 3 && r[1].shown.height == 2);
    assert(r[2].acquired.present && r[2].acquired.first == 0 && r[2].acquired.last == 1 && r[2].acquired.hash == r[1].shown.hash);
    // The fill is queued and busy in saves 3 to 5, with its batch in the state.
    for (size_t k = 3; k <= 5; ++k) assert(r[k].save.size() > quiet && r[k].shown == r[1].shown);
    assert(r[4].run.result.trace.pc == 0x200b8cb0 && r[4].run.result.registers[6] == 0x80000000);
    assert(r[6].run.result.registers[6] == 0 && r[6].save.size() == quiet && !(r[6].shown == r[1].shown));
    assert(r[7].acquired.present && r[7].acquired.hash == r[6].shown.hash);
    // The host batch carries its pixels: 32x32, or the 44 columns the scissor leaves of 64x32.
    assert(r[7].save.size() > quiet + 2000 && r[8].save.size() == r[7].save.size());
    assert(r[8].run.result.trace.pc == 0x200b8cb0 && r[8].run.result.registers[6] == 0x80000000);
    assert(r[9].run.result.registers[6] == 0 && r[9].save.size() == quiet && !(r[9].shown == r[6].shown));
    assert(r[10].acquired.present && r[10].acquired.hash == r[9].shown.hash && terminalAt(r[10], g));
    if (!mono) assert(r[10].run.result.registers[6] == 0x07e0f800);
}
// The CPU waits in WFI for timer 2 with the display and a fill running on programmed graphics
// clocks. The interrupt stays masked, so the guest carries on after the wait. Saves in the middle of the wait.
std::vector<Record> graphicsWait(bool fast, bool stepped) {
    GraphicsGuest g;
    g.setup(); g.display();
    const auto shown = g.retired;
    g.clocks();
    const auto programmed = g.retired;
    const auto filling = g.fill();
    g.store(0x00223008, 58); g.store(0x00203008, 30000); g.store(0x00203000, 0x13);
    const auto waiting = g.wait();
    g.store(0x00203000, 0); g.op(0, 0x00203014, false, 0); g.store(0x00203014, 0);
    g.busy();
    const auto woken = g.retired;
    const auto host = g.hostData(false);
    g.spin(2000);
    g.busy();
    g.end();
    Guest guest{stepped ? "graphics wait, stepped" : "graphics wait", g.words, graphicsOptions(false), false, Imx1ProbeEntry::linkedAlias};
    guest.options.idleStepped = stepped;
    const uint64_t all = 10000000;
    std::vector<Call> calls{until(shown), until(programmed, 0, true), until(filling)};
    for (uint64_t idle : {1, 2, 40, 700, 5000, 20000}) calls.push_back(until(waiting + 2, idle, idle == 20000));
    calls.push_back(until(waiting + 2, all));
    calls.push_back(until(woken, all));
    calls.push_back(until(host, all));
    calls.push_back(until(g.retired, all));
    const auto r = roundTrip(guest, fast, calls, [](const std::vector<Record>&, size_t k) { return k >= 3 && k <= 9 && k != 5; });
    assert(r.size() == 14);
    const size_t quiet = r[2].save.size();
    constexpr uint64_t idle[] = {1, 2, 40, 700, 5000, 20000};
    for (size_t k = 4; k <= 9; ++k)
        assert(r[k].run.result.stop == Imx1ProbeStop::idleBudget && r[k].run.result.idleHclk == idle[k - 4] &&
               r[k].run.retiredInstructions == r[4].run.retiredInstructions);
    // The fill is still queued 700 clocks into the wait and done by 5000, and the display shows it.
    for (size_t k = 3; k <= 7; ++k) assert(r[k].save.size() > quiet && r[k].shown == r[2].shown);
    assert(r[8].save.size() == quiet && !(r[8].shown == r[2].shown));
    // That frame is pending in the mid-wait save 8.
    assert(r[9].acquired.present && r[9].acquired.hash == r[8].shown.hash);
    assert(r[10].run.result.idleHclk > 20000 && r[10].run.retiredInstructions == waiting + 2);
    assert(r[11].run.result.registers[6] == 0 && r[12].save.size() > quiet + 2000);
    assert(!(r[13].shown == r[8].shown) && terminalAt(r[13], g) && r[13].run.result.irqEntries == 0);
    assert(stepped ? r[13].idleAdvances == r[13].run.result.idleHclk : r[13].idleAdvances * 8 < r[13].run.result.idleHclk);
    return r;
}
// The idle skip and the stepped loop leave the same state at every save, with far fewer passes.
void graphicsWaits(bool fast) {
    const auto skipped = graphicsWait(fast, false), stepped = graphicsWait(fast, true);
    assert(skipped.size() == stepped.size());
    for (size_t k = 0; k < skipped.size(); ++k) assert(skipped[k].save == stepped[k].save);
}
// CS4 with a port 0 input and an indexed sample, saved between the byte accesses.
void cs4Inputs(bool fast) {
    Guest g{"CS4 inputs", setup(), {}};
    auto& words = g.words;
    load(words, 0, 0x00220020); store(words, 0, 0, 0x3e70); store(words, 0, 4, 0xffff4301);
    load(words, 0, 0x15000000);
    for (uint32_t word : {0xe3a0100fu, 0xe5c01000u, 0xe3a01001u, 0xe5c01001u, 0xe3a01020u, 0xe5c01000u,
                          0xe5d02001u, 0xe5d03000u, 0xe7f000f0u}) words.push_back(word);
    g.options.bootstrapPeripherals = true; g.options.cs4Port0InputKnown = true; g.options.cs4Port0Input = 0x5a;
    assert(g.options.cs4IndexedInputs.set(1, 0x20, 0x77));
    const auto r = roundTrip(g, fast, std::vector<Call>(40, slice(1, 0)), [](const std::vector<Record>& all, size_t k) {
        return (all[k].run.result.trace.instruction & 0xfffff000) == 0xe5c01000;
    });
    const auto& end = r.back().run;
    if (end.status != Imx1MachineStatus::terminal || end.result.trace.instruction != 0xe7f000f0)
        std::fprintf(stderr, "CS4 guest stop=%s pc=%08x address=%08x status=%s\n", imx1ProbeStopName(end.result.stop),
                     end.result.trace.pc, end.result.address, imx1BootstrapAccessName(end.result.peripheralStatus));
    assert(end.status == Imx1MachineStatus::terminal && end.result.trace.instruction == 0xe7f000f0);
    assert(end.result.registers[2] == 0x77 && end.result.registers[3] == 0x5a);
}
// The strict profile's CS5 reset helper with a status input, saved in each of its phases.
void cs5ResetInput(bool fast) {
    GraphicsGuest g;
    g.store(0x00220028, 0x3e00); g.store(0x0022002c, 0x11111e01);
    std::vector<Call> calls;
    calls.push_back(until(g.op(0xbac84, 0x16000018, false, 0, 1)));
    calls.push_back(until(g.op(0xbac9c, 0x16000018, true, 0x29, 1)));
    calls.push_back(until(g.op(0xbaca4, 0x16000018, false, 0, 1)));
    calls.push_back(until(g.op(0xbace8, 0x16000018, true, 0x21, 1)));
    calls.push_back(until(g.op(0xbacf0, 0x16000018, false, 0, 1)));
    g.end();
    calls.push_back(until(g.retired));
    Guest guest{"CS5 reset input", g.words, {}, false, Imx1ProbeEntry::linkedAlias};
    guest.options.bootstrapPeripherals = true; guest.options.cs5ResetStatusInputKnown = true; guest.options.cs5ResetStatusInput = 0x21;
    const auto r = roundTrip(guest, fast, calls, always);
    assert(r.size() == 7 && r[1].run.result.registers[6] == 0x21 && r[3].run.result.registers[6] == 0x29 &&
           r[5].run.result.registers[6] == 0x21 && terminalAt(r[6], g));
}
void refusals() {
    const auto g = timerGuest();
    Bytes exact, other;
    {
        Live a(g, false);
        imx1MachineRunSlice(a.machine, 500, 1);
        exact = a.save();
        auto check = [&](const Bytes& b) { return imx1MachineCheckState(a.machine, b.data(), b.size()).status; };
        assert(check(exact) == Imx1StateStatus::ok);
        for (size_t size : {size_t(0), size_t(20), exact.size() / 2, exact.size() - 1}) {
            Bytes cut(exact.begin(), exact.begin() + size);
            assert(check(cut) == Imx1StateStatus::truncated);
        }
        Bytes bad = exact; bad[0] ^= 1; assert(check(bad) == Imx1StateStatus::badMagic);
        // A flipped bit anywhere is refused, and a refused load leaves the machine as it was.
        for (size_t at = 8; at < exact.size(); at += exact.size() / 97 + 1) {
            bad = exact; bad[at] ^= 0x10;
            assert(check(bad) != Imx1StateStatus::ok);
            assert(imx1MachineLoadState(a.machine, bad.data(), bad.size()).status != Imx1StateStatus::ok);
        }
        assert(a.save() == exact);
        const auto noted = imx1MachineCheckState(a.machine, exact.data(), exact.size(), 0, "another build");
        assert(noted.status == Imx1StateStatus::ok && noted.buildNoteDiffers);
        imx1MachineStop(a.machine);
        assert(imx1MachineSaveState(a.machine, other) == Imx1StateStatus::notAtBoundary);
        assert(check(exact) == Imx1StateStatus::notAtBoundary);
    }
    {
        // An Exact state loads into a Fast machine and says so, then runs like Fast from there.
        Live f(g, true);
        const auto info = imx1MachineLoadState(f.machine, exact.data(), exact.size());
        assert(info.status == Imx1StateStatus::ok && info.engineChanged);
        const auto done = imx1MachineRunSlice(f.machine, 500, 10000);
        assert(done.status == Imx1MachineStatus::terminal && done.result.registers[6] == 0x55 && done.result.irqEntries == 1);
    }
    {
        auto changed = g; changed.words[changed.words.size() - 1] ^= 1;
        Live m(changed, false);
        assert(imx1MachineCheckState(m.machine, exact.data(), exact.size()).status == Imx1StateStatus::romMismatch);
        assert(imx1MachineLoadState(m.machine, exact.data(), exact.size()).status == Imx1StateStatus::romMismatch);
    }
    {
        auto changed = g; changed.options.rtcInitialTimeKnown = true;
        Live m(changed, false);
        assert(imx1MachineCheckState(m.machine, exact.data(), exact.size()).status == Imx1StateStatus::optionsMismatch);
        // The boot input flag excuses only the GPIO A and D levels and the scheduled event.
        assert(imx1MachineCheckState(m.machine, exact.data(), exact.size(), imx1StateAllowBootInputs).status ==
               Imx1StateStatus::optionsMismatch);
    }
    {
        // Other GPIO A and D levels at reset are refused, and taken with the boot input flag. The
        // state then runs on with the pin levels it holds.
        auto changed = g; changed.options.gpioAInput ^= 0x400; changed.options.gpioDInput ^= 0x1000;
        Live m(changed, false);
        const auto refused = imx1MachineCheckState(m.machine, exact.data(), exact.size());
        assert(refused.status == Imx1StateStatus::optionsMismatch && !std::strncmp(refused.detail, "gpio", 4));
        assert(imx1MachineLoadState(m.machine, exact.data(), exact.size()).status == Imx1StateStatus::optionsMismatch);
        assert(imx1MachineLoadState(m.machine, exact.data(), exact.size(), imx1StateAllowBootInputs).status == Imx1StateStatus::ok);
        const auto done = imx1MachineRunSlice(m.machine, 500, 10000);
        assert(done.status == Imx1MachineStatus::terminal && done.result.registers[6] == 0x55 && done.result.irqEntries == 1);
    }
    {
        // The idle stepping choice is not part of a state.
        auto stepped = g; stepped.options.idleStepped = !stepped.options.idleStepped;
        Live m(stepped, false);
        m.load(exact);
        assert(m.save() == exact);
    }
    {
        // A frame that was acquired and not acknowledged stops a save and a load.
        GraphicsGuest display;
        display.setup(); display.display(); display.end();
        const Guest shown{"acquired frame", display.words, graphicsOptions(false), false, Imx1ProbeEntry::linkedAlias};
        Bytes pending;
        {
            Live a(shown, false);
            imx1MachineRunSlice(a.machine, uint32_t(display.retired), 0);
            pending = a.save();
            uint32_t width = 0, height = 0, first = 0, last = 0;
            assert(imx1MachineAcquireFrame(a.machine, width, height, first, last) && width == 3 && height == 2);
            assert(imx1MachineSaveState(a.machine, other) == Imx1StateStatus::notAtBoundary);
            assert(imx1MachineCheckState(a.machine, pending.data(), pending.size()).status == Imx1StateStatus::notAtBoundary);
            assert(imx1MachineLoadState(a.machine, pending.data(), pending.size()).status == Imx1StateStatus::notAtBoundary);
            imx1MachineAcknowledgeFrame(a.machine);
            assert(a.save() != pending);
            a.load(pending);
            assert(a.save() == pending && imx1MachineAcquireFrame(a.machine, width, height, first, last));
            imx1MachineAcknowledgeFrame(a.machine);
        }
        // The packed mono policy is part of the machine.
        auto mono = shown; mono.options.graphicsVirtualPackedMono = true;
        Live m(mono, false);
        assert(imx1MachineCheckState(m.machine, pending.data(), pending.size()).status == Imx1StateStatus::optionsMismatch);
    }
}
}
// zodiac-machine-state-reject.cpp includes this file for its guests and renames this.
#ifndef ZODIAC_STATE_MAIN
#define ZODIAC_STATE_MAIN main
#endif
int ZODIAC_STATE_MAIN() {
    unsetenv("FROND_JIT"); cpuSetJitSync(true); setenv("FROND_JIT_HOT", "1", 1);
    for (bool fast : {false, true}) {
        timerWfi(fast);
        penWait(fast);
        controlsWait(fast);
        uartWfi(fast);
        for (bool ldm : {false, true}) thumbBl(fast, ldm, ldm);
        maskedIrq(fast);
        uninspectedSave(fast);
        mmuCaches(fast, false);
        mmuCaches(fast, true);
        sdram(fast);
        std::printf("PASS: %s saves continue identically on fresh and reused machines, slices and bounded runs\n",
                    fast ? "Fast" : "Exact");
        cs4Inputs(fast);
        cs5ResetInput(fast);
        graphicsBatches(fast, false, false);
        graphicsBatches(fast, false, true);
        graphicsBatches(fast, true, true);
        graphicsWaits(fast);
        std::printf("PASS: %s graphics saves with a batch queued and busy, host pixels, packed mono and a frame pending "
                    "continue identically, and so do saves inside a WFI wait on graphics clocks, skipped and stepped\n",
                    fast ? "Fast" : "Exact");
    }
    keepMemory();
    std::puts("PASS: the power-on state loaded with its memory kept starts over with RAM as it was");
    refusals();
    std::puts("PASS: damaged, truncated and mismatched states are refused without changing the machine");
    return 0;
}
