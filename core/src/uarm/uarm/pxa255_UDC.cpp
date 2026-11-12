//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr
#include "pxa255_UDC.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "cputil.h"
#include "savestate/savestateAll.h"

// Intel PXA255 Developer's Manual (March 2003), chapter 12. Fixed endpoint
// directions and FIFO addresses differ from the PXA270's programmable endpoints.
struct Pxa255Udc {
    PxaDma* dma;
    PxaIc* ic;
    uint32_t reg4{};
    uint8_t ccr{0xa0}, uicr0{0xff}, uicr1{0xff};
    uint8_t cfr{}, sir[2]{}, fnHigh{0x40};
    uint16_t frame{};
    uint8_t csr[16]{};
    struct Fifo { uint8_t data[2][64]{}, size[2]{}, head{}, count{}, fill{}, pos{}; } fifo[16];
    uint8_t rx0[16]{}, tx0[16]{}, rxPos{}, rxSize{}, txSize{};
    bool ready0{};
    uint8_t reservedLogged[0x1000/32]{}; // Bitmap of reserved word offsets already logged.
    bool clockEnabled{true}; // Derived from the connected clock manager, not portable UDC state.
    uint8_t address{}, pendingAddress{}, configuration{}, remoteWake{};
    // Hardware-decoded request: data IN, status IN, status OUT, or stall.
    uint8_t hw{}, hwLength{}, hwData[2]{};
    bool gated{};
    uint32_t enableCount{};
    void (*notify)(void*){};
    void* notifyContext{};

    template<class T> void DoSaveLoad(T& h, uint32_t version) {
        // Preserve the original eight-byte v0 chunk prefix.
        h.Do32(reg4).Do(typename T::Pack8() << ccr << uicr0 << uicr1);
        if (!version) return;
        h.Do8(cfr).DoBuffer(sir,2).Do8(fnHigh).Do16(frame).DoBuffer(csr,16);
        for(auto& f:fifo) h.DoBuffer(f.data,sizeof(f.data)).DoBuffer(f.size,2)
            .Do8(f.head).Do8(f.count).Do8(f.fill).Do8(f.pos);
        h.DoBuffer(rx0,16).DoBuffer(tx0,16).Do8(rxPos).Do8(rxSize).Do8(txSize)
            .DoBool(ready0).Do8(address).Do8(pendingAddress).Do8(configuration).Do8(remoteWake)
            .Do8(hw).Do8(hwLength).DoBuffer(hwData,2).DoBool(gated);
    }
};
using R=Pxa270UdcHostResult;
namespace {
constexpr unsigned fifoAddress[16]={0x80,0x100,0x180,0x200,0x400,0xa0,0x600,0x680,0x700,0x900,0xc0,0xb00,0xb80,0xc00,0xe00,0xe0};
bool inEp(unsigned ep) {return ep && (ep%5==1 || ep%5==0);}
bool outEp(unsigned ep) {return ep && ep%5==2;}
unsigned packetSize(unsigned ep) {return ep%5==0?8:64;}
constexpr unsigned bulkEp[]={1,2,6,7,11,12};
constexpr unsigned bulkRequest[]={25,26,30,31,35,36};
bool dmaRequested(Pxa255Udc* u,unsigned ep) {
    if(!u->clockEnabled || !(u->ccr&1))return false;
    const auto& f=u->fifo[ep];
    if(inEp(ep))return f.count<2 && 64-f.fill>=32;
    return f.count && f.size[f.head]-f.pos>=32;
}
void refreshDma(Pxa255Udc* u) {
    if(u->dma)for(unsigned i=0;i<6;++i)
        pxaDmaExternalReq(u->dma,bulkRequest[i],dmaRequested(u,bulkEp[i]));
}
template<unsigned Ep>bool dmaLevel(void* context) {
    return dmaRequested(static_cast<Pxa255Udc*>(context),Ep);
}
template<unsigned Ep>uint_fast8_t dmaBytes(void* context) {
    auto* u=static_cast<Pxa255Udc*>(context);
    if(!dmaRequested(u,Ep))return 0;
    const auto& f=u->fifo[Ep];
    return inEp(Ep)?64-f.fill:f.size[f.head]-f.pos;
}
// DME delays the packet interrupt until fewer than 32 bytes remain. RPC
// remains set until software acknowledges it and exposes the next buffer.
void receiveEvent(Pxa255Udc* u,unsigned ep);
void irq(Pxa255Udc* u) {
    refreshDma(u);
    pxaIcInt(u->ic,PXA_I_USB, (u->sir[0]&~u->uicr0) || (u->sir[1]&~u->uicr1) ||
        ((u->ccr&0x40) && !(u->ccr&0x80)) || ((u->ccr&0x18) && !(u->ccr&0x20)) ||
        ((u->fnHigh&0x80) && !(u->fnHigh&0x40)));
}
void event(Pxa255Udc* u,unsigned ep) {
    const uint8_t mask=ep<8?u->uicr0:u->uicr1;
    if(!(mask&(1u<<(ep%8))))u->sir[ep/8]|=1u<<(ep%8);
    irq(u);
}
void receiveEvent(Pxa255Udc* u,unsigned ep) {
    const auto& f=u->fifo[ep];
    if(f.count && (!(u->csr[ep]&8) || f.size[f.head]-f.pos<32))event(u,ep);
}
void clearPackets(Pxa255Udc* u) {
    for(auto& f:u->fifo)f={};
    std::memset(u->csr,0,sizeof(u->csr));
    u->rxPos=u->rxSize=u->txSize=0;u->ready0=false;u->hw=0;u->gated=false;
    u->address=u->pendingAddress=u->configuration=u->remoteWake=0;
    u->sir[0]=u->sir[1]=0;
}
void commit(Pxa255Udc::Fifo& f) {
    if(f.count==2)return;
    f.size[(f.head+f.count)&1]=f.fill;++f.count;f.fill=0;
}
void consume(Pxa255Udc::Fifo& f) {if(f.count){f.head^=1;--f.count;f.pos=0;}}
uint8_t status(Pxa255Udc* u,unsigned ep) {
    if(!ep)return (u->csr[0]&0xb1) | (u->remoteWake?8:0) | (u->rxPos<u->rxSize?0x40:0);
    const auto& f=u->fifo[ep];
    if(inEp(ep))return (u->csr[ep]&0x3a) | (f.count<2?1:0);
    if(outEp(ep))return (u->csr[ep]&0x38) | (f.count?3:0) |
        (f.count && f.pos<f.size[f.head]?0x40:0) | (f.count && f.size[f.head]<64?0x80:0);
    return 0; // Isochronous endpoints are not connected to this host transport.
}
R stall(Pxa255Udc* u,unsigned ep) {
    u->csr[ep]|=0x10;event(u,ep);return R::Stall;
}
R peek(Pxa255Udc* u,unsigned ep,unsigned* size) {
    if(!pxa255UdcHostEnabled(u))return R::Disabled;
    if(ep & 0x70)return R::Timeout;
    ep &= 15;
    if(ep && !inEp(ep))return R::Timeout;
    if((u->csr[ep]&0x20) || (!ep && u->hw==4))return R::Stall;
    if(!ep) {
        if(u->hw==1){*size=u->hwLength;return R::Ack;}
        if(u->hw==2 || u->hw==5){
            if(u->gated && (u->cfr&4) && !(u->cfr&0x80))return R::Nak;
            *size=0;return R::Ack;
        }
        if(u->csr[0]&1)return R::Nak;
        if(!u->ready0)return R::Nak;
        *size=u->txSize;return R::Ack;
    }
    if(!u->configuration)return R::Timeout;
    auto& f=u->fifo[ep];if(!f.count)return R::Nak;
    *size=f.size[f.head];return R::Ack;
}
bool access(void* ctx,uint32_t pa,uint_fast8_t size,bool write,void* buf) {
    auto* u=static_cast<Pxa255Udc*>(ctx);unsigned off=pa-0x40600000;
    if(size!=1 && size!=2 && size!=4)return false;
    uint32_t value=0;if(write)std::memcpy(&value,buf,size);
    int ep=-1;
    for(unsigned i=0;i<16;++i)if(off==fifoAddress[i]){ep=i;break;}
    if(ep>=0) {
        // PXA25x UDDR registers transfer exactly one byte, even on word accesses.
        if(!(u->ccr&1))value=0;
        else if(!ep) {
            if(write) {if(!u->ready0 && u->txSize<16){u->tx0[u->txSize++]=value;if(u->txSize==16)u->ready0=true;}}
            else value=u->rxPos<u->rxSize?u->rx0[u->rxPos++]:0;
        } else {
            auto& f=u->fifo[ep];
            if(write && inEp(ep)) {
                if(f.count<2 && f.fill<packetSize(ep)) {
                    f.data[(f.head+f.count)&1][f.fill++]=value;
                    if(f.fill==packetSize(ep))commit(f);
                }
            } else if(!write) {
                const unsigned remaining=f.count?f.size[f.head]-f.pos:0;
                value=outEp(ep) && remaining?f.data[f.head][f.pos++]:0;
                if(outEp(ep) && (u->csr[ep]&8) && remaining==32)receiveEvent(u,ep);
            }
        }
    } else if(off==0) {
        if(write) {
            const bool enabled=u->ccr&1;
            u->ccr=(u->ccr&0x58&~value) | (u->ccr&2) | (value&0xa1);
            if(!(value&1)) {
                // UDE=0 resets the entire UDC, including pending SOF/USB IRQs.
                clearPackets(u);u->ccr=value&0xa0;u->cfr=0;
                u->uicr0=u->uicr1=0xff;u->fnHigh=0x40;u->frame=0;
            }
            else if(!enabled){++u->enableCount;u->ccr|=2;}
        } else value=u->ccr;
    } else if(off==4) {if(write)u->reg4=value;else value=u->reg4;}
    else if(off==8) {if(write)u->cfr=value&0x84;else value=u->cfr;}
    else if(off>=0x10 && off<0x50 && !(off&3)) {
        const unsigned e=(off-0x10)/4;auto& f=u->fifo[e];
        if(write && (u->ccr&1)) {
            if(!e) {
                u->csr[0]&=~(value&0x91);
                if(value&4){u->txSize=0;u->ready0=false;}
                if(value&2)u->ready0=true;
                if(value&0x20)u->csr[0]|=0x20;
            } else if(inEp(e)) {
                u->csr[e]&=~(value&0x1a);
                u->csr[e]=(u->csr[e]&~0x20)|(value&0x20);
                if(value&4)f={};
                if(value&0x80)commit(f);
            } else if(outEp(e)) {
                const bool wasDma=u->csr[e]&8;
                u->csr[e]=(u->csr[e]&~0x38)|(value&0x28)|(u->csr[e]&0x10&~value);
                if(value&2)consume(f);
                if((value&2) || wasDma!=bool(u->csr[e]&8))receiveEvent(u,e);
            }
        } else if(!write)value=status(u,e);
    } else if(off==0x50 || off==0x54) {
        auto& mask=off==0x50?u->uicr0:u->uicr1;if(write)mask=value;else value=mask;
    } else if(off==0x58 || off==0x5c) {
        auto& bits=u->sir[(off-0x58)/4];if(write)bits&=~value;else value=bits;
    } else if(off==0x60) {
        if(write)u->fnHigh=(u->fnHigh&0x80&~value)|(value&0x40);
        else value=u->fnHigh|((u->frame>>8)&7);
    } else if(off==0x64) {if(!write)value=u->frame&255;}
    else if(off>=0x68 && off<=0x7c && !(off&3)) {
        constexpr unsigned eps[]={2,4,7,9,12,14};const auto& f=u->fifo[eps[(off-0x68)/4]];
        if(!write)value=f.count && f.pos<f.size[f.head]?f.size[f.head]-f.pos-1:0;
    } else {
        // Reserved UDC space. The manual (2.8) calls access unpredictable; Sony's TG50 USB
        // driver stores to 0x40600210 and HotSyncs on real hardware, so as a judgement call
        // aligned word accesses to offsets no register decodes read zero and ignore writes.
        // Anything else is refused.
        if(size!=4 || (off&3) || off>=0x1000)return false;
        uint8_t& seen=u->reservedLogged[off>>5];const uint8_t bit=1u<<((off>>2)&7);
        if(!(seen&bit)) {
            seen|=bit;
            fprintf(stderr,"UDC: ignoring %s at reserved 0x%08lx\n",write?"write":"read",(unsigned long)pa);
        }
        if(!write)std::memset(buf,0,size);
        return true;
    }
    refreshDma(u);
    if(write){irq(u);if(u->notify)u->notify(u->notifyContext);}
    else std::memcpy(buf,&value,size);
    return true;
}
}

Pxa255Udc* pxa255UdcInit(ArmMem* mem,PxaIc* ic,PxaDma* dma) {
    auto* u=new Pxa255Udc{};u->ic=ic;u->dma=dma;
    if(!memRegionAdd(mem,0x40600000,0x1000,access,u))ERR("cannot add UDC to MEM");
    if(dma) {
        const PxaDmaPeripheral bulk[]={
            {dmaBytes<1>,nullptr,nullptr,u,0,dmaLevel<1>},
            {dmaBytes<2>,nullptr,nullptr,u,0,dmaLevel<2>},
            {dmaBytes<6>,nullptr,nullptr,u,0,dmaLevel<6>},
            {dmaBytes<7>,nullptr,nullptr,u,0,dmaLevel<7>},
            {dmaBytes<11>,nullptr,nullptr,u,0,dmaLevel<11>},
            {dmaBytes<12>,nullptr,nullptr,u,0,dmaLevel<12>}};
        for(unsigned i=0;i<6;++i)pxaDmaSetPeripheral(dma,bulkRequest[i],&bulk[i]);
    }
    return u;
}
void pxa255UdcSetClockEnabled(Pxa255Udc* u,bool enabled,bool refresh) {
    if(!u || u->clockEnabled==enabled)return;
    u->clockEnabled=enabled;
    if(refresh) {
        refreshDma(u);
        if(u->notify)u->notify(u->notifyContext);
    }
}
bool pxa255UdcHostEnabled(const Pxa255Udc* u){return u && u->clockEnabled && (u->ccr&1);}
bool pxa255UdcHostListening(const Pxa255Udc* u){return pxa255UdcHostEnabled(u) && !(u->uicr0&1) && !(u->ccr&0x80);}
uint32_t pxa255UdcHostEnableCount(const Pxa255Udc* u){return u->enableCount;}
bool pxa255UdcHostSofWanted(const Pxa255Udc* u){return !(u->fnHigh&0x40);}
uint8_t pxa255UdcHostAddress(const Pxa255Udc* u){return u->address;}
void pxa255UdcHostSetNotify(Pxa255Udc* u,void(*notify)(void*),void* context){u->notify=notify;u->notifyContext=context;}
bool pxa255UdcHostReset(Pxa255Udc* u){
    if(!pxa255UdcHostEnabled(u))return false;
    clearPackets(u);u->frame=0;u->ccr|=0x42;irq(u);return true;
}
bool pxa255UdcHostFrame(Pxa255Udc* u){
    if(!pxa255UdcHostEnabled(u))return false;
    u->frame=(u->frame+1)&2047;u->fnHigh|=0x80;irq(u);return true;
}
R pxa255UdcHostPeekIn(Pxa255Udc* u,uint8_t ep,unsigned* size){return peek(u,ep,size);}
R pxa255UdcHostSetup(Pxa255Udc* u,const uint8_t s[8]) {
    if(!pxa255UdcHostEnabled(u))return R::Disabled;
    u->txSize=0;u->ready0=false;u->rxPos=u->rxSize=0;u->csr[0]=0;u->hw=0;u->gated=false;u->cfr&=~0x80;
    const unsigned value=s[2]|s[3]<<8,index=s[4]|s[5]<<8,length=s[6]|s[7]<<8;
    auto data=[&](unsigned v){u->hw=1;u->hwLength=std::min(length,2u);u->hwData[0]=v;u->hwData[1]=v>>8;};
    const unsigned recipient=s[0]&31;
    if(!(s[0]&0x60)) {
        if(s[1]==5 && s[0]==0 && value<128 && !index && !length){u->pendingAddress=value;u->hw=5;return R::Ack;}
        if(s[1]==0 && (s[0]&0x80) && recipient<=2){data(recipient==0?u->remoteWake*2:recipient==2 && (index&15)?bool(u->csr[index&15]&0x20):0);return R::Ack;}
        if(s[1]==8 && s[0]==0x80){data(u->configuration);u->hwLength=std::min(length,1u);return R::Ack;}
        if(s[1]==10 && s[0]==0x81){data(0);u->hwLength=std::min(length,1u);return R::Ack;}
        if((s[1]==1 || s[1]==3) && !length) {
            if(recipient==0 && value==1)u->remoteWake=s[1]==3;
            else if(recipient==2 && !value && (index&15)) {
                auto& csr=u->csr[index&15];if(s[1]==3)csr|=0x20;else csr&=~0x30;
            } else {u->hw=4;return R::Ack;}
            u->hw=2;return R::Ack;
        }
        if(s[1]==9 || s[1]==11) {
            if((s[1]==9 && (s[0]!=0 || value>3)) || (s[1]==11 && (s[0]!=1 || value)) || length){u->hw=4;return R::Ack;}
            if(s[1]==9)u->configuration=value;
            for(unsigned i=1;i<16;++i){u->fifo[i]={};u->csr[i]=0;}
            u->hw=2;u->gated=true;
            // Unlike PXA270, these two hardware-decoded requests also reach software.
        }
    }
    std::memcpy(u->rx0,s,8);u->rxSize=8;u->csr[0]=0x81;event(u,0);return R::Ack;
}
R pxa255UdcHostOut(Pxa255Udc* u,const uint8_t* data,unsigned size) {
    if(!pxa255UdcHostEnabled(u))return R::Disabled;
    if(size>16 || (size && !data))return R::Timeout;
    if((u->csr[0]&0x20) || u->hw==4)return stall(u,0);
    if(u->csr[0]&1)return R::Nak;
    u->txSize=0;u->ready0=false;
    if(u->hw==3 && !size){u->hw=0;return R::Ack;}
    if(size)std::memcpy(u->rx0,data,size);
    u->rxPos=0;u->rxSize=size;u->csr[0]|=1;event(u,0);return R::Ack;
}
R pxa255UdcHostIn(Pxa255Udc* u,uint8_t data[16],unsigned* size) {
    const auto r=peek(u,0,size);if(r!=R::Ack)return r==R::Stall?stall(u,0):r;
    if(u->hw==1){std::memcpy(data,u->hwData,*size);u->hw=3;return R::Ack;}
    if(u->hw==2 || u->hw==5){if(u->hw==5){u->address=u->pendingAddress;u->pendingAddress=0;}u->hw=0;return R::Ack;}
    std::memcpy(data,u->tx0,*size);u->txSize=0;u->ready0=false;event(u,0);return R::Ack;
}
R pxa255UdcHostEpIn(Pxa255Udc* u,uint8_t ep,uint8_t data[64],unsigned* size) {
    if(ep & 0x70)return R::Timeout;
    ep &= 15;
    const auto r=peek(u,ep,size);if(r!=R::Ack)return r==R::Stall?stall(u,ep):r;
    if(!ep)return pxa255UdcHostIn(u,data,size);
    auto& f=u->fifo[ep];std::memcpy(data,f.data[f.head],*size);consume(f);u->csr[ep]|=2;event(u,ep);return R::Ack;
}
R pxa255UdcHostEpOut(Pxa255Udc* u,uint8_t ep,const uint8_t* data,unsigned size) {
    if(!pxa255UdcHostEnabled(u))return R::Disabled;
    if(ep>=16 || !outEp(ep) || !u->configuration || size>64 || (size && !data))return R::Timeout;
    if(u->csr[ep]&0x20)return stall(u,ep);
    auto& f=u->fifo[ep];if(f.count==2)return R::Nak;
    const unsigned tail=(f.head+f.count)&1;if(size)std::memcpy(f.data[tail],data,size);
    f.size[tail]=size;++f.count;receiveEvent(u,ep);refreshDma(u);return R::Ack;
}
template<class T>void pxa255UdcSave(Pxa255Udc* u,T& state){
    auto* chunk=state.GetChunk(ChunkType::pxa255udc,1);if(!chunk)ERR("cannot allocate UDC state");
    SaveChunkHelper h(*chunk);u->DoSaveLoad(h,1);
}
void pxa255UdcLoad(Pxa255Udc* u,SavestateLoader<ChunkType>& loader){
    uint32_t version;auto* chunk=loader.GetChunkOrFail(ChunkType::pxa255udc,1,"pxa255udc",version);if(!chunk)return;
    Pxa255Udc restored{};restored.dma=u->dma;restored.ic=u->ic;restored.notify=u->notify;restored.notifyContext=u->notifyContext;restored.enableCount=u->enableCount;restored.clockEnabled=u->clockEnabled;
    LoadChunkHelper h(*chunk);restored.DoSaveLoad(h,version);
    bool valid=!chunk->HasError() && restored.rxPos<=restored.rxSize && restored.rxSize<=16 && restored.txSize<=16 &&
        restored.address<128 && restored.pendingAddress<128 && restored.configuration<=3 && restored.remoteWake<=1 && restored.hw<=5 && restored.hwLength<=2;
    for(auto& f:restored.fifo)valid=valid && f.head<2 && f.count<=2 && f.fill<=64 && f.pos<=64 && f.size[0]<=64 && f.size[1]<=64 && (!f.count || f.pos<=f.size[f.head]);
    if(!valid){loader.NotifyError();return;}
    *u=restored;irq(u);
}
template void pxa255UdcSave<Savestate<ChunkType>>(Pxa255Udc*,Savestate<ChunkType>&);
template void pxa255UdcSave<SavestateProbe<ChunkType>>(Pxa255Udc*,SavestateProbe<ChunkType>&);
