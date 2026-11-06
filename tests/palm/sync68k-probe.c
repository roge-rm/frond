/* For the frond-68k install check: each sysAppLaunchCmdSyncNotify adds a "sync" record to
   FrondSyncLog, making it if needed, and each normal launch a "launch" record before it draws its
   name and waits for events. Runs without globals on the notify. A notify that finds the
   FrondSyncSwitch database deletes it and switches to Memo Pad, which queues an appStopEvent for the
   app in front. */
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

static void switchIfAsked(void) {
    LocalID marker = DmFindDatabase(0, "FrondSyncSwitch"), memo;
    if (!marker) return;
    DmDeleteDatabase(0, marker);
    memo = DmFindDatabase(0, "Memo Pad");
    if (memo) SysUIAppSwitch(0, memo, sysAppLaunchCmdNormalLaunch, NULL);
}

UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType event;
    if (command == sysAppLaunchCmdSyncNotify) {
        logLine("sync");
        switchIfAsked();
    }
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
