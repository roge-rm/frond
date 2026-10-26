#include <PalmOS.h>
#include <NetMgr.h>
static UInt16 ref;
static NetSocketRef sock = -1;
static const Char payload[] = "hello from Palm\n";
static void logResult(const Char* label, Err error, UInt32 value) {
    Char line[120];
    LocalID id = DmFindDatabase(0, "FrondNetLog");
    DmOpenRef db;
    UInt16 index = dmMaxRecordIndex;
    MemHandle record;
    StrPrintF(line, "%s err=%04x value=%lu", label, error, value);
    if (!id) { DmCreateDatabase(0, "FrondNetLog", 'FrNt', 'DATA', false); id = DmFindDatabase(0, "FrondNetLog"); }
    db = DmOpenDatabase(0, id, dmModeReadWrite);
    if (!db) return;
    record = DmNewRecord(db, &index, StrLen(line) + 1);
    if (record) {
        void* bytes = MemHandleLock(record);
        DmWrite(bytes, 0, line, StrLen(line) + 1);
        MemHandleUnlock(record); DmReleaseRecord(db, index, true);
    }
    DmCloseDatabase(db);
}
static void closeNetwork(void) {
    Err error; Int16 result;
    if (sock >= 0) { result = NetLibSocketClose(ref, sock, 0, &error); logResult("socket-close", error, result); sock = -1; }
    if (ref) { logResult("lib-close", NetLibClose(ref, false), 0); ref = 0; }
}
static void probe(void) {
    Err error; UInt16 ifError = 0;
    NetHostInfoBufType host;
    NetHostInfoPtr result;
    NetSocketAddrINType address;
    Int16 sent;
    error = SysLibFind("Net.lib", &ref);
    logResult("find", error, ref);
    /* Only exercise the opt-in simulator, never open the real radio. */
    if (error || ref != 0x7ffe) { ref = 0; return; }
    error = NetLibOpen(ref, &ifError);
    logResult("open", error, ifError);
    if (error) { ref = 0; return; }
    result = NetLibGetHostByName(ref, "missing.frond.test", &host, 0, &error);
    logResult("dns-missing", error, result != 0);
    result = NetLibGetHostByName(ref, "frond.test", &host, 0, &error);
    logResult("dns", error, result ? *(UInt32*)result->addrListP[0] : 0);
    if (!result) { closeNetwork(); return; }
    sock = NetLibSocketOpen(ref, netSocketAddrINET, netSocketTypeStream, 0, 0, &error);
    logResult("socket", error, sock);
    if (sock < 0) { closeNetwork(); return; }
    MemSet(&address, sizeof(address), 0);
    address.family = netSocketAddrINET;
    address.port = NetHToNS(7); address.addr = NetHToNL(0x0a000002);
    sent = NetLibSocketConnect(ref, sock, (NetSocketAddrType*)&address, sizeof(address), 100, &error);
    logResult("connect", error, sent);
    if (sent < 0) { closeNetwork(); return; }
    sent = NetLibSend(ref, sock, (void*)payload, StrLen(payload), 0, 0, 0, 0, &error);
    logResult("send", error, sent);
}
static void receiveEcho(void) {
    Char data[64]; Err error; Int16 count;
    NetFDSetType reads;
    if (sock < 0) return;
    netFDZero(&reads); netFDSet(sock, &reads);
    count = NetLibSelect(ref, sock + 1, &reads, 0, 0, 0, &error);
    logResult("select", error, count);
    MemSet(data, sizeof(data), 0);
    count = NetLibReceive(ref, sock, data, sizeof(data)-1, 0, 0, 0, 0, &error);
    logResult("receive", error, count);
    logResult("echo-match", StrCompare(data, payload) ? 1 : 0, 0);
    count = NetLibReceive(ref, sock, data, sizeof(data)-1, 0, 0, 0, 0, &error);
    logResult("empty", error, count);
    closeNetwork();
}
UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType event; Err menuError;
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    probe(); FrmGotoForm(1000);
    do {
        EvtGetEvent(&event, evtWaitForever);
        if (!SysHandleEvent(&event) && !MenuHandleEvent(0, &event, &menuError)) {
            if (event.eType == frmLoadEvent) FrmSetActiveForm(FrmInitForm(event.data.frmLoad.formID));
            else if (event.eType == frmOpenEvent) FrmDrawForm(FrmGetActiveForm());
            else if (event.eType == ctlSelectEvent && event.data.ctlSelect.controlID == 1002) receiveEcho();
            else FrmHandleEvent(FrmGetActiveForm(), &event);
        }
    } while (event.eType != appStopEvent);
    closeNetwork(); FrmCloseAllForms(); return 0;
}
