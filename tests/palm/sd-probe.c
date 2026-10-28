#include <PalmOS.h>
#include <VFSMgr.h>
#include <ExpansionMgr.h>

#define MOUNT_WAIT_SECONDS 30UL

static void logResult(const Char* label, Err error, UInt32 value) {
    Char line[120];
    LocalID id = DmFindDatabase(0, "FrondSDLog");
    DmOpenRef db;
    UInt16 index = dmMaxRecordIndex;
    MemHandle record;
    StrPrintF(line, "%s err=%04x value=%lu", label, error, value);
    if (!id) { DmCreateDatabase(0, "FrondSDLog", 'FrSD', 'DATA', false); id = DmFindDatabase(0, "FrondSDLog"); }
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
static Err probe(void) {
    UInt32 iterator = vfsIteratorStart, count = 0, bytes = 0;
    UInt32 used = 0, total = 0, started = TimGetTicks();
    UInt16 volume = 0;
    FileRef file = 0;
    Char data[64];
    const Char expected[] = "Hello from host\n", written[] = "Written by Palm TX\n";
    Err error;
    logResult("begin", 0, started);
    error = VFSVolumeEnumerate(&volume, &iterator);
    logResult("enumerate", error, volume);
    if (error) { logResult("complete", error, started); return error; }
    error = VFSVolumeSize(volume, &used, &total);
    logResult("volume-size", error, total);
    logResult("volume-used", error, used);
    error = VFSFileOpen(volume, "/HOST.TXT", vfsModeRead, &file);
    logResult("host-open", error, 0);
    if (!error) {
        MemSet(data, sizeof(data), 0);
        error = VFSFileRead(file, sizeof(data) - 1, data, &bytes);
        logResult("host-read", error, bytes);
        logResult("host-match", StrCompare(data, expected) ? 1 : 0, 0);
        logResult("host-close", VFSFileClose(file), 0);
    }
    error = VFSFileOpen(volume, "/GUEST.TXT", vfsModeRead, &file);
    logResult("prior-open", error, 0);
    if (!error) {
        MemSet(data, sizeof(data), 0);
        error = VFSFileRead(file, sizeof(data) - 1, data, &bytes);
        logResult("prior-read", error, bytes);
        logResult("prior-match", StrCompare(data, written) ? 1 : 0, 0);
        logResult("prior-close", VFSFileClose(file), 0);
    }
    error = VFSFileOpen(volume, "/GUEST.TXT", vfsModeWrite | vfsModeCreate | vfsModeTruncate, &file);
    logResult("guest-open", error, 0);
    if (!error) {
        error = VFSFileWrite(file, StrLen(written), written, &count);
        logResult("guest-write", error, count);
        logResult("guest-close", VFSFileClose(file), 0);
    }
    error = VFSFileOpen(volume, "/GUEST.TXT", vfsModeRead, &file);
    logResult("guest-reopen", error, 0);
    if (!error) {
        MemSet(data, sizeof(data), 0);
        error = VFSFileRead(file, sizeof(data) - 1, data, &bytes);
        logResult("guest-read", error, bytes);
        logResult("guest-match", StrCompare(data, written) ? 1 : 0, 0);
        logResult("guest-read-close", VFSFileClose(file), 0);
    }
    logResult("complete", 0, started);
    return errNone;  // File errors are logged and must never trigger another attempt.
}
// Hold mode, used for eject and sleep tests: when /HOLD.TXT holds a block count
// and an optional pass interval ("300" or "60 20"), keep /HOLD.DAT open and append
// one 512-byte block every interval event-loop passes (a pass is at most 0.1 s),
// then close it and log its size. The first write error stops writing; the file
// is still closed.
static UInt16 holdVolume;
static FileRef holdFile;
static UInt32 holdTarget, holdDone, holdInterval, holdPass;
static Boolean holdError;
static void holdStart(void) {
    UInt32 iterator = vfsIteratorStart, bytes = 0;
    FileRef file;
    Char text[12];
    holdTarget = holdDone = 0; holdFile = 0; holdError = false;
    if (VFSVolumeEnumerate(&holdVolume, &iterator)) return;
    if (VFSFileOpen(holdVolume, "/HOLD.TXT", vfsModeRead, &file)) return;
    MemSet(text, sizeof(text), 0);
    VFSFileRead(file, sizeof(text) - 1, text, &bytes);
    VFSFileClose(file);
    holdTarget = StrAToI(text);
    holdInterval = StrChr(text, ' ') ? StrAToI(StrChr(text, ' ') + 1) : 1;
    if (holdInterval < 1) holdInterval = 1;
    holdPass = 0;
    if (holdTarget > 4000) holdTarget = 4000;
    if (!holdTarget) return;
    logResult("hold-open", VFSFileOpen(holdVolume, "/HOLD.DAT", vfsModeWrite | vfsModeCreate | vfsModeTruncate, &holdFile), holdTarget);
}
static void holdFinish(void) {
    UInt32 size = 0;
    FileRef file;
    Err error;
    logResult("hold-close", VFSFileClose(holdFile), holdDone);
    holdFile = 0; holdTarget = 0;
    error = VFSFileOpen(holdVolume, "/HOLD.DAT", vfsModeRead, &file);
    if (!error) { error = VFSFileSize(file, &size); VFSFileClose(file); }
    logResult("hold-size", error, size);
}
static void holdStep(void) {
    UInt8 block[512];
    UInt32 count = 0;
    Err error;
    if (!holdFile || holdError) return;
    if (++holdPass % holdInterval) return;
    MemSet(block, sizeof(block), (UInt8)(holdDone & 0xff));
    error = VFSFileWrite(holdFile, sizeof(block), block, &count);
    if (error || count != sizeof(block)) {
        holdError = true;
        logResult("hold-write", error ? error : 1, holdDone);
        holdFinish();
        return;
    }
    if (++holdDone % 50 == 0) logResult("hold-progress", 0, holdDone);
    if (holdDone >= holdTarget) holdFinish();
}
UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType event;
    Err menuError;
    Boolean mountPending;
    UInt32 nextMount, mountDeadline;
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    mountPending = probe() == expErrEnumerationEmpty;
    if (!mountPending) holdStart();
    nextMount = TimGetTicks() + SysTicksPerSecond();
    mountDeadline = TimGetTicks() + MOUNT_WAIT_SECONDS * SysTicksPerSecond();
    FrmGotoForm(1000);
    do {
        // This test probe cooperates with the host API bridge by revisiting its
        // event-loop boundary while idle.
        EvtGetEvent(&event, SysTicksPerSecond() / 10);
        if (!SysHandleEvent(&event) && !MenuHandleEvent(0, &event, &menuError)) {
            if (event.eType == frmLoadEvent) {
                FrmSetActiveForm(FrmInitForm(event.data.frmLoad.formID));
            } else if (event.eType == frmOpenEvent) FrmDrawForm(FrmGetActiveForm());
            else FrmHandleEvent(FrmGetActiveForm(), &event);
        }
        // Expansion Manager mount notifications are asynchronous after reset.
        // Keep processing UI events, and preserve every enumeration-empty run.
        // Each new attempt has its own begin/complete pair. Never retry I/O.
        if (event.eType != appStopEvent) holdStep();
        if (event.eType != appStopEvent && mountPending) {
            UInt32 now = TimGetTicks();
            if ((Int32)(now - mountDeadline) >= 0) {
                logResult("begin", errNone, now);
                logResult("mount-timeout", expErrEnumerationEmpty, mountDeadline);
                logResult("complete", expErrEnumerationEmpty, now);
                mountPending = false;
            } else if ((Int32)(now - nextMount) >= 0) {
                mountPending = probe() == expErrEnumerationEmpty;
                nextMount = TimGetTicks() + SysTicksPerSecond();
            }
        }
    } while (event.eType != appStopEvent);
    if (holdFile) holdFinish();
    FrmCloseAllForms();
    return 0;
}
