#include <NetMgr.h>
#include <PalmOS.h>
static UInt16 ref;
static NetSocketRef pendingSocket = -1;
static Char hostname[128] = "example.com", path[128] = "/";
static UInt16 httpPort = 80, udpPort;
static UInt32 address;
static const Char checkpointText[] = "checkpoint\n";
typedef struct {
    UInt32 phase, calls;
} Tone;
static UInt32 readLittle32(const void* value) {
    const UInt8* b = value;
    return (UInt32)b[0] | ((UInt32)b[1] << 8) | ((UInt32)b[2] << 16) | ((UInt32)b[3] << 24);
}
static void logResult(const Char* label, Err error, UInt32 value) {
    Char line[120];
    LocalID id = DmFindDatabase(0, "FrondHostLog");
    DmOpenRef db;
    UInt16 index = dmMaxRecordIndex;
    MemHandle record;
    StrPrintF(line, "%s err=%04x value=%lu", label, error, value);
    {
        RectangleType area = {{2, 44}, {156, 14}};
        WinEraseRectangle(&area, 0);
        WinDrawChars(line, StrLen(line), 2, 44);
    }
    if (!id) {
        DmCreateDatabase(0, "FrondHostLog", 'FrHN', 'DATA', false);
        id = DmFindDatabase(0, "FrondHostLog");
    }
    db = DmOpenDatabase(0, id, dmModeReadWrite);
    if (!db) return;
    record = DmNewRecord(db, &index, StrLen(line) + 1);
    if (record) {
        void* bytes = MemHandleLock(record);
        DmWrite(bytes, 0, line, StrLen(line) + 1);
        MemHandleUnlock(record);
        DmReleaseRecord(db, index, true);
    }
    DmCloseDatabase(db);
}
static void config(void) {
    DmOpenRef db = DmOpenDatabaseByTypeCreator('DATA', 'Fhcf', dmModeReadOnly);
    Char text[512], *p, *next;
    MemHandle record;
    UInt32 size;
    if (!db) return;
    record = DmQueryRecord(db, 0);
    if (!record) {
        DmCloseDatabase(db);
        return;
    }
    size = MemHandleSize(record);
    if (size >= sizeof(text)) size = sizeof(text) - 1;
    MemMove(text, MemHandleLock(record), size);
    text[size] = 0;
    MemHandleUnlock(record);
    DmCloseDatabase(db);
    p = text;
    next = StrChr(p, '\n');
    if (!next) return;
    *next++ = 0;
    StrNCopy(hostname, p, sizeof(hostname) - 1);
    p = next;
    next = StrChr(p, '\n');
    if (!next) return;
    *next++ = 0;
    httpPort = StrAToI(p);
    p = next;
    next = StrChr(p, '\n');
    if (!next) return;
    *next++ = 0;
    udpPort = StrAToI(p);
    p = next;
    next = StrChr(p, '\n');
    if (next) *next = 0;
    if (*p) StrNCopy(path, p, sizeof(path) - 1);
}
static void closeSocket(NetSocketRef socket) {
    Err error;
    if (socket >= 0) NetLibSocketClose(ref, socket, 0, &error);
}
static void closeNetwork(void) {
    closeSocket(pendingSocket);
    pendingSocket = -1;
    if (ref) {
        NetLibClose(ref, true);
        ref = 0;
    }
}
static NetSocketRef connectSocket(void) {
    NetSocketRef socket;
    NetSocketAddrINType peer;
    Err error;
    Int16 result;
    socket = NetLibSocketOpen(ref, netSocketAddrINET, netSocketTypeStream, 0, 500, &error);
    logResult("tcp-open", error, socket);
    if (socket < 0) return -1;
    peer.family = netSocketAddrINET;
    peer.port = NetHToNS(httpPort);
    peer.addr = address;
    result = NetLibSocketConnect(ref, socket, (NetSocketAddrType*)&peer, sizeof(peer), 500, &error);
    logResult("tcp-connect", error, result);
    if (result < 0) {
        closeSocket(socket);
        return -1;
    }
    return socket;
}
static void http(void) {
    NetSocketRef socket = connectSocket();
    Err error;
    Int16 n;
    UInt32 total = 0, kept = 0;
    static Char request[384], data[1025], response[4096];
    if (socket < 0) return;
    StrPrintF(request, "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, hostname);
    n = NetLibSend(ref, socket, request, StrLen(request), 0, 0, 0, 500, &error);
    logResult("http-send", error, n);
    response[0] = 0;
    if (n >= 0) do {
            n = NetLibReceive(ref, socket, data, sizeof(data) - 1, 0, 0, 0, 2000, &error);
            if (n > 0) {
                UInt16 copy = n;
                total += n;
                if (copy > sizeof(response) - 1 - kept) copy = sizeof(response) - 1 - kept;
                MemMove(response + kept, data, copy);
                kept += copy;
                response[kept] = 0;
            }
        } while (n > 0 && total < 32768);
    logResult("http-receive", error, total);
    logResult("http-header", StrNCompare(response, "HTTP/", 5) ? 1 : 0, 0);
    logResult("http-body",
              (StrStr(response, "frond-host-ok") || StrStr(response, "Example Domain")) ? 0 : 1, 0);
    closeSocket(socket);
}
static void udp(void) {
    NetSocketRef socket;
    NetSocketAddrINType peer, from;
    UInt16 fromLength;
    Err error;
    Int16 n;
    Char bytes[32];
    NetFDSetType reads;
    UInt32 nonblocking;
    if (!udpPort) return;
    socket = NetLibSocketOpen(ref, netSocketAddrINET, netSocketTypeDatagram, 0, 100, &error);
    logResult("udp-open", error, socket);
    if (socket < 0) return;
    peer.family = netSocketAddrINET;
    peer.port = NetHToNS(udpPort);
    peer.addr = address;
    n = NetLibSend(ref, socket, "ping", 4, 0, &peer, sizeof(peer), 100, &error);
    logResult("udp-send", error, n);
    netFDZero(&reads);
    netFDSet(socket, &reads);
    n = NetLibSelect(ref, socket + 1, &reads, 0, 0, 100, &error);
    logResult("udp-select", error, n);
    MemSet(bytes, sizeof(bytes), 0);
    fromLength = sizeof(from);
    n = NetLibReceive(ref, socket, bytes, sizeof(bytes) - 1, netIOFlagPeek, &from, &fromLength, 100,
                      &error);
    logResult("udp-peek", error, n);
    n = NetLibReceive(ref, socket, bytes, sizeof(bytes) - 1, 0, &from, &fromLength, 100, &error);
    logResult("udp-receive", error, n);
    logResult("udp-match", StrCompare(bytes, "pong") ? 1 : 0, NetNToHS(from.port));
    nonblocking = 1;
    NetLibSocketOptionSet(ref, socket, netSocketOptLevelSocket, netSocketOptSockNonBlocking,
                          &nonblocking, sizeof(nonblocking), 0, &error);
    n = NetLibReceive(ref, socket, bytes, sizeof(bytes) - 1, 0, 0, 0, 100, &error);
    logResult("udp-wouldblock", error, n);
    nonblocking = 0;
    NetLibSocketOptionSet(ref, socket, netSocketOptLevelSocket, netSocketOptSockNonBlocking,
                          &nonblocking, sizeof(nonblocking), 0, &error);
    n = NetLibReceive(ref, socket, bytes, sizeof(bytes) - 1, 0, 0, 0, 10, &error);
    logResult("udp-timeout", error, n);
    closeSocket(socket);
}
static void run(void) {
    Err error;
    UInt16 ifError = 0;
    static NetHostInfoBufType host;
    NetHostInfoPtr info;
    SndStreamRef stream = 0;
    MemHandle armCode = 0;
    Tone* tone;
    closeNetwork();
    config();
    logResult("start", 0, 0);
    error = SysLibFind("Net.lib", &ref);
    logResult("find", error, ref);
    if (error || ref != 0x7ffe) {
        ref = 0;
        return;
    }
    error = NetLibOpen(ref, &ifError);
    logResult("open", error, ifError);
    if (error) {
        ref = 0;
        return;
    }
    tone = MemPtrNew(sizeof(Tone));
    if (tone) {
        MemSet(tone, sizeof(Tone), 0);
        armCode = DmGetResource('armc', 1000);
        error = armCode ? SndStreamCreate(&stream, sndOutput, 22050, sndInt16Little, sndStereo,
                                          (SndStreamBufferCallback)MemHandleLock(armCode), tone,
                                          0, true)
                        : 1;
        logResult("tone-create", error, 0);
        if (!error) logResult("tone-start", SndStreamStart(stream), 0);
    }
    info = NetLibGetHostByName(ref, hostname, &host, 500, &error);
    logResult("dns", error, info ? *(UInt32*)info->addrListP[0] : 0);
    if (info) {
        address = *(UInt32*)info->addrListP[0];
        http();
        if (!StrCompare(path, "/wait-control")) SysTaskDelay(400);
        udp();
    }
    if (stream) {
        logResult("tone-delete-start", 0, 0);
        SndStreamDelete(stream);
        logResult("tone-calls", 0, readLittle32(&tone->calls));
    }
    if (armCode) {
        MemHandleUnlock(armCode);
        DmReleaseResource(armCode);
    }
    if (tone) MemPtrFree(tone);
    if (info && udpPort) {
        pendingSocket = connectSocket();
        if (pendingSocket >= 0) {
            Int16 n = NetLibSend(ref, pendingSocket, (void*)checkpointText, StrLen(checkpointText),
                                 0, 0, 0, 100, &error);
            logResult("checkpoint-send", error, n);
        }
    }
    logResult("ready", 0, 0);
}
static void receivePending(void) {
    Char bytes[32];
    Err error;
    Int16 n;
    if (pendingSocket < 0) return;
    MemSet(bytes, sizeof(bytes), 0);
    n = NetLibReceive(ref, pendingSocket, bytes, sizeof(bytes) - 1, 0, 0, 0, 100, &error);
    logResult("checkpoint-receive", error, n);
    if (n >= 0) logResult("checkpoint-match", StrCompare(bytes, checkpointText) ? 1 : 0, 0);
    closeNetwork();
}
UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType event;
    Err menuError;
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    FrmSetActiveForm(FrmInitForm(1000));
    FrmDrawForm(FrmGetActiveForm());
    run();
    do {
        EvtGetEvent(&event, evtWaitForever);
        if (!SysHandleEvent(&event) && !MenuHandleEvent(0, &event, &menuError)) {
            if (event.eType == frmLoadEvent)
                FrmSetActiveForm(FrmInitForm(event.data.frmLoad.formID));
            else if (event.eType == frmOpenEvent)
                FrmDrawForm(FrmGetActiveForm());
            else if (event.eType == ctlSelectEvent && event.data.ctlSelect.controlID == 1002)
                receivePending();
            else if (event.eType == ctlSelectEvent && event.data.ctlSelect.controlID == 1003)
                run();
            else
                FrmHandleEvent(FrmGetActiveForm(), &event);
        }
    } while (event.eType != appStopEvent);
    closeNetwork();
    FrmCloseAllForms();
    return 0;
}
