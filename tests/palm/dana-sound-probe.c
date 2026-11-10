/* Dana guest Sound Manager calls and register readback for sound qualification. */
#include <PalmOS.h>
static DmOpenRef logDb;
static void record(const Char* tag, Err err) {
    Char row[180]; UInt16 index=dmMaxRecordIndex; MemHandle h;
    StrPrintF(row,"%s err=%d ticks=%lu clock=%lu pll=%x/%x mux=%x pcr=%x pwm=%x/%x/%x",tag,err,TimGetTicks(),
        *(volatile UInt32*)0x12e,*(volatile UInt16*)0xfffff200,*(volatile UInt16*)0xfffff202,
        *(volatile UInt8*)0xfffff40b,*(volatile UInt8*)0xfffff003,*(volatile UInt16*)0xfffff500,
        *(volatile UInt8*)0xfffff503,*(volatile UInt8*)0xfffff504);
    h=DmNewRecord(logDb,&index,StrLen(row)+1);
    if(h) { DmWrite(MemHandleLock(h),0,row,StrLen(row)+1); MemHandleUnlock(h); DmReleaseRecord(logDb,index,true); }
}
static void tone(SndCmdIDType op, Int32 freq, UInt16 ms, UInt16 amp,const Char* tag) {
    SndCommandType c; c.cmd=op;c.param1=freq;c.param2=ms;c.param3=amp;
    record(tag,SndDoCmd(NULL,&c,false));
}
UInt32 PilotMain(UInt16 command, MemPtr parameters, UInt16 flags) {
    EventType e; UInt16 sys,alarm,game,zero=0;
    if(command!=sysAppLaunchCmdNormalLaunch)return 0;
    DmCreateDatabase(0,"FrondDanaSoundLog",'FDSn','DATA',false);
    logDb=DmOpenDatabaseByTypeCreator('DATA','FDSn',dmModeReadWrite);
    if(!logDb)return 1;
    SndGetDefaultVolume(&sys,&alarm,&game);
    SysTaskDelay(50);
    tone(sndCmdFreqDurationAmp,440,300,sndMaxAmp,"blocking440");SysTaskDelay(30);
    tone(sndCmdFrqOn,1000,300,sndMaxAmp,"async1000");SysTaskDelay(60);
    tone(sndCmdNoteOn,69,300,127,"note69");SysTaskDelay(60);
    SndPlaySystemSound(sndClick);record("click",0);SysTaskDelay(30);
    SndPlaySystemSound(sndWarning);record("warning",0);SysTaskDelay(30);
    SndSetDefaultVolume(&zero,&zero,&zero);
    SndPlaySystemSound(sndInfo);SndPlaySystemSound(sndClick);SndPlaySystemSound(sndWarning);
    record("muted",0);SysTaskDelay(50);
    SndSetDefaultVolume(&sys,&alarm,&game);
    tone(sndCmdFrqOn,1200,8000,sndMaxAmp,"pending1200");
    do {
        EvtGetEvent(&e,10);
        if(e.eType==keyDownEvent) {
            Char tag[30];StrPrintF(tag,"key-%x",e.data.keyDown.chr);record(tag,0);
            if(e.data.keyDown.chr=='t')tone(sndCmdFrqOn,1200,8000,sndMaxAmp,"pending1200");
            if(e.data.keyDown.chr=='q')tone(sndCmdQuiet,0,0,0,"quiet");
        }
        SysHandleEvent(&e);
    }while(e.eType!=appStopEvent);
    DmCloseDatabase(logDb);return 0;
}
