/* Round trip through the host serial endpoint with RTS/CTS flow control.
   For each baud rate: send a 70-byte request, then stop reading for one second
   while the host answers with the request bytes inverted and a 2048-byte burst.
   The Palm receive queue is smaller than the burst, so it only arrives intact
   if RTS holds the host back. */
#include <PalmOS.h>
#include <SerialMgr.h>
#define CREATOR 'FrSh'
#define BURST 2048
#define REPLY (64 + BURST)
static DmOpenRef logDB;
static UInt16 failures;
static Char token[33];
static UInt8 reply[REPLY];
static void record(const Char* text) {
    UInt16 index = dmMaxRecordIndex;
    MemHandle h = DmNewRecord(logDB, &index, StrLen(text) + 1);
    if (!h) { ++failures; return; }
    if (DmWrite(MemHandleLock(h), 0, text, StrLen(text) + 1)) ++failures;
    MemHandleUnlock(h); DmReleaseRecord(logDB, index, true);
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
static UInt8 pattern(UInt16 i, UInt8 phase) { return (UInt8)(i * 37 + phase * 11 + (i >> 3)); }
static UInt8 burst(UInt16 i, UInt8 phase) { return (UInt8)(i * 7 + (i >> 8) + phase); }
static void exchange(UInt8 phase, UInt32 baud) {
    UInt8 request[70];
    UInt16 port = 0, i, lineErrors = 0, bad = 0, valueLen;
    UInt32 flags, sent = 0, got = 0, status = 0, started, elapsed;
    Err error, openError, sendError = 0, receiveError = 0;
    Char text[160];
    openError = SrmOpen(serPortCradleRS232Port, baud, &port);
    if (openError) {
        StrPrintF(text, "phase=%u baud=%lu open=%04x", phase, baud, openError);
        record(text); ++failures; return;
    }
    flags = srmSettingsFlagBitsPerChar8 | srmSettingsFlagStopBits1 | srmSettingsFlagRTSAutoM |
            srmSettingsFlagCTSAutoM | srmSettingsFlagFlowControlIn;
    valueLen = sizeof(flags);
    error = SrmControl(port, srmCtlSetFlags, &flags, &valueLen);
    SrmGetStatus(port, &status, &lineErrors);
    request[0] = 'F'; request[1] = 'R'; request[2] = 'S'; request[3] = 'H';
    request[4] = phase; request[5] = (UInt8)(baud / 2400);
    for (i = 0; i < 64; ++i) request[6 + i] = pattern(i, phase);
    started = TimGetTicks();
    if (!error) {
        sent = SrmSend(port, request, sizeof(request), &sendError);
        if (!sendError) sendError = SrmSendWait(port);
    }
    /* Leave the reply waiting so the driver has to hold the host back. */
    SysTaskDelay(SysTicksPerSecond());
    /* SrmReceive waits for the whole count, so ask for less than the receive queue. */
    while (!error && got < REPLY) {
        UInt32 want = REPLY - got > 64 ? 64 : REPLY - got;
        UInt32 n = SrmReceive(port, reply + got, want, 3 * SysTicksPerSecond(), &receiveError);
        got += n;
        if (receiveError == serErrLineErr) { SrmClearErr(port); ++bad; }
        else if (receiveError) break;
    }
    elapsed = TimGetTicks() - started;
    for (i = 0; i < got && i < REPLY; ++i)
        if (reply[i] != (i < 64 ? (UInt8)~pattern(i, phase) : burst(i - 64, phase))) ++bad;
    SrmGetStatus(port, &status, &lineErrors);
    StrPrintF(text, "phase=%u baud=%lu ctl=%04x sent=%lu send=%04x got=%lu recv=%04x bad=%u line=%04x cts=%u dsr=%u ticks=%lu",
              phase, baud, error, sent, sendError, got, receiveError, bad, lineErrors,
              (status & srmStatusCtsOn) ? 1 : 0, (status & srmStatusDsrOn) ? 1 : 0, elapsed);
    record(text);
    if (error || sendError || sent != sizeof(request) || got != REPLY || bad || lineErrors) ++failures;
    error = SrmClose(port);
    if (error) ++failures;
}
static void run(void) {
    LocalID old;
    Char text[100];
    if (!config()) return;
    old = DmFindDatabase(0, "FrondSerialHostLog");
    if (old && DmDeleteDatabase(0, old)) return;
    if (DmCreateDatabase(0, "FrondSerialHostLog", CREATOR, 'LOGS', false)) return;
    logDB = DmOpenDatabase(0, DmFindDatabase(0, "FrondSerialHostLog"), dmModeReadWrite);
    if (!logDB) return;
    StrPrintF(text, "begin token=%s version=1", token); record(text);
    exchange(1, 9600);
    exchange(2, 57600);
    exchange(3, 115200);
    StrPrintF(text, "complete token=%s failures=%u", token, failures); record(text);
    DmCloseDatabase(logDB); logDB = 0;
}
UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType event;
    Err menuError;
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    FrmGotoForm(1000); run();
    do {
        EvtGetEvent(&event, SysTicksPerSecond() / 10);
        if (!SysHandleEvent(&event) && !MenuHandleEvent(0, &event, &menuError)) {
            if (event.eType == frmLoadEvent) FrmSetActiveForm(FrmInitForm(event.data.frmLoad.formID));
            else if (event.eType == frmOpenEvent) FrmDrawForm(FrmGetActiveForm());
            else FrmHandleEvent(FrmGetActiveForm(), &event);
        }
    } while (event.eType != appStopEvent);
    FrmCloseAllForms(); return 0;
}
