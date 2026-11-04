// Checks frond-68k's 68000 timing table against the Motorola MC68000 user's manual, section 8:
// clocks with no wait states and bus reads from program space, for each opcode listed. Then the
// bus time LCD DMA and DRAM refresh take, against the MC68EZ328 and MC68VZ328 manuals.

#include <cstdio>

#include "EmBusTiming.h"
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
        {0x4290, "clr.l (a0)", 20, 1, kTimingReadFirst},
        {0x4250, "clr.w (a0)", 12, 1, kTimingReadFirst},
        {0x4239, "clr.b (xxx).l", 20, 3, kTimingReadFirst},
        {0x4440, "neg.w d0", 4, 1, kTimingPlain},
        {0x4a80, "tst.l d0", 4, 1, kTimingPlain},
        {0x4a90, "tst.l (a0)", 12, 1, kTimingPlain},
        {0x4ad0, "tas (a0)", 18, 1, kTimingPlain},
        {0x56c0, "sne d0", 4, 1, kTimingSccReg},
        {0x56d0, "sne (a0)", 12, 1, kTimingReadFirst},
        {0x50e8, "st (d16,a0)", 16, 2, kTimingReadFirst},
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
        {0x40d0, "move sr,(a0)", 12, 1, kTimingReadFirst},
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

    // The EZ manual's bandwidth example (12.1.4.1): 320x240 at 2 bits a pixel, 2 clocks a word,
    // 60 Hz at 16.58 MHz, takes 6.92% of the bus. Here as fast page DRAM with BC 00 (4, 2, 2...)
    // the line's 40 words go in bursts of 3 words (8 clocks), and LRRA sets a 60 Hz frame.
    void CheckBusTiming() {
        EmEzBusRegisters ez{};
        ez.pllControl = 0x2420;  // LCDCLK and SYSCLK both DMACLK / 1 (reset value)
        ez.lcdScreenWidth = 320;
        ez.lcdScreenHeight = 239;
        ez.lcdPanelControl = 0x01;   // four greys
        ez.lcdPixelClock = 2;        // three system clocks a pixel
        ez.lcdClockControl = 0x80;   // on, 16-bit
        ez.lcdRefreshRateAdj = 58;   // (6 + 58 + 320) x 3 = 1152 clocks a line, 60 Hz at 240 lines
        ez.lcdInDram = true;
        ez.dramEnabled = true;
        ez.dramControl = 0x8800 | 0x0007;  // on, refresh from the system clock, REF in DRAMMC
        ez.dramConfig = 7;                 // 32 x 8 = 256 clocks, the manual's 15.44 us
        ez.systemClockHz = 16580608;
        EmBusSteal steal = EmEzBusSteal(ez);
        Expect(steal.lcdLineClocks == 1152, "ez lcd line clocks", steal.lcdLineClocks, 1152, "lcd");
        // 40 words: 14 bursts (4 + 2 + 2), the last of one word: 13 x 8 + 4 = 108.
        Expect(steal.lcdDmaClocks == 108, "ez lcd dma clocks", steal.lcdDmaClocks, 108, "lcd");
        Expect(steal.refreshPeriod == 256, "ez refresh period", steal.refreshPeriod, 256, "refresh");
        Expect(steal.refreshClocks == 4, "ez refresh hold", steal.refreshClocks, 4, "refresh");

        // EDO: 4, 1, 1, 1, 1 fills the eight clocks with five words, 8 bursts of 8 = 64 clocks.
        ez.dramControl |= 0x0400;
        steal = EmEzBusSteal(ez);
        Expect(steal.lcdDmaClocks == 64, "ez edo dma clocks", steal.lcdDmaClocks, 64, "lcd");
        // The manual's 2 clocks a word, from SRAM with one wait state: 80 clocks, 6.94%.
        ez.lcdInDram = false;
        ez.lcdClockControl = 0x81;
        steal = EmEzBusSteal(ez);
        Expect(steal.lcdDmaClocks == 80, "ez sram dma clocks", steal.lcdDmaClocks, 80, "lcd");
        const double share = 100.0 * steal.lcdDmaClocks / steal.lcdLineClocks;
        Expect(share > 6.9 && share < 7.0, "ez dma share x100", unsigned(share * 100), 692, "lcd");

        // 32 kHz refresh: REF 0 is twice 32 kHz, REF 3 a quarter.
        ez.dramControl = 0x8000;
        ez.dramConfig = 0;
        const unsigned fast = EmEzBusSteal(ez).refreshPeriod;
        Expect(fast == 253, "ez refresh 64 kHz", fast, 253, "refresh");
        ez.dramConfig = 3;
        const unsigned slow = EmEzBusSteal(ez).refreshPeriod;
        Expect(slow == 2024, "ez refresh 8 kHz", slow, 2024, "refresh");
        // Self refresh, the DRAM controller or LCD off: nothing taken.
        ez.dramControl = 0xc000;
        ez.lcdClockControl = 0x01;
        steal = EmEzBusSteal(ez);
        Expect(!steal.refreshPeriod && !steal.lcdLineClocks, "ez off", steal.refreshPeriod, 0, "off");

        // VZ: (12 + XMAX + RRA) a line, DMACR's burst length, SDRAM refresh for RACL clocks.
        EmVzBusRegisters vz{};
        vz.pllControl = 0x2420;
        vz.lcdScreenWidth = 160;
        vz.lcdScreenHeight = 159;
        vz.lcdPanelControl = 0x02;  // sixteen greys: 4 bits, 40 words a line
        vz.lcdPixelClock = 0;
        vz.lcdClockControl = 0x80;
        vz.lcdRefreshRateAdj = 28;  // 200 clocks a line
        vz.lcdDmaControl = 0x62;    // reset value: bursts of 6 words
        vz.lcdInDram = true;
        vz.dramEnabled = true;
        vz.sdram = true;
        vz.sdramControl = 0xd03c;  // on, continuous pages, refresh, CAS latency 1, RACL 3
        vz.dramControl = 0x8800;
        vz.dramConfig = 15;  // 512 clocks
        vz.systemClockHz = 33161216;
        steal = EmVzBusSteal(vz);
        Expect(steal.lcdLineClocks == 200, "vz lcd line clocks", steal.lcdLineClocks, 200, "lcd");
        // 40 words in 7 bursts of up to 6: 7 x 4 + 33 x 1 = 61.
        Expect(steal.lcdDmaClocks == 61, "vz lcd dma clocks", steal.lcdDmaClocks, 61, "lcd");
        Expect(steal.refreshPeriod == 512 && steal.refreshClocks == 3, "vz sdram refresh", steal.refreshPeriod,
               512, "refresh");

        // SDRAM pages: a read miss waits 3, a hit 1; writes 1 and 0; a refresh closes the pages.
        // Each access changes the page state, so each is made once.
        EmSdram::Set(EmVzSdramTiming(0xd03c, 0x0100));
        const struct {
            unsigned address;
            bool write, precharge;
            unsigned words, wait;
            const char* text;
        } sdram[] = {
            {0x1000, false, false, 1, 3, "sdram read miss"},     {0x1002, false, false, 2, 2, "sdram long read hit"},
            {0x1004, true, false, 1, 0, "sdram write hit"},      {0x9000, true, false, 1, 1, "sdram write miss"},
            {0x9000, false, true, 1, 3, "sdram after refresh"},
        };
        for (const auto& a : sdram) {
            if (a.precharge) EmSdram::PrechargeAll();
            const unsigned wait = EmSdram::Access(a.address, a.write, a.words);
            Expect(wait == a.wait, "wait clocks", wait, a.wait, a.text);
        }

        // Charge: a 10-clock DMA due inside an instruction that spent all its time on the bus
        // costs 10; one that spent half of it costs 5; events before the instruction cost nothing.
        steal = EmBusSteal{};
        steal.lcdLineClocks = 1000;
        steal.lcdDmaClocks = 10;
        EmBusTiming::Set(steal, 0);
        Expect(EmBusTiming::next == 1000, "next event", unsigned(EmBusTiming::next), 1000, "charge");
        const unsigned full = EmBusTiming::Charge(990, 1002, 12, 12, 0);
        Expect(full == 10, "full bus share", full, 10, "charge");
        const unsigned half = EmBusTiming::Charge(1995, 2015, 20, 10, 0);
        Expect(half == 5, "half bus share", half, 5, "charge");
        const unsigned stopped = EmBusTiming::Charge(9500, 9510, 10, 10, 0);
        Expect(stopped == 0, "after a stop", stopped, 0, "charge");
        Expect(EmBusTiming::next == 10000, "caught up", unsigned(EmBusTiming::next), 10000, "charge");
        EmBusTiming::Set(EmBusSteal{}, 0);
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

    CheckBusTiming();

    if (failures) {
        printf("%d timing checks failed\n", failures);
        return 1;
    }
    printf("68000 timing table matches the manual (%zu opcodes)\n", sizeof(cases) / sizeof(cases[0]));
    return 0;
}
