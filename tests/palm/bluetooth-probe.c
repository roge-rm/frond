/* Original Exchange Manager probe. The ROM owns discovery, Bluetooth RFCOMM and OBEX. */
#include <PalmOS.h>
#include <ExgMgr.h>
#include <BtExgLib.h>
#define CREATOR 'FrBO'
#define PAYLOAD_SIZE 4096
static void logLine(const Char* text) {
    DmOpenRef db; LocalID id=DmFindDatabase(0,"FrondBTExgLog");
    UInt16 index=dmMaxRecordIndex;MemHandle h;void* p;
    if(!id){if(DmCreateDatabase(0,"FrondBTExgLog",CREATOR,'DATA',false))return;id=DmFindDatabase(0,"FrondBTExgLog");}
    db=DmOpenDatabase(0,id,dmModeReadWrite);if(!db)return;
    h=DmNewRecord(db,&index,StrLen(text)+1);
    if(h){p=MemHandleLock(h);DmWrite(p,0,text,StrLen(text)+1);MemHandleUnlock(h);DmReleaseRecord(db,index,true);}
    DmCloseDatabase(db);
}
static void result(const Char* stage,Err error,UInt32 bytes) {
    Char line[96];StrPrintF(line,"%s err=%04x bytes=%lu",stage,(UInt16)error,bytes);logLine(line);
}
static void sendPayload(void) {
    ExgSocketType socket;Err error=0,disconnected;UInt32 offset=0,n,size;UInt8 buffer[256];UInt16 i;
    MemSet(&socket,sizeof(socket),0);
    socket.name=btexgScheme ":FrondBTPayload.frbt";socket.description="Frond Bluetooth test";
    socket.type="application/x-frond-bt";socket.length=PAYLOAD_SIZE;socket.target=CREATOR;
    logLine("send-begin");error=ExgPut(&socket);result("put",error,0);
    if(error)return;
    while(!error && offset<PAYLOAD_SIZE){
        for(i=0;i<sizeof(buffer);++i){UInt32 at=offset+i;buffer[i]=(UInt8)((at*73+17)^((at>>8)*31));}
        size=PAYLOAD_SIZE-offset;if(size>sizeof(buffer))size=sizeof(buffer);
        n=ExgSend(&socket,buffer,size,&error);
        if(n>size){error=1;break;}
        if(!n && !error){error=1;break;}offset+=n;
    }
    result("send",error,offset);disconnected=ExgDisconnect(&socket,error);
    result("disconnect",disconnected,offset);
    if(!error && !disconnected && offset==PAYLOAD_SIZE)logLine("send-complete");
}
static void receivePayload(ExgSocketPtr socket) {
    Err error;UInt32 total=0,n;UInt8 buffer[256];DmOpenRef db=0;LocalID id;
    UInt16 index=dmMaxRecordIndex;MemHandle record=0;void* ptr=0;
    logLine("receive-begin");error=ExgAccept(socket);result("accept",error,0);
    if(!error){
        id=DmFindDatabase(0,"FrondBTReceived");if(id)DmDeleteDatabase(0,id);
        error=DmCreateDatabase(0,"FrondBTReceived",CREATOR,'DATA',false);
        if(!error){id=DmFindDatabase(0,"FrondBTReceived");db=DmOpenDatabase(0,id,dmModeReadWrite);if(!db)error=1;}
        if(!error){record=DmNewRecord(db,&index,PAYLOAD_SIZE);if(!record)error=1;else ptr=MemHandleLock(record);}
    }
    while(!error){
        n=ExgReceive(socket,buffer,sizeof(buffer),&error);
        if(n>sizeof(buffer) || total+n>PAYLOAD_SIZE){error=1;break;}
        if(n){DmWrite(ptr,total,buffer,n);total+=n;}else break;
    }
    if(record){MemHandleUnlock(record);DmReleaseRecord(db,index,true);}
    if(db)DmCloseDatabase(db);
    result("receive",error,total);
    { Err disconnected=ExgDisconnect(socket,error);result("receive-disconnect",disconnected,total);
      if(!error && !disconnected && total==PAYLOAD_SIZE)logLine("receive-complete"); }
}
UInt32 PilotMain(UInt16 command,MemPtr parameters,UInt16 flags) {
    EventType event;Err error;
    if(command==sysAppLaunchCmdExgAskUser){((ExgAskParamPtr)parameters)->result=exgAskOk;return 0;}
    if(command==sysAppLaunchCmdExgReceiveData){receivePayload((ExgSocketPtr)parameters);return 0;}
    if(command!=sysAppLaunchCmdNormalLaunch)return 0;
    error=ExgRegisterDatatype(CREATOR,exgRegTypeID,"application/x-frond-bt","Frond Bluetooth payload",0);result("register",error,0);
    EvtSetAutoOffTimer(SetAtLeast,300);
    FrmGotoForm(1000);
    do{
        EvtGetEvent(&event,5);
        EvtResetAutoOffTimer();
        if(!SysHandleEvent(&event)){
            if(event.eType==frmLoadEvent){FrmSetActiveForm(FrmInitForm(event.data.frmLoad.formID));FrmDrawForm(FrmGetActiveForm());}
            else if(event.eType==frmOpenEvent || event.eType==frmUpdateEvent)FrmDrawForm(FrmGetActiveForm());
            else if(event.eType==ctlSelectEvent && event.data.ctlSelect.controlID==1002){
                sendPayload();
                if(FrmGetActiveForm())FrmDrawForm(FrmGetActiveForm());
            }
            else FrmHandleEvent(FrmGetActiveForm(),&event);
        }
    }while(event.eType!=appStopEvent);
    FrmCloseAllForms();return 0;
}
