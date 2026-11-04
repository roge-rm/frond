#include <PalmOS.h>

/* 68000 timing loops for the speed check. Each loop runs a fixed count against the system tick,
 * and the screen shows its count, ticks and the clock it implies from the 68000 manual's clocks
 * per pass (no wait states). The results also sit in a RAM witness for a saved session, with the
 * bus cycles per pass so a checker can add the wait states the chip selects set. Builds with
 * the 3.5 SDK and calls only 3.1 APIs. */
#define LOOPS 12

static volatile struct {
    UInt32 magic[4];
    UInt32 version, done, ticksPerSecond, loops;
    UInt32 count[LOOPS], ticks[LOOPS], clocks[LOOPS];
} witness;

/* subq.l #1,d0 (8) and a taken bne.s (10). */
static void loopBranch(UInt32 n) {
    asm volatile("1: subq.l #1,%0\n\tbne.s 1b" : "=d"(n) : "0"(n));
}

/* Eight nop (4 each) and the loop. */
static void loopNop(UInt32 n) {
    asm volatile("1: nop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\tnop\n\t"
                 "subq.l #1,%0\n\tbne.s 1b" : "=d"(n) : "0"(n));
}

/* Four move.l (a0),d1 (12 each) and the loop: from RAM, ROM or the registers. */
static void loopRead(UInt32 n, volatile void *from) {
    asm volatile("1: move.l (%2),%%d1\n\tmove.l (%2),%%d1\n\tmove.l (%2),%%d1\n\tmove.l (%2),%%d1\n\t"
                 "subq.l #1,%0\n\tbne.s 1b" : "=d"(n) : "0"(n), "a"(from) : "d1");
}

/* Four move.b (a0),d1 (8 each) and the loop. */
static void loopReadByte(UInt32 n, volatile void *from) {
    asm volatile("1: move.b (%2),%%d1\n\tmove.b (%2),%%d1\n\tmove.b (%2),%%d1\n\tmove.b (%2),%%d1\n\t"
                 "subq.l #1,%0\n\tbne.s 1b" : "=d"(n) : "0"(n), "a"(from) : "d1");
}

/* bsr.s (18) to an rts (16) and the loop. */
static void loopCall(UInt32 n) {
    asm volatile("bra.s 2f\n3: rts\n2: bsr.s 3b\n\tsubq.l #1,%0\n\tbne.s 2b" : "=d"(n) : "0"(n));
}

/* mulu.w d1,d2 (38 + 2 per one bit in d1) and the loop. */
static void loopMultiply(UInt32 n, UInt32 source) {
    asm volatile("move.l %2,%%d1\n1: mulu.w %%d1,%%d2\n\tsubq.l #1,%0\n\tbne.s 1b"
                 : "=d"(n) : "0"(n), "d"(source) : "d1", "d2");
}

/* move.l d3,d2 (4), divu.w d1,d2 (100000 / 7: 118 by the 68000's microcode) and the loop. */
static void loopDivide(UInt32 n) {
    asm volatile("move.l #100000,%%d3\n\tmoveq #7,%%d1\n"
                 "1: move.l %%d3,%%d2\n\tdivu.w %%d1,%%d2\n\tsubq.l #1,%0\n\tbne.s 1b"
                 : "=d"(n) : "0"(n) : "d1", "d2", "d3");
}

/* move.w #99,d1 (8), 99 taken dbra (10) and one expired (14), and the loop. */
static void loopDbra(UInt32 n) {
    asm volatile("1: move.w #99,%%d1\n2: dbra %%d1,2b\n\tsubq.l #1,%0\n\tbne.s 1b"
                 : "=d"(n) : "0"(n) : "d1");
}

/* lsl.l d1,d2 with d1 = 8 (8 + 2 per bit) and the loop. */
static void loopShift(UInt32 n) {
    asm volatile("moveq #8,%%d1\n1: lsl.l %%d1,%%d2\n\tsubq.l #1,%0\n\tbne.s 1b"
                 : "=d"(n) : "0"(n) : "d1", "d2");
}

/* movem.l of six registers to the stack (8 + 8 each) and back (12 + 8 each), and the loop. */
static void loopMovem(UInt32 n) {
    asm volatile("1: movem.l %%d1-%%d4/%%a1-%%a2,-(%%sp)\n\tmovem.l (%%sp)+,%%d1-%%d4/%%a1-%%a2\n\t"
                 "subq.l #1,%0\n\tbne.s 1b" : "=d"(n) : "0"(n) : "d1", "d2", "d3", "d4", "a1", "a2");
}

static UInt32 timed(int which, UInt32 n) {
    static UInt32 cell;
    UInt32 start = TimGetTicks();
    while (TimGetTicks() == start) {
    }
    start = TimGetTicks();
    switch (which) {
        case 0: loopBranch(n); break;
        case 1: loopNop(n); break;
        case 2: loopRead(n, &cell); break;
        case 3: loopCall(n); break;
        case 4: loopMultiply(n, 0); break;
        case 5: loopMultiply(n, 0xffff); break;
        case 6: loopDivide(n); break;
        case 7: loopDbra(n); break;
        case 8: loopShift(n); break;
        case 9: loopMovem(n); break;
        /* The first long of the ROM's card header, and the system control register. */
        case 10: loopRead(n, (volatile void *)0x10c00000); break;
        default: loopReadByte(n, (volatile void *)0xfffff000); break;
    }
    return TimGetTicks() - start;
}

UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    static const char *names[LOOPS] = {"branch", "nop", "read", "call", "mulu0", "mulu1",
                                       "divu", "dbra", "lsl", "movem", "rom", "reg"};
    static const UInt32 clocks[LOOPS] = {18, 50, 66, 52, 56, 88, 140, 1030, 42, 134, 66, 50};
    static const UInt32 counts[LOOPS] = {900000, 330000, 250000, 320000, 300000, 190000,
                                         118000, 16000, 400000, 124000, 250000, 330000};
    Char line[64];
    EventType event;
    int i;
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    witness.magic[0] = 0x46723638; /* Fr68TIMEWITNESS1 */
    witness.magic[1] = 0x54494d45;
    witness.magic[2] = 0x5749544e;
    witness.magic[3] = 0x45535331;
    witness.version = 2;
    witness.loops = LOOPS;
    witness.ticksPerSecond = SysTicksPerSecond();
    WinEraseWindow();
    WinDrawChars("Frond 68k timing", 16, 2, 2);
    for (i = 0; i < LOOPS; i++) {
        UInt32 ticks = timed(i, counts[i]), perTick, khz;
        witness.count[i] = counts[i];
        witness.ticks[i] = ticks;
        witness.clocks[i] = clocks[i];
        perTick = ticks ? counts[i] * clocks[i] / ticks : 0;
        khz = perTick * witness.ticksPerSecond / 1000;
        StrPrintF(line, "%s %lu/%lu t %lu.%03lu MHz", names[i], counts[i], ticks, khz / 1000, khz % 1000);
        WinDrawChars(line, StrLen(line), 2, 14 + 10 * i);
    }
    witness.done = 1;
    StrPrintF(line, "%lu ticks per second", witness.ticksPerSecond);
    WinDrawChars(line, StrLen(line), 2, 14 + 10 * LOOPS);
    do {
        EvtGetEvent(&event, evtWaitForever);
        if (!SysHandleEvent(&event)) {
        }
    } while (event.eType != appStopEvent && event.eType != penDownEvent);
    return 0;
}
