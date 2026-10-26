/* Discovery only: no USB attach, HotSync, receive wait, or host connection. */
#include <PalmOS.h>
#include <SerialMgr.h>
#define CREATOR 'FrSr'
static DmOpenRef logDB;
static UInt16 failures;
static Char token[33];
static void record(const Char* text) {
    UInt16 index = dmMaxRecordIndex;
    MemHandle h = DmNewRecord(logDB, &index, StrLen(text) + 1);
    if (!h) { ++failures; return; }
    if (DmWrite(MemHandleLock(h), 0, text, StrLen(text) + 1)) ++failures;
    MemHandleUnlock(h); DmReleaseRecord(logDB, index, true);
}
static void value(const Char* label, Err error, UInt32 number) {
    Char text[100];
    StrPrintF(text, "%s err=%04x value=%lu", label, error, number); record(text);
}
static Boolean config(void) {
    DmOpenRef db = DmOpenDatabaseByTypeCreator('DATA', CREATOR, dmModeReadOnly);
    MemHandle h;
    if (!db) return false;
    h = DmQueryRecord(db, 0);
    if (!h || MemHandleSize(h) != 33) { DmCloseDatabase(db); return false; }
    MemMove(token, MemHandleLock(h), 33);
    MemHandleUnlock(h); DmCloseDatabase(db);
    return token[32] == 0 && StrLen(token) == 32;
}
static void info(UInt32 port, const Char* label) {
    DeviceInfoType device;
    Err error;
    MemSet(&device, sizeof(device), 0);
    error = SrmGetDeviceInfo(port, &device);
    value(label, error, device.serDevCreator);
    /* A zero error reports driver metadata, not a working physical link. */
    if (!error) {
        Char text[120];
        StrPrintF(text, "info port=%lu flags=%lu maxbaud=%lu", port,
                  device.serDevFtrInfo, device.serDevMaxBaudRate); record(text);
    }
}
static void run(void) {
    UInt16 count = 0, port = 0, i;
    UInt32 version = 0, sent, pending = 0, started;
    Err error, check;
    LocalID old;
    Char text[100];
    /* Deliberately includes NUL and high bytes. Acceptance is not wire receipt. */
    const UInt8 packet[] = {0x46,0x72,0x53,0x72,0x00,0x7f,0x80,0xff};
    if (!config()) return;
    old = DmFindDatabase(0, "FrondSerialLog");
    if (old && DmDeleteDatabase(0, old)) return;
    if (DmCreateDatabase(0, "FrondSerialLog", CREATOR, 'LOGS', false)) return;
    logDB = DmOpenDatabase(0, DmFindDatabase(0, "FrondSerialLog"), dmModeReadWrite);
    if (!logDB) return;
    StrPrintF(text, "begin token=%s version=2", token); record(text);
    error = FtrGet(sysFileCSerialMgr, sysFtrNewSerialVersion, &version);
    value("manager", error, version);
    if (!error) {
        error = SrmGetDeviceCount(&count); value("count", error, count);
        if (!error) for (i = 0; i < count && i < 16; ++i) {
            Char label[32];
            StrPrintF(label, "device-%u", i); info(i, label);
        }
        error = SrmOpen(serPortCradleRS232Port, 9600, &port);
        value("rs232-open", error, port);
        if (!error) {
            info(port, "rs232-info");
            sent = SrmSend(port, packet, sizeof(packet), &error);
            value("rs232-send", error, sent);
            /* No SrmSendWait: CTS/device completion may never arrive. */
            started = TimGetTicks();
            do {
                check = SrmSendCheck(port, &pending);
                if (check || !pending) break;
                SysTaskDelay(1);
            } while (TimGetTicks() - started < SysTicksPerSecond());
            value("rs232-pending", check, pending);
            value("rs232-flush", SrmSendFlush(port), 0);
            value("rs232-close", SrmClose(port), 0);
        }
    }
    StrPrintF(text, "complete token=%s failures=%u", token, failures); record(text);
    DmCloseDatabase(logDB); logDB = 0;
}
UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType event;
    Err menuError;
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    FrmGotoForm(1000); run();
    do {
        /* Cooperative fixture: export cannot inject into every blocked task. */
        EvtGetEvent(&event, SysTicksPerSecond() / 10);
        if (!SysHandleEvent(&event) && !MenuHandleEvent(0, &event, &menuError)) {
            if (event.eType == frmLoadEvent) FrmSetActiveForm(FrmInitForm(event.data.frmLoad.formID));
            else if (event.eType == frmOpenEvent) FrmDrawForm(FrmGetActiveForm());
            else FrmHandleEvent(FrmGetActiveForm(), &event);
        }
    } while (event.eType != appStopEvent);
    FrmCloseAllForms(); return 0;
}
