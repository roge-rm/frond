#ifndef CPUTIMING_H
#define CPUTIMING_H

/* 68000 instruction timing in clocks, from the Motorola MC68000 user's manual, section 8.
 *
 * gCpuTiming holds one entry per opcode: bits 0-7 are the clocks with no wait states, halved
 * (all 68000 counts are even), bits 8-11 the bus reads from program space and bits 12-15 a
 * kTiming kind for the parts that depend on data. Operand reads and writes go through the
 * counting get_* and put_* macros, which add each bank's wait states to gUaeAccessWait and its
 * bus tag to gUaeBusCount (bus cycles in the low half, DRAM cycles in the high half).
 * Handlers add data-dependent clocks (multiply, divide) to gUaeTimingExtra. Both are cleared
 * before each instruction and only touched on the emulation thread. */

#include "sysdeps.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    kTimingPlain,
    kTimingBccShort,  /* taken 10 (2 reads), not taken 8 (1) */
    kTimingBccWord,   /* taken 10, not taken 12 */
    kTimingDBcc,      /* condition true 12, count expired 14 (3 reads), branch 10 */
    kTimingSccReg,    /* 6 when true, 4 when false */
    kTimingShiftReg,  /* 2 clocks per bit of the count in Dx, modulo 64 */
    kTimingMovemWord, /* 4 clocks per register in the mask */
    kTimingMovemLong, /* 8 clocks per register */
    kTimingBitReg,    /* 2 more for a bit number from Dx of 16 or more */
    kTimingBitImm,    /* 2 more for an immediate bit number of 16 or more */
    kTimingReadFirst  /* CLR, Scc and MOVE from SR to memory: the 68000 reads the operand
                         before writing it, so the write's bus cycles count twice */
};

extern uae_u16 gCpuTiming[65536];
extern uae_u32 gUaeAccessWait;
extern uae_u32 gUaeTimingExtra;
extern uae_u32 gUaeBusCount;

/* Wait clocks of a CPU access to SDRAM (EmBusTiming.h). */
uae_u32 EmSdramAccess(uae_u32 address, int write, int words);

/* Builds gCpuTiming from table68k; call after read_table68k and do_merges. */
void BuildCpuTiming(void);

/* Clocks for the data-dependent instructions, without the effective address time. */
uae_u32 TimingMulu(uae_u16 source);
uae_u32 TimingMuls(uae_u16 source);
uae_u32 TimingDivu(uae_u32 dividend, uae_u16 divisor);
uae_u32 TimingDivs(uae_s32 dividend, uae_s16 divisor);

#ifdef __cplusplus
}
#endif

#endif
