/* Dials the default Network service with the guest's own NetLib (PPP over the serial port on
   frond-68k), looks up example.com, fetches / over HTTP and shows each step on the screen:
   "open", "dns", "connect", "sent", the status line and the byte count, then "done" or the
   failing step with its error. The same lines go into the FrondPPPLog database, one record, so a
   test can export and read them. */
#include <PalmOS.h>
#include <NetMgr.h>

static Int16 row = 2;
static Char log[400];

static void show(const Char* text) {
    WinDrawChars(text, StrLen(text), 2, row);
    row += 11;
    if (StrLen(log) + StrLen(text) + 2 < sizeof(log)) {
        StrCat(log, text);
        StrCat(log, "\n");
    }
}

static void save(void) {
    LocalID id = DmFindDatabase(0, "FrondPPPLog");
    DmOpenRef db;
    MemHandle record;
    UInt16 at = 0;
    if (id) DmDeleteDatabase(0, id);
    if (DmCreateDatabase(0, "FrondPPPLog", 'FPpp', 'DATA', false)) return;
    id = DmFindDatabase(0, "FrondPPPLog");
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

static void step(const Char* label, Err error, UInt32 value) {
    Char line[60];
    StrPrintF(line, "%s err=%lx %lu", label, (UInt32)error, value);
    show(line);
}

static void fetch(void) {
    UInt16 ref, ifError = 0;
    Err error = 0;
    NetSocketRef socket;
    NetSocketAddrINType peer;
    static NetHostInfoBufType host;
    NetHostInfoPtr info;
    Char request[96], data[200];
    Int16 n;
    UInt32 total = 0;
    Boolean first = true;

    if (SysLibFind("Net.lib", &ref)) {
        show("no Net.lib");
        return;
    }
    error = NetLibOpen(ref, &ifError);
    step("open", error ? error : ifError, 0);
    if ((error && error != netErrAlreadyOpen) || ifError) return;

    info = NetLibGetHostByName(ref, "example.com", &host, SysTicksPerSecond() * 30, &error);
    if (!info) {
        step("dns", error, 0);
        return;
    }
    peer.addr = host.address[0];
    {
        UInt8* a = (UInt8*)&peer.addr;
        Char line[40];
        StrPrintF(line, "dns %lu.%lu.%lu.%lu", (UInt32)a[0], (UInt32)a[1], (UInt32)a[2], (UInt32)a[3]);
        show(line);
    }
    socket = NetLibSocketOpen(ref, netSocketAddrINET, netSocketTypeStream, 0, SysTicksPerSecond() * 10, &error);
    if (socket < 0) {
        step("socket", error, 0);
        return;
    }
    peer.family = netSocketAddrINET;
    peer.port = NetHToNS(80);
    if (NetLibSocketConnect(ref, socket, (NetSocketAddrType*)&peer, sizeof(peer), SysTicksPerSecond() * 30,
                            &error) < 0) {
        step("connect", error, 0);
        NetLibSocketClose(ref, socket, 0, &error);
        return;
    }
    show("connect");
    StrCopy(request, "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n");
    n = NetLibSend(ref, socket, request, StrLen(request), 0, 0, 0, SysTicksPerSecond() * 10, &error);
    step("sent", n < 0 ? error : 0, n < 0 ? 0 : n);
    while ((n = NetLibReceive(ref, socket, data, sizeof(data) - 1, 0, 0, 0, SysTicksPerSecond() * 30, &error)) > 0) {
        if (first) {
            Char* end;
            data[n] = 0;
            end = StrChr(data, '\r');
            if (end) *end = 0;
            data[30] = 0;
            show(data);
            first = false;
        }
        total += n;
    }
    step("received", n < 0 ? error : 0, total);
    NetLibSocketClose(ref, socket, 0, &error);
    show(total ? "done" : "nothing received");
}

UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType event;
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    WinEraseWindow();
    show("Frond PPP probe");
    fetch();
    save();
    do {
        EvtGetEvent(&event, evtWaitForever);
        SysHandleEvent(&event);
    } while (event.eType != appStopEvent);
    return 0;
}
