// Actual PXA DMA descriptors service the I2S FIFOs through their normal MMIO path.
#include <array>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "pxa_I2S.h"
struct Region { uint32_t base, size; ArmMemAccessF access; void *context; };
static std::vector<Region> regions;
static uint32_t ram[256];
static bool dmaIrq;
static std::vector<uint32_t> transmitted;
bool memRegionAdd(ArmMem *, uint32_t base, uint32_t size, ArmMemAccessF access, void *context) {
    regions.push_back({base,size,access,context}); return true;
}
bool memAccess(ArmMem *, uint32_t address, uint_fast8_t size, bool write, void *data) {
    if (address >= 0x1000 && uint64_t(address)+size <= 0x1400) {
        auto *at = reinterpret_cast<uint8_t *>(ram)+address-0x1000;
        if (write) std::memcpy(at,data,size); else std::memcpy(data,at,size);
        return true;
    }
    for (auto &r : regions) if (address >= r.base && uint64_t(address)+size <= uint64_t(r.base)+r.size)
        return r.access(r.context,address,size,write,data);
    return false;
}
bool memIsBufferRange(ArmMem *, uint32_t address, uint32_t size, bool) {
    return address >= 0x1000 && uint64_t(address)+size <= 0x1400;
}
template <int size, bool write> bool memAccess(ArmMem *mem, uint32_t address, void *data) {
    return memAccess(mem,address,size,write,data);
}
template bool memAccess<4,false>(ArmMem *,uint32_t,void *);
void pxaIcInt(PxaIc *, uint_fast8_t number, bool value) { if (number == PXA_I_DMA) dmaIrq = value; }
extern "C" void uarmAbort() { std::abort(); }
static void wr(uint32_t address, uint32_t value) { assert(memAccess(nullptr,address,4,true,&value)); }
static uint32_t rd(uint32_t address) { uint32_t value{}; assert(memAccess(nullptr,address,4,false,&value)); return value; }
static uint32_t frame(void *, uint32_t tx, bool, bool, uint32_t, uint32_t) {
    transmitted.push_back(tx);
    return 0xface0000 + transmitted.size();
}
int main() {
    Reschedule reschedule{[](void *,uint32_t){},nullptr};
    auto *dma = pxaDmaInit(nullptr,reschedule,nullptr);
    auto *link = pxaI2sInit(nullptr,nullptr,dma);
    pxaI2sAttach(link,nullptr,frame,nullptr); pxaI2sSetExternalRate(link,44100);
    for (unsigned i=0;i<16;++i) ram[64+i]=0x80001200+i;
    // Two TX descriptors, each eight packed stereo words. Stop at the second link.
    ram[0]=0x1010;ram[1]=0x1100;ram[2]=0x40400080;ram[3]=0x9063c020;
    ram[4]=1;ram[5]=0x1120;ram[6]=0x40400080;ram[7]=0x9063c020;
    wr(0x40000100+DMA_CMR_I2S_TX*4,0x80);
    wr(0x40000200,0x1000);wr(0x40000000,0x80000000);
    pxaDmaPeriodic(dma); assert((rd(0x4000020c)&0x1fff)==32);
    wr(0x40400000,0x7701);
    for (unsigned i=0;i<16;++i) { pxaDmaPeriodic(dma); pxaI2sPeriodic(link); }
    for (unsigned i=0;i<16;++i) assert(transmitted[i]==0x80001200+i);
    assert((rd(0x40000000)&0x80000004)==4 && dmaIrq);
    // RX descriptor moves injected serial words into RAM through request2.
    wr(0x40400004,8);wr(0x40400004,0);
    ram[8]=1;ram[9]=0x40400080;ram[10]=0x1200;ram[11]=0x6063c020;
    wr(0x40000100+DMA_CMR_I2S_RX*4,0x81);
    wr(0x40000210,0x1020);wr(0x40000004,0x80000000);
    for (unsigned i=0;i<8;++i) pxaI2sPeriodic(link);
    pxaDmaPeriodic(dma);
    for (unsigned i=0;i<8;++i) assert(ram[128+i]==0xface0011+i);
    assert((rd(0x40000004)&0x80000004)==4);
    return 0;
}
