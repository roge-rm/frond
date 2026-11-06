/* For the frond-68k install check: each sysAppLaunchCmdSyncNotify adds a "sync" record to
   FrondSyncLog, making it if needed, and each normal launch a "launch" record before it draws its
   name and waits for events. Runs without globals on the notify. */
#include <PalmOS.h>

static void logLine(const char *text) {
    UInt16 at = dmMaxRecordIndex;
    MemHandle record;
    DmOpenRef log;
    if (!DmFindDatabase(0, "FrondSyncLog") && DmCreateDatabase(0, "FrondSyncLog", 'FrSy', 'DATA', false)) return;
    log = DmOpenDatabaseByTypeCreator('DATA', 'FrSy', dmModeReadWrite);
    if (!log) return;
    record = DmNewRecord(log, &at, StrLen(text) + 1);
    if (record) {
        DmWrite(MemHandleLock(record), 0, text, StrLen(text) + 1);
        MemHandleUnlock(record);
        DmReleaseRecord(log, at, true);
    }
    DmCloseDatabase(log);
}

UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType event;
    if (command == sysAppLaunchCmdSyncNotify) logLine("sync");
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    logLine("launch");
    WinEraseWindow();
    WinDrawChars("Frond sync", 10, 2, 2);
    do {
        EvtGetEvent(&event, evtWaitForever);
        SysHandleEvent(&event);
    } while (event.eType != appStopEvent);
    return 0;
}
