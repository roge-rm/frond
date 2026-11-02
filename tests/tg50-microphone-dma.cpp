// A production host source reaches RAM through request2 and real DMA descriptors.
#define main existingDmaChecks
#include "pxa-i2s-dma.cpp"
#undef main
#include "i2sdev_AK4534.h"
static I2cDeviceActionF i2c;
static void *codec;
bool pxaI2cDeviceAdd(PxaI2c *,I2cDeviceActionF f,void *p){i2c=f;codec=p;return true;}
static void reg(unsigned address,unsigned value){
    i2c(codec,i2cStart,0);assert(i2c(codec,i2cTx,0x20));assert(i2c(codec,i2cTx,address));assert(i2c(codec,i2cTx,value));i2c(codec,i2cStop,0);
}
int main(){
    Reschedule reschedule{[](void *,uint32_t){},nullptr};
    auto *dma=pxaDmaInit(nullptr,reschedule,nullptr);auto *link=pxaI2sInit(nullptr,nullptr,dma);
    auto *ak=ak4534Init(nullptr,link,nullptr);unsigned samples=0;
    Ak4534InputSource source{&samples,[](void*p,Ak4534InputRoute r,uint32_t n,uint32_t d)->int16_t{
        assert(r==Ak4534InputRoute::internalMic && n==44100 && d==1);return (++*static_cast<unsigned*>(p)&1)?-1000:1000;
    },nullptr,nullptr};ak4534SetInputSource(ak,&source);
    reg(0,0x83);reg(1,0x60);reg(4,0x6a);reg(7,0x0c);wr(0x40400000,0x7701);
    for(unsigned i=0;i<2081;++i){pxaI2sPeriodic(link);assert(rd(0x40400080)==0);}
    assert(samples==2081);
    ram[0]=1;ram[1]=0x40400080;ram[2]=0x1200;ram[3]=0x6063c020;
    wr(0x40000100+DMA_CMR_I2S_RX*4,0x81);wr(0x40000210,0x1000);wr(0x40000004,0x80000000);
    for(unsigned i=0;i<8;++i)pxaI2sPeriodic(link);pxaDmaPeriodic(dma);
    for(unsigned i=0;i<8;++i){assert(ram[128+i]>>16==0);const int value=int16_t(ram[128+i]);assert(i&1 ? value>=990 && value<=1010 : value<=-990 && value>=-1010);}
    assert((rd(0x40000004)&0x80000004)==4 && dmaIrq);
    ak4534Destroy(ak);
}
