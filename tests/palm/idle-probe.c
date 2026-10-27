#include <PalmOS.h>

/* No forms, drawing, timers, or database writes in the wait loop. The host
 * inspects this big-endian RAM witness in saved sessions. Keeping the fields
 * volatile also makes every EvtGetEvent return (including nil) observable.
 * A closed log records readiness before the first call, and counters only
 * after appStopEvent. A launch/app switch is controlled retrieval, not proof
 * that an earlier wait was reached. */
static volatile struct {
    UInt32 magic[4];
    UInt32 version, phase, entered, returned, nil, other, api;
} witness;

static void recordEvent(const Char *message) {
    LocalID id = DmFindDatabase(0, "FrondIdleLog");
    DmOpenRef db;
    UInt16 index = dmMaxRecordIndex;
    MemHandle record;
    if (!id) {
        if (DmCreateDatabase(0, "FrondIdleLog", 'FrId', 'DATA', false)) return;
        id = DmFindDatabase(0, "FrondIdleLog");
    }
    db = DmOpenDatabase(0, id, dmModeReadWrite);
    if (!db) return;
    record = DmNewRecord(db, &index, StrLen(message) + 1);
    if (record) {
        void *bytes = MemHandleLock(record);
        DmWrite(bytes, 0, message, StrLen(message) + 1);
        MemHandleUnlock(record);
        DmReleaseRecord(db, index, true);
    }
    DmCloseDatabase(db);
}

UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType event;
    Char message[100];
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    witness.magic[0] = 0x46724932; /* FrI2WAITRAM!v001 */
    witness.magic[1] = 0x57414954;
    witness.magic[2] = 0x52414d21;
    witness.magic[3] = 0x76303031;
    witness.version = 1;
    witness.phase = witness.entered = witness.returned = 0;
    witness.nil = witness.other = 0;
    witness.api = 1;
    recordEvent("idle-v2 event-loop-ready api=EvtGetEvent");
    for (;;) {
        witness.entered++;
        witness.phase = 1;
        EvtGetEvent(&event, evtWaitForever);
        witness.phase = 2;
        witness.returned++;
        if (event.eType == nilEvent) witness.nil++;
        else witness.other++;
        if (event.eType == appStopEvent) break;
        /* No UI/system handlers: this probe measures only event retrieval. */
    }
    witness.phase = 3;
    StrPrintF(message, "idle-v2 stopped entered=%lu returned=%lu nil=%lu other=%lu",
              witness.entered, witness.returned, witness.nil, witness.other);
    recordEvent(message);
    return 0;
}
