#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include "pxa_DMA.h"

static ArmMemAccessF accessRegister;
static void* controller;
static uint8_t source[16], destination[16];
static bool buffers=true;
static unsigned reads=0,writes=0;
bool memRegionAdd(ArmMem*,uint32_t,uint32_t,ArmMemAccessF f,void* context){accessRegister=f;controller=context;return true;}
void pxaIcInt(PxaIc*,uint_fast8_t,bool){}
extern "C" void uarmAbort(){abort();}
bool memAccess(ArmMem*,uint32_t address,uint_fast8_t size,bool write,void* data){
    uint8_t* storage=nullptr;
    if(address>=0x1000 && uint64_t(address)+size<=0x1010)storage=source+address-0x1000;
    if(address>=0x2000 && uint64_t(address)+size<=0x2010)storage=destination+address-0x2000;
    if(!storage)return false;
    if(write)++writes;else ++reads;
    if(write)memcpy(storage,data,size);else memcpy(data,storage,size);return true;
}
bool memIsBufferRange(ArmMem*,uint32_t address,uint32_t size,bool write){
    const uint64_t end=uint64_t(address)+size;
    return buffers && size && (address>=0x1000 && end<=0x1010 || address>=0x2000 && end<=0x2010);
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
    alarm(10); // bound regression failures in the non-flow burst loop
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
    // Unsupported partial peripheral beats must stop with BUSERR without a
    // process abort, FIFO side effects, count underflow or an infinite burst loop.
    configure();writeReg(0x20c,0xe000c003);writeReg(0,0xc0000000);
    pxaDmaExternalReq(dma,21,true);pxaDmaPeriodic(dma);
    assert((readReg(0)&0x80000009u)==9 && (readReg(0x20c)&0x1fff)==3);
    assert(readReg(0xf0)&1); // BUSERR drives DINT regardless of StopIrqEn
    writeReg(0,0x40000001);assert(!(readReg(0xf0)&1));
    memset(destination,0,sizeof(destination));configure();writeReg(0,0xc0000000);
    pxaDmaExternalReq(dma,21,true);pxaDmaPeriodic(dma);assert(!memcmp(source,destination,4));
    puts("PASS: unsupported partial peripheral beat reports BUSERR/STOP and recovers without abort");
    for(unsigned width=0;width<=3;++width)for(unsigned length=1;length<16;++length){
        writeReg(0,0x40000017);configure();
        for(unsigned i=0;i<16;++i){source[i]=uint8_t(0x80+i);destination[i]=0xa5;}
        writeReg(0x20c,0xc0210000|(width<<14)|length);writeReg(0,0xc0000000);
        pxaDmaPeriodic(dma);
        assert((readReg(0)&0x8000000du)==12 && !(readReg(0x20c)&0x1fff));
        assert(readReg(0x204)==0x1000+length && readReg(0x208)==0x2000+length);
        for(unsigned i=0;i<16;++i)assert(destination[i]==(i<length?source[i]:0xa5));
    }
    puts("PASS: documented WIDTH=00 and legacy-width RAM tails copy exact lengths and preserve guard bytes");
    writeReg(0,0x40000017);configure();reads=writes=0;
    writeReg(0x20c,0xe0010003);writeReg(0,0xc0000000);pxaDmaPeriodic(dma);
    assert((readReg(0)&0x80000009u)==9 && !reads && !writes);
    // A partial MMIO beat cannot be split; reject before access. In a non-flow
    // loop a stopped channel must not execute the failed burst again.
    writeReg(0,0x40000017);configure();buffers=false;reads=writes=0;
    memset(destination,0xa5,sizeof(destination));
    writeReg(0x20c,0xc001c00b);writeReg(0,0xc0000000);pxaDmaPeriodic(dma);
    assert((readReg(0)&0x80000009u)==9 && (readReg(0x20c)&0x1fff)==3);
    assert(reads==2 && writes==2);for(unsigned i=0;i<3;++i)pxaDmaPeriodic(dma);
    assert(reads==2 && writes==2 && destination[8]==0xa5);buffers=true;
    writeReg(0,0x40000017);configure();reads=writes=0;
    writeReg(0x204,0xfffffffc);writeReg(0x20c,0xc001c004);writeReg(0,0xc0000000);
    pxaDmaPeriodic(dma);assert((readReg(0)&0x80000009u)==9 && !reads && !writes);
    writeReg(0,0x40000017);configure();reads=writes=0;
    writeReg(0x208,0x3000);writeReg(0x20c,0xc001c004);writeReg(0,0xc0000000);
    pxaDmaPeriodic(dma);assert((readReg(0)&0x80000009u)==9 && reads==1 && !writes);
    for(unsigned i=0;i<3;++i)pxaDmaPeriodic(dma);
    assert(reads==1 && !writes && (readReg(0x20c)&0x1fff)==4);
    puts("PASS: reserved width and unsupported MMIO tails stop without shifts, repeats or out-of-range access");
    puts("PASS: failed full-width target access consumes the source once and terminates the non-flow loop");
    puts("PASS: DMA honours requests asserted before and after RUN without transferring while deasserted");
}
