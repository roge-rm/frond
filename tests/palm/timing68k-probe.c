#include <PalmOS.h>

/* 68000 timing loops for the speed check. Each loop runs a fixed count against the system tick,
 * and the screen shows its count, ticks and the clock it implies from the 68000 manual's clocks
 * per pass (no wait states). The results also sit in a RAM witness for a saved session. Builds
 * with the 3.5 SDK and calls only 3.1 APIs. */
#define LOOPS 5

static volatile struct {
    UInt32 magic[4];
    UInt32 version, done, ticksPerSecond;
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

/* Four move.l (a0),d1 (12 each) from RAM and the loop. */
static void loopRead(UInt32 n, volatile UInt32 *from) {
    asm volatile("1: move.l (%2),%%d1\n\tmove.l (%2),%%d1\n\tmove.l (%2),%%d1\n\tmove.l (%2),%%d1\n\t"
                 "subq.l #1,%0\n\tbne.s 1b" : "=d"(n) : "0"(n), "a"(from) : "d1");
}

/* bsr.s (18) to an rts (16) and the loop. */
static void loopCall(UInt32 n) {
    asm volatile("bra.s 2f\n3: rts\n2: bsr.s 3b\n\tsubq.l #1,%0\n\tbne.s 2b" : "=d"(n) : "0"(n));
}

/* mulu.w d1,d2 with d1 zero (38) and the loop. */
static void loopMultiply(UInt32 n) {
    asm volatile("moveq #0,%%d1\n1: mulu.w %%d1,%%d2\n\tsubq.l #1,%0\n\tbne.s 1b"
                 : "=d"(n) : "0"(n) : "d1", "d2");
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
        default: loopMultiply(n); break;
    }
    return TimGetTicks() - start;
}

UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    static const char *names[LOOPS] = {"branch", "nop", "read", "call", "mulu"};
    static const UInt32 clocks[LOOPS] = {18, 50, 66, 52, 56};
    static const UInt32 counts[LOOPS] = {1000000, 400000, 300000, 350000, 300000};
    Char line[64];
    EventType event;
    int i;
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    witness.magic[0] = 0x46723638; /* Fr68TIMEWITNESS1 */
    witness.magic[1] = 0x54494d45;
    witness.magic[2] = 0x5749544e;
    witness.magic[3] = 0x45535331;
    witness.version = 1;
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
        WinDrawChars(line, StrLen(line), 2, 18 + 12 * i);
    }
    witness.done = 1;
    StrPrintF(line, "%lu ticks per second", witness.ticksPerSecond);
    WinDrawChars(line, StrLen(line), 2, 18 + 12 * LOOPS);
    do {
        EvtGetEvent(&event, evtWaitForever);
        if (!SysHandleEvent(&event)) {
        }
    } while (event.eType != appStopEvent && event.eType != penDownEvent);
    return 0;
}
