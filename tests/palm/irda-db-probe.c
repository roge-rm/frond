/* SDK record-database probe; the original OS serializes and imports the PDB. */
#include <PalmOS.h>
#include <ExgMgr.h>
#define CREATOR 'FrPD'
#define DATA_CREATOR 'FrRD'
static const Char name[]="Frond5FRecord";
static const Char identity[]="Frond 5f original record database beam\nTX and E2 native Exchange Manager\n";
static void note(const Char* stage,Err error,UInt32 bytes){
 LocalID id=DmFindDatabase(0,"FrondDBBeamLog");DmOpenRef db;UInt16 i=dmMaxRecordIndex;MemHandle h;Char line[96];
 if(!id){if(DmCreateDatabase(0,"FrondDBBeamLog",CREATOR,'DATA',false))return;id=DmFindDatabase(0,"FrondDBBeamLog");}
 db=DmOpenDatabase(0,id,dmModeReadWrite);if(!db)return;
 StrPrintF(line,"%s err=%04x bytes=%lu",stage,(UInt16)error,bytes);h=DmNewRecord(db,&i,StrLen(line)+1);
 if(h){void* p=MemHandleLock(h);DmWrite(p,0,line,StrLen(line)+1);MemHandleUnlock(h);DmReleaseRecord(db,i,true);}DmCloseDatabase(db);
}
static Err create(void){
 DmOpenRef db;LocalID id=DmFindDatabase(0,name);Err e;UInt16 i=dmMaxRecordIndex,j;MemHandle h;void* p;UInt8 b[256];UInt32 at;
 if(id)return 1;
 e=DmCreateDatabase(0,name,DATA_CREATOR,'DATA',false);if(e)return e;
 id=DmFindDatabase(0,name);db=DmOpenDatabase(0,id,dmModeReadWrite);if(!db)return 2;
 h=DmNewRecord(db,&i,sizeof(identity));if(!h){DmCloseDatabase(db);return 3;}
 p=MemHandleLock(h);e=DmWrite(p,0,identity,sizeof(identity));MemHandleUnlock(h);DmReleaseRecord(db,i,true);
 i=dmMaxRecordIndex;h=e?0:DmNewRecord(db,&i,4096);if(!h){DmCloseDatabase(db);return e?e:4;}
 p=MemHandleLock(h);
 for(at=0;at<4096 && !e;at+=sizeof(b)){
  for(j=0;j<sizeof(b);++j){UInt32 n=at+j;b[j]=(UInt8)((n*73+17)^((n>>8)*31));}
  e=DmWrite(p,at,b,sizeof(b));
 }
 MemHandleUnlock(h);DmReleaseRecord(db,i,true);DmCloseDatabase(db);return e;
}
struct Sending {ExgSocketType socket;UInt32 bytes;};
static Err writeDatabase(const void* p,UInt32* count,void* context){
 struct Sending* s=(struct Sending*)context;Err e=0;UInt32 offered=*count;
 *count=ExgSend(&s->socket,p,offered,&e);s->bytes+=*count;
 if(!e && ((offered && !*count) || *count>offered))return 5;return e;
}
static void beam(void){
 struct Sending s;Err e,d;LocalID id;
 MemSet(&s,sizeof(s),0);e=create();note("create",e,0);if(e)return;
 id=DmFindDatabase(0,name);s.socket.name=exgBeamPrefix "Frond5FRecord.frdb";s.socket.description="Frond record database";s.socket.type="application/x-frond-pdb";s.socket.target=CREATOR;
 e=ExgPut(&s.socket);note("put",e,0);if(e)return;
 e=ExgDBWrite(writeDatabase,&s,0,id,0);note("write",e,s.bytes);
 d=ExgDisconnect(&s.socket,e);note("disconnect",d,s.bytes);
 if(!e && !d)note("complete",0,s.bytes);
}
struct Receiving {ExgSocketPtr socket;UInt32 bytes;};
static Err readDatabase(void* p,UInt32* count,void* context){
 struct Receiving* r=(struct Receiving*)context;Err e=0;UInt32 offered=*count;
 *count=ExgReceive(r->socket,p,offered,&e);r->bytes+=*count;
 if(*count>offered)return 6;return e;
}
static Boolean refuseReplacement(const Char* n,UInt16 v,UInt16 card,LocalID id,void* context){return false;}
static void receive(ExgSocketPtr socket){
 struct Receiving r;LocalID id=0;Boolean reset=false;Err e,d;
 r.socket=socket;r.bytes=0;e=ExgAccept(socket);note("accept",e,0);
 if(!e)e=ExgDBRead(readDatabase,refuseReplacement,&r,&id,0,&reset,true);
 note("read",e,r.bytes);d=ExgDisconnect(socket,e);note("receive-disconnect",d,r.bytes);
 if(!e && !d && id && !reset)note("receive-complete",0,r.bytes);
}
UInt32 PilotMain(UInt16 command,MemPtr parameters,UInt16 flags){
 EventType event;Err e;
 if(command==sysAppLaunchCmdExgAskUser){((ExgAskParamPtr)parameters)->result=exgAskOk;return 0;}
 if(command==sysAppLaunchCmdExgReceiveData){receive((ExgSocketPtr)parameters);return 0;}
 if(command!=sysAppLaunchCmdNormalLaunch)return 0;
 e=ExgRegisterDatatype(CREATOR,exgRegTypeID,"application/x-frond-pdb","Frond record database",0);note("register",e,0);
 FrmGotoForm(1000);
 do{EvtGetEvent(&event,SysTicksPerSecond());EvtResetAutoOffTimer();
  if(!SysHandleEvent(&event)){
   if(event.eType==frmLoadEvent){FrmSetActiveForm(FrmInitForm(event.data.frmLoad.formID));FrmDrawForm(FrmGetActiveForm());}
   else if(event.eType==ctlSelectEvent && event.data.ctlSelect.controlID==1002)beam();
   else FrmHandleEvent(FrmGetActiveForm(),&event);
  }
 }while(event.eType!=appStopEvent);FrmCloseAllForms();return 0;
}
