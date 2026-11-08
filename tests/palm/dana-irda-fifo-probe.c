/* Dana UART2 diagnostic, separate from the guest Exchange Manager transfer probe. */
#include <PalmOS.h>
#include <IrLib.h>
#define B(a) (*(volatile UInt8*)(a))
#define W(a) (*(volatile UInt16*)(a))
static void listen(void) {
    UInt16 ref,length=1; Boolean scan=false;
    if(!SysLibFind(irLibName,&ref)) ExgLibControl(ref,irSetScanningMode,&scan,&length);
    W(0xfffff306)|=0x1000;
    W(0xfffff910)=0;
    B(0xfffff430)|=0x10; B(0xfffff433)|=0x10; B(0xfffff431)&=~0x10;
    B(0xfffff43b)&=~0x30;
    /* The ROM's UART2 driver uses this 9600 baud, 8N1 SIR configuration. */
    W(0xfffff912)=0x020b; W(0xfffff916)=0x0800; W(0xfffff918)=0x0060;
    W(0xfffff910)=0xe508;
}
static void readFifo(void) {
    UInt8 bytes[64]; UInt16 count=0,value,index=dmMaxRecordIndex;
    LocalID id=DmFindDatabase(0,"FrondDanaIRFifo"); DmOpenRef db; MemHandle h; void* p;
    while(count<64) { value=W(0xfffff914); if(!(value&0x2000))break; bytes[count++]=(UInt8)value; }
    if(id) DmDeleteDatabase(0,id);
    if(DmCreateDatabase(0,"FrondDanaIRFifo",'FrIF','DATA',false))return;
    id=DmFindDatabase(0,"FrondDanaIRFifo");db=DmOpenDatabase(0,id,dmModeReadWrite);if(!db)return;
    h=DmNewRecord(db,&index,count?count:1);
    if(h) {p=MemHandleLock(h);if(count)DmWrite(p,0,bytes,count);MemHandleUnlock(h);DmReleaseRecord(db,index,true);}
    DmCloseDatabase(db);
}
UInt32 PilotMain(UInt16 command,MemPtr parameters,UInt16 flags) {
    EventType event;
    if(command!=sysAppLaunchCmdNormalLaunch)return 0;
    EvtSetAutoOffTimer(SetAtLeast,300);FrmGotoForm(1000);
    do {
        EvtGetEvent(&event,SysTicksPerSecond());EvtResetAutoOffTimer();
        if(!SysHandleEvent(&event)) {
            if(event.eType==frmLoadEvent) {FrmSetActiveForm(FrmInitForm(event.data.frmLoad.formID));FrmDrawForm(FrmGetActiveForm());}
            else if(event.eType==ctlSelectEvent && event.data.ctlSelect.controlID==1001)listen();
            else if(event.eType==ctlSelectEvent && event.data.ctlSelect.controlID==1002)readFifo();
            else if(event.eType==keyDownEvent && event.data.keyDown.chr=='r')readFifo();
            else FrmHandleEvent(FrmGetActiveForm(),&event);
        }
    } while(event.eType!=appStopEvent);
    W(0xfffff910)=0;B(0xfffff431)|=0x10;
    FrmCloseAllForms();return 0;
}
