#ifndef _XSCALE_COST_H_
#define _XSCALE_COST_H_

#include <cstdint>

// Core clocks each guest instruction costs, for the XScale (PXA25x, PXA27x) and the ARM920T (Zodiac).
// Everything here is a pure function of the instruction word and constants, in integers, so the
// interpreter's per-instruction sum and the JIT's per-block sum come out the same on every host.
//
// An instruction's charge is made of:
// - armCost / thumbCost: its issue cycles when its condition passed (conditionFailed when it didn't),
//   LDM/STM priced by register count;
// - per memory access it makes, by the region of the physical address: SDRAM, internal SRAM and flash go
//   through the data cache model (dcache_cost.h: line fills, write-backs, write buffer entries and uncached
//   accesses), the static chip selects and on-chip device registers take a region charge (regionCost).
//   A 64-bit access (LDRD, STRD, WLDRD, WSTRD) is two accesses, SWP is a load and a store, each LDM/STM
//   word is one. PACE's own accesses update the cache but aren't charged: the 68k op cost covers them.
// - paceOp per 68k instruction native PACE runs.
//
// Issue cycles are the XScale core developer's manual's (section 10.4) and the ARM920T TRM's (chapter 12).
// A branch's cost hangs on the branch target buffer, which isn't modelled: the table uses one fitted cost
// per taken B or BL. Result latency stalls aren't modelled, except an average load-use stall in the load
// cost. The cache and region charges are fitted to real FrondBench and Speedy times
// (doc/step-cost-model-plan.md, M1 and M1b notes).

namespace cost {

enum class Core : uint8_t { pxa25x, pxa27x, arm920t };

// Instruction classes. Every class has a cost in every table.
enum Class : uint8_t {
    dp,             // data processing, immediate or immediate shift, PLD
    dpRegShift,     // data processing with a register-specified shift
    dpPc,           // data processing that writes the PC (MOV pc, lr; ADD pc, ...; MOVS pc, lr)
    dpPcRegShift,   // the same with a register-specified shift
    mul,            // MUL, MLA
    mulS,           // MULS, MLAS, Thumb MUL
    mulLong,        // UMULL, SMULL
    mulLongAcc,     // UMLAL, SMLAL
    mulLongS,       // any of the four with S
    dspMul,         // SMULxy, SMLAxy, SMULWy, SMLAWy
    dspMulLong,     // SMLALxy
    clzSat,         // CLZ, QADD, QSUB, QDADD, QDSUB
    load,           // LDR, LDRB, LDRH, LDRSB, LDRSH, LDRT, LDRBT
    loadPc,         // LDR pc, ...
    store,          // STR, STRB, STRH, STRT, STRBT
    loadDouble,     // LDRD
    storeDouble,    // STRD
    swp,            // SWP, SWPB
    ldm,            // LDM without the PC, priced by words too
    ldmPc,          // LDM with the PC in the list (POP {.., pc})
    stm,            // STM, PUSH
    branch,         // B, BL (ARM), Thumb B, conditional B and BL's second half
    branchExchange, // BX, BLX (register or immediate), Thumb BLX's second half
    thumbBlPrefix,  // Thumb BL/BLX first half (LR = PC + offset)
    mrs,            // MRS
    msr,            // MSR of the flags or status field only
    msrControl,     // MSR that writes CPSR's control field (mode and interrupt masks)
    cp15,           // MCR to CP15
    cp15Read,       // MRC from CP15
    cp14,           // MCR, MRC to CP14 (clock and power, performance counters)
    dspAcc,         // PXA25x CP0 accumulator: MIA, MIAPH, MIAxy, MRA
    dspAccWrite,    // PXA25x MAR (accumulator write)
    wmmx,           // iWMMXt data processing and transfers (CDP, MCR, MRC, MCRR, MRRC on CP0, CP1)
    wmmxLoadStore,  // iWMMXt WLDR, WSTR (LDC, STC on CP0, CP1)
    swi,            // SWI
    exception,      // undefined instructions, BKPT, coprocessors that don't exist: the exception entry
    classCount
};

// A region of the physical address space for the region charge.
enum Region : uint8_t {
    sdram,
    sram,       // PXA27x internal SRAM, i.MX1 eSRAM
    flash,      // static chip select 0, the boot ROM
    staticBus,  // static chip selects 1-5 (NAND, DiskOnChip, Ethernet, the CLIÉ's sound and memory stick)
    device,     // on-chip device registers
    modelled,   // timed by its own bus model already (the Zodiac's CS5): no region charge
    regionCount
};

struct Table {
    uint8_t cls[classCount];  // passed instruction, LDM/STM before their words
    uint8_t ldmWord;          // per LDM/STM word
    uint8_t ldmMin;           // LDM/STM with no or few words cost at least this
    uint8_t ldmPcMin;         // LDM with the PC costs at least this
    uint8_t conditionFailed;
    uint8_t paceOp;           // one 68k instruction run by native PACE
};

// XScale (PXA25x and PXA27x share the core): Intel XScale core developer's manual (273473-002) 10.4,
// minimum issue latencies, in build/step-cost-model/manuals/:
//   data processing 1, register-specified shift or RRX 2 (table 10-5); writing the PC 4 more (table 10-4).
//   MUL/MLA issue in 1 without S; with S 2-4 by the size of Rs: 3. UMULL/SMULL 1, UMLAL/SMLAL 2, any of
//   them with S 3-5: 4 (table 10-6). Result latency stalls on the next instruction aren't counted.
//   SMULxy, SMLAxy, SMULWy, SMLAWy 1, SMLALxy 2. CLZ, QADD and friends 1 (10-9, 10-17).
//   LDR, STR and their byte and halfword forms 1; LDRD 1, STRD 2; LDR to the PC 8 (10-11, 10-4).
//   SWP 5 (10-13). LDM and STM 2 + one per register; LDM with the PC 10 + max(0, n - 3) with n counting
//   the PC, so 7 + one per register and 10 at least (10-12, 10-4).
//   B and BL 1 when the BTB predicts them, 5 when not (10-3); BX, BLX 5 taken (10-4).
//   MRS 1, MSR 2, 6 when it updates the mode bits (10-10).
//   MCR to CP15 2, MRC from CP15 4 (10-14). MCR and MRC to CP14 8 (10-15).
//   SWI, BKPT and undefined: 6 to the handler's first instruction (10-16).
//   PXA25x accumulator: MIA, MIAPH, MIAxy 1, MRA 1, MAR 2 (10-7, 10-8).
//   Thumb is timed as its ARM equivalent, but a taken BX, BLX, POP {pc} or MOV/ADD pc costs one more
//   (10.4.11): thumbCost adds it. A condition that fails costs 1.
//   iWMMXt data processing, transfers and loads issue in 1 (Wireless MMX developer's guide, not in
//   build/: an estimate).
// Fitted (M1b notes): single loads 2 (issue plus an average load-use stall), B and BL 4 (issue plus an
// average for BTB misses and other stalls the table doesn't see), a PACE 68k op 54. Memory beyond that is
// the region charge (device and static bus) or the data cache model's charge (dcache_cost.h).
constexpr Table kXscale = {
    {
        /* dp */ 1, /* dpRegShift */ 2, /* dpPc */ 5, /* dpPcRegShift */ 6,
        /* mul */ 1, /* mulS */ 3, /* mulLong */ 1, /* mulLongAcc */ 2, /* mulLongS */ 4,
        /* dspMul */ 1, /* dspMulLong */ 2,
        /* clzSat */ 1, /* load */ 2, /* loadPc */ 8, /* store */ 1, /* loadDouble */ 2, /* storeDouble */ 2,
        /* swp */ 5, /* ldm */ 2, /* ldmPc */ 7, /* stm */ 2,
        /* branch */ 4, /* branchExchange */ 5, /* thumbBlPrefix */ 1,
        /* mrs */ 1, /* msr */ 2, /* msrControl */ 6, /* cp15 */ 2, /* cp15Read */ 4, /* cp14 */ 8,
        /* dspAcc */ 1, /* dspAccWrite */ 2, /* wmmx */ 1, /* wmmxLoadStore */ 1, /* swi */ 6, /* exception */ 6,
    },
    /* ldmWord */ 1, /* ldmMin */ 3, /* ldmPcMin */ 10, /* conditionFailed */ 1,
    /* paceOp */ 54,
};

// ARM920T (ARM9TDMI core), ARM920T TRM (DDI 0151C) table 12-2, all from cached memory:
//   data processing 1, register-specified shift 2, writing the PC 2 more (pipeline refill).
//   MUL/MLA 2 + m, long multiplies 3 + m, m 1-4 by the size of Rs (12.2.1): m = 2 (16-bit operands), so
//   4 and 5; S changes nothing. ARMv4T has no DSP multiplies, CLZ, LDRD, BLX or PLD: undefined on it.
//   LDR, STR 1 (a load-use interlock on the next instruction isn't counted), LDR to the PC 5. SWP 2.
//   LDM and STM one per register, 2 for one register; LDM with the PC n + 4, so 5 at least.
//   B, BL, BX 3. SWI and undefined 3. MCR and MRC b + 1 with no busy-wait: 1. MRS 1, MSR 1, 3 when it
//   writes the control field (ARM9TDMI TRM). A condition that fails costs 1.
// A PACE 68k op 40: the Zodiac's Speedy Calc time on the T|X's Speedy mix, until a Zodiac Speedy run
// tunes it (M1 notes).
constexpr Table kArm920 = {
    {
        /* dp */ 1, /* dpRegShift */ 2, /* dpPc */ 3, /* dpPcRegShift */ 4,
        /* mul */ 4, /* mulS */ 4, /* mulLong */ 5, /* mulLongAcc */ 5, /* mulLongS */ 5,
        /* dspMul */ 3, /* dspMulLong */ 3,
        /* clzSat */ 3, /* load */ 1, /* loadPc */ 5, /* store */ 1, /* loadDouble */ 3, /* storeDouble */ 3,
        /* swp */ 2, /* ldm */ 0, /* ldmPc */ 4, /* stm */ 0,
        /* branch */ 3, /* branchExchange */ 3, /* thumbBlPrefix */ 1,
        /* mrs */ 1, /* msr */ 1, /* msrControl */ 3, /* cp15 */ 1, /* cp15Read */ 1, /* cp14 */ 3,
        /* dspAcc */ 3, /* dspAccWrite */ 3, /* wmmx */ 3, /* wmmxLoadStore */ 3, /* swi */ 3, /* exception */ 3,
    },
    /* ldmWord */ 1, /* ldmMin */ 2, /* ldmPcMin */ 5, /* conditionFailed */ 1,
    /* paceOp */ 40,
};

constexpr const Table& tableFor(Core core) { return core == Core::arm920t ? kArm920 : kXscale; }

constexpr unsigned popcount16(uint32_t v) {
    unsigned n = 0;
    for (v &= 0xffff; v; v &= v - 1) n++;
    return n;
}

// Class of an ARM word. LDM/STM words go in words (0 for everything else). The condition isn't
// looked at: a failed one costs conditionFailed whatever its class.
constexpr Class armClass(uint32_t w, Core core, unsigned& words) {
    words = 0;
    const bool v5 = core != Core::arm920t;
    if ((w >> 28) == 0xf) {  // unconditional space: BLX immediate and PLD on v5TE, nothing else
        if (v5 && (w & 0x0e000000u) == 0x0a000000u) return branchExchange;
        if (v5 && (w & 0x0d70f000u) == 0x0550f000u) return dp;
        return exception;
    }
    switch ((w >> 25) & 7) {
        case 0: {
            if ((w & 0x0ffffff0u) == 0x012fff10u) return branchExchange;  // BX
            if ((w & 0x0ffffff0u) == 0x012fff30u) return v5 ? branchExchange : exception;  // BLX register
            if ((w & 0x0fff0ff0u) == 0x016f0f10u) return v5 ? clzSat : exception;  // CLZ
            if ((w & 0x0ff000f0u) == 0x01200070u) return exception;  // BKPT
            if ((w & 0x0f9000f0u) == 0x01000050u) return v5 ? clzSat : exception;  // QADD, QSUB, QDADD, QDSUB
            if ((w & 0x0fc000f0u) == 0x00000090u) return (w & (1u << 20)) ? mulS : mul;
            if ((w & 0x0f8000f0u) == 0x00800090u) return (w & (1u << 20)) ? mulLongS : (w & (1u << 21)) ? mulLongAcc : mulLong;
            if ((w & 0x0fb00ff0u) == 0x01000090u) return swp;
            if ((w & 0x0e000090u) == 0x00000090u && (w & 0x60u)) {  // halfword, signed byte, LDRD, STRD
                const bool l = w & (1u << 20);
                const unsigned sh = (w >> 5) & 3;
                if (!l && sh == 2) return v5 ? loadDouble : exception;
                if (!l && sh == 3) return v5 ? storeDouble : exception;
                if (l) return ((w >> 12) & 15) == 15 ? loadPc : load;
                return store;
            }
            if ((w & 0x0f900090u) == 0x01000080u) {  // ARMv5TE signed multiplies
                if (!v5) return exception;
                return ((w >> 21) & 3) == 2 ? dspMulLong : dspMul;
            }
            if ((w & 0x0fbf0fffu) == 0x010f0000u) return mrs;
            if ((w & 0x0fb0fff0u) == 0x0120f000u) return ((w & (1u << 16)) && !(w & (1u << 22))) ? msrControl : msr;
            if ((w & 0x0f900000u) == 0x01000000u) return exception;  // the rest of the miscellaneous space
            break;
        }
        case 1:
            if ((w & 0x0fb0f000u) == 0x0320f000u)
                return ((w & (1u << 16)) && !(w & (1u << 22))) ? msrControl : msr;
            if ((w & 0x0fb00000u) == 0x03000000u) return exception;
            break;
        case 2:
        case 3:
            if (((w >> 25) & 7) == 3 && (w & 0x10u)) return exception;
            if (w & (1u << 20)) return ((w >> 12) & 15) == 15 ? loadPc : load;
            return store;
        case 4:
            words = popcount16(w);
            if (w & (1u << 20)) return (w & 0x8000u) ? ldmPc : ldm;
            return stm;
        case 5:
            return branch;
        case 6: {
            const unsigned cp = (w >> 8) & 15;
            if (core == Core::pxa27x && cp <= 1) {
                if ((w & 0x0fe00000u) == 0x0c400000u) return wmmx;  // TMCRR, TMRRC
                return wmmxLoadStore;
            }
            if (core == Core::pxa25x && cp == 0 && (w & 0x0fe00000u) == 0x0c400000u)
                return (w & (1u << 20)) ? dspAcc : dspAccWrite;  // MRA, MAR
            return exception;
        }
        default: {  // 7
            if (w & (1u << 24)) return swi;
            const unsigned cp = (w >> 8) & 15;
            if (w & 0x10u) {  // MCR, MRC
                if (cp == 15) return (w & (1u << 20)) ? cp15Read : cp15;
                if (cp == 14) return v5 ? cp14 : exception;
                if (core == Core::pxa27x && cp <= 1) return wmmx;  // TMCR, TMRC, TMIA, TEXTRC, ...
                if (core == Core::pxa25x && cp == 0) return dspAcc;  // MIA, MIAPH, MIAxy
                return exception;
            }
            if (core == Core::pxa27x && cp <= 1) return wmmx;  // iWMMXt data processing
            return exception;
        }
    }
    // Data processing (register or immediate).
    const unsigned op = (w >> 21) & 15;
    const bool test = op >= 8 && op <= 11;
    const bool regShift = !(w & (1u << 25)) && (w & 0x10u);
    if (!test && ((w >> 12) & 15) == 15) return regShift ? dpPcRegShift : dpPc;
    return regShift ? dpRegShift : dp;
}

// Class of a 16-bit Thumb instruction (each half of BL and BLX on its own).
constexpr Class thumbClass(uint32_t h, Core core, unsigned& words) {
    words = 0;
    h &= 0xffff;
    const bool v5 = core != Core::arm920t;
    switch (h >> 11) {
        case 0x08:
            if (!(h & 0x400)) {  // ALU operations
                const unsigned op = (h >> 6) & 15;
                if (op == 13) return mulS;
                if (op == 2 || op == 3 || op == 4 || op == 7) return dpRegShift;  // LSL, LSR, ASR, ROR by register
                return dp;
            }
            switch ((h >> 8) & 3) {
                case 3:  // BX, BLX register
                    if ((h & 0x80) && !v5) return exception;
                    return branchExchange;
                case 1:  // CMP high
                    return dp;
                default:  // ADD, MOV high
                    return ((h & 7) | ((h >> 4) & 8)) == 15 ? dpPc : dp;
            }
        case 0x09:
            return load;  // LDR pc-relative
        case 0x0a:
        case 0x0b:
            return ((h >> 9) & 7) >= 3 ? load : store;
        case 0x0c: case 0x0d: case 0x0e: case 0x0f:  // word and byte, immediate offset
        case 0x10: case 0x11:                        // halfword, immediate offset
        case 0x12: case 0x13:                        // SP-relative
            return (h & 0x800) ? load : store;
        case 0x14:
        case 0x15:
            return dp;  // ADD to PC or SP
        case 0x16:
        case 0x17:
            switch ((h >> 8) & 15) {
                case 0x0:
                    return dp;  // ADD, SUB SP
                case 0x4:
                case 0x5:
                    words = popcount16(h & 0x1ff);
                    return stm;  // PUSH
                case 0xc:
                case 0xd:
                    words = popcount16(h & 0x1ff);
                    return (h & 0x100) ? ldmPc : ldm;  // POP
                default:
                    return exception;  // BKPT and the undefined rest
            }
        case 0x18:
        case 0x19:
            words = popcount16(h & 0xff);
            return (h & 0x800) ? ldm : stm;
        case 0x1a:
        case 0x1b:
            if (((h >> 8) & 15) == 15) return swi;
            if (((h >> 8) & 15) == 14) return exception;
            return branch;
        case 0x1c:
            return branch;
        case 0x1d:
            return (v5 && !(h & 1)) ? branchExchange : exception;  // BLX second half
        case 0x1e:
            return thumbBlPrefix;
        case 0x1f:
            return branch;  // BL second half
        default:
            return dp;  // shifts, ADD/SUB, MOV/CMP/ADD/SUB immediate
    }
}

constexpr uint32_t classCost(const Table& t, Class c, unsigned words) {
    if (c == ldm || c == stm) {
        const uint32_t n = t.cls[c] + t.ldmWord * words;
        return n < t.ldmMin ? t.ldmMin : n;
    }
    if (c == ldmPc) {
        const uint32_t n = t.cls[c] + t.ldmWord * words;
        return n < t.ldmPcMin ? t.ldmPcMin : n;
    }
    return t.cls[c];
}

// Clocks for an ARM instruction whose condition passed.
constexpr uint32_t armCost(uint32_t word, Core core) {
    unsigned words = 0;
    const Class c = armClass(word, core, words);
    return classCost(tableFor(core), c, words);
}

// The XScale's extra cycle for a taken Thumb BX, BLX, POP {pc} or MOV/ADD pc (manual 10.4.11).
constexpr uint32_t thumbPenalty(Class c, Core core) {
    return core != Core::arm920t && (c == branchExchange || c == ldmPc || c == dpPc) ? 1 : 0;
}

// Clocks for a 16-bit Thumb instruction whose condition passed (only conditional B has one).
constexpr uint32_t thumbCost(uint32_t half, Core core) {
    unsigned words = 0;
    const Class c = thumbClass(half, core, words);
    return classCost(tableFor(core), c, words) + thumbPenalty(c, core);
}

// A Thumb BL or BLX pair as one 32-bit instruction (first half in the top 16 bits, as dynarmic reads
// it): the sum of its halves, so it costs what the interpreter charges running them one by one.
constexpr uint32_t thumbPairCost(uint32_t pair, Core core) {
    return thumbCost(pair >> 16, core) + thumbCost(pair & 0xffff, core);
}

// An instruction as dynarmic reads it, whose condition passed: an ARM word, a 16-bit Thumb halfword, or
// a Thumb pair as one 32-bit word (above 0xffff). Frond's own ARM words (0xfff9xxxx: PACE entry and
// dispatch, peepholes) cost 1, as in the I-cache.
constexpr uint32_t jitInstructionCost(bool thumb, uint32_t instruction, Core core) {
    if (thumb) return instruction > 0xffff ? thumbPairCost(instruction, core) : thumbCost(instruction, core);
    if ((instruction & 0xffff0000u) == 0xfff90000u) return 1;
    return armCost(instruction, core);
}

// thumbCost of every halfword, one table per core, built on first use.
inline const uint8_t* thumbCostTable(Core core) {
    struct Tables {
        uint8_t cost[3][65536];
        Tables() {
            for (unsigned k = 0; k < 3; k++)
                for (uint32_t h = 0; h < 65536; h++) cost[k][h] = uint8_t(thumbCost(h, Core(k)));
        }
    };
    static const Tables tables;
    return tables.cost[unsigned(core)];
}

constexpr uint32_t conditionFailedCost(Core core) { return tableFor(core).conditionFailed; }
constexpr uint32_t paceOpCost(Core core) { return tableFor(core).paceOp; }

// The largest cost a single passed instruction can have (LDM with all 16 registers and the PC).
constexpr uint32_t maxInstructionCost(Core core) {
    const Table& t = tableFor(core);
    uint32_t m = classCost(t, ldmPc, 16);
    for (unsigned c = 0; c < classCount; c++)
        if (classCost(t, Class(c), 0) > m) m = classCost(t, Class(c), 0);
    return m;
}

// Region of a physical address.
constexpr Region pxaRegion(uint32_t pa) {
    if (pa >= 0xA0000000u && pa < 0xC0000000u) return sdram;  // SDRAM banks 0-3
    if (pa >= 0x5C000000u && pa < 0x5C040000u) return sram;   // PXA27x internal SRAM
    if (pa < 0x04000000u) return flash;                       // static chip select 0
    if (pa < 0x40000000u) return staticBus;                   // static chip selects 1-5, PCMCIA
    return device;
}

constexpr Region imx1Region(uint32_t pa) {
    if (pa >= 0x08000000u && pa < 0x10000000u) return sdram;  // CSD0, CSD1
    if (pa >= 0x00300000u && pa < 0x00320000u) return sram;   // eSRAM
    if (pa >= 0x10000000u && pa < 0x12000000u) return flash;  // CS0
    if (pa >= 0x16000000u && pa < 0x17000000u) return modelled;  // CS5: Frond's CS5 bus model times it
    if (pa >= 0x12000000u && pa < 0x16000000u) return staticBus;  // CS1-CS4
    return device;
}

// The clocks a region charge is worked out from, in kHz.
struct Clocks {
    uint32_t core;    // the CPU clock
    uint32_t bus;     // the system bus (PXA27x) or PXbus (PXA25x), the i.MX1's HCLK
    uint32_t memory;  // the memory controller and SDRAM
};

// PXA25x CCCR (L, M, N; the PXbus runs at half the run frequency) with CLKCFG's turbo bit, PXA25x
// developer's manual 3.4.1 and table 3-20. Crystal 3.6864 MHz.
constexpr Clocks pxa25xClocks(uint32_t cccr, bool turbo) {
    const uint32_t lCode = cccr & 31, mCode = (cccr >> 5) & 3, nCode = (cccr >> 7) & 7;
    const uint32_t l = lCode == 1 ? 27 : lCode == 3 ? 36 : lCode == 5 ? 45 : 27;
    const uint32_t memory = (3686400u * l + 500) / 1000;  // kHz
    const uint32_t m = mCode == 2 ? 2 : mCode == 3 ? 4 : 1;
    const uint32_t run = memory * m;
    // N in halves: 2 = 1.0, 3 = 1.5, 4 = 2.0, 6 = 3.0.
    const uint32_t n2 = nCode == 3 ? 3 : nCode == 4 ? 4 : nCode == 6 ? 6 : 2;
    return {turbo ? run * n2 / 2 : run, run / 2, memory};
}

// The PXA25x core clock in Hz, exact (the clocks above are in kHz): crystal 3.6864 MHz x L x M x N.
constexpr uint64_t pxa25xCoreHz(uint32_t cccr, bool turbo) {
    const uint32_t lCode = cccr & 31, mCode = (cccr >> 5) & 3, nCode = (cccr >> 7) & 7;
    const uint32_t l = lCode == 1 ? 27 : lCode == 3 ? 36 : lCode == 5 ? 45 : 27;
    const uint32_t m = mCode == 2 ? 2 : mCode == 3 ? 4 : 1;
    const uint32_t n2 = nCode == 3 ? 3 : nCode == 4 ? 4 : nCode == 6 ? 6 : 2;
    return uint64_t(3686400) * l * m * (turbo ? n2 : 2) / 2;
}

// PXA27x CCCR (L, 2N, A) with CLKCFG's turbo, half-turbo and fast-bus bits, PXA27x developer's
// manual 3.4 (figure 3-1's equations) and table 3-31. Oscillator 13 MHz.
constexpr Clocks pxa27xClocks(uint32_t cccr, uint32_t clkcfg) {
    uint32_t l = cccr & 31;
    if (l < 2) l = 2;
    uint32_t n2 = (cccr >> 7) & 15;
    if (n2 < 2) n2 = 2;
    const bool a = cccr & (1u << 25);
    const bool turbo = clkcfg & 1, halfTurbo = clkcfg & 4, fastBus = clkcfg & 8;
    const uint32_t run = 13000u * l;
    uint32_t core = run;
    if (turbo) core = run * n2 / 2;
    if (halfTurbo) core = run * n2 / 4;
    const uint32_t bus = fastBus ? run : run / 2;
    const uint32_t memory = !a ? run / (l <= 10 ? 1 : l <= 20 ? 2 : 4) : fastBus ? run : run / 2;
    return {core, bus, memory};
}

// The PXA27x core clock in Hz (the oscillator is 13 MHz, so the kHz figure above is exact).
constexpr uint64_t pxa27xCoreHz(uint32_t cccr, uint32_t clkcfg) { return uint64_t(pxa27xClocks(cccr, clkcfg).core) * 1000; }

// The region charges: memory controller and bus clocks per access, in sixteenths, for a load and for a
// store, turned into core clocks by the clock ratios and rounded once (half up). SDRAM, SRAM and flash
// are cacheable and priced by the data cache model (dcache_cost.h), so they charge nothing here.
struct RegionCharge {
    uint16_t loadMemory16, loadBus16;
    uint16_t storeMemory16, storeBus16;
};

constexpr RegionCharge kXscaleRegions[regionCount] = {
    /* sdram */ {0, 0, 0, 0},
    /* sram */ {0, 0, 0, 0},
    /* flash */ {0, 0, 0, 0},
    // Static chip selects: 48 memory clocks. Fitted from the T|X FrondBench Database test alone (its NAND
    // data writes), so the least certain charge here.
    /* staticBus */ {768, 0, 768, 0},
    // On-chip device registers: 4 bus clocks. Estimate: the runs' device accesses are polls and idle
    // loops whose time is set by the device, not by the charge.
    /* device */ {0, 64, 0, 64},
    /* modelled */ {0, 0, 0, 0},
};

// i.MX1 (Zodiac): the same charges against HCLK, which clocks both the bus and the SDRAM. Estimates
// until a Zodiac Speedy run tunes them (M1 notes). CS5 is timed by Frond's CS5 model.
constexpr RegionCharge kArm920Regions[regionCount] = {
    /* sdram */ {0, 0, 0, 0},
    /* sram */ {0, 0, 0, 0},
    /* flash */ {0, 0, 0, 0},
    /* staticBus */ {0, 64, 0, 64},
    /* device */ {0, 64, 0, 64},
    /* modelled */ {0, 0, 0, 0},
};

// Core clocks one load or store to a region costs at these clocks.
constexpr uint32_t regionCost(Core core, Region r, bool store, Clocks c) {
    const RegionCharge& g = (core == Core::arm920t ? kArm920Regions : kXscaleRegions)[r];
    const uint64_t memory16 = store ? g.storeMemory16 : g.loadMemory16, bus16 = store ? g.storeBus16 : g.loadBus16;
    const uint64_t num = memory16 * c.core * c.bus + bus16 * c.core * c.memory;
    const uint64_t den = uint64_t(16) * c.memory * c.bus;
    return den ? uint32_t((num + den / 2) / den) : 0;
}

// The clocks each device's ROM sets for its busy code (M0 notes, COSTCLK): CCCR, then CLKCFG.
constexpr Clocks kClocksTx = pxa27xClocks(0x190, 0x9);       // T|X and Zire 72: 312 MHz turbo, fast bus
constexpr Clocks kClocksE2 = pxa25xClocks(0x141, true);      // Tungsten E2, E3: 199 MHz run, PXbus 99.5
constexpr Clocks kClocksTg50 = pxa25xClocks(0x221, true);    // CLIÉ TG50: 199 MHz turbo on a 99.5 MHz run, PXbus 50
// i.MX1 on the Zodiac: FCLK 200 MHz from the MCU PLL, HCLK (BCLK) 96 MHz, SDRAM on HCLK.
constexpr Clocks kClocksZodiac = {200000, 96000, 96000};

}  // namespace cost

#endif
