/* 68k launcher for the Zodiac TwGfx test program: loads the 'ARMZ' #1 code with TwLoadModule
 * (OEMDispatch2 trap a443, selector 388) and flags TNA | QuickRun, so the OS calls its entry. */
typedef unsigned short UInt16;
typedef unsigned long UInt32;

static UInt16 twLoadModule(UInt32* entry) {
    register UInt16 result asm("d0");
    asm volatile(
        "move.l %1,-(%%sp)\n\t"
        "pea 3\n\t"
        "pea 1\n\t"
        "clr.l -(%%sp)\n\t"
        "clr.l -(%%sp)\n\t"
        "clr.l -(%%sp)\n\t"
        "move.w #388,%%d2\n\t"
        "trap #15\n\t"
        ".word 0xa443\n\t"
        "lea 24(%%sp),%%sp"
        : "=d"(result) : "a"(entry) : "d1", "d2", "a0", "a1", "memory");
    return result;
}

UInt32 PilotMain(UInt16 cmd, void* params, UInt16 flags) {
    UInt32 entry = 0;
    (void)params; (void)flags;
    if (cmd == 0) twLoadModule(&entry);
    return 0;
}
