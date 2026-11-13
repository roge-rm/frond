#include <PalmOS.h>
#include <VFSMgr.h>
#include <ExpansionMgr.h>

// Logs what the Expansion Manager says about the slot, formats the stick when a FrondMSFormat
// database is installed, then writes and reads /GUEST.TXT. Results go to FrondMSLog.
static void logResult(const Char* label, Err error, UInt32 value) {
    Char line[120];
    LocalID id = DmFindDatabase(0, "FrondMSLog");
    DmOpenRef db;
    UInt16 index = dmMaxRecordIndex;
    MemHandle record;
    StrPrintF(line, "%s err=%04x value=%lu", label, error, value);
    if (!id) { DmCreateDatabase(0, "FrondMSLog", 'FrMS', 'DATA', false); id = DmFindDatabase(0, "FrondMSLog"); }
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
UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    UInt32 iterator = expIteratorStart, slotCount = 0, bytes = 0, count = 0, used = 0, total = 0;
    UInt16 slot = 0, library = 0, volume = 0;
    ExpCardInfoType card;
    FileRef file;
    Char data[64];
    const Char written[] = "Written by CLIE T415\n";
    Err error;
    if (command != sysAppLaunchCmdNormalLaunch) return 0;
    logResult("begin", 0, TimGetTicks());
    while (iterator != expIteratorStop) {
        error = ExpSlotEnumerate(&slot, &iterator);
        logResult("slot-enumerate", error, slot);
        if (error) break;
        slotCount++;
        library = 0;
        error = ExpSlotLibFind(slot, &library);
        logResult("slot-lib", error, library);
        logResult("card-present", ExpCardPresent(slot), slot);
        MemSet(&card, sizeof(card), 0);
        error = ExpCardInfo(slot, &card);
        logResult("card-info", error, card.capabilityFlags);
        logResult("card-name", 0, StrLen(card.deviceClassStr));
    }
    logResult("slots", 0, slotCount);
    iterator = vfsIteratorStart;
    error = VFSVolumeEnumerate(&volume, &iterator);
    logResult("volume-enumerate", error, volume);
    if (DmFindDatabase(0, "FrondMSFormat") && slotCount) {
        VFSSlotMountParamType mount;
        DmDeleteDatabase(0, DmFindDatabase(0, "FrondMSFormat"));
        if (!error) logResult("unmount", VFSVolumeUnmount(volume), volume);
        MemSet(&mount, sizeof(mount), 0);
        mount.vfsMountParam.volRefNum = vfsInvalidVolRef;
        mount.vfsMountParam.mountClass = vfsMountClass_SlotDriver;
        mount.slotLibRefNum = library;
        mount.slotRefNum = slot;
        error = VFSVolumeFormat(0, 0, (VFSAnyMountParamPtr)&mount);
        logResult("format", error, mount.vfsMountParam.volRefNum);
        if (!error) volume = mount.vfsMountParam.volRefNum;
        iterator = vfsIteratorStart;
        error = VFSVolumeEnumerate(&volume, &iterator);
        logResult("volume-enumerate", error, volume);
    }
    if (!error) {
        logResult("volume-size", VFSVolumeSize(volume, &used, &total), total);
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
    }
    logResult("complete", 0, TimGetTicks());
    return errNone;
}
