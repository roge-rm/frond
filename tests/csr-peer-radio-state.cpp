#include "CsrBcspRadio.h"
#include <cassert>
#include <cstdio>
static unsigned detach{},publications{};
void pxaUartSetPacing(PxaUart*,bool){}
void pxaUartSetFuncs(PxaUart*,PxaUartReadF r,PxaUartWriteF w,PxaUartClientIsActive a,PxaUartClientTick t,void*){if(!r && !w && !a && !t)++detach;}
void pxaUartSetModeObserver(PxaUart*,void(*)(void*),void*){}
bool pxaUartSetModemInputs(PxaUart*,uint8_t){++publications;return true;}
PxaIrState pxaUartIrState(const PxaUart*){return {921600,0x1b,false,false,true};}
bool pxaUartReceiverEnabled(const PxaUart*){return true;}
bool pxaUartPlainEnabled(const PxaUart*){return true;}
void pxaUartKick(PxaUart*){}
uint8_t pxaUartGetModemOutputs(const PxaUart*){return 3;}
bool tg50ucGpioIsOutput(const TG50uc*,unsigned,unsigned){return false;}
bool tg50ucGpioState(const TG50uc*,unsigned,unsigned){return false;}
int main(){
 CsrBcspRadio owner({[](void*)->uint64_t{return 0;},nullptr},nullptr,nullptr);
 auto modern=owner.Save();assert(modern[0]==3&&owner.Load(modern));
 auto legacy=modern;legacy[0]=1;legacy.erase(legacy.begin()+4,legacy.begin()+7);assert(owner.Load(legacy)&&owner.Save()==modern);
 auto v2=modern;v2[0]=2;v2.erase(v2.begin()+6);assert(owner.Load(v2)&&owner.Save()==modern);
 for(unsigned at:{1,2,3,4,5,6}){auto bad=modern;bad[at]=2;assert(!owner.Load(bad)&&owner.Save()==modern);}
 puts("PASS v1/v2 radio compatibility and v3 bounded deferred-external flags refusal");
}
