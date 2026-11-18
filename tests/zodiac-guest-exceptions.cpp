// Zodiac guest exceptions: with imx1MachineSetGuestExceptions on, a data abort (ARM, in a hot loop, and Thumb), a
// prefetch abort and an undefined instruction (ARM and Thumb) reach the guest's own vectors with the ARM920T's entry
// state: mode, I set, F kept, ARM state, SPSR, the link for each kind, FSR and FAR for a data abort and both left
// alone for the others, the prefetch FSR (c5, op2 1) for a prefetch abort only. The data abort handler reads FSR
// and FAR with CRm 5 and 6, as the Zodiac ROM's does. Data aborts on loads and stores with writeback, and an LDM
// that loads its base, leave the base as it was (the ARM920T's base restored model). With CP15 c1 A set, misaligned
// halfword and word loads and stores (ARM and Thumb, PACE's LDRH of an odd 68k PC among them) take an alignment data
// abort, FSR 1; with A clear a misaligned halfword stops the machine. Exact, lean-only Fast and Fast blocks end the
// same. The observer sees a prefetch abort's step as not fetched. Off, the machine stops on them.
// LDM and STM go from the lowest register and address up in every mode, as the ARM920T does: a DA or DB transfer that
// straddles the edge stores or loads its word below the edge before the abort.
#define ZODIAC_STATE_MAIN stateMain
#include "zodiac-machine-state.cpp"

namespace {
enum class Kind { dataLoop, dataThumb, prefetch, undefinedArm, undefinedThumb, ldrPre, ldrPost, strPre, ldmWriteback,
                  ldmBase, stmWriteback, stmdbWriteback, stmdaWriteback, ldmdbWriteback, thumbLdm, thumbStm, ldrhOdd,
                  strhOdd, thumbLdrhOdd, thumbLdrWord, ldrhOddNoA };
bool alignment(Kind k) {
    return k == Kind::ldrhOdd || k == Kind::strhOdd || k == Kind::thumbLdrhOdd || k == Kind::thumbLdrWord ||
           k == Kind::ldrhOddNoA;
}
const char* kindName(Kind k) {
    switch (k) {
        case Kind::dataLoop: return "data abort loop";
        case Kind::dataThumb: return "data abort thumb";
        case Kind::prefetch: return "prefetch abort";
        case Kind::undefinedArm: return "undefined arm";
        case Kind::undefinedThumb: return "undefined thumb";
        case Kind::ldrPre: return "ldr pre-indexed writeback";
        case Kind::ldrPost: return "ldr post-indexed";
        case Kind::strPre: return "str pre-indexed writeback";
        case Kind::ldmWriteback: return "ldmia writeback";
        case Kind::ldmBase: return "ldmia loading its base";
        case Kind::stmWriteback: return "stmia writeback";
        case Kind::stmdbWriteback: return "stmdb writeback";
        case Kind::stmdaWriteback: return "stmda writeback";
        case Kind::ldmdbWriteback: return "ldmdb writeback";
        case Kind::thumbLdm: return "thumb ldmia";
        case Kind::thumbStm: return "thumb stmia";
        case Kind::ldrhOdd: return "ldrh odd post-indexed";
        case Kind::strhOdd: return "strh odd";
        case Kind::thumbLdrhOdd: return "thumb ldrh odd";
        case Kind::thumbLdrWord: return "thumb ldr misaligned";
        case Kind::ldrhOddNoA: return "ldrh odd, alignment checks off";
    }
    return "?";
}
constexpr uint32_t handlerBase = 0x100, fsrMark = 0xa, farMark = 0x12345678, ifsrMark = 0xc, unmapped = 0x00400000;
// The section translation fault in domain 0 and the alignment fault (DDI0151C table 3-7).
constexpr uint32_t sectionFault = 5, alignmentFault = 1;
// The writeback cases' boundary: VA section 5 is a coarse table whose last 4 KiB page (0x005ff000) is the
// top of SRAM, which takes stores, and section 6 is unmapped.
constexpr uint32_t edge = 0x00600000;
// The word below the edge holds this before the faulting instruction; every handler reads it into r8.
constexpr uint32_t belowMark = 0x66666666;

// faultAddress: FAR after a data abort; base: r2 after it, the value it had before the faulting instruction.
struct FaultGuest { std::vector<uint32_t> words; uint32_t faultPc, faultAddress, base; };

// The graphics guest's setup gives quiet spans for the lean path and the JIT. Then sections: VA 0 the ROM (for
// the vectors), 2 peripherals, 3 SRAM, 5 the edge page (edge) and 0x200 the ROM; 4 and 6 are unmapped. Each handler reads FSR into r9,
// FAR into r10, the prefetch FSR into r11 and the word below the edge into r8 (through r6), puts its number in r12
// (1 undefined, 2 prefetch abort, 3 data abort) and stops on a reserved word.
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
        g.word(at + 8, 0xee15bf30);  // mrc p15, 0, r11, c5, c0, 1
        g.word(at + 12, 0xe5968000);  // ldr r8, [r6]
        g.word(at + 16, 0xe3a0c001 + n);
        g.word(at + 20, 0xffffffff);
    }
    std::vector<uint32_t> words;
    const auto pc = [&] { return 0x20000000 + g.cursor + uint32_t(words.size()) * 4; };
    load(words, 0, 0x00304000);
    store(words, 0, 0x00, 0x20000c1a); store(words, 0, 0x08, 0x00200c12); store(words, 0, 0x0c, 0x00300c12);
    load(words, 0, 0x00304800); store(words, 0, 0, 0x20000c1a);
    load(words, 0, 0x00304000); store(words, 0, 0x14, 0x00310011);  // section 5: coarse table at 0x00310000
    load(words, 0, 0x00310000); store(words, 0, 0x3fc, 0x0031fff2);  // its page 0xff: SRAM 0x0031f000, AP 3
    load(words, 0, 0x00304000);
    words.push_back(0xee020f10);                           // TTB
    load(words, 1, 1); words.push_back(0xee031f10);        // domain 0 client
    // MMU, D- and I-cache on, and alignment fault checking (A) for the alignment cases, as the Zodiac ROM sets it.
    load(words, 1, alignment(kind) && kind != Kind::ldrhOddNoA ? 0x107f : 0x107d); words.push_back(0xee011f10);
    load(words, 1, fsrMark); words.push_back(0xee051f10);  // FSR
    load(words, 1, farMark); words.push_back(0xee061f10);  // FAR
    load(words, 1, ifsrMark); words.push_back(0xee051f30);  // prefetch FSR
    load(words, 4, 0x44444444); load(words, 5, 0x55555555);
    load(words, 0, edge - 0x1000); store(words, 0, 0xffc, belowMark);
    load(words, 6, edge - 4);
    uint32_t faultPc = 0, faultAddress = unmapped, base = unmapped;
    // One ARM load or store with r2 at value, faulting at edge.
    const auto arm = [&](uint32_t value, uint32_t instruction) {
        load(words, 2, value);
        base = value;
        faultAddress = edge;
        faultPc = pc();
        words.push_back(instruction);
    };
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
    case Kind::ldrPre: arm(edge - 4, 0xe5b25004); break;          // ldr r5, [r2, #4]!
    case Kind::ldrPost: arm(edge, 0xe4925004); break;             // ldr r5, [r2], #4
    case Kind::strPre: arm(edge + 4, 0xe5225004); break;          // str r5, [r2, #-4]!
    case Kind::ldmWriteback: arm(edge - 4, 0xe8b20030); break;    // ldmia r2!, {r4, r5}: r4 loads
    case Kind::ldmBase: arm(edge - 4, 0xe8920024); break;         // ldmia r2, {r2, r5}: r2 loads
    case Kind::stmWriteback: arm(edge - 4, 0xe8a20030); break;    // stmia r2!, {r4, r5}
    case Kind::stmdbWriteback: arm(edge + 4, 0xe9220030); break;  // stmdb r2!, {r4, r5}: r4 stores
    case Kind::stmdaWriteback: arm(edge, 0xe8220030); break;      // stmda r2!, {r4, r5}: r4 stores
    case Kind::ldmdbWriteback: arm(edge + 4, 0xe9320030); break;  // ldmdb r2!, {r4, r5}: r4 loads
    case Kind::thumbLdm:
        load(words, 2, base = edge - 4);
        faultAddress = edge;
        thumb(0xca30);  // ldmia r2!, {r4, r5}
        break;
    case Kind::thumbStm:
        load(words, 2, base = edge - 4);
        faultAddress = edge;
        thumb(0xc230);  // stmia r2!, {r4, r5}
        break;
    // Misaligned accesses to SRAM, which is mapped: the fault is the alignment.
    case Kind::ldrhOdd: case Kind::ldrhOddNoA:
        arm(0x0030086f, 0xe0d250b2); faultAddress = 0x0030086f; break;  // ldrh r5, [r2], #2
    case Kind::strhOdd: arm(0x00308001, 0xe1c250b0); faultAddress = 0x00308001; break;  // strh r5, [r2]
    case Kind::thumbLdrhOdd:
        load(words, 2, base = faultAddress = 0x00308003);
        thumb(0x8815);  // ldrh r5, [r2]
        break;
    case Kind::thumbLdrWord:
        load(words, 2, base = faultAddress = 0x00308002);
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
    return {g.words, faultPc, faultAddress, base};
}

struct Outcome { Imx1MachineRun run; Imx1MachineCheapStatus cheap; };
// Steps the observer saw with no instruction fetched, and the last one's trace.
struct Unfetched { unsigned count = 0; Imx1ProbeTrace last{}; };
void observe(void* context, const Imx1ProbeTrace& t) {
    if (t.fetched) return;
    auto& u = *static_cast<Unfetched*>(context);
    ++u.count;
    u.last = t;
}
Outcome run(const std::vector<uint32_t>& words, bool fast, bool guestExceptions, Unfetched* unfetched = nullptr) {
    auto o = graphicsOptions(false);
    o.ramMiB = 32;
    Imx1ProbeResult error{};
    Imx1Machine* m = imx1MachineCreate(reinterpret_cast<const uint8_t*>(words.data()), uint32_t(words.size() * 4),
                                       Imx1ProbeEntry::linkedAlias, false, o, unfetched ? observe : nullptr,
                                       unfetched, &error);
    assert(m);
    assert(!imx1MachineGuestExceptions(m) && imx1MachineSetGuestExceptions(m, guestExceptions));
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
                "r10=%08x r11=%08x r12=%u lr=%08x\n", name, engine, imx1ProbeStopName(r.stop), r.trace.pc, r.address,
                (unsigned long long)o.cheap.retired, r.cpsr, r.spsr, r.registers[2], r.registers[7], r.registers[9],
                r.registers[10], r.registers[11], r.registers[12], r.registers[14]);
    std::fflush(stdout);
}

void check(Kind kind) {
    const auto g = guest(kind);
    const bool thumb = kind == Kind::dataThumb || kind == Kind::undefinedThumb || kind == Kind::thumbLdm ||
                       kind == Kind::thumbStm || kind == Kind::thumbLdrhOdd || kind == Kind::thumbLdrWord;
    const bool undefined = kind == Kind::undefinedArm || kind == Kind::undefinedThumb;
    const bool prefetch = kind == Kind::prefetch;
    const bool data = !undefined && !prefetch;
    const uint32_t before = thumb ? 0xf3 : 0xd3;
    // The off switch: the machine stops at the faulting instruction.
    for (bool fast : {false, true}) {
        const auto o = run(g.words, fast, false);
        print(kindName(kind), fast ? "off fast" : "off exact", o);
        assert(o.run.status == Imx1MachineStatus::terminal && o.run.result.trace.pc == g.faultPc);
        assert(o.run.result.stop == (undefined || alignment(kind) ? Imx1ProbeStop::unsupportedInstruction :
                                     Imx1ProbeStop::exception));
    }
    if (kind == Kind::ldrhOddNoA) {
        // A clear: ARMv4 leaves a misaligned halfword unpredictable, so the machine stops before it with the guest's
        // exceptions on too, with nothing changed.
        for (bool fast : {false, true}) {
            const auto o = run(g.words, fast, true);
            print(kindName(kind), fast ? "fast" : "exact", o);
            assert(o.run.status == Imx1MachineStatus::terminal && o.run.result.trace.pc == g.faultPc &&
                   o.run.result.stop == Imx1ProbeStop::unsupportedInstruction && o.run.result.registers[2] == g.base);
        }
        return;
    }
    Outcome results[3];
    Unfetched unfetched;
    const char* engines[3] = {"exact", "lean", "blocks"};
    for (unsigned e = 0; e < 3; ++e) {
        imx1SetFastBlocksDefault(e == 2);
        results[e] = run(g.words, e != 0, true, e ? nullptr : &unfetched);
        print(kindName(kind), engines[e], results[e]);
    }
    imx1SetFastBlocksDefault(true);
    const auto& r = results[0].run.result;
    const uint32_t handler = handlerBase + (undefined ? 0 : data ? 0x40 : 0x20);
    assert(results[0].run.status == Imx1MachineStatus::terminal && r.stop == Imx1ProbeStop::unsupportedInstruction);
    assert(r.trace.pc == handler + 20 && r.trace.instruction == 0xffffffff && r.trace.fetched);
    // Only the prefetch abort's step has no instruction.
    assert(unfetched.count == (prefetch ? 1u : 0u));
    if (prefetch) assert(unfetched.last.pc == g.faultPc && unfetched.last.instruction == 0);
    assert(r.registers[12] == (undefined ? 1u : data ? 3u : 2u));
    assert((r.cpsr & 0xff) == (0xc0u | (undefined ? 0x1bu : 0x17u)) && (r.spsr & 0xff) == before &&
           r.cpsr >> 28 == r.spsr >> 28);
    assert(r.registers[14] == g.faultPc + (undefined ? (thumb ? 2u : 4u) : data ? 8u : 4u));
    if (data) {
        assert(r.registers[9] == (alignment(kind) ? alignmentFault : sectionFault) &&
               r.registers[10] == g.faultAddress && r.registers[2] == g.base);
        if (kind == Kind::dataLoop) assert(r.registers[7] == 200 - 62);
        // The transfers that reach the word below the edge first take it before the abort.
        const bool storesBelow = kind == Kind::stmWriteback || kind == Kind::stmdbWriteback ||
                                 kind == Kind::stmdaWriteback || kind == Kind::thumbStm;
        const bool loadsBelow = kind == Kind::ldmWriteback || kind == Kind::ldmdbWriteback || kind == Kind::thumbLdm;
        assert(r.registers[8] == (storesBelow ? 0x44444444u : belowMark));
        if (loadsBelow) assert(r.registers[4] == belowMark);
    } else {
        assert(r.registers[9] == fsrMark && r.registers[10] == farMark);
    }
    assert(r.registers[11] == (prefetch ? sectionFault : ifsrMark));
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
    for (auto kind : {Kind::dataLoop, Kind::dataThumb, Kind::prefetch, Kind::undefinedArm, Kind::undefinedThumb,
                      Kind::ldrPre, Kind::ldrPost, Kind::strPre, Kind::ldmWriteback, Kind::ldmBase,
                      Kind::stmWriteback, Kind::stmdbWriteback, Kind::stmdaWriteback, Kind::ldmdbWriteback,
                      Kind::thumbLdm, Kind::thumbStm, Kind::ldrhOdd, Kind::strhOdd, Kind::thumbLdrhOdd,
                      Kind::thumbLdrWord, Kind::ldrhOddNoA})
        check(kind);
    std::puts("PASS: data and prefetch aborts and undefined instructions reach the guest's vectors with the ARM920T's "
              "entry state and base restored, DA and DB transfers going upward, misaligned accesses take alignment "
              "aborts with A set and stop without it, the same in Exact and Fast, and stop the machine with the switch off");
}
