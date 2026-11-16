// The cost table (core/src/uarm/uarm/xscale_cost.h): every class has a cost and is reached by some
// word, manual words give the manual's figure, the region map and charges, and a hash of the costs of
// every Thumb halfword and a fixed sweep of ARM words, the same on every host.
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "xscale_cost.h"

using namespace cost;

static int failures = 0;
static bool reached[classCount];

static void expect(bool ok, const char *what, unsigned long long got, unsigned long long want) {
    if (ok) return;
    std::printf("FAIL %s: got %llu, want %llu\n", what, got, want);
    failures++;
}

static void arm(Core core, uint32_t word, Class wantClass, uint32_t wantCost, const char *what) {
    unsigned words = 0;
    const Class c = armClass(word, core, words);
    reached[c] = true;
    expect(c == wantClass, what, c, wantClass);
    expect(armCost(word, core) == wantCost, what, armCost(word, core), wantCost);
}

static void thumb(Core core, uint32_t half, Class wantClass, uint32_t wantCost, const char *what) {
    unsigned words = 0;
    const Class c = thumbClass(half, core, words);
    reached[c] = true;
    expect(c == wantClass, what, c, wantClass);
    expect(thumbCost(half, core) == wantCost, what, thumbCost(half, core), wantCost);
}

// Checked at compile time too, so the table is a constant expression.
static_assert(armCost(0xe0810002u, Core::pxa27x) == 1, "ADD");
static_assert(thumbPairCost(0xf000f800u, Core::pxa27x) == 2, "Thumb BL");
static_assert(regionCost(Core::pxa27x, sdram, false, kClocksTx) == 2, "T|X SDRAM load");

int main() {
    const Core cores[] = {Core::pxa25x, Core::pxa27x, Core::arm920t};

    // Every class has a cost in every table, and every class is reached by some word on some core: a
    // sweep of every Thumb halfword and random ARM words, and the known words below.
    for (Core core : cores) {
        const Table &t = tableFor(core);
        for (unsigned c = 0; c < classCount; c++) {
            const uint32_t one = classCost(t, Class(c), 1);
            expect(one > 0, "class has a cost", c, 1);
        }
        unsigned words = 0;
        for (uint32_t h = 0; h < 0x10000; h++) reached[thumbClass(h, core, words)] = true;
        uint32_t x = 0x12345678;
        for (uint32_t i = 0; i < 1u << 22; i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            reached[armClass(x, core, words)] = true;
        }
    }
    // XScale manual figures (issue cycles) for passed instructions, and the fitted ones.
    const Core x = Core::pxa27x;
    arm(x, 0xe0810002u, dp, 1, "ADD r0, r1, r2");
    arm(x, 0xe0810312u, dpRegShift, 2, "ADD r0, r1, r2, LSL r3");
    arm(x, 0xe3a00001u, dp, 1, "MOV r0, #1");
    arm(x, 0xe1500001u, dp, 1, "CMP r0, r1");
    arm(x, 0xe1a0f00eu, dpPc, 5, "MOV pc, lr");
    arm(x, 0xe1b0f00eu, dpPc, 5, "MOVS pc, lr");
    arm(x, 0xe24ff004u, dpPc, 5, "SUB pc, pc, #4");
    arm(x, 0xe0000291u, mul, 2, "MUL r0, r1, r2");
    arm(x, 0xe0100291u, mulS, 3, "MULS r0, r1, r2");
    arm(x, 0xe0203291u, mul, 2, "MLA r0, r1, r2, r3");
    arm(x, 0xe0810392u, mulLong, 3, "UMULL r0, r1, r2, r3");
    arm(x, 0xe0d10392u, mulLongS, 4, "SMULLS r0, r1, r2, r3");
    arm(x, 0xe1000281u, dspMul, 1, "SMLABB r0, r1, r2, r0");
    arm(x, 0xe1600281u, dspMul, 1, "SMULBB r0, r1, r2");
    arm(x, 0xe1210281u, dspMul, 1, "SMLAWB r1, r1, r2, r0");
    arm(x, 0xe1410281u, dspMulLong, 2, "SMLALBB r0, r1, r1, r2");
    arm(x, 0xe16f0f11u, clzSat, 1, "CLZ r0, r1");
    arm(x, 0xe1020051u, clzSat, 1, "QADD r0, r1, r2");
    arm(x, 0xe5910000u, load, 1, "LDR r0, [r1]");
    arm(x, 0xe5d10000u, load, 1, "LDRB r0, [r1]");
    arm(x, 0xe1d100b0u, load, 1, "LDRH r0, [r1]");
    arm(x, 0xe1d100f0u, load, 1, "LDRSH r0, [r1]");
    arm(x, 0xe49df004u, loadPc, 8, "LDR pc, [sp], #4");
    arm(x, 0xe5810000u, store, 1, "STR r0, [r1]");
    arm(x, 0xe1c100b0u, store, 1, "STRH r0, [r1]");
    arm(x, 0xe1c120d0u, loadDouble, 1, "LDRD r2, [r1]");
    arm(x, 0xe1c120f0u, storeDouble, 2, "STRD r2, [r1]");
    arm(x, 0xe1010092u, swp, 5, "SWP r0, r2, [r1]");
    arm(x, 0xe8910003u, ldm, 2 + 2 * kXscale.ldmWord, "LDMIA r1, {r0, r1}");
    arm(x, 0xe8bd00f0u, ldm, 2 + 4 * kXscale.ldmWord, "LDMIA sp!, {r4-r7}");
    arm(x, 0xe92d40f0u, stm, 2 + 5 * kXscale.ldmWord, "STMDB sp!, {r4-r7, lr}");
    arm(x, 0xe8bd8000u, ldmPc, 10, "LDMIA sp!, {pc}");
    arm(x, 0xe8bd80f0u, ldmPc, 7 + 5 * kXscale.ldmWord, "LDMIA sp!, {r4-r7, pc}");
    arm(x, 0xea000000u, branch, kXscale.cls[branch], "B");
    arm(x, 0xeb000000u, branch, kXscale.cls[branch], "BL");
    arm(x, 0xe12fff1eu, branchExchange, 5, "BX lr");
    arm(x, 0xe12fff33u, branchExchange, 5, "BLX r3");
    arm(x, 0xfa000000u, branchExchange, 5, "BLX immediate");
    arm(x, 0xe10f0000u, mrs, 1, "MRS r0, cpsr");
    arm(x, 0xe14f0000u, mrs, 1, "MRS r0, spsr");
    arm(x, 0xe128f000u, msr, 2, "MSR cpsr_f, r0");
    arm(x, 0xe121f000u, msrControl, 6, "MSR cpsr_c, r0");
    arm(x, 0xe161f000u, msr, 2, "MSR spsr_c, r0");
    arm(x, 0xe321f0d3u, msrControl, 6, "MSR cpsr_c, #0xd3");
    arm(x, 0xee070f9au, cp15, 2, "MCR p15, 0, r0, c7, c10, 4");
    arm(x, 0xee110f10u, cp15, 2, "MRC p15, 0, r0, c1, c0, 0");
    arm(x, 0xee160e10u, cp14, 2, "MRC p14, 0, r0, c6, c0, 0 (CCLKCFG)");
    arm(x, 0xef000000u, swi, 5, "SWI 0");
    arm(x, 0xe7f000f0u, exception, 5, "undefined");
    arm(x, 0xe1200070u, exception, 5, "BKPT");
    arm(x, 0xf5d1f000u, dp, 1, "PLD [r1]");
    // iWMMXt (PXA27x), the CP0 accumulator (PXA25x).
    arm(x, 0xee000180u, wmmx, 1, "iWMMXt data processing (CDP p1)");
    arm(x, 0xee100110u, wmmx, 1, "TMRC");
    arm(x, 0xec410000u, wmmx, 1, "TMCRR");
    arm(x, 0xedd10100u, wmmxLoadStore, 1, "WLDRD");
    arm(x, 0xedc10100u, wmmxLoadStore, 1, "WSTRD");
    arm(Core::pxa25x, 0xee200010u, dspAcc, 1, "MIA (PXA25x)");
    arm(Core::pxa25x, 0xec410000u, dspAcc, 1, "MAR (PXA25x)");
    arm(Core::pxa25x, 0xee000180u, exception, 5, "CDP p1 on the PXA25x");
    arm(Core::pxa25x, 0xedd10100u, exception, 5, "LDC p1 on the PXA25x");
    expect(conditionFailedCost(x) == 1, "XScale condition failed", conditionFailedCost(x), 1);
    expect(paceOpCost(x) == kXscale.paceOp, "XScale PACE op", paceOpCost(x), kXscale.paceOp);

    // Thumb, priced as its ARM equivalent; a BL or BLX pair is the sum of its halves.
    thumb(x, 0x1888u, dp, 1, "ADDS r0, r1, r2");
    thumb(x, 0x2001u, dp, 1, "MOVS r0, #1");
    thumb(x, 0x4088u, dpRegShift, 2, "LSLS r0, r1");
    thumb(x, 0x4348u, mulS, 3, "MULS r0, r1");
    thumb(x, 0x4770u, branchExchange, 5, "BX lr");
    thumb(x, 0x4798u, branchExchange, 5, "BLX r3");
    thumb(x, 0x46f7u, dpPc, 5, "MOV pc, lr");
    thumb(x, 0x4800u, load, 1, "LDR r0, [pc]");
    thumb(x, 0x6808u, load, 1, "LDR r0, [r1]");
    thumb(x, 0x5888u, load, 1, "LDR r0, [r1, r2]");
    thumb(x, 0x5288u, store, 1, "STRH r0, [r1, r2]");
    thumb(x, 0x8008u, store, 1, "STRH r0, [r1]");
    thumb(x, 0x9800u, load, 1, "LDR r0, [sp]");
    thumb(x, 0xb082u, dp, 1, "SUB sp, #8");
    thumb(x, 0xb510u, stm, 2 + 2 * kXscale.ldmWord, "PUSH {r4, lr}");
    thumb(x, 0xbd10u, ldmPc, 7 + 2 * kXscale.ldmWord, "POP {r4, pc}");
    thumb(x, 0xbd00u, ldmPc, 10, "POP {pc}");
    thumb(x, 0xc903u, ldm, 2 + 2 * kXscale.ldmWord, "LDMIA r1!, {r0, r1}");
    thumb(x, 0xd0feu, branch, kXscale.cls[branch], "BEQ");
    thumb(x, 0xe7feu, branch, kXscale.cls[branch], "B");
    thumb(x, 0xdf00u, swi, 5, "SWI 0");
    thumb(x, 0xde00u, exception, 5, "undefined");
    thumb(x, 0xbe00u, exception, 5, "BKPT");
    thumb(x, 0xf000u, thumbBlPrefix, 1, "BL first half");
    thumb(x, 0xf800u, branch, kXscale.cls[branch], "BL second half");
    thumb(x, 0xe800u, branchExchange, 5, "BLX second half");
    expect(thumbPairCost(0xf000f800u, x) == 1 + kXscale.cls[branch], "BL pair", thumbPairCost(0xf000f800u, x),
           1 + kXscale.cls[branch]);
    expect(thumbPairCost(0xf000e800u, x) == 6, "BLX pair", thumbPairCost(0xf000e800u, x), 6);

    // ARM920T (ARMv4T): TRM figures; v5TE-only words are undefined there.
    const Core a = Core::arm920t;
    arm(a, 0xe0810002u, dp, 1, "920T ADD");
    arm(a, 0xe0810312u, dpRegShift, 2, "920T ADD, LSL r3");
    arm(a, 0xe1a0f00eu, dpPc, 3, "920T MOV pc, lr");
    arm(a, 0xe0000291u, mul, 3, "920T MUL");
    arm(a, 0xe0810392u, mulLong, 4, "920T UMULL");
    arm(a, 0xe5910000u, load, 1, "920T LDR");
    arm(a, 0xe49df004u, loadPc, 5, "920T LDR pc");
    arm(a, 0xe8910001u, ldm, 2, "920T LDMIA r1, {r0}");
    arm(a, 0xe8bd00f0u, ldm, 4, "920T LDMIA sp!, {r4-r7}");
    arm(a, 0xe8bd80f0u, ldmPc, 9, "920T LDMIA sp!, {r4-r7, pc}");
    arm(a, 0xea000000u, branch, 3, "920T B");
    arm(a, 0xe12fff1eu, branchExchange, 3, "920T BX lr");
    arm(a, 0xe121f000u, msrControl, 3, "920T MSR cpsr_c");
    arm(a, 0xe16f0f11u, exception, 3, "920T CLZ");
    arm(a, 0xfa000000u, exception, 3, "920T BLX immediate");
    arm(a, 0xe1c120d0u, exception, 3, "920T LDRD");
    arm(a, 0xee000180u, exception, 3, "920T CDP p1");
    thumb(a, 0x4770u, branchExchange, 3, "920T Thumb BX lr");
    thumb(a, 0x4798u, exception, 3, "920T Thumb BLX r3");
    thumb(a, 0xe800u, exception, 3, "920T Thumb BLX second half");
    expect(thumbPairCost(0xf000f800u, a) == 4, "920T BL pair", thumbPairCost(0xf000f800u, a), 4);
    expect(paceOpCost(a) == kArm920.paceOp, "920T PACE op", paceOpCost(a), kArm920.paceOp);
    expect(maxInstructionCost(x) == 7 + 16u * kXscale.ldmWord, "XScale largest cost", maxInstructionCost(x),
           7 + 16u * kXscale.ldmWord);
    expect(maxInstructionCost(a) == 20, "920T largest cost", maxInstructionCost(a), 20);

    for (unsigned c = 0; c < classCount; c++) expect(reached[c], "class reached by a word", c, 1);

    // Regions.
    expect(pxaRegion(0xa0000000u) == sdram, "PXA SDRAM", pxaRegion(0xa0000000u), sdram);
    expect(pxaRegion(0xa3ffffffu) == sdram, "PXA SDRAM end", pxaRegion(0xa3ffffffu), sdram);
    expect(pxaRegion(0x5c000000u) == sram, "PXA SRAM", pxaRegion(0x5c000000u), sram);
    expect(pxaRegion(0x5c040000u) == device, "PXA past SRAM", pxaRegion(0x5c040000u), device);
    expect(pxaRegion(0x00000000u) == flash, "PXA flash", pxaRegion(0), flash);
    expect(pxaRegion(0x04000000u) == staticBus, "PXA CS1", pxaRegion(0x04000000u), staticBus);
    expect(pxaRegion(0x14000000u) == staticBus, "PXA CS5", pxaRegion(0x14000000u), staticBus);
    expect(pxaRegion(0x40a00010u) == device, "PXA OSCR", pxaRegion(0x40a00010u), device);
    expect(pxaRegion(0x48000000u) == device, "PXA memory controller", pxaRegion(0x48000000u), device);
    expect(imx1Region(0x08000000u) == sdram, "i.MX1 SDRAM", imx1Region(0x08000000u), sdram);
    expect(imx1Region(0x0c000000u) == sdram, "i.MX1 CSD1", imx1Region(0x0c000000u), sdram);
    expect(imx1Region(0x00300000u) == sram, "i.MX1 eSRAM", imx1Region(0x00300000u), sram);
    expect(imx1Region(0x10000000u) == flash, "i.MX1 CS0", imx1Region(0x10000000u), flash);
    expect(imx1Region(0x12000000u) == staticBus, "i.MX1 CS1", imx1Region(0x12000000u), staticBus);
    expect(imx1Region(0x16000000u) == modelled, "i.MX1 CS5", imx1Region(0x16000000u), modelled);
    expect(imx1Region(0x00209000u) == device, "i.MX1 AIPI", imx1Region(0x00209000u), device);

    // Clocks from CCCR and CLKCFG (PXA25x table 3-1, PXA27x figure 3-1's equations).
    expect(kClocksTx.core == 312000 && kClocksTx.bus == 208000 && kClocksTx.memory == 104000, "T|X clocks",
           kClocksTx.core, 312000);
    expect(kClocksE2.core == 199066 && kClocksE2.bus == 99533 && kClocksE2.memory == 99533, "E2 clocks",
           kClocksE2.core, 199066);
    expect(kClocksTg50.core == 199066 && kClocksTg50.bus == 49766 && kClocksTg50.memory == 99533, "TG50 clocks",
           kClocksTg50.core, 199066);
    const Clocks txRun = pxa27xClocks(0x190, 0x8);
    expect(txRun.core == 208000 && txRun.bus == 208000, "T|X run mode", txRun.core, 208000);
    const Clocks txSlowBus = pxa27xClocks(0x190, 0x1);
    expect(txSlowBus.core == 312000 && txSlowBus.bus == 104000, "T|X turbo without fast bus", txSlowBus.bus, 104000);
    const Clocks pxa25xReset = pxa25xClocks(0x121, false);
    expect(pxa25xReset.core == 99533 && pxa25xReset.bus == 49766, "PXA25x reset", pxa25xReset.core, 99533);

    // Region charges in core clocks: {device clocks, region, load cost, store cost}.
    struct Charge {
        Core core;
        Clocks clocks;
        Region region;
        uint32_t load, store;
        const char *what;
    } charges[] = {
        {Core::pxa27x, kClocksTx, sdram, 2, 1, "T|X SDRAM"},
        {Core::pxa27x, kClocksTx, flash, 2, 2, "T|X flash"},
        {Core::pxa27x, kClocksTx, staticBus, 150, 150, "T|X static"},
        {Core::pxa27x, kClocksTx, device, 6, 6, "T|X device"},
        {Core::pxa27x, kClocksTx, sram, 2, 2, "T|X SRAM"},
        {Core::pxa25x, kClocksE2, sdram, 1, 1, "E2 SDRAM"},
        {Core::pxa25x, kClocksE2, staticBus, 100, 100, "E2 static"},
        {Core::pxa25x, kClocksE2, device, 8, 8, "E2 device"},
        {Core::pxa25x, kClocksTg50, sdram, 1, 2, "TG50 SDRAM"},
        {Core::pxa25x, kClocksTg50, staticBus, 100, 100, "TG50 static"},
        {Core::pxa25x, kClocksTg50, device, 16, 16, "TG50 device"},
        {Core::pxa27x, kClocksTx, modelled, 0, 0, "PXA modelled"},
        {Core::arm920t, kClocksZodiac, sdram, 1, 1, "Zodiac SDRAM"},
        {Core::arm920t, kClocksZodiac, sram, 2, 2, "Zodiac eSRAM"},
        {Core::arm920t, kClocksZodiac, flash, 1, 1, "Zodiac flash"},
        {Core::arm920t, kClocksZodiac, staticBus, 8, 8, "Zodiac CS1-CS4"},
        {Core::arm920t, kClocksZodiac, device, 8, 8, "Zodiac device"},
        {Core::arm920t, kClocksZodiac, modelled, 0, 0, "Zodiac CS5"},
    };
    for (const Charge &c : charges) {
        const uint32_t l = regionCost(c.core, c.region, false, c.clocks), s = regionCost(c.core, c.region, true, c.clocks);
        expect(l == c.load, c.what, l, c.load);
        expect(s == c.store, c.what, s, c.store);
    }

    // One hash over every Thumb halfword's and a fixed ARM sweep's costs on every core, and the
    // region charges at the device clocks: the same number on every host.
    uint64_t hash = 0xcbf29ce484222325ull;
    auto mix = [&hash](uint32_t v) {
        for (int i = 0; i < 4; i++) {
            hash ^= (v >> (8 * i)) & 0xff;
            hash *= 0x100000001b3ull;
        }
    };
    for (Core core : cores) {
        for (uint32_t h = 0; h < 0x10000; h++) mix(thumbCost(h, core));
        for (uint32_t w = 0; w < 1u << 20; w++) mix(armCost(w * 0x0fffu + (w << 20), core));
        for (unsigned r = 0; r < regionCount; r++)
            for (Clocks c : {kClocksTx, kClocksE2, kClocksTg50, kClocksZodiac}) {
                mix(regionCost(core, Region(r), false, c));
                mix(regionCost(core, Region(r), true, c));
            }
        mix(paceOpCost(core));
        mix(conditionFailedCost(core));
    }
    const uint64_t wantHash = 0x4b533ccd7cece3c4ull;
    if (hash != wantHash) {
        std::printf("FAIL cost hash: got 0x%016llx, want 0x%016llx\n", (unsigned long long)hash,
                    (unsigned long long)wantHash);
        failures++;
    }

    if (failures) {
        std::printf("test-xscale-cost: %d failures\n", failures);
        return 1;
    }
    std::printf("test-xscale-cost: ok (hash 0x%016llx)\n", (unsigned long long)hash);
    return 0;
}
