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
 auto* owner=new CsrBcspRadio({[](void*)->uint64_t{return 0;},nullptr},nullptr,nullptr);
 auto first=owner->CaptureInput();auto nested=owner->CaptureInput();delete owner;
 const auto before=publications;assert(detach==1);
 nested={};first={};assert(publications==before);
 puts("PASS CSR owner destroyed before nested checkpoint closure release, no UART callback/publication after death");
}
