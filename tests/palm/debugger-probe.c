#include <PalmOS.h>

/* A RAM descriptor, never a file offset masquerading as a guest address.
 * The host validates the locked code resource against the supplied PRC. */
extern void debuggerSemantics(volatile UInt32 *witness);
extern void debuggerException(void);
static volatile struct {
    Char magic[16];
    UInt32 version, code, codeSize, entry, exception, calls, stores;
    UInt32 buffer;
} witness;

UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType event;
    MemHandle code;
    UInt32 start;
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    code = DmGetResource('code', 1);
    if (!code) return 1;
    MemMove((void *)witness.magic, "FrDBSTEP-RAM-v01", 16);
    witness.version = 1;
    witness.code = (UInt32)MemHandleLock(code);
    witness.codeSize = MemHandleSize(code);
    witness.entry = (UInt32)debuggerSemantics;
    witness.exception = (UInt32)debuggerException;
    witness.calls = witness.stores = 0;
    start = TimGetTicks();
    for (;;) {
        debuggerSemantics((volatile UInt32 *)&witness);
        /* A normal timed app path reaches the deliberate exception. No host
         * register/memory edits, replay, key, or pen input selects this path.
         * 30 guest seconds leave time for the private preparation checkpoint. */
        if (TimGetTicks() - start >= 30 * SysTicksPerSecond()) debuggerException();
        EvtGetEvent(&event, 1);
        if (event.eType == appStopEvent) break;
    }
    MemHandleUnlock(code);
    DmReleaseResource(code);
    return 0;
}
