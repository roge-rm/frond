// The Zodiac's Fast engine against Exact on MMU guests. Plain ARM and Thumb loops run on the lean path, a page
// table change only shows after a CP15 TLB flush, a domain change and an SDRAM mode change take the lean path's
// host pages away, a cacheable load of unbacked memory stops both engines at the same pc, and states saved under
// either engine carry on under Fast as an uninterrupted Exact run does. Code rewritten in place drops what Fast
// decoded from it. Timer count reads answered on the lean path give Exact's values and bus count, a cacheable timer
// mapping stops both engines at the same read, and a DMA controller write that starts the PCM DMA takes its buffer
// page off the lean path.
#define ZODIAC_STATE_MAIN stateMain
#include "zodiac-machine-state.cpp"

namespace {
// 200 rounds of: load [r2], add it to reg.
void sumLoop(std::vector<uint32_t>& words, unsigned reg) {
    words.push_back(0xe3a00000 | reg << 12);  // mov reg, #0
    words.push_back(0xe3a070c8);              // mov r7, #200
    const size_t top = words.size();
    words.push_back(0xe5925000);              // ldr r5, [r2]
    words.push_back(0xe0800005 | reg << 16 | reg << 12);
    words.push_back(0xe2577001);              // subs r7, r7, #1
    words.push_back(0x1a000000 | (uint32_t(int32_t(top) - int32_t(words.size()) - 2) & 0xffffff));
}

// The same in Thumb, with a BL pair to a counter in each round: r4 = 200 * [r2], r6 = 200. Ends in ARM.
void thumbLoop(std::vector<uint32_t>& words) {
    words.push_back(0xe28f0001);  // add r0, pc, #1
    words.push_back(0xe12fff10);  // bx r0: Thumb from the next word
    const uint16_t h[] = {
        0x2400,          // movs r4, #0
        0x2600,          // movs r6, #0
        0x23c8,          // movs r3, #200
        0x6815,          // loop: ldr r5, [r2]
        0x1964,          // adds r4, r4, r5
        0xf000, 0xf804,  // bl count
        0x3b01,          // subs r3, #1
        0xd1f9,          // bne loop
        0xa002,          // adr r0, back (the word after the nop)
        0x4700,          // bx r0
        0x3601,          // count: adds r6, #1
        0x4770,          // bx lr
        0x46c0,          // nop
    };
    for (size_t i = 0; i < std::size(h); i += 2) words.push_back(h[i] | uint32_t(h[i + 1]) << 16);
}

enum class Tail { tlb, domain, sdram, code, timer, cachedTimer, dma, dmaOther };

// The graphics guest's setup with programmed clocks gives the step quiet spans, which the lean path runs.
// The test code follows it, running from the linked alias at 0x20000000.
std::vector<uint32_t> guest(Tail tail, uint32_t& stopPc, uint32_t& savePc) {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    std::vector<uint32_t> words;
    const auto pc = [&] { return 0x20000000 + g.cursor + uint32_t(words.size()) * 4; };
    // SDRAM bank 0: precharge, eight refreshes, mode, normal.
    load(words, 0, 0x00221000);
    load(words, 1, 0x91120200); words.push_back(0xe5801000);
    load(words, 2, 0x08200000); words.push_back(0xe5923000);
    load(words, 1, 0xa1120200); words.push_back(0xe5801000);
    load(words, 2, 0x08000000);
    for (unsigned i = 0; i < 8; ++i) words.push_back(0xe5923000);
    load(words, 1, 0xb1120200); words.push_back(0xe5801000);
    load(words, 2, 0x08111800); words.push_back(0xe5923000);
    load(words, 1, 0x81129200); words.push_back(0xe5801000);
    // The same offset in SRAM and SDRAM holds different words.
    load(words, 0, 0x00308100); store(words, 0, 0, 0x11111111);
    load(words, 0, 0x08008100); store(words, 0, 0, 0x22222222);
    // A function in SRAM: mov r9, #1; bx lr.
    load(words, 0, 0x00310000); store(words, 0, 0, 0xe3a09001); store(words, 0, 4, 0xe12fff1e);
    // Sections: 2 peripherals, 3 SRAM cached, 4 SRAM uncached, 5 SRAM cached, 6 unbacked SDRAM cached,
    // 7 SRAM uncached in domain 1 with no access for a client, 8 SDRAM uncached, 0x160 graphics, 0x200 ROM.
    load(words, 0, 0x00304000);
    store(words, 0, 0x08, 0x00200c12); store(words, 0, 0x0c, 0x00300c1e);
    store(words, 0, 0x10, 0x00300c12); store(words, 0, 0x14, 0x00300c1e); store(words, 0, 0x18, 0x0a000c1e);
    store(words, 0, 0x1c, 0x00300032); store(words, 0, 0x20, 0x08000c12);
    load(words, 0, 0x00304580); store(words, 0, 0, 0x16000c12);
    load(words, 0, 0x00304800); store(words, 0, 0, 0x20000c1a);
    load(words, 0, 0x00304000);
    words.push_back(0xee020f10);                          // TTB
    load(words, 1, 0x0d); words.push_back(0xee031f10);     // domain 0 client, 1 manager
    load(words, 1, 0x107d); words.push_back(0xee011f10);   // MMU, D- and I-cache on
    load(words, 2, 0x00508100);
    thumbLoop(words);                                     // r4 = 200 * SRAM word, r6 = 200
    switch (tail) {
    case Tail::tlb:
        sumLoop(words, 8);                                // r8 = 200 * SRAM word
        load(words, 3, 0x00404014);
        load(words, 1, 0x08000c1e); words.push_back(0xe5831000);  // section 5 now maps SDRAM, the TLB keeps the old one
        sumLoop(words, 9);                                // r9 still sums the SRAM word
        words.push_back(0xe3a00000);                      // mov r0, #0: the operand is SBZ
        words.push_back(0xee070f16);                      // invalidate the D-cache, its lines are virtually tagged
        words.push_back(0xee080f17);                      // invalidate the TLBs
        sumLoop(words, 10);                               // r10 = 200 * SDRAM word
        // A dirty line: allocate it with a load, then store. A state saved here has the word only in the D-cache.
        load(words, 0, 0x00308200); load(words, 1, 0x33333333);
        words.push_back(0xe5903000);                      // ldr r3, [r0]
        words.push_back(0xe5801000);                      // str r1, [r0]
        savePc = pc();
        words.push_back(0xe5903000);                      // ldr r3, [r0]: the dirty word, from the cache
        sumLoop(words, 8);                                // r8 = 200 * SDRAM word
        words.push_back(0xee070f3a);                      // clean the line by address
        words.push_back(0xee070f9a);                      // drain the write buffer
        load(words, 2, 0x00408200);
        words.push_back(0xe592c000);                      // ldr r12, [r2]: uncached, sees the cleaned word
        load(words, 11, 0x00600040);
        stopPc = pc();
        words.push_back(0xe59bc000);                      // ldr r12, [r11]: cacheable and unbacked
        break;
    case Tail::domain:
        load(words, 2, 0x00708100);
        sumLoop(words, 8);                                // r8 = 200 * SRAM word: a manager isn't checked
        load(words, 1, 0x05); words.push_back(0xee031f10);  // domain 1 client: no access
        sumLoop(words, 9);
        stopPc = pc() - 16;                               // the first ldr of that loop aborts
        break;
    case Tail::sdram:
        load(words, 0, 0x00221000);
        words.push_back(0xe5903000);                      // ldr r3, [r0]: walks the table now, which drops host pages
        load(words, 2, 0x00808100);
        sumLoop(words, 8);                                // r8 = 200 * SDRAM word
        load(words, 1, 0x91120200); words.push_back(0xe5801000);  // precharge mode: reads are commands
        words.push_back(0xe3a03032);                      // mov r3, #50: a delay, so the loop below starts lean
        words.push_back(0xe2533001);                      // subs r3, r3, #1
        words.push_back(0x1afffffd);                      // bne
        sumLoop(words, 9);
        stopPc = pc() - 16;                               // the first ldr of that loop is out of sequence
        break;
    case Tail::timer:
        // Timer 1 counting from PERCLK1, then 200 reads of TCN and 100 of TCN and TSTAT in one LDM. TCN is
        // answered on the lean path; TSTAT leaves it, and the step runs the LDM again.
        load(words, 0, 0x00202000);
        store(words, 0, 4, 7); store(words, 0, 0, 0x103);
        words.push_back(0xe3a08000);                      // mov r8, #0
        words.push_back(0xe3a070c8);                      // mov r7, #200
        words.push_back(0xe5905010);                      // loop: ldr r5, [r0, #0x10]
        words.push_back(0xe0888005);                      // add r8, r8, r5
        words.push_back(0xe2577001);                      // subs r7, r7, #1
        words.push_back(0x1afffffb);                      // bne loop
        words.push_back(0xe2803010);                      // add r3, r0, #0x10
        words.push_back(0xe3a09000);                      // mov r9, #0
        words.push_back(0xe3a07064);                      // mov r7, #100
        words.push_back(0xe8930006);                      // loop: ldmia r3, {r1, r2}
        words.push_back(0xe0899001);                      // add r9, r9, r1
        words.push_back(0xe2577001);                      // subs r7, r7, #1
        words.push_back(0x1afffffb);                      // bne loop
        stopPc = pc();
        break;
    case Tail::cachedTimer:
        // Section 9 maps the peripherals cacheable, through the uncached view of the table. The step stops on a
        // cacheable timer read; the lean path has to leave it to the step.
        load(words, 3, 0x00404024);
        load(words, 1, 0x00200c1a); words.push_back(0xe5831000);
        load(words, 11, 0x00902010);
        words.push_back(0xe3a03032);                      // mov r3, #50: a delay, so the read below is lean
        words.push_back(0xe2533001);                      // subs r3, r3, #1
        words.push_back(0x1afffffd);                      // bne
        stopPc = pc();
        words.push_back(0xe59bc000);                      // ldr r12, [r11]
        break;
    case Tail::dma:
    case Tail::dmaOther: {
        // 200 stores to an SRAM page put it on the lean path. The PCM DMA then starts, repeating over the
        // first 16 bytes of that page, and 200 more stores follow, to the same page or (dmaOther) the next.
        const uint32_t buffer = 0x00409000;               // SRAM 0x00309000, uncached
        load(words, 2, buffer + 0x100);
        const auto stores = [&] {
            words.push_back(0xe3a070c8);                  // mov r7, #200
            words.push_back(0xe5827000);                  // loop: str r7, [r2]
            words.push_back(0xe2577001);                  // subs r7, r7, #1
            words.push_back(0x1afffffc);                  // bne loop
        };
        stores();
        load(words, 0, 0x00209000);
        store(words, 0, 0, 1); store(words, 0, 8, 0x3ff);
        load(words, 0, 0x00218000);
        store(words, 0, 8, 0x9900); store(words, 0, 0x14, 0x6100); store(words, 0, 0x20, 4); store(words, 0, 0xc, 0xe8);
        for (unsigned i = 0; i < 8; ++i) store(words, 0, 0, 0);
        load(words, 0, buffer);
        for (unsigned i = 0; i < 4; ++i) store(words, 0, i * 4, 0x12340001 + i * 0x10002);
        load(words, 0, 0x00209300);
        store(words, 0, 0, buffer - 0x00100000); load(words, 1, 0x00218000); words.push_back(0xe5801004);
        store(words, 0, 8, 16); store(words, 0, 0xc, 0x208c); store(words, 0, 0x10, 16); store(words, 0, 0x14, 8);
        store(words, 0, 0xc, 0x208d);
        if (tail == Tail::dmaOther) load(words, 2, buffer + 0x1100);
        else load(words, 2, buffer + 0x100);
        stores();
        stopPc = pc();
        break;
    }
    case Tail::code:
        load(words, 0, 0x00310000);
        words.push_back(0xe1a0e00f);                      // mov lr, pc
        words.push_back(0xe12fff10);                      // bx r0: r9 = 1
        words.push_back(0xe1a0a009);                      // mov r10, r9
        sumLoop(words, 8);
        load(words, 1, 0xe3a09002); words.push_back(0xe5801000);  // rewrite it as mov r9, #2, no invalidate
        sumLoop(words, 11);
        words.push_back(0xe1a0e00f);                      // mov lr, pc
        words.push_back(0xe12fff10);                      // bx r0
        stopPc = pc();
        break;
    }
    words.push_back(0xe7f000f0);
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    return g.words;
}

struct Outcome {
    Imx1MachineRun run;
    Imx1MachineCheapStatus cheap;
    uint64_t lean, drops, timerReads;
};

// A bus observer keeps Fast off the lean path, so every access goes through the step and is counted once there.
Imx1Machine* make(const std::vector<uint32_t>& words, bool fast, bool observed = false) {
    auto o = graphicsOptions(false);
    o.ramMiB = 32;
    if (observed) o.busObserver = [](void*, const Imx1ProbeAccess&) {};
    Imx1ProbeResult error{};
    Imx1Machine* m = imx1MachineCreate(reinterpret_cast<const uint8_t*>(words.data()), uint32_t(words.size() * 4),
                                       Imx1ProbeEntry::linkedAlias, false, o, nullptr, nullptr, &error);
    assert(m);
    if (fast) assert(imx1MachineSetFastEngine(m, true) && imx1MachineFastEngine(m));
    return m;
}

bool running(Imx1Machine* m) { return imx1MachineGetStatus(m).status == Imx1MachineStatus::yielded; }

// Small slices, so lean spans end at slice edges as well as at the guest's own exits.
Outcome finish(Imx1Machine* m) {
    for (unsigned n = 0; n < 100000 && running(m); ++n) imx1MachineRunSlice(m, 997, 0);
    Outcome out{imx1MachineInspect(m), imx1MachineGetStatus(m), imx1MachineLeanRetired(m),
                imx1MachineLeanCodeDrops(m), imx1MachineLeanTimerReads(m)};
    imx1MachineStop(m);
    return out;
}

Outcome runGuest(const std::vector<uint32_t>& words, bool fast, bool observed = false) {
    return finish(make(words, fast, observed));
}

// Runs a fresh machine past the save point, so a load has to undo what it holds.
void runAhead(Imx1Machine* m) {
    for (unsigned n = 0; n < 100000 && running(m) && imx1MachineGetStatus(m).retired < 5600; ++n)
        imx1MachineRunSlice(m, 97, 0);
    assert(running(m));
}

// Runs one instruction at a time up to pc, then saves.
std::vector<uint8_t> saveAt(Imx1Machine* m, uint32_t pc) {
    for (unsigned n = 0; n < 1000000 && running(m) && imx1MachineGetStatus(m).pc != pc; ++n)
        imx1MachineRunSlice(m, 1, 0);
    assert(running(m) && imx1MachineGetStatus(m).pc == pc);
    std::vector<uint8_t> state;
    const auto status = imx1MachineSaveState(m, state);
    if (status != Imx1StateStatus::ok) std::fprintf(stderr, "save: %s\n", imx1StateStatusName(status));
    assert(status == Imx1StateStatus::ok);
    return state;
}

void loadInto(Imx1Machine* m, const std::vector<uint8_t>& state) {
    const auto info = imx1MachineLoadState(m, state.data(), state.size());
    if (info.status != Imx1StateStatus::ok) std::fprintf(stderr, "load: %s\n", imx1StateStatusName(info.status));
    assert(info.status == Imx1StateStatus::ok);
}

void print(const char* name, const Outcome& o) {
    const auto& r = o.run.result;
    std::printf("%s: stop=%s pc=%08x address=%08x retired=%llu ticks=%llu bus=%llu lean=%llu drops=%llu timer=%llu r4=%08x "
                "r6=%u r8=%08x r9=%08x r10=%08x r12=%08x\n", name, imx1ProbeStopName(r.stop), r.trace.pc, r.address,
                (unsigned long long)o.cheap.retired, (unsigned long long)o.cheap.time.ticks,
                (unsigned long long)r.busAccesses, (unsigned long long)o.lean, (unsigned long long)o.drops, (unsigned long long)o.timerReads,
                r.registers[4], r.registers[6], r.registers[8], r.registers[9], r.registers[10], r.registers[12]);
    std::fflush(stdout);
}

// Fast ends as Exact does: registers, stop, pc, address, retired and ticks.
void same(const Outcome& exact, const Outcome& fast) {
    const auto& e = exact.run.result;
    const auto& f = fast.run.result;
    assert(!std::memcmp(e.registers, f.registers, sizeof(e.registers)) && e.cpsr == f.cpsr);
    assert(f.stop == e.stop && f.trace.pc == e.trace.pc && f.trace.step == e.trace.step && f.address == e.address);
    assert(exact.cheap.retired == fast.cheap.retired && exact.cheap.status == fast.cheap.status);
    assert(exact.cheap.time.ticks == fast.cheap.time.ticks);
}

constexpr uint32_t sramWord = 0x11111111, sdramWord = 0x22222222;
}  // namespace

int main() {
    uint32_t stopPc = 0, savePc = 0;
    {
        const auto words = guest(Tail::tlb, stopPc, savePc);
        const auto exact = runGuest(words, false), fast = runGuest(words, true);
        print("tlb exact", exact); print("tlb fast", fast);
        const auto& e = exact.run.result;
        // Four ARM loops of 800 and a Thumb loop of 1400 run mostly lean.
        assert(exact.lean == 0 && fast.lean > 3000);
        assert(e.registers[4] == 200u * sramWord && e.registers[6] == 200 && e.registers[9] == 200u * sramWord &&
               e.registers[10] == 200u * sdramWord && e.registers[8] == 200u * sdramWord &&
               e.registers[12] == 0x33333333 && e.registers[3] == 0x33333333);
        assert(e.stop == Imx1ProbeStop::unsupportedDataCache && e.trace.pc == stopPc);
        same(exact, fast);

        // Saved under Fast mid-run, loaded into another Fast machine that ran ahead first.
        Imx1Machine* a = make(words, true);
        const auto state = saveAt(a, savePc);
        imx1MachineStop(a);
        Imx1Machine* b = make(words, true);
        runAhead(b);
        loadInto(b, state);
        const auto fastLoaded = finish(b);
        print("tlb fast saved, fast loaded", fastLoaded);
        same(exact, fastLoaded);
        assert(fastLoaded.run.result.busAccesses == fast.run.result.busAccesses);

        // Saved under Exact with the line dirty, loaded into a Fast machine that ran ahead first.
        Imx1Machine* c = make(words, false);
        const auto exactState = saveAt(c, savePc);
        imx1MachineStop(c);
        Imx1Machine* d = make(words, true);
        runAhead(d);
        loadInto(d, exactState);
        const auto swapped = finish(d);
        print("tlb exact saved, fast loaded", swapped);
        same(exact, swapped);
    }
    {
        // A stale host page would let the loop run on after the domain change.
        const auto words = guest(Tail::domain, stopPc, savePc);
        const auto exact = runGuest(words, false), fast = runGuest(words, true);
        print("domain exact", exact); print("domain fast", fast);
        assert(exact.run.result.registers[8] == 200u * sramWord && fast.lean > 1000);
        assert(exact.run.result.stop == Imx1ProbeStop::exception && exact.run.result.trace.pc == stopPc);
        same(exact, fast);
    }
    {
        // Precharge mode makes SDRAM reads commands: the host page goes with the SDRAM controller write, so the
        // next read goes to the step and stops there as in Exact.
        const auto words = guest(Tail::sdram, stopPc, savePc);
        const auto exact = runGuest(words, false), fast = runGuest(words, true);
        print("sdram exact", exact); print("sdram fast", fast);
        assert(exact.run.result.registers[8] == 200u * sdramWord && fast.lean > 1000);
        assert(exact.run.result.stop != Imx1ProbeStop::budget && exact.run.result.trace.pc == stopPc);
        same(exact, fast);
    }
    {
        // Code rewritten without an I-cache invalidate: Exact's I-cache keeps the old line, Fast follows memory
        // and drops what it decoded.
        const auto words = guest(Tail::code, stopPc, savePc);
        const auto exact = runGuest(words, false), fast = runGuest(words, true);
        print("code exact", exact); print("code fast", fast);
        assert(exact.run.result.registers[10] == 1 && exact.run.result.registers[9] == 1);
        assert(fast.run.result.registers[10] == 1 && fast.run.result.registers[9] == 2 && fast.drops > 0);
        assert(fast.run.result.stop == exact.run.result.stop && fast.cheap.retired == exact.cheap.retired);
    }
    {
        // Timer reads on the lean path give Exact's values. Bus accesses count what retires: the LDM that leaves
        // the lean path at TSTAT is counted by the step alone, as with the lean path off.
        const auto words = guest(Tail::timer, stopPc, savePc);
        const auto exact = runGuest(words, false), fast = runGuest(words, true), observed = runGuest(words, true, true);
        print("timer exact", exact); print("timer fast", fast); print("timer fast observed", observed);
        assert(exact.run.result.stop == Imx1ProbeStop::unsupportedInstruction && exact.run.result.trace.pc == stopPc);
        assert(exact.run.result.registers[8] && exact.run.result.registers[9]);
        same(exact, fast);
        same(exact, observed);
        assert(fast.timerReads >= 250 && observed.lean == 0 && observed.timerReads == 0);
        assert(fast.run.result.busAccesses == observed.run.result.busAccesses);
    }
    {
        const auto words = guest(Tail::cachedTimer, stopPc, savePc);
        const auto exact = runGuest(words, false), fast = runGuest(words, true);
        print("cached timer exact", exact); print("cached timer fast", fast);
        assert(exact.run.result.stop == Imx1ProbeStop::unsupportedDataCache && exact.run.result.trace.pc == stopPc);
        assert(fast.lean > 0);
        same(exact, fast);
    }
    {
        // Stores to the PCM DMA buffer page come at their own time in the step once the DMA is running.
        const auto words = guest(Tail::dma, stopPc, savePc), other = guest(Tail::dmaOther, stopPc, savePc);
        const auto exact = runGuest(words, false), fast = runGuest(words, true);
        const auto otherExact = runGuest(other, false), otherFast = runGuest(other, true);
        print("dma exact", exact); print("dma fast", fast);
        print("dma other page exact", otherExact); print("dma other page fast", otherFast);
        const auto& pcm = exact.run.result.pcm;
        std::printf("dma pcm: active=%d source=%08x remaining=%u words=%llu\n", pcm.dmaActive, pcm.source, pcm.remaining,
                    (unsigned long long)pcm.transmittedWords); std::fflush(stdout);
        assert(exact.run.result.stop == Imx1ProbeStop::unsupportedInstruction && exact.run.result.trace.pc == stopPc);
        same(exact, fast);
        same(otherExact, otherFast);
        assert(otherFast.lean >= fast.lean + 150);
    }
    std::puts("PASS: Fast lean ARM and Thumb loops, CP15 TLB and domain changes, SDRAM mode change, save and load, "
              "rewritten code, unsupported access, timer reads, a cacheable timer and the PCM DMA buffer match Exact");
}
