/* Prints one line to an IrDA printer with the guest's own IrLib, as a print app would: discovery,
   an IrLAP connection, an IAS query for IrLPT's IrDA:IrLMP:LsapSel, a plain IrLMP connection to
   that LSAP (no Tiny TP), the job as one data packet, then an IrLAP disconnect. Each step goes on
   the screen and into the FrondIRPrintLog database, one record, ending in "done" or the step that
   failed. */
#include <PalmOS.h>
#include <IrLib.h>

#define JOB "Frond IR print test\r\n"

static Int16 row = 2;
static Char log[500];
static UInt16 ref;
static IrConnect con;
static IrPacket packet;
static IrIasQuery query;
static UInt8 queryBuf[] = "\x05IrLPT\x12IrDA:IrLMP:LsapSel";
static UInt8 result[32];
static IrDeviceAddr peer;
// The callback's last event, handled by the main loop.
static volatile Int16 event = -1;
// Every event since the last reset, one bit each, so a later one can't hide the one waited for.
static volatile UInt32 seen = 0;
static volatile Int16 queried = 0;

static void show(const Char* text) {
    WinDrawChars(text, StrLen(text), 2, row);
    row += 11;
    if (StrLen(log) + StrLen(text) + 2 < sizeof(log)) {
        StrCat(log, text);
        StrCat(log, "\n");
    }
}

static void save(void) {
    LocalID id = DmFindDatabase(0, "FrondIRPrintLog");
    DmOpenRef db;
    MemHandle record;
    UInt16 at = 0;
    if (id) DmDeleteDatabase(0, id);
    if (DmCreateDatabase(0, "FrondIRPrintLog", 'FrIP', 'DATA', false)) return;
    id = DmFindDatabase(0, "FrondIRPrintLog");
    db = id ? DmOpenDatabase(0, id, dmModeReadWrite) : 0;
    if (!db) return;
    record = DmNewRecord(db, &at, StrLen(log) + 1);
    if (record) {
        DmWrite(MemHandleLock(record), 0, log, StrLen(log) + 1);
        MemHandleUnlock(record);
        DmReleaseRecord(db, at, true);
    }
    DmCloseDatabase(db);
}

static void step(const Char* label, UInt32 value) {
    Char line[60];
    StrPrintF(line, "%s %lu", label, value);
    show(line);
}

static void callback(IrConnect* c, IrCallBackParms* parms) {
    if (parms->event == LEVENT_DISCOVERY_CNF && parms->deviceList && parms->deviceList->nItems)
        peer = parms->deviceList->dev[0].hDevice;
    event = parms->event;
    if (parms->event < 32) seen |= 1UL << parms->event;
}

static void queried_cb(IrStatus status) { queried = 1; }

// Runs the event loop until the callback reports want (or a failure event), up to ticks.
static Boolean wait(Int16 want, Int32 ticks) {
    EventType e;
    UInt32 end = TimGetTicks() + ticks;
    while (TimGetTicks() < end) {
        if (event == want || (want >= 0 && (seen & (1UL << want)))) return true;
        if (want != LEVENT_LAP_DISCON_IND && (event == LEVENT_LAP_DISCON_IND || event == LEVENT_LM_DISCON_IND)) return false;
        if (want == -2 && queried) return true;
        EvtGetEvent(&e, 2);
        SysHandleEvent(&e);
    }
    return false;
}

static void print(void) {
    Err error;
    IrStatus status;
    UInt8 lsap = 0;
    Int16 tries;
    if (SysLibFind(irLibName, &ref)) { show("no IrLib"); return; }
    error = IrOpen(ref, irOpenOptSpeed57600);
    step("open", error);
    status = IrBind(ref, &con, callback);
    step("bind", status);
    if (status != IR_STATUS_SUCCESS) return;
    // The media reads busy for a moment after the stack opens; try again for up to 5 s.
    for (tries = 0; tries < 10; tries++) {
        event = -1;
        seen = 0;
        status = IrDiscoverReq(ref, &con);
        if (status != IR_STATUS_MEDIA_BUSY) break;
        wait(-3, 50);
    }
    step("discover", status);
    step("tries", tries);
    if (status != IR_STATUS_PENDING || !wait(LEVENT_DISCOVERY_CNF, 1000) || !peer.u32) { show("no printer found"); goto unbind; }
    step("found", peer.u32);
    event = -1;
    seen = 0;
    status = IrConnectIrLap(ref, peer);
    if (status != IR_STATUS_PENDING || !wait(LEVENT_LAP_CON_CNF, 1000)) { step("irlap failed", status); goto unbind; }
    show("irlap");
    query.queryBuf = queryBuf;
    query.queryLen = sizeof(queryBuf) - 1;
    query.result = result;
    query.resultBufSize = sizeof(result);
    query.callBack = queried_cb;
    status = IrIAS_Query(ref, &query);
    if (status != IR_STATUS_PENDING || !wait(-2, 1000)) { step("ias failed", status); goto disconnect; }
    // One result: object id (2), type (1), then a 32-bit integer.
    if (query.retCode == IAS_RET_SUCCESS && query.resultLen >= 7 && result[2] == IAS_ATTRIB_INTEGER) lsap = result[6];
    step("ias lsap", lsap);
    if (!lsap) goto disconnect;
    con.rLsap = lsap;
    IrSetConTypeLMP(&con);
    packet.buff = 0;
    packet.len = 0;
    event = -1;
    seen = 0;
    status = IrConnectReq(ref, &con, &packet, 0);
    if (status != IR_STATUS_PENDING || !wait(LEVENT_LM_CON_CNF, 1000)) { step("connect failed", status); step("events", seen); goto disconnect; }
    show("connected");
    packet.buff = (UInt8*)JOB;
    packet.len = StrLen(JOB);
    event = -1;
    seen = 0;
    status = IrDataReq(ref, &con, &packet);
    if (status != IR_STATUS_PENDING || !wait(LEVENT_PACKET_HANDLED, 1000)) { step("send failed", status); goto disconnect; }
    step("sent", packet.len);
disconnect:
    event = -1;
    seen = 0;
    status = IrDisconnectIrLap(ref);
    wait(LEVENT_LAP_DISCON_IND, 500);
    step("disconnect", status);
    if (lsap) show("done");
unbind:
    IrUnbind(ref, &con);
    IrClose(ref);
}

UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType e;
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    WinEraseWindow();
    show("Frond IR print probe");
    print();
    save();
    do {
        EvtGetEvent(&e, evtWaitForever);
        SysHandleEvent(&e);
    } while (e.eType != appStopEvent);
    return 0;
}
