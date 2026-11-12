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
// ends as an Exact run does. Graphics memory loads of every width, the identity read and GPIO, AITC and ASP status
// reads on the lean path give Exact's values. Fast blocks answer those reads and board writes in generated code,
// take an IRQ such a write lets in within a block, answer CGM, timer and DMA register reads and watchdog and DISR
// writes there, pass the ROM's timer delay loop at once and run exception returns and line-end Thumb BL pairs.
#define ZODIAC_STATE_MAIN stateMain
#include "zodiac-machine-state.cpp"

#include <string>

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

enum class Tail { tlb, domain, sdram, code, timer, cachedTimer, dma, dmaOther, ttbr, user, hotCode, freshCode };

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
    case Tail::ttbr: {
        // A second table at 0x0030c000 (written through the uncached view), a copy of the first with section 5
        // mapping SDRAM. After the TTBR write and a TLB flush the loop sees SDRAM.
        load(words, 2, 0x00508100);
        sumLoop(words, 8);                                // r8 = 200 * SRAM word, from a host page
        load(words, 0, 0x00404000); load(words, 1, 0x0040c000);
        words.push_back(0xe3a07f81);                      // mov r7, #0x204: words 0 to 0x200 (the ROM section)
        words.push_back(0xe4903004);                      // loop: ldr r3, [r0], #4
        words.push_back(0xe4813004);                      // str r3, [r1], #4
        words.push_back(0xe2577001);                      // subs r7, r7, #1
        words.push_back(0x1afffffb);                      // bne loop
        load(words, 3, 0x0040c014);
        load(words, 1, 0x08000c1e); words.push_back(0xe5831000);
        load(words, 0, 0x0030c000);
        words.push_back(0xee020f10);                      // TTB
        words.push_back(0xe3a00000);                      // mov r0, #0
        words.push_back(0xee070f16);                      // invalidate the D-cache
        words.push_back(0xee080f17);                      // invalidate the TLBs
        sumLoop(words, 9);                                // r9 = 200 * SDRAM word
        load(words, 11, 0x00600040);
        stopPc = pc();
        words.push_back(0xe59bc000);                      // ldr r12, [r11]: cacheable and unbacked
        break;
    }
    case Tail::user:
        // Section 10: SRAM, privileged access only. Read in SVC from a host page, then in user mode, where the
        // first load aborts.
        load(words, 3, 0x00404028);
        load(words, 1, 0x00300412); words.push_back(0xe5831000);
        load(words, 2, 0x00a08100);
        sumLoop(words, 8);                                // r8 = 200 * SRAM word
        words.push_back(0xe321f0d0);                      // msr cpsr_c, #0xd0: user, IRQ and FIQ masked
        sumLoop(words, 9);
        stopPc = pc() - 16;
        break;
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
    case Tail::hotCode:
        // As code, but the function's page is stored to and the function called often enough to be translated
        // before it is rewritten.
        load(words, 0, 0x00310000);
        load(words, 3, 64);
        words.push_back(0xe5800100);                      // str r0, [r0, #0x100]
        words.push_back(0xe2533001);                      // subs r3, r3, #1
        words.push_back(0x1afffffc);                      // bne back to the str
        load(words, 3, 8);
        words.push_back(0xe1a0e00f);                      // mov lr, pc
        words.push_back(0xe12fff10);                      // bx r0: r9 = 1
        words.push_back(0xe2533001);                      // subs r3, r3, #1
        words.push_back(0x1afffffb);                      // bne back to the call
        words.push_back(0xe1a0a009);                      // mov r10, r9
        sumLoop(words, 8);
        load(words, 1, 0xe3a09002); words.push_back(0xe5801000);  // rewrite it as mov r9, #2, no invalidate
        sumLoop(words, 11);
        words.push_back(0xe1a0e00f);                      // mov lr, pc
        words.push_back(0xe12fff10);                      // bx r0
        stopPc = pc();
        break;
    case Tail::freshCode:
        // 12 rounds, each on a new uncached SRAM page: store mov r9, #1; bx lr, call it, store mov r9, #2 over
        // it and call it again. r10 = 12, r11 = 24 in both engines.
        load(words, 0, 0x00411000);
        load(words, 1, 0xe3a09001); load(words, 4, 0xe3a09002); load(words, 5, 0xe12fff1e);
        words.push_back(0xe3a0a000);                      // mov r10, #0
        words.push_back(0xe3a0b000);                      // mov r11, #0
        load(words, 3, 12);
        for (uint32_t w : {0xe5801000u,                   // loop: str r1, [r0]
                           0xe5805004u,                   // str r5, [r0, #4]
                           0xe1a0e00fu, 0xe12fff10u,      // mov lr, pc; bx r0: r9 = 1
                           0xe08aa009u,                   // add r10, r10, r9
                           0xe5804000u,                   // str r4, [r0]
                           0xe1a0e00fu, 0xe12fff10u,      // mov lr, pc; bx r0: r9 = 2
                           0xe08bb009u,                   // add r11, r11, r9
                           0xe2800a01u,                   // add r0, r0, #0x1000
                           0xe2533001u,                   // subs r3, r3, #1
                           0x1afffff3u})                  // bne loop
            words.push_back(w);
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
    uint64_t jit;
    uint64_t memoryReads, boardReads;
    uint64_t memory;
};

// A bus observer keeps Fast off the lean path, so every access goes through the step and is counted once there.
Imx1Machine* make(const std::vector<uint32_t>& words, bool fast, bool observed = false, bool icacheIndex = false) {
    auto o = graphicsOptions(false);
    o.ramMiB = 32;
    o.arm920IcacheIndexCompatibility = icacheIndex;
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
                imx1MachineLeanCs5Writes(m), imx1MachineLeanCs5Reads(m), imx1MachineJitRetired(m),
                imx1MachineLeanCs5MemoryReads(m), imx1MachineLeanBoardReads(m), imx1MachineMemoryHash(m)};
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

// Fast with Dynarmic blocks ends with Exact's results: registers, stop, pc and address, at other times.
void sameResult(const Outcome& exact, const Outcome& fast) {
    const auto& e = exact.run.result;
    const auto& f = fast.run.result;
    assert(!std::memcmp(e.registers, f.registers, sizeof(e.registers)) && e.cpsr == f.cpsr);
    assert(f.stop == e.stop && f.trace.pc == e.trace.pc && f.address == e.address);
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
    // The timer reads come after a store's deferred wait clocks and are answered with them counted.
    assert(fast.timerReads >= 100);
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
// A loop of graphics memory loads of every width (LDR, LDRB, LDRH, LDRSB, LDRSH, in ARM and in Thumb), the
// identity read of the register read helper at 200871dc, and GPIO, AITC and ASP status reads, with graphics memory
// stores between them. On the lean path they give Exact's values, and the graphics memory reads add their wait
// clocks. delay moves the loop against the display's frames.
std::vector<uint32_t> readsGuest(uint32_t delay) {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    g.spin(1 + delay);
    g.word(0x871dc, 0xe5906000);                          // ldr r6, [r0]
    g.word(0x871e0, 0xe12fff1e);                          // bx lr
    std::vector<uint32_t> words;
    load(words, 0, 0x16010000); load(words, 1, 0x0021c000); load(words, 2, 0x16800100);
    load(words, 3, 0x00223000); load(words, 4, 0x0021501c);
    words.push_back(0xe3a08000);                          // mov r8, #0
    words.push_back(0xe3a09000);                          // mov r9, #0
    words.push_back(0xe3a0a000);                          // mov r10, #0
    words.push_back(0xe3a0b000);                          // mov r11, #0
    words.push_back(0xe3a070c8);                          // mov r7, #200
    const size_t top = words.size();
    words.push_back(0xe1e05007);                          // loop: mvn r5, r7
    words.push_back(0xe5825000);                          // str r5, [r2]
    words.push_back(0xe5827004);                          // str r7, [r2, #4]
    for (uint32_t insn : {0xe1d250b0u, 0xe1d250d1u, 0xe1d250f2u, 0xe5d25003u, 0xe5925004u}) {
        words.push_back(insn);                            // ldrh [r2], ldrsb [r2, #1], ldrsh [r2, #2], ldrb [r2, #3], ldr [r2, #4]
        words.push_back(0xe0888005);                      // add r8, r8, r5
    }
    words.push_back(0xe1a0e00f);                          // mov lr, pc
    const size_t call = words.size();
    words.push_back(0);                                   // b 200871dc
    words.push_back(0xe0269fe9);                          // eor r9, r6, r9, ror #31
    for (uint32_t insn : {0xe5915030u, 0xe5935014u}) {    // ldr r5, [r1, #0x30] (IMR_A), ldr r5, [r3, #0x14] (INTENABLEL)
        words.push_back(insn);
        words.push_back(0xe08aa005);                      // add r10, r10, r5
    }
    words.push_back(0xe5945000);                          // ldr r5, [r4] (ASP ISTATR)
    words.push_back(0xe08bb005);                          // add r11, r11, r5
    words.push_back(0xe2577001);                          // subs r7, r7, #1
    words.push_back(0x1a000000 | ((uint32_t(top) - uint32_t(words.size()) - 2) & 0xffffff));  // bne loop
    words.push_back(0xe28fc001);                          // add r12, pc, #1
    words.push_back(0xe12fff1c);                          // bx r12: Thumb from the next word
    const uint32_t base = 0x20000000 + g.cursor;
    const uint32_t thumb = base + uint32_t(words.size()) * 4;
    std::vector<uint16_t> h = {0x2000, 0x2602, 0x2364};   // movs r0, #0; movs r6, #2; movs r3, #100
    const auto here = [&] { return thumb + uint32_t(h.size()) * 2; };
    const uint32_t loop = here();
    h.push_back(0x8013);                                  // loop: strh r3, [r2, #0]
    // ldrh r5, [r2, #2]; ldrb r5, [r2, #1]; ldrsh r5, [r2, r6]; ldrsb r5, [r2, r6]; ldrb r5, [r2, r6]; ldr r5, [r2, #4]
    for (uint16_t insn : {0x8855, 0x7855, 0x5f95, 0x5795, 0x5d95, 0x6855}) {
        h.push_back(insn);
        h.push_back(0x1940);                              // adds r0, r0, r5
    }
    h.push_back(0x3b01);                                  // subs r3, #1
    h.push_back(uint16_t(0xd100 | (((loop - (here() + 4)) >> 1) & 0xff)));  // bne loop
    const uint32_t back = (here() + 4 + 3) & ~3u;
    h.push_back(uint16_t(0xa500 | ((back - ((here() + 4) & ~3u)) >> 2)));  // adr r5, back
    h.push_back(0x4728);                                  // bx r5
    while (here() < back) h.push_back(0x46c0);            // nop
    for (size_t i = 0; i < h.size(); i += 2) words.push_back(h[i] | uint32_t(h[i + 1]) << 16);
    words.push_back(0xe1a0c000);                          // mov r12, r0
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.branch(base - 0x20000000 + uint32_t(call) * 4, 0x871dc);
    g.end();
    return g.words;
}
void leanReads() {
    uint64_t memory = 0, identity = 0, board = 0;
    for (uint32_t delay = 0; delay < 64; delay += 3) {
        const auto words = readsGuest(delay);
        const auto exact = runGuest(words, false), fast = runGuest(words, true);
        if (delay == 0) { print("lean reads exact", exact); print("lean reads fast", fast); }
        const auto& e = exact.run.result;
        assert(e.stop == Imx1ProbeStop::unsupportedInstruction && e.registers[7] == 0 && e.registers[3] == 0);
        assert(e.registers[8] && e.registers[12] && !(e.cpsr & 0x20));
        same(exact, fast);
        assert(e.cs5WaitClocks == fast.run.result.cs5WaitClocks && e.graphicsFrameHash == fast.run.result.graphicsFrameHash);
        memory += fast.memoryReads;
        identity += fast.cs5Reads;
        board += fast.boardReads;
    }
    std::printf("lean reads: %llu graphics memory reads, %llu CS5 register reads and %llu board reads on the lean path\n",
                (unsigned long long)memory, (unsigned long long)identity, (unsigned long long)board);
    std::fflush(stdout);
    assert(memory >= 20000 && identity >= 2000 && board >= 4000);
}
// A loop of RTC reads (RTCISR, HOURMIN and SECONDS) with the sampling status SAM7 enabled, set every 64 CLK32 ticks
// and cleared by the guest whenever it reads it set. On the lean path the reads give Exact's values, since the span
// ends at each sampling period and second.
std::vector<uint32_t> rtcGuest(uint32_t delay) {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    g.spin(1 + delay);
    std::vector<uint32_t> words;
    load(words, 1, 0x00204000); load(words, 5, 0x8000);
    words.push_back(0xe5815018);                          // str r5, [r1, #0x18] (RTCIENR)
    words.push_back(0xe3a08000);                          // mov r8, #0
    words.push_back(0xe3a09000);                          // mov r9, #0
    words.push_back(0xe3a0a000);                          // mov r10, #0
    words.push_back(0xe3a0b000);                          // mov r11, #0
    load(words, 7, 120000);
    const size_t top = words.size();
    words.push_back(0xe5915014);                          // loop: ldr r5, [r1, #0x14] (RTCISR)
    words.push_back(0xe0888005);                          // add r8, r8, r5
    words.push_back(0xe3150902);                          // tst r5, #0x8000
    words.push_back(0x15815014);                          // strne r5, [r1, #0x14]
    words.push_back(0x128aa001);                          // addne r10, r10, #1
    words.push_back(0x108bb007);                          // addne r11, r11, r7: the iteration it was seen in
    for (uint32_t insn : {0xe5915000u, 0xe5915004u}) {    // ldr r5, [r1] (HOURMIN), ldr r5, [r1, #4] (SECONDS)
        words.push_back(insn);
        words.push_back(0xe0899005);                      // add r9, r9, r5
    }
    words.push_back(0xe2577001);                          // subs r7, r7, #1
    words.push_back(0x1a000000 | ((uint32_t(top) - uint32_t(words.size()) - 2) & 0xffffff));  // bne loop
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.end();
    return g.words;
}
Outcome rtcRun(const std::vector<uint32_t>& words, bool fast) {
    auto o = graphicsOptions(false);
    o.ramMiB = 32;
    o.rtcInitialTimeKnown = true;
    o.rtcInitialTime = {1, 12, 34, 56};
    Imx1ProbeResult error{};
    Imx1Machine* m = imx1MachineCreate(reinterpret_cast<const uint8_t*>(words.data()), uint32_t(words.size() * 4),
                                       Imx1ProbeEntry::linkedAlias, false, o, nullptr, nullptr, &error);
    assert(m);
    if (fast) assert(imx1MachineSetFastEngine(m, true) && imx1MachineFastEngine(m));
    return finish(m);
}
void rtcReads() {
    uint64_t board = 0, cleared = 0;
    for (uint32_t delay : {0u, 5u, 17u}) {
        const auto words = rtcGuest(delay);
        const auto exact = rtcRun(words, false), fast = rtcRun(words, true);
        if (delay == 0) { print("rtc reads exact", exact); print("rtc reads fast", fast); }
        const auto& e = exact.run.result;
        assert(e.stop == Imx1ProbeStop::unsupportedInstruction && e.registers[7] == 0 && e.registers[9] && e.registers[11]);
        same(exact, fast);
        board += fast.boardReads;
        cleared += e.registers[10];
    }
    std::printf("rtc reads: %llu board reads on the lean path, SAM7 seen set %llu times\n", (unsigned long long)board,
                (unsigned long long)cleared);
    std::fflush(stdout);
    assert(board >= 600000 && cleared >= 12);
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

// Fast with blocks leaves memory as lean Fast does (neither keeps cache contents, which Exact's memory would lack)
// and shows Exact's frame.
void sameMemory(const Outcome& exact, const Outcome& lean, const Outcome& fast) {
    assert(fast.memory == lean.memory);
    assert(fast.run.result.graphicsFrameHash == exact.run.result.graphicsFrameHash);
}
Outcome runLean(const std::vector<uint32_t>& words) {
    imx1SetFastBlocksDefault(false);
    const auto lean = runGuest(words, true);
    imx1SetFastBlocksDefault(true);
    return lean;
}

// The MMU guests again with Dynarmic blocks in Fast: direct host pages across a TLB flush, a domain change, an
// SDRAM mode change, a TTBR change and a switch to user mode, code rewritten in place, new code pages stored to,
// run and rewritten within one run of the JIT, and stores to the PCM DMA buffer page, which never get a direct
// page. Then some of them with the compile thread, compiling ahead and the hot count as shipped.
void blocks() {
    imx1SetFastBlocksDefault(true);
    // Every block compiled at once, in place, so runs repeat and the loops run in the JIT.
    cpuSetJitSync(true);
    setenv("FROND_JIT_HOT", "1", 1);
    uint32_t stopPc = 0, savePc = 0;
    const auto both = [&](Tail tail, const char* name) {
        const auto words = guest(tail, stopPc, savePc);
        const auto exact = runGuest(words, false), fast = runGuest(words, true), lean = runLean(words);
        std::string n = name;
        print((n + " exact").c_str(), exact); print((n + " blocks").c_str(), fast);
        std::printf("%s blocks: %llu instructions in the JIT, memory %016llx, lean %016llx\n", name,
                    (unsigned long long)fast.jit, (unsigned long long)fast.memory, (unsigned long long)lean.memory);
        assert(fast.jit > 400);
        sameMemory(exact, lean, fast);
        return std::pair{exact, fast};
    };
    {
        const auto [exact, fast] = both(Tail::tlb, "tlb");
        assert(exact.run.result.registers[9] == 200u * sramWord && exact.run.result.registers[10] == 200u * sdramWord);
        sameResult(exact, fast);
    }
    {
        const auto [exact, fast] = both(Tail::ttbr, "ttbr");
        const auto& e = exact.run.result;
        assert(e.registers[8] == 200u * sramWord && e.registers[9] == 200u * sdramWord);
        assert(e.stop == Imx1ProbeStop::unsupportedDataCache && e.trace.pc == stopPc);
        sameResult(exact, fast);
    }
    {
        const auto [exact, fast] = both(Tail::domain, "domain");
        assert(exact.run.result.stop == Imx1ProbeStop::exception && exact.run.result.trace.pc == stopPc);
        sameResult(exact, fast);
    }
    {
        const auto [exact, fast] = both(Tail::user, "user");
        assert(exact.run.result.registers[8] == 200u * sramWord);
        assert(exact.run.result.stop == Imx1ProbeStop::exception && exact.run.result.trace.pc == stopPc);
        sameResult(exact, fast);
    }
    {
        const auto [exact, fast] = both(Tail::sdram, "sdram");
        assert(exact.run.result.stop != Imx1ProbeStop::budget && exact.run.result.trace.pc == stopPc);
        sameResult(exact, fast);
    }
    {
        // Rewritten without an I-cache invalidate: the JIT drops what it translated from the page.
        const auto [exact, fast] = both(Tail::code, "code");
        assert(fast.run.result.registers[10] == 1 && fast.run.result.registers[9] == 2);
        assert(fast.run.result.stop == exact.run.result.stop && fast.run.result.trace.pc == exact.run.result.trace.pc);
    }
    {
        // The same once the function is translated and its page was writable in the JIT.
        const auto [exact, fast] = both(Tail::hotCode, "hot code");
        assert(fast.run.result.registers[10] == 1 && fast.run.result.registers[9] == 2);
        assert(fast.run.result.stop == exact.run.result.stop && fast.run.result.trace.pc == exact.run.result.trace.pc);
    }
    {
        // Compiled in place mid-run, so the new page's code is translated with no lean step in between: its
        // marking has to take away the JIT's writable page and the host TLB's write entry for the rewrite.
        setenv("FROND_JIT_THREAD", "0", 1);
        const auto [exact, fast] = both(Tail::freshCode, "fresh code");
        unsetenv("FROND_JIT_THREAD");
        assert(exact.run.result.registers[10] == 12 && exact.run.result.registers[11] == 24);
        sameResult(exact, fast);
    }
    {
        const auto [exact, fast] = both(Tail::timer, "timer");
        assert(fast.run.result.stop == exact.run.result.stop && fast.run.result.trace.pc == stopPc);
    }
    {
        // The 8192 stores to the buffer page while the DMA runs go to the step, where each one is a bus access;
        // the same stores to the next page run on a direct page and aren't counted.
        const auto [exact, fast] = both(Tail::dma, "dma");
        const auto [otherExact, otherFast] = both(Tail::dmaOther, "dma other page");
        assert(exact.run.result.stop == Imx1ProbeStop::unsupportedInstruction && exact.run.result.trace.pc == stopPc);
        sameResult(exact, fast);
        sameResult(otherExact, otherFast);
        std::printf("dma blocks: bus accesses %llu, other page %llu\n", (unsigned long long)fast.run.result.busAccesses,
                    (unsigned long long)otherFast.run.result.busAccesses);
        assert(fast.run.result.busAccesses >= 8192 && otherFast.run.result.busAccesses < 4096);
        assert(fast.run.result.pcm.transmittedWords >= 16);
    }
    // As shipped: the compile thread, compiling ahead, hot 4. Compiles land at other times on each run.
    cpuSetJitSync(false);
    unsetenv("FROND_JIT_HOT");
    uint64_t shipped = 0;
    for (Tail tail : {Tail::tlb, Tail::domain, Tail::user, Tail::code, Tail::hotCode, Tail::freshCode, Tail::dmaOther}) {
        const auto words = guest(tail, stopPc, savePc);
        const auto exact = runGuest(words, false), fast = runGuest(words, true), lean = runLean(words);
        assert(fast.run.result.stop == exact.run.result.stop && fast.run.result.trace.pc == exact.run.result.trace.pc);
        if (tail != Tail::code && tail != Tail::hotCode) sameResult(exact, fast);
        else assert(fast.run.result.registers[10] == 1 && fast.run.result.registers[9] == 2);
        sameMemory(exact, lean, fast);
        shipped += fast.jit;
    }
    std::printf("shipped setup: %llu instructions in the JIT\n", (unsigned long long)shipped);
    assert(shipped > 1000);
    std::fflush(stdout);
}

// Long JIT runs (milestone 10), with every block compiled at once in place. Graphics memory, identity, GPIO, AITC and
// ASP reads are answered in generated code (the identity read by its access pc), with Exact's results.
void jitReads() {
    const uint64_t before = imx1MachineJitReads(nullptr), registers = imx1MachineJitRegisterReads(nullptr);
    const auto words = readsGuest(0);
    const auto exact = runGuest(words, false), fast = runGuest(words, true);
    print("jit reads exact", exact); print("jit reads blocks", fast);
    const uint64_t reads = imx1MachineJitReads(nullptr) - before;
    const uint64_t identity = imx1MachineJitRegisterReads(nullptr) - registers;
    std::printf("jit reads: %llu instructions in generated code, %llu reads answered there, %llu of them identity reads\n",
                (unsigned long long)fast.jit, (unsigned long long)reads, (unsigned long long)identity);
    std::fflush(stdout);
    sameResult(exact, fast);
    assert(fast.run.result.graphicsFrameHash == exact.run.result.graphicsFrameHash);
    assert(fast.jit > 2000 && reads >= 1500 && identity >= 100);
}

// 3000 graphics memory reads in a loop, the display off so the quiet span is long. A JIT run ends at the slice
// (997 instructions), which counts instructions only, so the reads near its end are answered in generated code as
// long as the span holds their wait clocks.
void jitSliceEndReads() {
    GraphicsGuest g;
    g.setup(); g.clocks();
    g.store(0x16800100, 0x1234);
    std::vector<uint32_t> words;
    load(words, 2, 0x16800100); load(words, 7, 3000); load(words, 8, 0);
    const size_t top = words.size();
    words.push_back(0xe5925000);                          // loop: ldr r5, [r2]
    words.push_back(0xe0888005);                          // add r8, r8, r5
    words.push_back(0xe2577001);                          // subs r7, r7, #1
    words.push_back(0x1a000000 | ((uint32_t(top) - uint32_t(words.size()) - 2) & 0xffffff));  // bne loop
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.end();
    const auto exact = runGuest(g.words, false);
    const uint64_t before = imx1MachineJitReads(nullptr);
    const auto fast = runGuest(g.words, true);
    const uint64_t reads = imx1MachineJitReads(nullptr) - before;
    print("jit slice end reads exact", exact); print("jit slice end reads blocks", fast);
    std::printf("jit slice end reads: %llu of 3000 answered in generated code, sum %08x\n", (unsigned long long)reads,
                fast.run.result.registers[8]);
    std::fflush(stdout);
    assert(exact.run.result.registers[7] == 0 && exact.run.result.registers[8] == 3000u * 0x1234);
    sameResult(exact, fast);
    // 2805 here; the rest are in the slices' last instructions, which go to the lean path and the step. Against the
    // instruction budget, 2168.
    assert(reads >= 2600);
}

// A run of GPIO IMR writes in generated code, then an AITC INTENNUM write that lets a forced IRQ in. Exact takes it
// right after the store; Fast blocks take the write in generated code and the IRQ at the end of its block, here
// within 64 instructions.
void jitBoardWrites() {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    g.store(0x0022300c, 31);                              // INTDISNUM 31
    g.store(0x00223054, 1u << 31);                        // force it
    g.word(g.cursor, 0xe321f013); g.cursor += 4; ++g.retired;  // IRQ and FIQ on
    std::vector<uint32_t> words;
    load(words, 0, 0x00223000); load(words, 1, 0x0021c000); load(words, 2, 31); load(words, 3, 400);
    load(words, 4, 0);
    words.push_back(0xe5814030);                          // loop: str r4, [r1, #0x30] (IMR_A)
    words.push_back(0xe2533001);                          // subs r3, r3, #1
    words.push_back(0x1afffffc);                          // bne loop
    const uint32_t base = 0x20000000 + g.cursor;
    words.push_back(0xe5802008);                          // str r2, [r0, #8]: INTENNUM 31
    const uint32_t after = base + uint32_t(words.size()) * 4;
    for (unsigned i = 0; i < 300; ++i) words.push_back(0xe2855001);  // add r5, r5, #1
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.end();
    const uint64_t before = imx1MachineJitBoardWrites(nullptr);
    const auto exact = runLines(g.words, false, false), fast = runLines(g.words, true, false);
    printLines("jit board writes exact", exact); printLines("jit board writes blocks", fast);
    const uint64_t writes = imx1MachineJitBoardWrites(nullptr) - before;
    std::printf("jit board writes: %llu in generated code, IRQ taken %u instructions late\n", (unsigned long long)writes,
                fast.entries.empty() ? 0u : (fast.entries[0].interruptedPc - after) / 4);
    std::fflush(stdout);
    assert(exact.entries.size() == 1 && exact.entries[0].interruptedPc == after);
    assert(exact.out.run.result.trace.pc == 0x18 && fast.out.run.result.trace.pc == 0x18);
    assert(fast.entries.size() == 1 && fast.entries[0].interruptedPc >= after &&
           fast.entries[0].interruptedPc - after <= 4 * 64);
    assert(writes >= 300);
}

// A graphics memory store and 62 other instructions a round while timer 1's compare IRQ comes due, with the display
// off so the quiet spans are long. Each store's wait clocks count against the JIT run's budget with the cycles the
// run has already done, so the IRQ comes at Exact's round, not up to a whole run late.
Lines storeTimerLines(bool fast) {
    GraphicsGuest g;
    g.setup(); g.clocks();
    g.store(0x00223008, 59);                              // INTENNUM 59: timer 1
    g.store(0x00202004, 255);                             // TPRER1
    g.store(0x00202008, 3000);                            // TCMP1
    g.store(0x00202000, 0x13);                            // TCTL1: on, PERCLK1, IRQ
    g.word(g.cursor, 0xe321f013); g.cursor += 4; ++g.retired;  // IRQ and FIQ on
    std::vector<uint32_t> words;
    load(words, 2, 0x16800100); load(words, 7, 1000000);
    const size_t top = words.size();
    words.push_back(0xe5827000);                          // loop: str r7, [r2]
    for (unsigned i = 0; i < 60; ++i) words.push_back(0xe2855001);  // add r5, r5, #1
    words.push_back(0xe2577001);                          // subs r7, r7, #1
    words.push_back(0x1a000000 | ((uint32_t(top) - uint32_t(words.size()) - 2) & 0xffffff));  // bne loop
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.end();
    return runLines(g.words, fast, false);
}
void jitStoreTimer() {
    const auto exact = storeTimerLines(false), fast = storeTimerLines(true);
    // Counted from the start of the last machine, the Fast one.
    std::printf("jit store timer: %llu JIT runs\n", (unsigned long long)imx1MachineJitRuns(nullptr));
    printLines("jit store timer exact", exact); printLines("jit store timer blocks", fast);
    assert(exact.entries.size() == 1 && fast.entries.size() == 1);
    const uint32_t e = 1000000 - exact.out.run.result.registers[7], f = 1000000 - fast.out.run.result.registers[7];
    std::printf("jit store timer: IRQ after %u stores against %u, ticks %llu against %llu, %llu stores on the lean path\n",
                f, e, (unsigned long long)fast.out.cheap.time.ticks, (unsigned long long)exact.out.cheap.time.ticks,
                (unsigned long long)fast.out.cs5Writes);
    std::fflush(stdout);
    assert(exact.out.run.result.trace.pc == 0x18 && fast.out.run.result.trace.pc == 0x18);
    assert(fast.out.cs5Writes * 2 >= f);
    // A round is 63 instructions; Fast takes the IRQ at a block end, so within a round of Exact's.
    assert(f <= e + 1 && f + 1 >= e);
}

// CGM CSCR, both timers' control and status and DMA DISR and DIMR, read 150 times, with watchdog service and DISR
// writes in the same loop: Fast blocks answer all of them in generated code, with Exact's results.
void jitBoardRegisters() {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    g.store(0x00202004, 255);                             // TPRER1
    g.store(0x00202000, 0x03);                            // TCTL1: on, PERCLK1
    std::vector<uint32_t> words;
    load(words, 0, 0x0021b000); load(words, 1, 0x00202000); load(words, 2, 0x00203000); load(words, 3, 0x00209000);
    load(words, 4, 0); load(words, 6, 0x00201000); load(words, 7, 150); load(words, 9, 0x5555); load(words, 10, 0);
    const size_t top = words.size();
    for (uint32_t w : {0xe5905000u, 0xe5915000u, 0xe5915014u, 0xe5925000u, 0xe5925014u, 0xe5935004u, 0xe5935008u}) {
        words.push_back(w);                               // ldr r5, [...]
        words.push_back(0xe0844005);                      // add r4, r4, r5
    }
    words.push_back(0xe5869004);                          // str r9, [r6, #4]: WSR
    words.push_back(0xe583a004);                          // str r10, [r3, #4]: DISR
    words.push_back(0xe2577001);                          // subs r7, r7, #1
    words.push_back(0x1a000000 | ((uint32_t(top) - uint32_t(words.size()) - 2) & 0xffffff));  // bne top
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.end();
    const uint64_t reads = imx1MachineJitReads(nullptr), writes = imx1MachineJitBoardWrites(nullptr);
    const auto exact = runGuest(g.words, false), fast = runGuest(g.words, true);
    print("jit board registers exact", exact); print("jit board registers blocks", fast);
    const uint64_t r = imx1MachineJitReads(nullptr) - reads, w = imx1MachineJitBoardWrites(nullptr) - writes;
    std::printf("jit board registers: %llu reads and %llu writes in generated code\n", (unsigned long long)r,
                (unsigned long long)w);
    std::fflush(stdout);
    assert(exact.run.result.registers[7] == 0 && exact.run.result.registers[4] != 0);
    sameResult(exact, fast);
    assert(r >= 7 * 130 && w >= 2 * 130);
}

// The draw wait's ring polls (read pointer at 200b8c64, write pointer at 200b8c70, busy at 200b8cb0) 200 times while
// a fill runs: Fast blocks answer them in generated code with Exact's values.
void jitRingPolls() {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    g.fill();
    g.word(0xb8c64, 0xe590521c); g.word(0xb8c68, 0xe12fff1e);  // ldr r5, [r0, #0x21c]; bx lr
    g.word(0xb8c70, 0xe5906220); g.word(0xb8c74, 0xe12fff1e);  // ldr r6, [r0, #0x220]; bx lr
    g.word(0xb8cb0, 0xe5904140); g.word(0xb8cb4, 0xe12fff1e);  // ldr r4, [r0, #0x140]; bx lr
    std::vector<uint32_t> words;
    load(words, 0, 0x16010000); load(words, 7, 200); load(words, 8, 0); load(words, 9, 0); load(words, 10, 0);
    const size_t top = words.size();
    std::vector<std::pair<size_t, uint32_t>> calls;
    for (auto [stub, add] : {std::pair{0xb8c64u, 0xe0888005u}, {0xb8c70u, 0xe0899006u}, {0xb8cb0u, 0xe08aafa4u}}) {
        words.push_back(0xe1a0e00f);                      // mov lr, pc
        calls.push_back({words.size(), stub});
        words.push_back(0);                               // b stub
        words.push_back(add);                             // add r8, r8, r5 / add r9, r9, r6 / add r10, r10, r4, lsr #31
    }
    words.push_back(0xe2577001);                          // subs r7, r7, #1
    words.push_back(0x1a000000 | ((uint32_t(top) - uint32_t(words.size()) - 2) & 0xffffff));  // bne top
    const uint32_t base = g.cursor;
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    for (auto [at, stub] : calls) g.branch(base + uint32_t(at) * 4, stub);
    g.end();
    const uint64_t before = imx1MachineJitRegisterReads(nullptr);
    const auto exact = runGuest(g.words, false), fast = runGuest(g.words, true);
    print("jit ring polls exact", exact); print("jit ring polls blocks", fast);
    const uint64_t r = imx1MachineJitRegisterReads(nullptr) - before;
    std::printf("jit ring polls: %llu register reads in generated code, read pointer sum %08x, busy %u\n",
                (unsigned long long)r, exact.run.result.registers[8], exact.run.result.registers[10]);
    std::fflush(stdout);
    assert(exact.run.result.registers[7] == 0 && exact.run.result.registers[9] != 0);
    sameResult(exact, fast);
    assert(r >= 300);
}

// The ROM's timer delay loop (2009f678) waiting 120 counts of timer 1 at prescaler 256, 20 times. Fast blocks skip
// the rest of each quiet span the wait covers, so fewer instructions run, and end the loop as Exact does in about
// Exact's guest time. With stores, each wait comes right after graphics memory stores, whose wait clocks are still
// deferred when the skip looks at the timer.
void delayLoop(bool stores = false) {
    const char* name = stores ? "delay after stores" : "delay";
    GraphicsGuest g;
    // With stores the display stays off, so the quiet span holds the stores and the wait after them.
    g.setup(); if (!stores) g.display(); g.clocks();
    g.store(0x00202004, 255);                             // TPRER1
    g.store(0x00202000, 0x03);                            // TCTL1: on, PERCLK1
    std::vector<uint32_t> words;
    load(words, 6, 0x00202000); load(words, 7, 20); load(words, 8, 0); load(words, 9, 0); load(words, 2, 0x16800100);
    const size_t outer = words.size();
    // outer: 600 adds, so the stores land in a JIT run, not the slice's tail, then str r7, [r2] 4 times
    if (stores) for (unsigned i = 0; i < 604; ++i) words.push_back(i < 600 ? 0xe2855001 : 0xe5827000);
    words.push_back(0xe5960010);                          // ldr r0, [r6, #16]
    words.push_back(0xe5961010);                          // loop: ldr r1, [r6, #16]
    words.push_back(0xe0411000);                          // sub r1, r1, r0
    words.push_back(0xe3510078);                          // cmp r1, #120
    words.push_back(0x3afffffb);                          // bcc loop
    words.push_back(0xe1510008);                          // cmp r1, r8
    words.push_back(0x81a08001);                          // movhi r8, r1: the most counts any wait saw
    words.push_back(0xe0899001);                          // add r9, r9, r1: all the counts the waits saw
    words.push_back(0xe2577001);                          // subs r7, r7, #1
    words.push_back(0x1a000000 | ((uint32_t(outer) - uint32_t(words.size()) - 2) & 0xffffff));  // bne outer
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.end();
    const uint64_t skips = imx1MachineDelaySkips(nullptr);
    const auto exact = runGuest(g.words, false), fast = runGuest(g.words, true);
    print((std::string(name) + " exact").c_str(), exact); print((std::string(name) + " blocks").c_str(), fast);
    const uint64_t n = imx1MachineDelaySkips(nullptr) - skips;
    std::printf("%s: %llu graphics stores answered, %llu waits skipped, retired %llu against %llu, ticks %llu against "
                "%llu\n", name, (unsigned long long)fast.cs5Writes, (unsigned long long)n,
                (unsigned long long)fast.cheap.retired, (unsigned long long)exact.cheap.retired,
                (unsigned long long)fast.cheap.time.ticks, (unsigned long long)exact.cheap.time.ticks);
    std::fflush(stdout);
    const auto& e = exact.run.result;
    assert(e.stop == Imx1ProbeStop::unsupportedInstruction && e.registers[7] == 0 && e.registers[1] == 120);
    // The timer counts each wait starts from differ with the time Fast keeps, so only the loop's end is compared.
    const auto& f = fast.run.result;
    std::printf("%s blocks r0=%08x r1=%u, most %u against %u, sum %u against %u\n", name, f.registers[0], f.registers[1],
                f.registers[8], e.registers[8], f.registers[9], e.registers[9]);
    std::fflush(stdout);
    // Each wait ends at the count Exact's does, at least one skip per wait, and in Exact's time to 2% and a tick.
    assert(e.registers[8] == 120 && e.registers[9] == 20 * 120);
    assert(f.stop == e.stop && f.trace.pc == e.trace.pc && f.registers[7] == 0 && f.registers[8] == e.registers[8] &&
           f.registers[9] == e.registers[9]);
    assert(n >= 20 && fast.cheap.retired * 5 < exact.cheap.retired * 4);
    // After stores the skip comes at the end of the slice (997) the wait starts in, not one slice later: about one
    // slice a wait.
    assert(!stores || fast.cheap.retired < 30 * 997);
    const uint64_t et = exact.cheap.time.ticks, ft = fast.cheap.time.ticks;
    assert(ft * 50 + 50 >= et * 49 && ft * 50 <= et * 51 + 50);
}

// The ROM's vertical wait (200adb08 to 200adb20) called 12 times, timer 1 read after each return. 200adb08 polls
// the scan status until the display is in its rows, 200adb14 until it has left them. Fast blocks skip the polls
// that would read the same value and charge their time, so Fast with the skip ends as Fast without it and as Exact
// do: same registers, timer counts, retired count, guest time and wait clocks.
void scanPolls() {
    GraphicsGuest g;
    // A bigger frame, 256 by 200, with the status rows 20 to 150, so each wait polls for a while.
    g.setup(); g.display(0xc80100, 0x960014); g.clocks();
    g.store(0x00202004, 15);                              // TPRER1
    g.store(0x00202000, 0x03);                            // TCTL1: on, PERCLK1
    const uint32_t loop[] = {0xe590154c, 0xe3110e40, 0x0afffffc, 0xe590154c, 0xe3110e40, 0x1afffffc,
                             0xe3a00000, 0xe12fff1e};
    for (unsigned i = 0; i < 8; ++i) g.word(0xadb08 + 4 * i, loop[i]);
    std::vector<uint32_t> words;
    load(words, 6, 0x00202000); load(words, 7, 12);
    words.push_back(0xe3a09000);                          // mov r9, #0
    const size_t top = words.size();
    load(words, 0, 0x16010000);                           // outer: r0 = graphics registers
    words.push_back(0xe1a0e00f);                          // mov lr, pc
    const size_t call = words.size();
    words.push_back(0);                                   // b 200adb08
    words.push_back(0xe5961010);                          // ldr r1, [r6, #16]
    words.push_back(0xe0899001);                          // add r9, r9, r1: the counts each return saw
    words.push_back(0xe2577001);                          // subs r7, r7, #1
    words.push_back(0x1a000000 | ((uint32_t(top) - uint32_t(words.size()) - 2) & 0xffffff));  // bne outer
    const uint32_t base = g.cursor;
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.branch(base + uint32_t(call) * 4, 0xadb08);
    g.end();
    const uint64_t skips = imx1MachineScanPollSkips(nullptr), rounds = imx1MachineScanPollRounds(nullptr);
    const auto exact = runGuest(g.words, false), fast = runGuest(g.words, true);
    const uint64_t n = imx1MachineScanPollSkips(nullptr) - skips, r = imx1MachineScanPollRounds(nullptr) - rounds;
    imx1SetScanPollSkip(false);
    const auto plain = runGuest(g.words, true);
    imx1SetScanPollSkip(true);
    assert(imx1MachineScanPollSkips(nullptr) - skips == n);
    print("scan polls exact", exact); print("scan polls fast", fast); print("scan polls no skip", plain);
    const auto& e = exact.run.result;
    const auto& f = fast.run.result;
    const auto& p = plain.run.result;
    std::printf("scan polls: %llu skips of %llu rounds, timer sum %u fast, %u no skip, %u exact, last %u %u %u\n",
                (unsigned long long)n, (unsigned long long)r, f.registers[9], p.registers[9], e.registers[9],
                f.registers[1], p.registers[1], e.registers[1]);
    std::fflush(stdout);
    assert(e.stop == Imx1ProbeStop::unsupportedInstruction && e.registers[7] == 0);
    same(plain, fast);
    same(exact, fast);
    assert(f.cs5WaitClocks == p.cs5WaitClocks && f.cs5WaitClocks == e.cs5WaitClocks);
    assert(n >= 1000 && r >= 10000);
}

// The ROM's vertical wait, given at 0xadb08.
void waitLoop(GraphicsGuest& g) {
    const uint32_t loop[] = {0xe590154c, 0xe3110e40, 0x0afffffc, 0xe590154c, 0xe3110e40, 0x1afffffc,
                             0xe3a00000, 0xe12fff1e};
    for (unsigned i = 0; i < 8; ++i) g.word(0xadb08 + 4 * i, loop[i]);
}

// The vertical wait called 12 times with IRQs on and timer 2 raising one after comp counts. Nothing is mapped at
// the vector, so the run stops there. Fast with the skip takes the IRQ where Fast without it and Exact do, also when
// it comes inside a wait. Returns whether it did.
bool scanPollIrq(uint32_t comp, uint64_t& skips) {
    GraphicsGuest g;
    g.setup(); g.display(0xc80100, 0x960014); g.clocks();
    waitLoop(g);
    g.store(0x00223008, 58);                              // AITC: timer 2 on
    g.word(g.cursor, 0xe321f013); g.cursor += 4; ++g.retired;  // IRQ and FIQ on
    g.store(0x00203008, comp); g.store(0x00203000, 0x13);  // timer 2: PERCLK1, compare IRQ
    std::vector<uint32_t> words;
    load(words, 7, 12);
    const size_t top = words.size();
    load(words, 0, 0x16010000);                           // outer: r0 = graphics registers
    words.push_back(0xe1a0e00f);                          // mov lr, pc
    const size_t call = words.size();
    words.push_back(0);                                   // b 200adb08
    words.push_back(0xe2577001);                          // subs r7, r7, #1
    words.push_back(0x1a000000 | ((uint32_t(top) - uint32_t(words.size()) - 2) & 0xffffff));  // bne outer
    const uint32_t base = g.cursor;
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.branch(base + uint32_t(call) * 4, 0xadb08);
    g.end();
    const uint64_t before = imx1MachineScanPollSkips(nullptr);
    const auto exact = runLines(g.words, false, false), fast = runLines(g.words, true, false);
    skips = imx1MachineScanPollSkips(nullptr) - before;
    imx1SetScanPollSkip(false);
    const auto plain = runLines(g.words, true, false);
    imx1SetScanPollSkip(true);
    const bool ok = exact.entries.size() == 1 && exact.out.run.result.trace.pc == 0x18 && sameLines(exact, fast) &&
                    sameLines(plain, fast);
    const uint32_t at = exact.entries.empty() ? 0 : exact.entries[0].interruptedPc;
    const bool inWait = at >= 0x200adb08 && at <= 0x200adb18;
    if (!ok) {
        std::printf("scan poll irq comp %u: %llu skips%s\n", comp, (unsigned long long)skips, inWait ? ", in the wait" : "");
        printLines("  exact", exact); printLines("  fast", fast); printLines("  no skip", plain);
    }
    assert(ok);
    return inWait;
}
// The vertical wait called once with IRQs off while timer 2 raises one, then entered again at 0xadb04, where msr
// lets the IRQ in. The second wait starts with r1 = 0 and Z set, as the skip wants, outside the status rows, so the
// skip could pass rounds there, but the IRQ is taken at 200adb08 as in Exact. spin moves the second entry.
bool scanPollUnmask(uint32_t spin, uint64_t& skips) {
    GraphicsGuest g;
    g.setup(); g.display(0xc80100, 0x960014); g.clocks();
    waitLoop(g);
    g.word(0xadb04, 0xe321f013);                          // msr cpsr_c, #0x13: IRQ and FIQ on
    g.store(0x00223008, 58);                              // AITC: timer 2 on
    g.store(0x00203008, 1000); g.store(0x00203000, 0x13);  // timer 2: PERCLK1, compare IRQ
    std::vector<uint32_t> words;
    load(words, 0, 0x16010000);
    words.push_back(0xe1a0e00f);                          // mov lr, pc
    const size_t first = words.size();
    words.push_back(0);                                   // b 200adb08
    load(words, 2, spin);
    words.push_back(0xe2522001);                          // subs r2, r2, #1
    words.push_back(0x5afffffd);                          // bpl: spin + 1 rounds
    words.push_back(0xe3a01000);                          // mov r1, #0
    words.push_back(0xe3110e40);                          // tst r1, #0x400: Z set, N and C clear
    load(words, 0, 0x16010000);
    words.push_back(0xe1a0e00f);                          // mov lr, pc
    const size_t second = words.size();
    words.push_back(0);                                   // b 200adb04
    const uint32_t base = g.cursor;
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.branch(base + uint32_t(first) * 4, 0xadb08);
    g.branch(base + uint32_t(second) * 4, 0xadb04);
    g.end();
    const uint64_t before = imx1MachineScanPollSkips(nullptr);
    const auto exact = runLines(g.words, false, false), fast = runLines(g.words, true, false);
    skips = imx1MachineScanPollSkips(nullptr) - before;
    imx1SetScanPollSkip(false);
    const auto plain = runLines(g.words, true, false);
    imx1SetScanPollSkip(true);
    const bool ok = exact.entries.size() == 1 && exact.entries[0].interruptedPc == 0x200adb08 &&
                    exact.out.run.result.trace.pc == 0x18 && sameLines(exact, fast) && sameLines(plain, fast);
    if (!ok || spin == 0) {
        std::printf("scan poll unmask spin %u: %llu skips\n", spin, (unsigned long long)skips);
        printLines("  exact", exact); printLines("  fast", fast); printLines("  no skip", plain);
    }
    assert(ok);
    return true;
}
// With the MMU on and the graphics registers privileged only, the vertical wait runs once in SVC mode, then is entered
// again at 0xadb04, where msr switches to user mode: r1 = 0 and Z set as the skip wants, but the first load there
// aborts. Fast stops at the abort at 200adb08 as Exact does, without passing rounds.
void scanPollUser() {
    GraphicsGuest g;
    g.setup(); g.display(0xc80100, 0x960014); g.clocks();
    waitLoop(g);
    g.word(0xadb04, 0xe321f0d0);                          // msr cpsr_c, #0xd0: user, IRQ and FIQ off
    std::vector<uint32_t> words;
    // Sections: 0x200 the ROM, 0x160 graphics, privileged only. MMU on, caches off.
    load(words, 0, 0x00304580); store(words, 0, 0, 0x16000412);
    load(words, 0, 0x00304800); store(words, 0, 0, 0x20000c12);
    load(words, 0, 0x00304000);
    words.push_back(0xee020f10);                          // TTB
    load(words, 1, 0x0d); words.push_back(0xee031f10);     // domain 0 client, 1 manager
    load(words, 1, 0x79); words.push_back(0xee011f10);     // MMU on
    load(words, 0, 0x16010000);
    words.push_back(0xe1a0e00f);                          // mov lr, pc
    const size_t first = words.size();
    words.push_back(0);                                   // b 200adb08
    words.push_back(0xe3a01000);                          // mov r1, #0
    words.push_back(0xe3110e40);                          // tst r1, #0x400: Z set, N and C clear
    load(words, 0, 0x16010000);
    words.push_back(0xe1a0e00f);                          // mov lr, pc
    const size_t second = words.size();
    words.push_back(0);                                   // b 200adb04
    const uint32_t base = g.cursor;
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.branch(base + uint32_t(first) * 4, 0xadb08);
    g.branch(base + uint32_t(second) * 4, 0xadb04);
    const uint64_t before = imx1MachineScanPollSkips(nullptr);
    const auto exact = runGuest(g.words, false), fast = runGuest(g.words, true);
    const uint64_t n = imx1MachineScanPollSkips(nullptr) - before;
    imx1SetScanPollSkip(false);
    const auto plain = runGuest(g.words, true);
    imx1SetScanPollSkip(true);
    print("scan poll user exact", exact); print("scan poll user fast", fast); print("scan poll user no skip", plain);
    std::printf("scan poll user: %llu skips\n", (unsigned long long)n);
    std::fflush(stdout);
    const auto& e = exact.run.result;
    assert(e.stop == Imx1ProbeStop::exception && e.trace.pc == 0x200adb08);
    same(plain, fast);
    same(exact, fast);
}
void scanPollIrqs() {
    unsigned inWait = 0, withSkips = 0;
    uint64_t total = 0;
    for (uint32_t comp = 400000; comp < 400000 + 16 * 600001; comp += 600001) {
        uint64_t skips;
        if (scanPollIrq(comp, skips)) ++inWait;
        if (skips) ++withSkips;
        total += skips;
    }
    std::printf("scan poll irqs: %u of 16 in a wait, %u with skips, %llu skips\n", inWait, withSkips,
                (unsigned long long)total);
    assert(inWait == 16 && withSkips == 16);
    total = 0;
    for (uint32_t spin = 0; spin < 2000; spin += 250) {
        uint64_t skips;
        scanPollUnmask(spin, skips);
        total += skips;
    }
    std::printf("scan poll unmask: %llu skips\n", (unsigned long long)total);
    std::fflush(stdout);
    scanPollUser();
}

// The same loop with timer 1 at PERCLK1 (prescaler 1), a count every few instructions, so a wait ends inside one JIT
// run. Timer reads in generated code see the cycles run so far, so each wait ends as Exact's does, within a few
// counts of 120, and the loop runs about as many instructions as Exact.
void delayShort() {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    g.store(0x00202004, 0);                               // TPRER1
    g.store(0x00202000, 0x03);                            // TCTL1: on, PERCLK1
    std::vector<uint32_t> words;
    load(words, 6, 0x00202000); load(words, 7, 20); load(words, 8, 0); load(words, 9, 0);
    const size_t outer = words.size();
    words.push_back(0xe5960010);                          // outer: ldr r0, [r6, #16]
    words.push_back(0xe5961010);                          // loop: ldr r1, [r6, #16]
    words.push_back(0xe0411000);                          // sub r1, r1, r0
    words.push_back(0xe3510078);                          // cmp r1, #120
    words.push_back(0x3afffffb);                          // bcc loop
    words.push_back(0xe1510008);                          // cmp r1, r8
    words.push_back(0x81a08001);                          // movhi r8, r1: the most counts any wait saw
    words.push_back(0xe0899001);                          // add r9, r9, r1: all the counts the waits saw
    words.push_back(0xe2577001);                          // subs r7, r7, #1
    words.push_back(0x1a000000 | ((uint32_t(outer) - uint32_t(words.size()) - 2) & 0xffffff));  // bne outer
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.end();
    const auto exact = runGuest(g.words, false), fast = runGuest(g.words, true);
    print("delay short exact", exact); print("delay short blocks", fast);
    const auto& e = exact.run.result;
    const auto& f = fast.run.result;
    std::printf("delay short: r1 %u against %u, most %u against %u, sum %u against %u, retired %llu against %llu\n",
                f.registers[1], e.registers[1], f.registers[8], e.registers[8], f.registers[9], e.registers[9],
                (unsigned long long)fast.cheap.retired, (unsigned long long)exact.cheap.retired);
    std::fflush(stdout);
    assert(e.stop == Imx1ProbeStop::unsupportedInstruction && e.registers[7] == 0 && e.registers[1] >= 120 &&
           e.registers[1] <= 124);
    // Generated code sees the timer at block ends, so a wait may end a count or two after Exact's, not more, and
    // the 20 waits together no fewer counts than Exact's and at most 5 more.
    assert(f.stop == e.stop && f.trace.pc == e.trace.pc && f.registers[7] == 0 && f.registers[1] >= 120 &&
           f.registers[8] <= e.registers[8] + 2 && f.registers[9] <= e.registers[9] + 5 && f.registers[9] >= e.registers[9]);
    assert(fast.cheap.retired * 4 <= exact.cheap.retired * 5);
}

// The ROM's domain switch (200ccfcc): read DACR, set domain 0 to manager, write it, then write the old value back,
// 200 times. Fast blocks run the MRC and MCR in generated code, with Exact's results.
void jitDomain() {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    std::vector<uint32_t> words;
    load(words, 7, 200);
    const size_t top = words.size();
    words.push_back(0xee130f10);                          // mrc p15, 0, r0, c3, c0, 0
    words.push_back(0xe3802003);                          // orr r2, r0, #3
    words.push_back(0xee032f10);                          // mcr p15, 0, r2, c3, c0, 0
    words.push_back(0xe2844001);                          // add r4, r4, #1
    words.push_back(0xee133f10);                          // mrc p15, 0, r3, c3, c0, 0
    words.push_back(0xe0855003);                          // add r5, r5, r3
    words.push_back(0xee030f10);                          // mcr p15, 0, r0, c3, c0, 0
    words.push_back(0xe2577001);                          // subs r7, r7, #1
    words.push_back(0x1a000000 | ((uint32_t(top) - uint32_t(words.size()) - 2) & 0xffffff));  // bne top
    words.push_back(0xee136f10);                          // mrc p15, 0, r6, c3, c0, 0
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.end();
    const uint64_t calls = cpuJitDomainCalls();
    const auto exact = runGuest(g.words, false), fast = runGuest(g.words, true);
    const uint64_t n = cpuJitDomainCalls() - calls;
    print("jit domain exact", exact); print("jit domain blocks", fast);
    std::printf("jit domain: %llu DACR reads and writes in generated code\n", (unsigned long long)n);
    std::fflush(stdout);
    assert(exact.run.result.registers[7] == 0 && exact.run.result.registers[4] == 200);
    sameResult(exact, fast);
    assert(n >= 4 * 150);
}

// An exception return (MOVS PC, LR to a Thumb loop) and a Thumb BL whose prefix is the last halfword of a 32-byte
// line, 200 times: both run in generated code, the returns counted there. The mode change ends the run, so one run
// per return and no more.
void jitReturns() {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    while ((g.cursor & 31) != 0) { g.word(g.cursor, 0xe1a00000); g.cursor += 4; ++g.retired; }
    const uint32_t base = 0x20000000 + g.cursor;
    // ARM at base: r7 = 200; loop: lr = thumb | 1 ... set SPSR to SVC with T, movs pc, lr.
    std::vector<uint32_t> words;
    load(words, 7, 200);                                  // 3 words
    const uint32_t loopAt = base + uint32_t(words.size()) * 4;
    // add lr, pc, #(thumb - (here + 8)); msr spsr_fsxc, #0xf3 (SVC, I, F, T); movs pc, lr
    const uint32_t thumbAt = base + 0x40;
    words.push_back(0xe28fe000 | ((thumbAt - (loopAt + 8)) & 0xff));
    words.push_back(0xe36ff0f3);
    words.push_back(0xe1b0f00e);
    while (base + words.size() * 4 < thumbAt) words.push_back(0xe1a00000);
    // Thumb at thumbAt (line start): nops up to offset 30, BL prefix there, suffix at 32: bl func.
    std::vector<uint16_t> h;
    while (h.size() < 15) h.push_back(0x46c0);
    const uint32_t blAt = thumbAt + 30;
    const uint32_t funcAt = thumbAt + 0x60;
    const int32_t offset = int32_t(funcAt - (blAt + 4));
    h.push_back(uint16_t(0xf000 | ((offset >> 12) & 0x7ff)));
    h.push_back(uint16_t(0xf800 | ((offset >> 1) & 0x7ff)));
    // After the call: subs r7, #1; beq done; bx to ARM loop (adr r5, ...).
    h.push_back(0x3f01);                                  // subs r7, #1
    const uint32_t doneAt = thumbAt + 0x50;
    const uint32_t beqAt = thumbAt + uint32_t(h.size()) * 2;
    h.push_back(uint16_t(0xd000 | (((doneAt - (beqAt + 4)) >> 1) & 0xff)));  // beq done
    const uint32_t ldrAt = thumbAt + uint32_t(h.size()) * 2;
    h.push_back(0);                                       // ldr r5, [pc, #literal]
    h.push_back(0x4728);                                  // bx r5
    while ((thumbAt + h.size() * 2) & 3) h.push_back(0x46c0);
    const uint32_t literalAt = thumbAt + uint32_t(h.size()) * 2;
    h[(ldrAt - thumbAt) / 2] = uint16_t(0x4d00 | ((literalAt - ((ldrAt + 4) & ~3u)) / 4));
    h.push_back(uint16_t(loopAt)); h.push_back(uint16_t(loopAt >> 16));
    while (thumbAt + h.size() * 2 < doneAt) h.push_back(0x46c0);
    h.push_back(0x2601);                                  // done: movs r6, #1
    h.push_back(0xdeff);                                  // undefined: the run stops here
    while (thumbAt + h.size() * 2 < funcAt) h.push_back(0x46c0);
    h.push_back(0x3401);                                  // func: adds r4, #1
    h.push_back(0x4770);                                  // bx lr
    for (size_t i = 0; i < h.size(); i += 2) words.push_back(h[i] | uint32_t(i + 1 < h.size() ? h[i + 1] : 0x46c0) << 16);
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    const uint64_t returns = cpuJitExceptionReturns();
    const auto exact = runGuest(g.words, false), fast = runGuest(g.words, true);
    const uint64_t n = imx1MachineJitRuns(nullptr);  // the last machine's
    const uint64_t r = cpuJitExceptionReturns() - returns;
    print("jit returns exact", exact); print("jit returns blocks", fast);
    std::printf("jit returns: %llu JIT runs, %llu exception returns in generated code\n", (unsigned long long)n,
                (unsigned long long)r);
    std::fflush(stdout);
    assert(exact.run.result.registers[4] == 200 && exact.run.result.registers[6] == 1);
    sameResult(exact, fast);
    assert(n <= 210 && r >= 180);
}

// A Thumb loop of SWI, count times, with the MMU mapping the ROM at 0; the handler at the SWI vector adds 1 to r4 and returns with MOVS PC, LR. The SWI,
// the return and the Thumb code after it all run in generated code, so a run of 200 takes few more steps than a
// run of 1 (each of the two would add 200 if it went through the step).
void swiGuest(GraphicsGuest& g, uint32_t count) {
    while ((g.cursor & 31) != 0) { g.word(g.cursor, 0xe1a00000); g.cursor += 4; }
    const uint32_t base = g.cursor;
    std::vector<uint32_t> words;
    // Sections: 0 the ROM, for the vectors, 0x200 the ROM. MMU on, caches off.
    load(words, 0, 0x00304000);
    store(words, 0, 0, 0x20000c1a);
    load(words, 0, 0x00304800); store(words, 0, 0, 0x20000c1a);
    load(words, 0, 0x00304000);
    words.push_back(0xee020f10);                          // TTB
    load(words, 1, 0x0d); words.push_back(0xee031f10);     // domain 0 client, 1 manager
    load(words, 1, 0x79); words.push_back(0xee011f10);     // MMU on
    load(words, 7, count);
    const uint32_t thumbAt = base + 0x100, handlerAt = base + 0x140;
    const uint32_t step = thumbAt + 1 - (base + uint32_t(words.size()) * 4 + 8);
    assert(step < 256);
    words.push_back(0xe28f5000 | step);                   // add r5, pc, #step
    words.push_back(0xe12fff15);                          // bx r5
    while (base + words.size() * 4 < thumbAt) words.push_back(0xe1a00000);
    words.push_back(0x3f01df00);                          // loop: swi 0; subs r7, #1
    words.push_back(0x2601d1fc);                          // bne loop; movs r6, #1
    words.push_back(0x46c0deff);                          // undefined: the run stops here
    while (base + words.size() * 4 < handlerAt) words.push_back(0xe1a00000);
    words.push_back(0xe2844001);                          // handler: add r4, r4, #1
    words.push_back(0xe1b0f00e);                          // movs pc, lr
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.branch(8, handlerAt);
}
Outcome swiLoop(uint32_t count, bool fast) {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    swiGuest(g, count);
    return runGuest(g.words, fast);
}

// The SWI loop with IRQs on while timer 1's compare IRQ comes due, display off so the quiet spans are long. A SWI that
// goes on in the same JIT run leaves the run's budget as it was, so the IRQ comes within a few rounds of Exact's,
// not after the loop.
Lines swiTimerLines(bool fast) {
    GraphicsGuest g;
    g.setup(); g.clocks();
    g.store(0x00223008, 59);                              // INTENNUM 59: timer 1
    g.store(0x00202004, 255);                             // TPRER1
    g.store(0x00202008, 3000);                            // TCMP1
    g.store(0x00202000, 0x13);                            // TCTL1: on, PERCLK1, IRQ
    g.word(g.cursor, 0xe321f013); g.cursor += 4; ++g.retired;  // IRQ and FIQ on
    swiGuest(g, 1000000);
    g.word(0x18, 0xe7f000f0);                             // IRQ vector: undefined, the run stops there
    return runLines(g.words, fast, false);
}
void jitSwiTimer() {
    const auto exact = swiTimerLines(false), fast = swiTimerLines(true);
    const uint64_t runs = imx1MachineJitRuns(nullptr);  // the Fast machine's, counted from its start
    printLines("jit swi timer exact", exact); printLines("jit swi timer blocks", fast);
    const uint32_t e = 1000000 - exact.out.run.result.registers[7], f = 1000000 - fast.out.run.result.registers[7];
    std::printf("jit swi timer: IRQ after %u rounds against %u, ticks %llu against %llu, %llu in %llu JIT runs\n", f, e,
                (unsigned long long)fast.out.cheap.time.ticks, (unsigned long long)exact.out.cheap.time.ticks,
                (unsigned long long)fast.out.jit, (unsigned long long)runs);
    std::fflush(stdout);
    assert(exact.entries.size() == 1 && fast.entries.size() == 1);
    assert(exact.out.run.result.trace.pc == 0x18 && fast.out.run.result.trace.pc == 0x18);
    assert(e > 1000 && e < 900000);
    // A round is 5 instructions, a run at most a slice of 997.
    assert(f + 2 >= e && f <= e + 200);
}

void jitSwi() {
    const auto exact = swiLoop(200, false);
    // JIT runs (counted from each machine's start): a privileged SWI and the return to Thumb go on in the same run, so
    // 200 rounds take few more runs than 1 (each would add two if the SWI and the switch back ended the run).
    const auto fast = swiLoop(200, true);
    const uint64_t runs200 = imx1MachineJitRuns(nullptr);
    const auto once = swiLoop(1, true);
    const uint64_t runs1Round = imx1MachineJitRuns(nullptr);
    std::printf("jit swi: %llu JIT runs for 200 rounds, %llu for 1\n", (unsigned long long)runs200,
                (unsigned long long)runs1Round);
    const auto steps = [](const Outcome& o) { return o.cheap.retired - o.lean - o.jit; };
    print("jit swi exact", exact); print("jit swi blocks", fast);
    std::printf("jit swi: %llu steps for 200 rounds, %llu for 1\n", (unsigned long long)steps(fast),
                (unsigned long long)steps(once));
    std::fflush(stdout);
    assert(exact.run.result.registers[4] == 200 && exact.run.result.registers[6] == 1);
    assert(once.run.result.registers[4] == 1);
    sameResult(exact, fast);
    assert(steps(fast) < steps(once) + 100);
    assert(runs200 < runs1Round + 100);
}

// A loop of a control register read and I- and D-cache operations by index, count times: all run in the JIT's p15
// slot, so a run of 200 takes few more steps than a run of 1 (each would add 200 through the step).
Outcome cacheOpLoop(uint32_t count, bool fast) {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    std::vector<uint32_t> words;
    load(words, 7, count);
    load(words, 1, 0);
    const size_t top = words.size();
    words.push_back(0xee110f10);                          // mrc p15, 0, r0, c1, c0, 0
    words.push_back(0xee071f55);                          // mcr p15, 0, r1, c7, c5, 2: I-cache entry by index
    words.push_back(0xee071f15);                          // mcr p15, 0, r1, c7, c5, 0: whole I-cache
    words.push_back(0xee071f5a);                          // mcr p15, 0, r1, c7, c10, 2: clean D-cache entry by index
    words.push_back(0xe0844000);                          // add r4, r4, r0
    words.push_back(0xe2577001);                          // subs r7, r7, #1
    words.push_back(0x1a000000 | ((uint32_t(top) - uint32_t(words.size()) - 2) & 0xffffff));  // bne top
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.end();
    return finish(make(g.words, fast, false, true));  // c7, c5, 2 as the Zodiac takes it
}

// MCR c7, c5, 1 (I-cache line invalidate) 200 times on a line address, then once with bit 0 set, which the
// ARM920T refuses as Exact does: Fast blocks stop on it as Exact does.
Outcome lineInvalidate(bool fast) {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    std::vector<uint32_t> words;
    load(words, 7, 200);
    load(words, 1, 0x20000000);
    const size_t top = words.size();
    words.push_back(0xee071f35);                          // mcr p15, 0, r1, c7, c5, 1
    words.push_back(0xe2844001);                          // add r4, r4, #1
    words.push_back(0xe2577001);                          // subs r7, r7, #1
    words.push_back(0x1a000000 | ((uint32_t(top) - uint32_t(words.size()) - 2) & 0xffffff));  // bne top
    words.push_back(0xe3811001);                          // orr r1, r1, #1
    words.push_back(0xee071f35);                          // mcr p15, 0, r1, c7, c5, 1: refused
    words.push_back(0xe2844001);                          // add r4, r4, #1
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.end();
    return finish(make(g.words, fast, false, true));
}

void jitLineInvalidate() {
    const auto exact = lineInvalidate(false), fast = lineInvalidate(true);
    print("jit line invalidate exact", exact); print("jit line invalidate blocks", fast);
    assert(exact.run.result.registers[4] == 200 && exact.run.result.registers[7] == 0);
    sameResult(exact, fast);
}

void jitCacheOps() {
    const auto exact = cacheOpLoop(200, false), fast = cacheOpLoop(200, true), once = cacheOpLoop(1, true);
    const auto steps = [](const Outcome& o) { return o.cheap.retired - o.lean - o.jit; };
    print("jit cache ops exact", exact); print("jit cache ops blocks", fast);
    std::printf("jit cache ops: %llu steps for 200 rounds, %llu for 1\n", (unsigned long long)steps(fast),
                (unsigned long long)steps(once));
    std::fflush(stdout);
    assert(exact.run.result.registers[7] == 0 && exact.run.result.registers[4] == 200 * exact.run.result.registers[0]);
    sameResult(exact, fast);
    assert(steps(fast) < steps(once) + 100);
}

void jitLong() {
    imx1SetFastBlocksDefault(true);
    cpuSetJitSync(true);
    setenv("FROND_JIT_HOT", "1", 1);
    jitReads();
    jitSliceEndReads();
    jitBoardWrites();
    jitStoreTimer();
    jitBoardRegisters();
    jitRingPolls();
    jitSwiTimer();
    delayLoop();
    delayLoop(true);
    delayShort();
    scanPolls();
    scanPollIrqs();
    jitDomain();
    jitReturns();
    jitSwi();
    jitCacheOps();
    jitLineInvalidate();
    cpuSetJitSync(false);
    unsetenv("FROND_JIT_HOT");
}

// MULS with Rd == Rm and a Thumb BX PC from a halfword address (an armlink veneer) run on the lean path, with
// Exact's results.
void aliasOps(bool veneer) {
    GraphicsGuest g;
    g.setup(); g.display(); g.clocks();
    std::vector<uint32_t> words;
    load(words, 0, 1); load(words, 1, 3); load(words, 7, 200); load(words, 9, 0); load(words, 10, 0);
    const size_t top = words.size();
    if (veneer) {
        words.push_back(0xe28f2001);                      // top: add r2, pc, #1
        words.push_back(0xe12fff12);                      // bx r2: Thumb from the next word
        words.push_back(0x477846c0);                      // nop; bx pc from top+10, so ARM from top+12
        words.push_back(0xe28aa001);                      // add r10, r10, #1
    } else {
        words.push_back(0xe0100190);                      // top: muls r0, r0, r1
        words.push_back(0xe0899000);                      // add r9, r9, r0
    }
    words.push_back(0xe2577001);                          // subs r7, r7, #1
    words.push_back(0x1a000000 | ((uint32_t(top) - uint32_t(words.size()) - 2) & 0xffffff));  // bne top
    for (auto w : words) { g.word(g.cursor, w); g.cursor += 4; }
    g.end();
    const auto exact = runGuest(g.words, false), fast = runGuest(g.words, true);
    const char* name = veneer ? "thumb bx pc" : "muls rd rm";
    std::printf("%s: lean %llu\n", name, (unsigned long long)fast.lean);
    print(name, exact);
    const auto& e = exact.run.result;
    assert(e.stop == Imx1ProbeStop::unsupportedInstruction && e.registers[7] == 0);
    uint32_t product = 1, sum = 0;
    for (unsigned i = 0; i < 200; ++i) sum += product *= 3;
    if (veneer) assert(e.registers[10] == 200 && !(e.cpsr & 0x20));
    else assert(e.registers[0] == product && e.registers[9] == sum);
    // 200 rounds of 7 (veneer) or 4 instructions, nearly all of them lean.
    assert(exact.lean == 0 && fast.lean >= (veneer ? 1380u : 780u));
    same(exact, fast);
}

int main() {
    uint32_t stopPc = 0, savePc = 0;
    // Lean-only Fast first, which gives Exact's times as well.
    imx1SetFastBlocksDefault(false);
    forcedIrq();
    aliasOps(false);
    aliasOps(true);
    leanDraw(Trigger::eim, "draw after eim");
    leanDraw(Trigger::transaction, "draw after transaction");
    storeThenTimer();
    scanReads();
    thumbScanReads();
    leanReads();
    rtcReads();
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
    blocks();
    jitLong();
    std::puts("PASS: Fast blocks answer graphics, identity and board reads and board writes in generated code, take an "
              "IRQ a write lets in within a block, pass timer delay loops at once and run exception returns and line-end "
              "BL pairs, with Exact's results");
    std::puts("PASS: Fast blocks match Exact's results and lean Fast's memory across TLB, TTBR, domain, mode and SDRAM "
              "changes, rewritten code, new code pages, the shipped compile setup and the PCM DMA buffer page");
    std::puts("PASS: Fast lean ARM and Thumb loops, CP15 TLB and domain changes, SDRAM mode change, save and load, "
              "rewritten code, unsupported access, timer reads, a cacheable timer and the PCM DMA buffer match Exact");
}
