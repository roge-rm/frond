// Zodiac guest exceptions: with imx1SetGuestExceptions on, a data abort (ARM, in a hot loop, and Thumb), a prefetch
// abort and an undefined instruction (ARM and Thumb) reach the guest's own vectors with the ARM920T's entry state:
// mode, I set, F kept, ARM state, SPSR, the link for each kind, FSR and FAR for a data abort and both left alone
// for the others. The data abort handler reads FSR and FAR with CRm 5 and 6, as the Zodiac ROM's does. Exact,
// lean-only Fast and Fast blocks end the same. Off, the machine stops on them as before.
#define ZODIAC_STATE_MAIN stateMain
#include "zodiac-machine-state.cpp"

namespace {
enum class Kind { dataLoop, dataThumb, prefetch, undefinedArm, undefinedThumb };
const char* kindName(Kind k) {
    switch (k) {
        case Kind::dataLoop: return "data abort loop";
        case Kind::dataThumb: return "data abort thumb";
        case Kind::prefetch: return "prefetch abort";
        case Kind::undefinedArm: return "undefined arm";
        case Kind::undefinedThumb: return "undefined thumb";
    }
    return "?";
}
constexpr uint32_t handlerBase = 0x100, fsrMark = 0xa, farMark = 0x12345678, unmapped = 0x00400000;

struct FaultGuest { std::vector<uint32_t> words; uint32_t faultPc; };

// The graphics guest's setup gives quiet spans for the lean path and the JIT. Then sections: VA 0 the ROM (for
// the vectors), 2 peripherals, 3 SRAM and 0x200 the ROM; 4 and up are unmapped. Each handler reads FSR into r9 and
// FAR into r10, puts its number in r12 (1 undefined, 2 prefetch abort, 3 data abort) and stops on a reserved word.
FaultGuest guest(Kind kind) {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    const uint32_t vectors[] = {0x04, 0x0c, 0x10};
    for (unsigned n = 0; n < 3; ++n) {
        const uint32_t at = handlerBase + n * 0x20;
        g.branch(vectors[n], at);
        // The data abort handler reads them as the Zodiac ROM's does, with CRm 5 and 6.
        g.word(at, n == 2 ? 0xee159f15 : 0xee159f10);      // mrc p15, 0, r9, c5, c0 (or c5), 0
        g.word(at + 4, n == 2 ? 0xee16af16 : 0xee16af10);  // mrc p15, 0, r10, c6, c0 (or c6), 0
        g.word(at + 8, 0xe3a0c001 + n);
        g.word(at + 12, 0xffffffff);
    }
    std::vector<uint32_t> words;
    const auto pc = [&] { return 0x20000000 + g.cursor + uint32_t(words.size()) * 4; };
    load(words, 0, 0x00304000);
    store(words, 0, 0x00, 0x20000c1a); store(words, 0, 0x08, 0x00200c12); store(words, 0, 0x0c, 0x00300c12);
    load(words, 0, 0x00304800); store(words, 0, 0, 0x20000c1a);
    load(words, 0, 0x00304000);
    words.push_back(0xee020f10);                           // TTB
    load(words, 1, 1); words.push_back(0xee031f10);        // domain 0 client
    load(words, 1, 0x107d); words.push_back(0xee011f10);   // MMU, D- and I-cache on
    load(words, 1, fsrMark); words.push_back(0xee051f10);  // FSR
    load(words, 1, farMark); words.push_back(0xee061f10);  // FAR
    uint32_t faultPc = 0;
    const auto thumb = [&](uint16_t first) {
        words.push_back(0xe28f0001);  // add r0, pc, #1
        words.push_back(0xe12fff10);  // bx r0: Thumb from the next word
        faultPc = pc();
        words.push_back(first | 0x46c00000u);  // then nop
    };
    switch (kind) {
    case Kind::dataLoop: {
        // Loads 16 KiB apart from SRAM: the 63rd reaches section 4.
        load(words, 2, 0x00308000);
        words.push_back(0xe3a070c8);  // mov r7, #200
        faultPc = pc();
        words.push_back(0xe5925000);  // ldr r5, [r2]
        words.push_back(0xe2822901);  // add r2, r2, #0x4000
        words.push_back(0xe2577001);  // subs r7, r7, #1
        words.push_back(0x1afffffb);  // bne the ldr
        break;
    }
    case Kind::dataThumb:
        load(words, 2, unmapped);
        thumb(0x6815);  // ldr r5, [r2]
        break;
    case Kind::prefetch:
        load(words, 3, 0x00700000);
        words.push_back(0xe1a0f003);  // mov pc, r3
        faultPc = 0x00700000;
        break;
    case Kind::undefinedArm:
        faultPc = pc();
        words.push_back(0xe7f000f0);
        break;
    case Kind::undefinedThumb:
        thumb(0xde00);
        break;
    }
    words.push_back(0xffffffff);
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    return {g.words, faultPc};
}

struct Outcome { Imx1MachineRun run; Imx1MachineCheapStatus cheap; };
Outcome run(const std::vector<uint32_t>& words, bool fast) {
    auto o = graphicsOptions(false);
    o.ramMiB = 32;
    Imx1ProbeResult error{};
    Imx1Machine* m = imx1MachineCreate(reinterpret_cast<const uint8_t*>(words.data()), uint32_t(words.size() * 4),
                                       Imx1ProbeEntry::linkedAlias, false, o, nullptr, nullptr, &error);
    assert(m);
    if (fast) assert(imx1MachineSetFastEngine(m, true));
    for (unsigned n = 0; n < 100000 && imx1MachineGetStatus(m).status == Imx1MachineStatus::yielded; ++n)
        imx1MachineRunSlice(m, 997, 0);
    Outcome out{imx1MachineInspect(m), imx1MachineGetStatus(m)};
    imx1MachineStop(m);
    return out;
}
void print(const char* name, const char* engine, const Outcome& o) {
    const auto& r = o.run.result;
    std::printf("%s %s: stop=%s pc=%08x address=%08x retired=%llu cpsr=%08x spsr=%08x r2=%08x r7=%u r9=%08x "
                "r10=%08x r12=%u lr=%08x\n", name, engine, imx1ProbeStopName(r.stop), r.trace.pc, r.address,
                (unsigned long long)o.cheap.retired, r.cpsr, r.spsr, r.registers[2], r.registers[7], r.registers[9],
                r.registers[10], r.registers[12], r.registers[14]);
    std::fflush(stdout);
}

void check(Kind kind) {
    const auto g = guest(kind);
    const bool thumb = kind == Kind::dataThumb || kind == Kind::undefinedThumb;
    const bool undefined = kind == Kind::undefinedArm || kind == Kind::undefinedThumb;
    const bool data = kind == Kind::dataLoop || kind == Kind::dataThumb;
    const uint32_t before = thumb ? 0xf3 : 0xd3;
    // The off switch: the machine stops at the faulting instruction.
    imx1SetGuestExceptions(false);
    for (bool fast : {false, true}) {
        const auto o = run(g.words, fast);
        print(kindName(kind), fast ? "off fast" : "off exact", o);
        assert(o.run.status == Imx1MachineStatus::terminal && o.run.result.trace.pc == g.faultPc);
        assert(o.run.result.stop == (undefined ? Imx1ProbeStop::unsupportedInstruction : Imx1ProbeStop::exception));
    }
    imx1SetGuestExceptions(true);
    Outcome results[3];
    const char* engines[3] = {"exact", "lean", "blocks"};
    for (unsigned e = 0; e < 3; ++e) {
        imx1SetFastBlocksDefault(e == 2);
        results[e] = run(g.words, e != 0);
        print(kindName(kind), engines[e], results[e]);
    }
    imx1SetFastBlocksDefault(true);
    const auto& r = results[0].run.result;
    const uint32_t handler = handlerBase + (undefined ? 0 : data ? 0x40 : 0x20);
    assert(results[0].run.status == Imx1MachineStatus::terminal && r.stop == Imx1ProbeStop::unsupportedInstruction);
    assert(r.trace.pc == handler + 12 && r.trace.instruction == 0xffffffff);
    assert(r.registers[12] == (undefined ? 1u : data ? 3u : 2u));
    assert((r.cpsr & 0xff) == (0xc0u | (undefined ? 0x1bu : 0x17u)) && (r.spsr & 0xff) == before &&
           r.cpsr >> 28 == r.spsr >> 28);
    assert(r.registers[14] == g.faultPc + (undefined ? (thumb ? 2u : 4u) : data ? 8u : 4u));
    if (data) {
        assert(r.registers[9] == 5 && r.registers[10] == unmapped && r.registers[2] == unmapped);
        if (kind == Kind::dataLoop) assert(r.registers[7] == 200 - 62);
    } else {
        assert(r.registers[9] == fsrMark && r.registers[10] == farMark);
    }
    for (unsigned e = 1; e < 3; ++e) {
        const auto& f = results[e].run.result;
        assert(!std::memcmp(r.registers, f.registers, sizeof(r.registers)) && r.cpsr == f.cpsr && r.spsr == f.spsr);
        assert(f.stop == r.stop && f.trace.pc == r.trace.pc && results[e].cheap.retired == results[0].cheap.retired);
    }
    // Lean-only Fast keeps Exact's times as well.
    assert(results[1].cheap.time.ticks == results[0].cheap.time.ticks);
}
}  // namespace

int main() {
    cpuSetJitSync(true);
    setenv("FROND_JIT_HOT", "1", 1);
    for (auto kind : {Kind::dataLoop, Kind::dataThumb, Kind::prefetch, Kind::undefinedArm, Kind::undefinedThumb})
        check(kind);
    imx1SetGuestExceptions(false);
    std::puts("PASS: data and prefetch aborts and undefined instructions reach the guest's vectors with the ARM920T's "
              "entry state, the same in Exact and Fast, and stop the machine with the switch off");
}
