#include <PalmOS.h>
#include <PceNativeCall.h>

#define CREATOR 'FrCp'
static DmOpenRef logDB;
static UInt16 failures;
static Boolean queued;
static Char token[33], previous[33];
static void record(const Char* text) {
    UInt16 index = dmMaxRecordIndex;
    MemHandle h = DmNewRecord(logDB, &index, StrLen(text) + 1);
    if (!h) { ++failures; return; }
    if (DmWrite(MemHandleLock(h), 0, text, StrLen(text) + 1)) ++failures;
    MemHandleUnlock(h); DmReleaseRecord(logDB, index, true);
}
static void result(const Char* label, Boolean pass, UInt32 value) {
    Char text[100];
    if (!pass) ++failures;
    StrPrintF(text, "%s err=%04x value=%lu", label, pass ? 0 : 1, value);
    record(text);
}
static Boolean config(void) {
    DmOpenRef db = DmOpenDatabaseByTypeCreator('DATA', CREATOR, dmModeReadOnly);
    MemHandle h;
    const UInt8* data;
    if (!db) return false;
    h = DmQueryRecord(db, 0);
    if (!h || MemHandleSize(h) != 66) { DmCloseDatabase(db); return false; }
    data = MemHandleLock(h);
    MemMove(token, data, 33); MemMove(previous, data + 33, 33);
    MemHandleUnlock(h); DmCloseDatabase(db);
    return token[32] == 0 && previous[32] == 0 && StrLen(token) == 32;
}
static void memoryProbe(void) {
    MemHandle h = MemHandleNew(128);
    Boolean pass = false;
    if (h) {
        UInt8* p = MemHandleLock(h);
        UInt16 i;
        for (i = 0; i < 64; ++i) p[i] = i;
        MemMove(p + 1, p, 63);
        pass = p[0] == 0;
        for (i = 1; i < 64; ++i) if (p[i] != i - 1) pass = false;
        MemHandleUnlock(h);
        pass = pass && MemHandleResize(h, 256) == 0;
        if (pass) {
            p = MemHandleLock(h);
            pass = p[63] == 62 && MemHandleSize(h) == 256;
            MemHandleUnlock(h);
        }
        MemHandleFree(h);
    }
    result("memory", pass, 256);
}
static void databaseProbe(void) {
    const Char* name = "FrondCompatData";
    LocalID id = DmFindDatabase(0, name);
    DmOpenRef db;
    MemHandle h;
    UInt16 index = 0;
    Boolean pass = true;
    if (!*previous && id) { DmDeleteDatabase(0, id); id = 0; }
    if (!id) {
        if (*previous) pass = false;
        if (DmCreateDatabase(0, name, CREATOR, 'TEST', false)) pass = false;
        id = DmFindDatabase(0, name);
    }
    db = DmOpenDatabase(0, id, dmModeReadWrite);
    if (!db) { result("database", false, 0); return; }
    if (*previous) {
        h = DmQueryRecord(db, 0);
        if (!h || MemHandleSize(h) != 33) pass = false;
        else { if (MemCmp(MemHandleLock(h), previous, 33)) pass = false; MemHandleUnlock(h); }
    }
    h = DmNumRecords(db) ? DmGetRecord(db, 0) : DmNewRecord(db, &index, 33);
    if (h && MemHandleSize(h) != 33) {
        DmReleaseRecord(db, 0, false); h = 0;
    }
    if (!h) pass = false;
    else {
        if (DmWrite(MemHandleLock(h), 0, token, 33)) pass = false;
        MemHandleUnlock(h); if (DmReleaseRecord(db, 0, true)) pass = false;
    }
    if (DmCloseDatabase(db)) pass = false;
    db = DmOpenDatabase(0, id, dmModeReadOnly);
    h = db ? DmQueryRecord(db, 0) : 0;
    if (!h || MemHandleSize(h) != 33) pass = false;
    else { if (MemCmp(MemHandleLock(h), token, 33)) pass = false; MemHandleUnlock(h); }
    if (db && DmCloseDatabase(db)) pass = false;
    result("database", pass, *previous ? 2 : 1);
}
static void preferenceProbe(void) {
    Char readback[33];
    UInt16 size = sizeof(readback);
    Int16 version = PrefGetAppPreferences(CREATOR, 0, readback, &size, true);
    Boolean pass = !*previous || (version == 1 && size == 33 && !MemCmp(readback, previous, 33));
    PrefSetAppPreferences(CREATOR, 0, 1, token, 33, true);
    size = sizeof(readback);
    version = PrefGetAppPreferences(CREATOR, 0, readback, &size, true);
    pass = pass && version == 1 && size == 33 && !MemCmp(readback, token, 33);
    result("preferences", pass, *previous ? 2 : 1);
}
static UInt32 callback(UInt32 left, UInt16 right) { return (left ^ 0x13579bdfUL) + right; }
static void nativeProbe(UInt16 resource, const Char* label) {
    UInt32 data[7], expected = 0x12345678UL, value = 0;
    const Char* text = "Frond native ABI";
    MemHandle code = DmGetResource('armc', resource);
    UInt16 i;
    for (i = 0; i < 37; ++i) expected = (expected * 1664525UL + 1013904223UL) ^ (expected >> 7);
    MemSet(data, sizeof(data), 0);
    data[0] = 0x12345678UL; data[1] = 37;
    data[5] = (UInt32)callback; data[6] = (UInt32)text;
    if (code) {
        value = PceNativeCall((NativeFuncType*)MemHandleLock(code), data);
        MemHandleUnlock(code); DmReleaseResource(code);
    }
    result(label, code && data[2] == expected && value == (expected ^ 0xa55a5aa5UL)
           && data[3] == StrLen(text) && data[4] == callback(0x2468ace0UL, 0x1234), data[2]);
}
static void run(void) {
    Char text[100];
    LocalID old;
    UInt32 started, elapsed;
    EventType event;
    WinHandle saved, window;
    Err error = 0;
    if (!config()) return;
    old = DmFindDatabase(0, "FrondCompatLog");
    if (old && DmDeleteDatabase(0, old)) return;
    if (DmCreateDatabase(0, "FrondCompatLog", CREATOR, 'LOGS', false)) return;
    logDB = DmOpenDatabase(0, DmFindDatabase(0, "FrondCompatLog"), dmModeReadWrite);
    if (!logDB) return;
    failures = 0; queued = false;
    StrPrintF(text, "begin token=%s version=2", token); record(text);
    memoryProbe(); databaseProbe(); preferenceProbe();
    started = TimGetTicks(); SysTaskDelay(2); elapsed = TimGetTicks() - started;
    result("ticks", elapsed >= 2 && elapsed < SysTicksPerSecond() * 5UL, elapsed);
    window = WinCreateOffscreenWindow(16, 16, nativeFormat, &error);
    if (window) {
        RGBColorType black = {0, 0, 0, 0}, white = {0, 255, 255, 255}, pixel;
        UInt32 observed = 0;
        saved = WinSetDrawWindow(window);
        WinPushDrawState(); WinSetDrawMode(winPaint);
        WinSetForeColorRGB(&black, 0); WinSetBackColorRGB(&white, 0);
        WinPaintPixel(3, 4);
        if (!WinGetPixelRGB(3, 4, &pixel) && !pixel.r && !pixel.g && !pixel.b) observed |= 1;
        WinErasePixel(3, 4);
        if (!WinGetPixelRGB(3, 4, &pixel) && pixel.r == 255 && pixel.g == 255 && pixel.b == 255) observed |= 2;
        result("drawing", !error && observed == 3, observed);
        WinPopDrawState();
        WinSetDrawWindow(saved); WinDeleteWindow(window, false);
    } else result("drawing", false, error);
    nativeProbe(1000, "native-arm"); nativeProbe(1001, "native-thumb");
    /* Finish asynchronously through the application's real event dispatch. */
    MemSet(&event, sizeof(event), 0); event.eType = firstUserEvent;
    EvtAddEventToQueue(&event);
}
UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType event;
    Err menuError;
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    FrmGotoForm(1000); run();
    do {
        EvtGetEvent(&event, evtWaitForever);
        if (event.eType == firstUserEvent && logDB) {
            result("event-queue", true, 1);
            queued = true;
        } else if (event.eType == ctlSelectEvent && event.data.ctlSelect.controlID == 1003
                   && queued && logDB) {
            Char text[100];
            result("ui-control", true, 1003);
            StrPrintF(text, "complete token=%s failures=%u", token, failures); record(text);
            DmCloseDatabase(logDB); logDB = 0;
        } else if (!SysHandleEvent(&event) && !MenuHandleEvent(0, &event, &menuError)) {
            if (event.eType == frmLoadEvent) FrmSetActiveForm(FrmInitForm(event.data.frmLoad.formID));
            else if (event.eType == frmOpenEvent) FrmDrawForm(FrmGetActiveForm());
            else FrmHandleEvent(FrmGetActiveForm(), &event);
        }
    } while (event.eType != appStopEvent);
    if (logDB) { DmCloseDatabase(logDB); logDB = 0; }
    FrmCloseAllForms(); return 0;
}
