#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "pxa_DMA.h"

static ArmMemAccessF accessRegister;
static void* controller;
static uint8_t source[16], destination[16];
bool memRegionAdd(ArmMem*,uint32_t,uint32_t,ArmMemAccessF f,void* context){accessRegister=f;controller=context;return true;}
void pxaIcInt(PxaIc*,uint_fast8_t,bool){}
extern "C" void uarmAbort(){abort();}
bool memAccess(ArmMem*,uint32_t address,uint_fast8_t size,bool write,void* data){
    uint8_t* storage=nullptr;
    if(address>=0x1000 && uint64_t(address)+size<=0x1010)storage=source+address-0x1000;
    if(address>=0x2000 && uint64_t(address)+size<=0x2010)storage=destination+address-0x2000;
    if(!storage)return false;
    if(write)memcpy(storage,data,size);else memcpy(data,storage,size);return true;
}
template<int size,bool write> bool memAccess(ArmMem* mem,uint32_t address,void* data){return memAccess(mem,address,size,write,data);}
template bool memAccess<4,false>(ArmMem*,uint32_t,void*);
static void writeReg(unsigned offset,uint32_t value){assert(accessRegister(controller,0x40000000+offset,4,true,&value));}
static uint32_t readReg(unsigned offset){uint32_t value=0;assert(accessRegister(controller,0x40000000+offset,4,false,&value));return value;}
static void configure(){
    writeReg(0,0x40000000); // no descriptor fetch, not running
    writeReg(0x204,0x1000);writeReg(0x208,0x2000);
    writeReg(0x20c,0xe000c004); // increment both addresses; source flow; four-byte width and length
}
int main(){
    Reschedule reschedule{[](void*,uint32_t){},nullptr};
    auto* dma=pxaDmaInit(nullptr,reschedule,nullptr);
    source[0]=0x11;source[1]=0x22;source[2]=0x33;source[3]=0x44;
    writeReg(0x100+21*4,0x80);configure();
    pxaDmaExternalReq(dma,21,true);
    assert(readReg(0)&0x100);
    writeReg(0,0xc0000000);
    assert(readReg(0)&0x100); // RUN must retain an already asserted peripheral request.
    pxaDmaPeriodic(dma);assert(!memcmp(source,destination,4));
    pxaDmaExternalReq(dma,21,false);memset(destination,0,sizeof(destination));configure();
    writeReg(0,0xc0000000);pxaDmaPeriodic(dma);assert(destination[0]==0);
    pxaDmaExternalReq(dma,21,true);pxaDmaPeriodic(dma);assert(!memcmp(source,destination,4));
    puts("PASS: DMA honours requests asserted before and after RUN without transferring while deasserted");
}
