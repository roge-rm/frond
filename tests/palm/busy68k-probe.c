/* For the frond-68k busy check: on launch the probe makes FrondBusyLog and then runs 45 seconds
   without calling the event loop, drawing a line and adding a log record each pass, keeping the
   newest 64, as a game with a debug log does. Then it logs "done", draws "idle" and waits for
   events. */
#include <PalmOS.h>

#define BUSY_SECONDS 45
#define LOG_RECORDS 64

/* Opens the log, drops its oldest records over LOG_RECORDS, adds the line and closes it again. */
static void logLine(const char *text) {
    UInt16 at = dmMaxRecordIndex;
    UInt16 length = StrLen(text) + 1;
    MemHandle record;
    DmOpenRef log = DmOpenDatabaseByTypeCreator('DATA', 'FrBy', dmModeReadWrite);
    if (!log) return;
    while (DmNumRecords(log) >= LOG_RECORDS) DmRemoveRecord(log, 0);
    record = DmNewRecord(log, &at, length);
    if (record) {
        DmWrite(MemHandleLock(record), 0, text, length);
        MemHandleUnlock(record);
        DmReleaseRecord(log, at, true);
    }
    DmCloseDatabase(log);
}

UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType event;
    LocalID id;
    UInt32 end, lines = 0;
    char text[32];
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    WinEraseWindow();
    WinDrawChars("Frond 68k busy", 14, 2, 2);
    id = DmFindDatabase(0, "FrondBusyLog");
    if (id) DmDeleteDatabase(0, id);
    if (DmCreateDatabase(0, "FrondBusyLog", 'FrBy', 'DATA', false)) return 0;
    logLine("start");
    WinDrawChars("busy", 4, 2, 14);
    end = TimGetTicks() + BUSY_SECONDS * SysTicksPerSecond();
    while (TimGetTicks() < end) {
        WinDrawLine(lines % 160, 40, 159 - lines % 160, 100);
        StrPrintF(text, "lines %lu", ++lines);
        logLine(text);
    }
    logLine("done");
    WinEraseWindow();
    WinDrawChars("Frond 68k busy", 14, 2, 2);
    WinDrawChars("idle", 4, 2, 14);
    do {
        EvtGetEvent(&event, evtWaitForever);
        SysHandleEvent(&event);
    } while (event.eType != appStopEvent);
    return 0;
}
