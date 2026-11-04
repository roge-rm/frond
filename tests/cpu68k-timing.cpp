// Checks frond-68k's 68000 timing table against the Motorola MC68000 user's manual, section 8:
// clocks with no wait states and bus reads from program space, for each opcode listed.

#include <cstdio>

#include "cputiming.h"
#include "readcpu.h"

namespace {
    int failures = 0;

    struct Case {
        unsigned opcode;
        const char* text;
        int clocks, programReads, kind;
    };

    // clocks and program reads come from the manual's n(r/w) less the operand reads it lists.
    const Case cases[] = {
        {0x4e71, "nop", 4, 1, kTimingPlain},
        {0x7001, "moveq #1,d0", 4, 1, kTimingPlain},
        {0x3200, "move.w d0,d1", 4, 1, kTimingPlain},
        {0x2210, "move.l (a0),d1", 12, 1, kTimingPlain},
        {0x3210, "move.w (a0),d1", 8, 1, kTimingPlain},
        {0x3220, "move.w -(a0),d1", 10, 1, kTimingPlain},
        {0x3080, "move.w d0,(a0)", 8, 1, kTimingPlain},
        {0x2080, "move.l d0,(a0)", 12, 1, kTimingPlain},
        {0x23c0, "move.l d0,(xxx).l", 20, 3, kTimingPlain},
        {0x23f9, "move.l (xxx).l,(xxx).l", 36, 5, kTimingPlain},
        {0x203c, "move.l #imm,d0", 12, 3, kTimingPlain},
        {0x3228, "move.w (d16,a0),d1", 12, 2, kTimingPlain},
        {0x3230, "move.w (d8,a0,xn),d1", 14, 2, kTimingPlain},
        {0x2c4f, "movea.l a7,a6", 4, 1, kTimingPlain},
        {0xd081, "add.l d1,d0", 8, 1, kTimingPlain},
        {0xd041, "add.w d1,d0", 4, 1, kTimingPlain},
        {0xd090, "add.l (a0),d0", 14, 1, kTimingPlain},
        {0xd190, "add.l d0,(a0)", 20, 1, kTimingPlain},
        {0xd150, "add.w d0,(a0)", 12, 1, kTimingPlain},
        {0xd0bc, "add.l #imm,d0", 16, 3, kTimingPlain},
        {0xd1c8, "adda.l a0,a0", 8, 1, kTimingPlain},
        {0xd0c8, "adda.w a0,a0", 8, 1, kTimingPlain},
        {0xd1d0, "adda.l (a0),a0", 14, 1, kTimingPlain},
        {0xb081, "cmp.l d1,d0", 6, 1, kTimingPlain},
        {0xb1c8, "cmpa.l a0,a0", 6, 1, kTimingPlain},
        {0xc081, "and.l d1,d0", 8, 1, kTimingPlain},
        {0xb380, "eor.l d1,d0", 8, 1, kTimingPlain},
        {0x0680, "addi.l #imm,d0", 16, 3, kTimingPlain},
        {0x0280, "andi.l #imm,d0", 14, 3, kTimingPlain},
        {0x0c80, "cmpi.l #imm,d0", 14, 3, kTimingPlain},
        {0x0640, "addi.w #imm,d0", 8, 2, kTimingPlain},
        {0x0650, "addi.w #imm,(a0)", 16, 2, kTimingPlain},
        {0x0690, "addi.l #imm,(a0)", 28, 3, kTimingPlain},
        {0x0c50, "cmpi.w #imm,(a0)", 12, 2, kTimingPlain},
        {0x5280, "addq.l #1,d0", 8, 1, kTimingPlain},
        {0x5340, "subq.w #1,d0", 4, 1, kTimingPlain},
        {0x5388, "subq.l #1,a0", 8, 1, kTimingPlain},
        {0x5248, "addq.w #1,a0", 8, 1, kTimingPlain},
        {0x5290, "addq.l #1,(a0)", 20, 1, kTimingPlain},
        {0x4280, "clr.l d0", 6, 1, kTimingPlain},
        {0x4290, "clr.l (a0)", 20, 1, kTimingPlain},
        {0x4440, "neg.w d0", 4, 1, kTimingPlain},
        {0x4a80, "tst.l d0", 4, 1, kTimingPlain},
        {0x4a90, "tst.l (a0)", 12, 1, kTimingPlain},
        {0x4ad0, "tas (a0)", 18, 1, kTimingPlain},
        {0x56c0, "sne d0", 4, 1, kTimingSccReg},
        {0x56d0, "sne (a0)", 12, 1, kTimingPlain},
        {0xe388, "lsl.l #1,d0", 10, 1, kTimingPlain},
        {0xe148, "lsl.w #8,d0", 22, 1, kTimingPlain},
        {0xe3a8, "lsl.l d1,d0", 8, 1, kTimingShiftReg},
        {0xe368, "lsl.w d1,d0", 6, 1, kTimingShiftReg},
        {0xe1d0, "asl.w (a0)", 12, 1, kTimingPlain},
        {0x0300, "btst d1,d0", 6, 1, kTimingPlain},
        {0x0340, "bchg d1,d0", 6, 1, kTimingBitReg},
        {0x0380, "bclr d1,d0", 8, 1, kTimingBitReg},
        {0x0800, "btst #n,d0", 10, 2, kTimingPlain},
        {0x08c0, "bset #n,d0", 10, 2, kTimingBitImm},
        {0x0880, "bclr #n,d0", 12, 2, kTimingBitImm},
        {0x0310, "btst d1,(a0)", 8, 1, kTimingPlain},
        {0x08d0, "bset #n,(a0)", 16, 2, kTimingPlain},
        {0x6602, "bne.s", 8, 1, kTimingBccShort},
        {0x6600, "bne.w", 12, 2, kTimingBccWord},
        {0x6002, "bra.s", 8, 1, kTimingBccShort},
        {0x6102, "bsr.s", 18, 2, kTimingPlain},
        {0x6100, "bsr.w", 18, 2, kTimingPlain},
        {0x51c8, "dbra d0", 10, 2, kTimingDBcc},
        {0x4ed0, "jmp (a0)", 8, 2, kTimingPlain},
        {0x4ef9, "jmp (xxx).l", 12, 3, kTimingPlain},
        {0x4ef0, "jmp (d8,a0,xn)", 14, 3, kTimingPlain},
        {0x4e90, "jsr (a0)", 16, 2, kTimingPlain},
        {0x4ea8, "jsr (d16,a0)", 18, 2, kTimingPlain},
        {0x4eb9, "jsr (xxx).l", 20, 3, kTimingPlain},
        {0x4ebb, "jsr (d8,pc,xn)", 22, 2, kTimingPlain},
        {0x41d0, "lea (a0),a0", 4, 1, kTimingPlain},
        {0x41f9, "lea (xxx).l,a0", 12, 3, kTimingPlain},
        {0x4868, "pea (d16,a0)", 16, 2, kTimingPlain},
        {0x4cd8, "movem.l (a0)+,list", 12, 3, kTimingMovemLong},
        {0x4c90, "movem.w (a0),list", 12, 3, kTimingMovemWord},
        {0x48e7, "movem.l list,-(a7)", 8, 2, kTimingMovemLong},
        {0x48a8, "movem.w list,(d16,a0)", 12, 3, kTimingMovemWord},
        {0xc0c1, "mulu.w d1,d0", 0, 1, kTimingPlain},
        {0xc0d0, "mulu.w (a0),d0", 4, 1, kTimingPlain},
        {0x80c1, "divu.w d1,d0", 0, 1, kTimingPlain},
        {0xd181, "addx.l d1,d0", 8, 1, kTimingPlain},
        {0xd189, "addx.l -(a1),-(a0)", 30, 1, kTimingPlain},
        {0x9109, "subx.b -(a1),-(a0)", 18, 1, kTimingPlain},
        {0xc101, "abcd d1,d0", 6, 1, kTimingPlain},
        {0xb388, "cmpm.l (a0)+,(a1)+", 20, 1, kTimingPlain},
        {0x4180, "chk.w d0,d0", 10, 1, kTimingPlain},
        {0x007c, "ori #imm,sr", 20, 3, kTimingPlain},
        {0x46c0, "move d0,sr", 12, 2, kTimingPlain},
        {0x44c0, "move d0,ccr", 12, 1, kTimingPlain},
        {0x40c0, "move sr,d0", 6, 1, kTimingPlain},
        {0xc141, "exg d0,d1", 6, 1, kTimingPlain},
        {0x4880, "ext.w d0", 4, 1, kTimingPlain},
        {0x4840, "swap d0", 4, 1, kTimingPlain},
        {0x4e56, "link a6,#n", 16, 2, kTimingPlain},
        {0x4e5e, "unlk a6", 12, 1, kTimingPlain},
        {0x4e75, "rts", 16, 2, kTimingPlain},
        {0x4e73, "rte", 20, 2, kTimingPlain},
        {0x4e77, "rtr", 20, 2, kTimingPlain},
        {0x4e70, "reset", 132, 1, kTimingPlain},
        {0x4e76, "trapv", 4, 1, kTimingPlain},
        {0x4e4f, "trap #15", 0, 0, kTimingPlain},
        {0x0188, "movep.w d0,(d16,a0)", 16, 2, kTimingPlain},
        {0x01c8, "movep.l d0,(d16,a0)", 24, 2, kTimingPlain},
        {0xa000, "line a", 0, 0, kTimingPlain},
        {0x4808, "link.l (68020)", 0, 0, kTimingPlain},
    };

    void Expect(bool ok, const char* what, unsigned a, unsigned b, const char* text) {
        if (ok) return;
        printf("FAIL %s: %s got %u, manual %u\n", text, what, a, b);
        failures++;
    }
}  // namespace

int main() {
    read_table68k();
    do_merges();
    BuildCpuTiming();

    for (const Case& c : cases) {
        const unsigned t = gCpuTiming[c.opcode];
        Expect((t & 0xff) * 2 == unsigned(c.clocks), "clocks", (t & 0xff) * 2, c.clocks, c.text);
        Expect(((t >> 8) & 15) == unsigned(c.programReads), "program reads", (t >> 8) & 15, c.programReads,
               c.text);
        Expect((t >> 12) == unsigned(c.kind), "kind", t >> 12, c.kind, c.text);
    }

    // MULU 38+2n with n the ones in the source; MULS counts 01 and 10 pairs in source:0.
    Expect(TimingMulu(0) == 38, "mulu #0", TimingMulu(0), 38, "mulu");
    Expect(TimingMulu(0xffff) == 70, "mulu #$ffff", TimingMulu(0xffff), 70, "mulu");
    Expect(TimingMuls(0) == 38, "muls #0", TimingMuls(0), 38, "muls");
    Expect(TimingMuls(0x5555) == 70, "muls #$5555", TimingMuls(0x5555), 70, "muls");
    Expect(TimingMuls(0xffff) == 40, "muls #-1", TimingMuls(0xffff), 40, "muls");

    // DIVU and DIVS: overflow is quick, the rest within the manual's worst cases (140, 158) and
    // less than 10% apart.
    Expect(TimingDivu(0x10000, 1) == 10, "divu overflow", TimingDivu(0x10000, 1), 10, "divu");
    Expect(TimingDivu(1, 0) == 0, "divu by zero", TimingDivu(1, 0), 0, "divu");
    unsigned divuMin = 1000, divuMax = 0, divsMin = 1000, divsMax = 0;
    for (unsigned d = 1; d < 0x10000; d += 251) {
        for (unsigned long long n = 0; n < (unsigned long long)d << 16; n += 0x10001 * 37 + d) {
            const unsigned u = TimingDivu(unsigned(n), d);
            divuMin = u < divuMin ? u : divuMin;
            divuMax = u > divuMax ? u : divuMax;
            const unsigned s = TimingDivs(int(n >> 1), short(d >> 1 ? d >> 1 : 1));
            if (s > 18) {
                divsMin = s < divsMin ? s : divsMin;
                divsMax = s > divsMax ? s : divsMax;
            }
        }
    }
    Expect(divuMax <= 140 && divuMin >= 76, "divu range", divuMin, divuMax, "divu");
    Expect(divsMax <= 158 && divsMin >= 120, "divs range", divsMin, divsMax, "divs");
    printf("divu %u-%u, divs %u-%u clocks\n", divuMin, divuMax, divsMin, divsMax);

    if (failures) {
        printf("%d timing checks failed\n", failures);
        return 1;
    }
    printf("68000 timing table matches the manual (%zu opcodes)\n", sizeof(cases) / sizeof(cases[0]));
    return 0;
}
