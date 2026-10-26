#include <PalmOS.h>
static void logResult(const Char* label, Err error, UInt32 value) {
    Char line[120];
    LocalID id = DmFindDatabase(0, "FrondKeyboardLog");
    DmOpenRef db;
    UInt16 index = dmMaxRecordIndex;
    MemHandle record;
    StrPrintF(line, "%s err=%04x value=%lu", label, error, value);
    if (!id) { DmCreateDatabase(0, "FrondKeyboardLog", 'FrKb', 'DATA', false); id = DmFindDatabase(0, "FrondKeyboardLog"); }
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
    EventType event; Err menuError; Char line[80]; UInt16 count=0;
    if (command!=sysAppLaunchCmdNormalLaunch) return 0;
    logResult("start",0,0); FrmGotoForm(1000);
    do {
        EvtGetEvent(&event,evtWaitForever);
        if ((event.eType==keyDownEvent || event.eType==keyUpEvent || event.eType==keyHoldEvent) && count++<100) {
            StrPrintF(line,"event=%u chr=%04x code=%04x",event.eType,event.data.keyDown.chr,event.data.keyDown.keyCode);
            logResult(line,event.data.keyDown.modifiers,0);
        }
        if (!SysHandleEvent(&event) && !MenuHandleEvent(0,&event,&menuError)) {
            if (event.eType==frmLoadEvent) FrmSetActiveForm(FrmInitForm(event.data.frmLoad.formID));
            else if (event.eType==frmOpenEvent) FrmDrawForm(FrmGetActiveForm());
            else FrmHandleEvent(FrmGetActiveForm(),&event);
        }
    } while (event.eType!=appStopEvent);
    FrmCloseAllForms();return 0;
}
