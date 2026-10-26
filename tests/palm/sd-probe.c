#include <PalmOS.h>
#include <VFSMgr.h>

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
static void probe(void) {
    UInt32 iterator = vfsIteratorStart, count = 0, bytes = 0;
    UInt16 volume = 0;
    FileRef file = 0;
    Char data[64];
    const Char expected[] = "Hello from host\n", written[] = "Written by Palm TX\n";
    Err error = VFSVolumeEnumerate(&volume, &iterator);
    logResult("enumerate", error, volume);
    if (error) return;
    error = VFSFileOpen(volume, "/HOST.TXT", vfsModeRead, &file);
    logResult("host-open", error, 0);
    if (!error) {
        MemSet(data, sizeof(data), 0);
        error = VFSFileRead(file, sizeof(data) - 1, data, &bytes);
        logResult("host-read", error, bytes);
        logResult("host-match", StrCompare(data, expected) ? 1 : 0, 0);
        VFSFileClose(file);
    }
    error = VFSFileOpen(volume, "/GUEST.TXT", vfsModeRead, &file);
    logResult("prior-open", error, 0);
    if (!error) {
        MemSet(data, sizeof(data), 0);
        VFSFileRead(file, sizeof(data) - 1, data, &bytes);
        logResult("prior-match", StrCompare(data, written) ? 1 : 0, 0);
        VFSFileClose(file);
    }
    error = VFSFileOpen(volume, "/GUEST.TXT", vfsModeWrite | vfsModeCreate | vfsModeTruncate, &file);
    logResult("guest-open", error, 0);
    if (!error) {
        error = VFSFileWrite(file, StrLen(written), written, &count);
        logResult("guest-write", error, count);
        logResult("guest-close", VFSFileClose(file), 0);
    }
    error = VFSFileOpen(volume, "/GUEST.TXT", vfsModeRead, &file);
    if (!error) {
        MemSet(data, sizeof(data), 0);
        VFSFileRead(file, sizeof(data) - 1, data, &bytes);
        logResult("guest-match", StrCompare(data, written) ? 1 : 0, 0);
        VFSFileClose(file);
    }
}
UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType event;
    Err menuError;
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    probe();
    FrmGotoForm(1000);
    do {
        EvtGetEvent(&event, evtWaitForever);
        if (!SysHandleEvent(&event) && !MenuHandleEvent(0, &event, &menuError)) {
            if (event.eType == frmLoadEvent) {
                FrmSetActiveForm(FrmInitForm(event.data.frmLoad.formID));
            } else if (event.eType == frmOpenEvent) FrmDrawForm(FrmGetActiveForm());
            else FrmHandleEvent(FrmGetActiveForm(), &event);
        }
    } while (event.eType != appStopEvent);
    FrmCloseAllForms();
    return 0;
}
