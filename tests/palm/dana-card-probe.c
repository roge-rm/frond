#include <PalmOS.h>
#include <VFSMgr.h>
#include <ExpansionMgr.h>

static void logRow(const Char* label, Err error, UInt32 value) {
    Char line[128]; LocalID id = DmFindDatabase(0, "FrondDanaCards");
    DmOpenRef db; UInt16 index = dmMaxRecordIndex; MemHandle record;
    StrPrintF(line, "%s err=%04x value=%lu", label, error, value);
    if (!id) { DmCreateDatabase(0, "FrondDanaCards", 'FDCd', 'DATA', false); id = DmFindDatabase(0, "FrondDanaCards"); }
    db = DmOpenDatabase(0, id, dmModeReadWrite); if (!db) return;
    record = DmNewRecord(db, &index, StrLen(line) + 1);
    if (record) { void* p = MemHandleLock(record); DmWrite(p, 0, line, StrLen(line) + 1);
        MemHandleUnlock(record); DmReleaseRecord(db, index, true); }
    DmCloseDatabase(db);
}
static void copyCounter(UInt16 volume, UInt16 slot) {
    FileRef file; Char text[32], expected[32]; UInt32 bytes = 0, previous = 0; Err err;
    if (!VFSFileOpen(volume, "/COPY.TXT", vfsModeRead, &file)) {
        MemSet(text, sizeof(text), 0); VFSFileRead(file, sizeof(text)-1, text, &bytes);
        previous = StrAToI(text); VFSFileClose(file);
    }
    logRow("persisted-copy", 0, previous);
    StrPrintF(expected, "%lu\n", previous + 1);
    err = VFSFileOpen(volume, "/COPY.TXT", vfsModeWrite|vfsModeCreate|vfsModeTruncate, &file);
    logRow("copy-open", err, slot);
    if (err) return;
    err = VFSFileWrite(file, StrLen(expected), expected, &bytes); logRow("copy-write", err, bytes);
    logRow("copy-close", VFSFileClose(file), slot);
    err = VFSFileOpen(volume, "/COPY.TXT", vfsModeRead, &file);
    if (err) { logRow("copy-match", err, slot); return; }
    MemSet(text, sizeof(text), 0); VFSFileRead(file, sizeof(text)-1, text, &bytes);
    VFSFileClose(file); logRow("copy-match", StrCompare(text, expected)?1:0, slot);
}
static void probe(void) {
    UInt32 it = vfsIteratorStart, count = 0; UInt16 volume; Err err;
    logRow("begin", 0, 0);
    logRow("pd-data",0,*(volatile UInt8*)0xfffff419UL);
    logRow("pd-pol",0,*(volatile UInt8*)0xfffff41cUL);
    logRow("pd-enable",0,*(volatile UInt8*)0xfffff41dUL);
    logRow("pd-edge",0,*(volatile UInt8*)0xfffff41fUL);
    logRow("int-mask",0,*(volatile UInt16*)0xfffff306UL);
    logRow("int-pending",0,*(volatile UInt16*)0xfffff30eUL);
    { UInt32 si=expIteratorStart; UInt16 sr;
      while (ExpSlotEnumerate(&sr, &si)==errNone) {
        ExpCardInfoType ci; MemSet(&ci, sizeof(ci), 0);
        err=ExpCardInfo(sr,&ci); logRow("card-info",err,sr);
        if (!err) { logRow(ci.deviceClassStr,0,sr); logRow(ci.deviceUniqueIDStr,0,sr); }
        if (si==expIteratorStop) break;
      }
    }
    while ((err = VFSVolumeEnumerate(&volume, &it)) == errNone) {
        VolumeInfoType info; Char label[32], text[64], expected[64];
        FileRef file; UInt32 bytes = 0; UInt16 slot;
        MemSet(&info, sizeof(info), 0); err = VFSVolumeInfo(volume, &info);
        logRow("volume-info", err, info.slotRefNum); slot = info.slotRefNum;
        MemSet(label, sizeof(label), 0); err = VFSVolumeGetLabel(volume, label, sizeof(label));
        logRow(label, err, volume);
        err = VFSFileOpen(volume, "/HOST.TXT", vfsModeRead, &file); logRow("host-open", err, slot);
        if (!err) {
            MemSet(text, sizeof(text), 0); err = VFSFileRead(file, sizeof(text)-1, text, &bytes);
            logRow("host-read", err, bytes); logRow(text, 0, slot); VFSFileClose(file);
        }
        StrPrintF(expected, "Dana guest slot %u\n", slot);
        err = VFSFileOpen(volume, "/GUEST.TXT", vfsModeRead, &file);
        logRow("persisted-open", err, slot);
        if (!err) {
            MemSet(text, sizeof(text), 0); VFSFileRead(file, sizeof(text)-1, text, &bytes);
            logRow("persisted-match", StrCompare(text, expected)?1:0, slot);
            VFSFileClose(file);
        }
        err = VFSFileOpen(volume, "/GUEST.TXT", vfsModeWrite|vfsModeCreate|vfsModeTruncate, &file);
        logRow("guest-open", err, slot);
        if (!err) {
            err = VFSFileWrite(file, StrLen(expected), expected, &bytes); logRow("guest-write", err, bytes);
            logRow("guest-close", VFSFileClose(file), slot);
            err = VFSFileOpen(volume, "/GUEST.TXT", vfsModeRead, &file); logRow("guest-reopen", err, slot);
            if (!err) {
                MemSet(text, sizeof(text), 0); err = VFSFileRead(file, sizeof(text)-1, text, &bytes);
                logRow("guest-read", err, bytes); logRow("guest-match", StrCompare(text, expected)?1:0, slot);
                VFSFileClose(file);
            }
        }
        copyCounter(volume, slot);
        ++count;
        if (it == vfsIteratorStop) break;
    }
    logRow("complete", 0, count);
}
UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType event; Err error;
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    probe();
    do { EvtGetEvent(&event, SysTicksPerSecond()/10);
        if (event.eType == keyDownEvent && event.data.keyDown.chr == 'a') probe();
        else if (!SysHandleEvent(&event)) MenuHandleEvent(0, &event, &error);
    } while (event.eType != appStopEvent);
    return 0;
}
