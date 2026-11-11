// The Zodiac's Fast engine against Exact on MMU guests. Plain ARM and Thumb loops run on the lean path, a page
// table change only shows after a CP15 TLB flush, a domain change and an SDRAM mode change take the lean path's
// host pages away, a cacheable load of unbacked memory stops both engines at the same pc, and states saved under
// either engine carry on under Fast as an uninterrupted Exact run does. Code rewritten in place drops what Fast
// decoded from it. Timer count reads answered on the lean path give Exact's values and bus count, a cacheable timer
// mapping stops both engines at the same read, and a DMA controller write that starts the PCM DMA takes its buffer
// page off the lean path while the DMA sends what both paths wrote. An IRQ raised by an MMIO write and an FIQ that
// comes during a CS5 write (graphics memory or the one starting a fill) are taken where the stepped run takes them.
// Graphics memory stores right after a change that makes the frame scan again show in the frame as in Exact, and a
// timer read after a lean graphics memory store leaves the lean path. Scan status and acquisition reads on the lean
// path give Exact's values at Exact's times, in ARM and in Thumb, and a run that changes engine between slices
// ends as an Exact run does.
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
        // 200 stores to an SRAM page put it on the lean path, and a lean loop fills the first 16 bytes of it.
        // The PCM DMA then starts, repeating over those 16 bytes into SSI1, which sends them out. 2048 rounds of
        // stores of a count to all four words follow, to the buffer or (dmaOther) the next page. The run ends
        // while they are still sent, so in the dma case the last sample shows when the DMA read its word.
        const uint32_t buffer = 0x00409000;               // SRAM 0x00309000, uncached
        load(words, 2, buffer + 0x100);
        const auto stores = [&](bool many) {
            words.push_back(many ? 0xe3a07b02 : 0xe3a070c8);  // mov r7, #2048 or #200
            words.push_back(0xe5827000);                  // loop: str r7, [r2]
            if (many) {
                words.push_back(0xe5827004);              // str r7, [r2, #4]
                words.push_back(0xe5827008);              // str r7, [r2, #8]
                words.push_back(0xe582700c);              // str r7, [r2, #12]
            }
            words.push_back(0xe2577001);                  // subs r7, r7, #1
            words.push_back(many ? 0x1afffff9 : 0x1afffffc);  // bne loop
        };
        stores(false);
        // r3 = 4 * (r7 & 3), [buffer + r3] = r7 * 0x00010001 + 0x01000100 for r7 = 200..1: the words end as
        // 0x01040104, 0x01010101, 0x01020102, 0x01030103.
        load(words, 2, buffer);
        load(words, 4, 0x01000100);
        words.push_back(0xe3a070c8);                      // mov r7, #200
        words.push_back(0xe2073003);                      // loop: and r3, r7, #3
        words.push_back(0xe0875807);                      // add r5, r7, r7, lsl #16
        words.push_back(0xe0855004);                      // add r5, r5, r4
        words.push_back(0xe7825103);                      // str r5, [r2, r3, lsl #2]
        words.push_back(0xe2577001);                      // subs r7, r7, #1
        words.push_back(0x1afffff9);                      // bne loop
        load(words, 0, 0x00209000);
        store(words, 0, 0, 1); store(words, 0, 8, 0x3ff);
        load(words, 0, 0x00218000);
        store(words, 0, 8, 0x9900); store(words, 0, 0x14, 0x6100); store(words, 0, 0x20, 4); store(words, 0, 0xc, 0xe8);
        for (unsigned i = 0; i < 8; ++i) store(words, 0, 0, 0);
        load(words, 0, 0x00209300);
        store(words, 0, 0, buffer - 0x00100000); load(words, 1, 0x00218000); words.push_back(0xe5801004);
        store(words, 0, 8, 16); store(words, 0, 0xc, 0x208c); store(words, 0, 0x10, 16); store(words, 0, 0x14, 8);
        store(words, 0, 0xc, 0x208d);
        // DMA requests on, then transmit.
        load(words, 0, 0x00218000);
        store(words, 0, 0xc, 0x2e8); store(words, 0, 8, 0x9b00);
        if (tail == Tail::dmaOther) load(words, 2, buffer + 0x1100);
        else load(words, 2, buffer);
        stores(true);
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
    uint16_t pcmLast;
    uint64_t cs5Writes, cs5Reads;
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
                imx1MachineLeanCodeDrops(m), imx1MachineLeanTimerReads(m), imx1MachinePcmLastTransmitted(m),
                imx1MachineLeanCs5Writes(m), imx1MachineLeanCs5Reads(m)};
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

// An instruction whose access charged the span waits in the next one, and the step after it still looks at the
// interrupt lines. Checked against the stepped run, where every instruction and wait clock is charged alone.
struct Entries {
    std::vector<Imx1ProbeInterrupt> all;
};
struct Lines {
    Outcome out;
    std::vector<Imx1ProbeInterrupt> entries;
};
Lines runLines(const std::vector<uint32_t>& words, bool fast, bool stepped) {
    Entries e;
    auto o = graphicsOptions(false);
    o.idleStepped = stepped;
    o.interruptObserver = [](void* c, const Imx1ProbeInterrupt& i) { static_cast<Entries*>(c)->all.push_back(i); };
    o.interruptContext = &e;
    Imx1ProbeResult error{};
    Imx1Machine* m = imx1MachineCreate(reinterpret_cast<const uint8_t*>(words.data()), uint32_t(words.size() * 4),
                                       Imx1ProbeEntry::linkedAlias, false, o, nullptr, nullptr, &error);
    assert(m);
    if (fast) assert(imx1MachineSetFastEngine(m, true));
    const auto out = finish(m);
    return {out, e.all};
}
bool sameLines(const Lines& a, const Lines& b) {
    if (a.entries.size() != b.entries.size()) return false;
    for (size_t n = 0; n < a.entries.size(); ++n)
        if (a.entries[n].step != b.entries[n].step || a.entries[n].interruptedPc != b.entries[n].interruptedPc ||
            a.entries[n].fiq != b.entries[n].fiq) return false;
    const auto& x = a.out.run.result;
    const auto& y = b.out.run.result;
    return !std::memcmp(x.registers, y.registers, sizeof(x.registers)) && x.stop == y.stop && x.trace.pc == y.trace.pc &&
           a.out.cheap.retired == b.out.cheap.retired && a.out.cheap.time.ticks == b.out.cheap.time.ticks &&
           x.graphicsFrameHash == y.graphicsFrameHash && x.cs5WaitClocks == y.cs5WaitClocks;
}
void printLines(const char* name, const Lines& l) {
    std::printf("%s: stop=%s pc=%08x address=%08x cs5 writes=%llu retired=%llu ticks=%llu cs5wait=%llu lean=%llu entries=%zu",
                name, imx1ProbeStopName(l.out.run.result.stop), l.out.run.result.trace.pc, l.out.run.result.address,
                (unsigned long long)l.out.cs5Writes, (unsigned long long)l.out.cheap.retired,
                (unsigned long long)l.out.cheap.time.ticks, (unsigned long long)l.out.run.result.cs5WaitClocks,
                (unsigned long long)l.out.lean, l.entries.size());
    for (const auto& i : l.entries) std::printf(" [%llu %08x%s]", (unsigned long long)i.step, i.interruptedPc, i.fiq ? " fiq" : "");
    std::printf("\n");
    std::fflush(stdout);
}

// An AITC force write raises the IRQ: it is taken right after the store, at the bx lr of GraphicsGuest::op (0x200ff004).
void forcedIrq() {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    g.store(0x00223008, 31);
    g.word(g.cursor, 0xe321f013); g.cursor += 4; ++g.retired;  // IRQ and FIQ on
    g.spin(300);
    const auto mark = g.store(0x00223054, 1u << 31);
    g.spin(50);
    g.end();
    const auto exact = runLines(g.words, false, false), fast = runLines(g.words, true, false);
    const auto stepped = runLines(g.words, false, true);
    printLines("forced irq exact", exact); printLines("forced irq fast", fast); printLines("forced irq stepped", stepped);
    assert(stepped.entries.size() == 1 && stepped.entries[0].step == mark && stepped.entries[0].interruptedPc == 0x200ff004);
    // Nothing is mapped at the vector, so the run stops there.
    assert(stepped.out.run.result.trace.pc == 0x18);
    assert(sameLines(exact, stepped) && sameLines(fast, stepped));
}

// Timer 2 raises an FIQ after comp counts while the guest fills the engine's ring and submits the fill at
// 0xa9ed4 (graphics memory and register writes, each with CS5 wait clocks). Nothing is mapped at the vector,
// so the run stops there. Returns 1 when the FIQ came right after the submission, 2 right after a ring store.
uint64_t fastCs5Writes = 0;
unsigned commandFiq(uint32_t comp) {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    g.store(0x00223018, 1u << 26); g.store(0x00223008, 58);
    g.word(g.cursor, 0xe321f013); g.cursor += 4; ++g.retired;
    g.store(0x00203008, comp); g.store(0x00203000, 0x13);
    const auto submitted = g.fill();
    g.busy();
    g.spin(500);
    g.end();
    const auto exact = runLines(g.words, false, false), fast = runLines(g.words, true, false);
    const auto stepped = runLines(g.words, false, true);
    fastCs5Writes += fast.out.cs5Writes;
    assert(!exact.out.cs5Writes && !stepped.out.cs5Writes);
    const bool ok = sameLines(exact, stepped) && sameLines(fast, stepped) && stepped.entries.size() == 1 &&
                    stepped.out.run.result.trace.pc == 0x1c;
    const bool atSubmit = stepped.entries.size() == 1 && stepped.entries[0].step == submitted &&
                          stepped.entries[0].interruptedPc == 0x200a9ed8;
    if (!ok || (atSubmit && comp % 16 == 0)) {
        std::printf("comp %u%s\n", comp, atSubmit ? " (after the submission)" : "");
        printLines("  exact", exact); printLines("  fast", fast); printLines("  stepped", stepped);
    }
    assert(ok);
    return atSubmit ? 1 : stepped.entries[0].interruptedPc == 0x200ff004 && stepped.entries[0].step > submitted - 300 ? 2 : 0;
}
// A guest that changes what the scanout reads, then stores a pixel to each display row, which the lean path takes,
// and stops before the next frame. eim: the CS5 chip select goes off and back on, so the next frame advance scans
// the whole frame; transaction: a display transaction latches the surface again. delay moves the change against
// the display's frames.
enum class Trigger { eim, transaction };
std::vector<uint32_t> drawGuest(Trigger trigger, uint32_t delay) {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    g.spin(1000 + delay);
    if (trigger == Trigger::eim) {
        g.store(0x0022002c, 0x11111e00);
        g.store(0x0022002c, 0x11111e01);
    } else {
        g.graphics(0xaeb68, 0x4d8, 0x78);
        g.graphics(0xad720, 0x418, 0x800000);
        g.graphics(0xaeb68, 0x4d8, 0x7b);
    }
    std::vector<uint32_t> words;
    load(words, 2, 0x16800000); load(words, 3, 0x001f07e0);
    words.push_back(0xe5823000);                          // str r3, [r2]
    words.push_back(0xe5823008);                          // str r3, [r2, #8]
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.end();
    return g.words;
}
// What the frontend sees where the run stops: the frame hash, the pixels and the rows of the pending frame.
struct Shown {
    uint32_t hash = 0, first = 0, last = 0;
    bool present = false;
    std::vector<uint32_t> pixels;
    bool operator==(const Shown& o) const {
        return hash == o.hash && first == o.first && last == o.last && present == o.present && pixels == o.pixels;
    }
};
Shown drawn(const std::vector<uint32_t>& words, bool fast, Outcome& out) {
    Imx1Machine* m = make(words, fast);
    for (unsigned n = 0; n < 100000 && running(m); ++n) imx1MachineRunSlice(m, 997, 0);
    Shown s;
    s.hash = imx1MachineInspect(m).result.graphicsFrameHash;
    uint32_t width = 0, height = 0;
    if (const auto* pixels = imx1MachinePeekFrame(m, width, height)) s.pixels.assign(pixels, pixels + width * height);
    s.present = imx1MachineAcquireFrame(m, width, height, s.first, s.last) != nullptr;
    if (s.present) imx1MachineAcknowledgeFrame(m);
    out = finish(m);
    return s;
}
// Fast shows the frame Exact shows at each stop: with the stores before the frame advance Exact runs first, the
// frame would hold them early.
void leanDraw(Trigger trigger, const char* name) {
    uint64_t lean = 0;
    unsigned differ = 0, old = 0;
    for (uint32_t delay = 0; delay < 240; ++delay) {
        const auto words = drawGuest(trigger, delay);
        Outcome exact, fast;
        const auto e = drawn(words, false, exact), f = drawn(words, true, fast);
        assert(exact.run.result.stop == Imx1ProbeStop::unsupportedInstruction && !e.pixels.empty());
        same(exact, fast);
        assert(exact.run.result.cs5WaitClocks == fast.run.result.cs5WaitClocks);
        lean += fast.cs5Writes;
        old += (e.pixels[0] & 0xffffff) == 0x0000ff;
        if (e == f) continue;
        if (!differ++)
            std::printf("%s, delay %u: exact hash %08x rows %u-%u%s, fast hash %08x rows %u-%u%s\n", name, delay, e.hash,
                        e.first, e.last, e.present ? " pending" : "", f.hash, f.first, f.last, f.present ? " pending" : "");
    }
    std::printf("%s: %llu graphics memory writes on the lean path, %u of 240 stops still show the old pixel, %u differ\n",
                name, (unsigned long long)lean, old, differ);
    std::fflush(stdout);
    assert(lean >= 10 && !differ);
}

// Graphics memory stores and timer count reads in one loop: a timer read after a store whose wait clocks are
// still deferred leaves the lean path, and the step charges them first.
void storeThenTimer() {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    std::vector<uint32_t> words;
    load(words, 0, 0x00202000);
    store(words, 0, 4, 0); store(words, 0, 0, 0x103);   // timer 1 from PERCLK1, no prescaler
    load(words, 2, 0x16800100);
    words.push_back(0xe3a08000);                          // mov r8, #0
    words.push_back(0xe3a070c8);                          // mov r7, #200
    words.push_back(0xe5827000);                          // loop: str r7, [r2]
    words.push_back(0xe5905010);                          // ldr r5, [r0, #0x10]
    words.push_back(0xe0888005);                          // add r8, r8, r5
    words.push_back(0xe2577001);                          // subs r7, r7, #1
    words.push_back(0x1afffffa);                          // bne loop
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.end();
    const auto exact = runGuest(g.words, false), fast = runGuest(g.words, true);
    print("store then timer exact", exact); print("store then timer fast", fast);
    std::printf("store then timer: fast cs5 writes on the lean path %llu, timer reads on the lean path %llu\n",
                (unsigned long long)fast.cs5Writes, (unsigned long long)fast.timerReads);
    std::fflush(stdout);
    assert(exact.run.result.stop == Imx1ProbeStop::unsupportedInstruction && exact.run.result.registers[8]);
    assert(fast.cs5Writes >= 100);
    same(exact, fast);
    assert(exact.run.result.cs5WaitClocks == fast.run.result.cs5WaitClocks);
}
// The ROM's vertical wait reads the scan status (54c at 200adb14) in a loop, here with the acquisition
// configuration (d10) and, with store, a graphics memory store each round. On the lean path the reads give
// Exact's values at Exact's times and add their wait clocks. delay moves the loop against the display's frames.
std::vector<uint32_t> scanGuest(uint32_t delay, bool store) {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    g.spin(1 + delay);
    g.word(0xadb14, 0xe5906000);                          // ldr r6, [r0]
    g.word(0xadb18, 0xe12fff1e);                          // bx lr
    std::vector<uint32_t> words;
    load(words, 0, 0x1601054c); load(words, 1, 0x16010d10); load(words, 2, 0x16800100);
    words.push_back(0xe3a08000);                          // mov r8, #0
    words.push_back(0xe3a09000);                          // mov r9, #0
    words.push_back(0xe3a07c01);                          // mov r7, #256
    const size_t top = words.size();
    words.push_back(0xe1a0e00f);                          // loop: mov lr, pc
    const size_t call = words.size();
    words.push_back(0);                                   // b 200adb14
    words.push_back(0xe0268fe8);                          // eor r8, r6, r8, ror #31
    words.push_back(0xe5915000);                          // ldr r5, [r1]
    words.push_back(0xe0899005);                          // add r9, r9, r5
    if (store) words.push_back(0xe5827000);               // str r7, [r2]
    words.push_back(0xe2577001);                          // subs r7, r7, #1
    words.push_back(0x1a000000 | ((uint32_t(top) - uint32_t(words.size()) - 2) & 0xffffff));  // bne loop
    const uint32_t base = g.cursor;
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.branch(base + uint32_t(call) * 4, 0xadb14);
    g.end();
    return g.words;
}
void scanReads() {
    uint64_t reads = 0, writes = 0;
    unsigned mixed = 0;
    for (bool store : {false, true})
        for (uint32_t delay = 0; delay < 64; delay += 3) {
            const auto words = scanGuest(delay, store);
            const auto exact = runGuest(words, false), fast = runGuest(words, true);
            if (delay == 0) { print(store ? "scan reads, stores exact" : "scan reads exact", exact);
                              print(store ? "scan reads, stores fast" : "scan reads fast", fast); }
            assert(exact.run.result.stop == Imx1ProbeStop::unsupportedInstruction && exact.run.result.registers[7] == 0);
            same(exact, fast);
            assert(exact.run.result.cs5WaitClocks == fast.run.result.cs5WaitClocks &&
                   exact.run.result.graphicsFrameHash == fast.run.result.graphicsFrameHash);
            reads += fast.cs5Reads;
            writes += fast.cs5Writes;
            mixed += exact.run.result.registers[8] != 0 && exact.run.result.registers[8] != ~0u;
        }
    std::printf("scan reads: %llu CS5 register reads and %llu graphics memory writes on the lean path, %u of 44 runs "
                "saw the status change\n", (unsigned long long)reads, (unsigned long long)writes, mixed);
    std::fflush(stdout);
    assert(reads >= 5000 && writes >= 1000 && mixed >= 30);
}
// The same in Thumb, the scan status read by LDR(1) at 200adb14, LDR(2) at 200adb08 and LDR(4) at 200ad7b0 (sp
// points at 54c), with STR(1) and STR(2) to graphics memory in half the runs. The lean path takes them as it takes
// the ARM ones: in Thumb the loop sees the translated ARM word.
std::vector<uint32_t> thumbScanGuest(uint32_t delay, bool store) {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    g.spin(1 + delay);
    g.word(0xadb14, 0x6804 | 0x4770u << 16);              // ldr r4, [r0, #0]; bx lr
    g.word(0xadb08, 0x5844 | 0x4770u << 16);              // ldr r4, [r0, r1]; bx lr
    g.word(0xad7b0, 0x9c00 | 0x4770u << 16);              // ldr r4, [sp, #0]; bx lr
    std::vector<uint32_t> words;
    load(words, 0, 0x1601054c); load(words, 1, 0); load(words, 2, 0x16800100);
    words.push_back(0xe1a0b00d);                          // mov r11, sp
    load(words, 13, 0x1601054c);
    words.push_back(0xe3a06000);                          // mov r6, #0
    words.push_back(0xe3a03c01);                          // mov r3, #256
    words.push_back(0xe28fc001);                          // add r12, pc, #1
    words.push_back(0xe12fff1c);                          // bx r12: Thumb from the next word
    const uint32_t base = 0x20000000 + g.cursor;
    const uint32_t thumb = base + uint32_t(words.size()) * 4;
    std::vector<uint16_t> h;
    const auto here = [&] { return thumb + uint32_t(h.size()) * 2; };
    const auto bl = [&](uint32_t target) {
        const uint32_t off = target - (here() + 4);
        h.push_back(uint16_t(0xf000 | ((off >> 12) & 0x7ff)));
        h.push_back(uint16_t(0xf800 | ((off >> 1) & 0x7ff)));
    };
    const uint32_t top = here();
    for (uint32_t stub : {0x200adb14u, 0x200adb08u, 0x200ad7b0u}) {
        bl(stub);
        h.push_back(0x0076);                              // lsls r6, r6, #1
        h.push_back(0x4066);                              // eors r6, r4
    }
    if (store) {
        h.push_back(0x6013);                              // str r3, [r2, #0]
        h.push_back(0x5054);                              // str r4, [r2, r1]
    }
    h.push_back(0x3b01);                                  // subs r3, #1
    h.push_back(uint16_t(0xd100 | (((top - (here() + 4)) >> 1) & 0xff)));  // bne loop
    // adr r5, back; bx r5; back is the next word boundary after both.
    const uint32_t back = (here() + 4 + 3) & ~3u;
    h.push_back(uint16_t(0xa500 | ((back - ((here() + 4) & ~3u)) >> 2)));
    h.push_back(0x4728);
    while (here() < back) h.push_back(0x46c0);           // nop
    for (size_t i = 0; i < h.size(); i += 2) words.push_back(h[i] | uint32_t(h[i + 1]) << 16);
    words.push_back(0xe1a0d00b);                          // mov sp, r11
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.end();
    return g.words;
}
void thumbScanReads() {
    uint64_t reads = 0, writes = 0;
    unsigned mixed = 0, runs = 0;
    for (bool store : {false, true})
        for (uint32_t delay = 0; delay < 64; delay += 3, ++runs) {
            const auto words = thumbScanGuest(delay, store);
            const auto exact = runGuest(words, false), fast = runGuest(words, true);
            if (delay == 0) { print(store ? "thumb scan reads, stores exact" : "thumb scan reads exact", exact);
                              print(store ? "thumb scan reads, stores fast" : "thumb scan reads fast", fast); }
            assert(exact.run.result.stop == Imx1ProbeStop::unsupportedInstruction && exact.run.result.registers[3] == 0);
            assert(!(exact.run.result.cpsr & 0x20) && exact.run.result.registers[13] != 0x1601054c);
            same(exact, fast);
            assert(exact.run.result.cs5WaitClocks == fast.run.result.cs5WaitClocks &&
                   exact.run.result.graphicsFrameHash == fast.run.result.graphicsFrameHash);
            reads += fast.cs5Reads;
            writes += fast.cs5Writes;
            mixed += exact.run.result.registers[6] != 0;
        }
    std::printf("thumb scan reads: %llu CS5 register reads and %llu graphics memory writes on the lean path, %u of %u "
                "runs saw the status set\n", (unsigned long long)reads, (unsigned long long)writes, mixed, runs);
    std::fflush(stdout);
    assert(reads >= 10000 && writes >= 2000 && mixed >= 10);
}
// The engine changes between slices, every two slices, starting on either engine: in the scan read guests with
// graphics memory stores and in the MMU guests with the caches on (dirty lines left by Exact must reach memory
// before Fast reads it). Each run ends as an Exact run does, and the Fast slices still run on the lean path.
void engineSwitch() {
    uint64_t lean = 0, dirtyLean = 0;
    unsigned switches = 0;
    std::vector<std::vector<uint32_t>> guests;
    for (bool thumb : {false, true})
        for (uint32_t delay : {0u, 7u, 31u}) guests.push_back(thumb ? thumbScanGuest(delay, true) : scanGuest(delay, true));
    uint32_t stopPc = 0, savePc = 0;
    for (Tail tail : {Tail::tlb, Tail::domain, Tail::timer}) guests.push_back(guest(tail, stopPc, savePc));
    for (const auto& words : guests) {
        const auto exact = runGuest(words, false);
        for (bool first : {true, false}) {
            Imx1Machine* m = make(words, first);
            for (unsigned n = 0; n < 100000 && running(m); ++n) {
                if (n % 2 == 0 && n) {
                    const bool fast = (n / 2 % 2 == 0) == first;
                    assert(imx1MachineSetFastEngine(m, fast) && imx1MachineFastEngine(m) == fast);
                    ++switches;
                }
                imx1MachineRunSlice(m, 311 + n % 3 * 97, 0);
            }
            const auto mixed = finish(m);
            same(exact, mixed);
            assert(exact.run.result.cs5WaitClocks == mixed.run.result.cs5WaitClocks &&
                   exact.run.result.graphicsFrameHash == mixed.run.result.graphicsFrameHash);
            lean += mixed.lean;
        }
    }
    {
        // Exact to Fast where the tlb guest's line is dirty in Exact's D-cache: Fast reads it from memory.
        const auto words = guest(Tail::tlb, stopPc, savePc);
        const auto exact = runGuest(words, false);
        Imx1Machine* m = make(words, false);
        for (unsigned n = 0; n < 1000000 && running(m) && imx1MachineGetStatus(m).pc != savePc; ++n)
            imx1MachineRunSlice(m, 1, 0);
        assert(running(m) && imx1MachineSetFastEngine(m, true));
        const auto dirty = finish(m);
        print("engine switch, exact to fast with a dirty line", dirty);
        same(exact, dirty);
        dirtyLean = dirty.lean;
    }
    std::printf("engine switch: %u switches between slices, %llu instructions on the lean path, %llu after the dirty "
                "line\n", switches, (unsigned long long)lean, (unsigned long long)dirtyLean);
    std::fflush(stdout);
    assert(switches >= 100 && lean >= 5000 && dirtyLean > 0);
}
}  // namespace

int main() {
    uint32_t stopPc = 0, savePc = 0;
    forcedIrq();
    leanDraw(Trigger::eim, "draw after eim");
    leanDraw(Trigger::transaction, "draw after transaction");
    storeThenTimer();
    scanReads();
    thumbScanReads();
    engineSwitch();
    {
        // 1-400 land in the ring stores before the fill, 3300-3460 around its submission and after it, while
        // the engine runs.
        unsigned atSubmit = 0, atStore = 0;
        for (uint32_t comp = 1; comp <= 3460; comp = comp == 400 ? 3300 : comp + 1) {
            const auto at = commandFiq(comp);
            atSubmit += at == 1;
            atStore += at == 2;
        }
        std::printf("command fiq: %u taken right after the submission, %u right after a ring store, %llu graphics memory "
                    "writes on the lean path\n", atSubmit, atStore, (unsigned long long)fastCs5Writes);
        assert(atSubmit >= 32 && atStore >= 200 && fastCs5Writes > 0);
    }
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
        // Exact's D-cache fills the line the Thumb loop reads once, eight words, where Fast reads each of the
        // 200 loads from memory.
        assert(fast.run.result.busAccesses == exact.run.result.busAccesses + 200 - 8);
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
        for (const auto* o : {&exact, &fast, &otherExact, &otherFast}) {
            const auto& pcm = o->run.result.pcm;
            std::printf("dma pcm: active=%d source=%08x remaining=%u words=%llu last=%04x\n", pcm.dmaActive, pcm.source,
                        pcm.remaining, (unsigned long long)pcm.transmittedWords, o->pcmLast);
        }
        std::fflush(stdout);
        assert(exact.run.result.stop == Imx1ProbeStop::unsupportedInstruction && exact.run.result.trace.pc == stopPc);
        same(exact, fast);
        same(otherExact, otherFast);
        assert(otherFast.lean >= fast.lean + 150);
        // The DMA sent the samples the lean loop wrote (dmaOther) and the counts the step stored while it ran
        // (dma), the same ones at the same times in both engines.
        const auto sent = [](const Outcome& o) { return o.run.result.pcm.transmittedWords; };
        assert(sent(exact) >= 16 && sent(fast) == sent(exact) && fast.pcmLast == exact.pcmLast);
        assert(exact.pcmLast >= 1 && exact.pcmLast <= 2048);
        assert(sent(otherExact) >= 16 && sent(otherFast) == sent(otherExact) && otherFast.pcmLast == otherExact.pcmLast);
        assert(otherExact.pcmLast >= 0x0101 && otherExact.pcmLast <= 0x0104);
    }
    std::puts("PASS: Fast lean ARM and Thumb loops, CP15 TLB and domain changes, SDRAM mode change, save and load, "
              "rewritten code, unsupported access, timer reads, a cacheable timer and the PCM DMA buffer match Exact");
}
