// Real DMA engine and UDC FIFOs on a small synthetic physical bus.
#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "mem.h"
#include "pxa_DMA.cpp"
#include "pxa255_UDC.cpp"

struct ArmMem {
    struct Region { uint32_t base,size; ArmMemAccessF access; void* context; };
    std::vector<Region> regions;
    std::array<uint8_t,65536> ram{};
};
extern "C" void uarmAbort(){std::abort();}
bool memRegionAdd(ArmMem* m,uint32_t base,uint32_t size,ArmMemAccessF f,void* p){
    m->regions.push_back({base,size,f,p});return true;
}
bool memIsBufferRange(ArmMem*,uint32_t a,uint32_t n,bool){
    return a>=0xa0000000 && uint64_t(a)+n<=0xa0010000;
}
bool memAccess(ArmMem* m,uint32_t a,uint_fast8_t n,bool write,void* p){
    if(memIsBufferRange(m,a,n,write)){
        if(write)std::memcpy(m->ram.data()+a-0xa0000000,p,n);
        else std::memcpy(p,m->ram.data()+a-0xa0000000,n);
        return true;
    }
    for(auto& r:m->regions)if(a>=r.base && uint64_t(a)+n<=uint64_t(r.base)+r.size)
        return r.access(r.context,a,n,write,p);
    return false;
}
template<int N,bool W>bool memAccess(ArmMem* m,uint32_t a,void* p){return memAccess(m,a,N,W,p);}
static bool dmaIrq,usbIrq;
void pxaIcInt(PxaIc*,uint_fast8_t n,bool value){
    assert(n==PXA_I_DMA || n==PXA_I_USB);(n==PXA_I_DMA?dmaIrq:usbIrq)=value;
}
struct Fixture {
    ArmMem mem;
    PxaDma* dma=pxaDmaInit(&mem,{[](void*,uint32_t task){assert(task==RESCHEDULE_TASK_DMA);},nullptr},nullptr);
    Pxa255Udc* udc=pxa255UdcInit(&mem,nullptr,dma);
    Fixture(){
        dmaIrq=usbIrq=false;
        write(0x40600000,1);
        const uint8_t setup[]={0,9,1,0,0,0,0,0};
        assert(pxa255UdcHostSetup(udc,setup)==R::Ack);
        write(0x40600010,0x81);write(0x40600058,255);write(0x4060005c,255);
    }
    ~Fixture(){delete udc;std::free(dma);}
    void write(uint32_t a,uint32_t v,unsigned n=4){assert(memAccess(&mem,a,n,true,&v));}
    uint32_t read(uint32_t a,unsigned n=4){uint32_t v=0;assert(memAccess(&mem,a,n,false,&v));return v;}
    void uw(unsigned off,uint32_t v){write(0x40600000+off,v);}
    uint32_t ur(unsigned off){return read(0x40600000+off);}
    void map(unsigned line,unsigned ch){write(0x40000100+4*line,0x80|ch);}
    void start(unsigned line,unsigned ch,uint32_t source,uint32_t target,unsigned length,bool rx){
        map(line,ch);write(0x40000000+4*ch,0x40000017);
        write(0x40000204+16*ch,source);write(0x40000208+16*ch,target);
        write(0x4000020c+16*ch,(rx?0x60234000u:0x90234000u)|length);
        write(0x40000000+4*ch,0xc0000000);
    }
    void ticks(unsigned n){while(n--)pxaDmaPeriodic(dma);}
    bool request(unsigned ch){return read(0x40000000+4*ch)&0x100;}
    unsigned left(unsigned ch){return read(0x4000020c+16*ch)&0x1fff;}
    void pattern(unsigned begin,unsigned n,unsigned seed){for(unsigned i=0;i<n;++i)mem.ram[begin+i]=uint8_t(i*37+seed);}
    std::vector<uint8_t> in(unsigned ep){
        uint8_t b[64];unsigned n=99;assert(pxa255UdcHostEpIn(udc,ep,b,&n)==R::Ack);return {b,b+n};
    }
    template<class T>void Save(T& s){pxaDmaSave(dma,s);pxa255UdcSave(udc,s);}
    void Load(SavestateLoader<ChunkType>& s){pxaDmaLoad(dma,s);pxa255UdcLoad(udc,s);}
    std::vector<uint8_t> save(){Savestate<ChunkType> s;assert(s.Save(*this));auto* b=static_cast<uint8_t*>(s.GetBuffer());return {b,b+s.GetSize()};}
    bool load(std::vector<uint8_t>& s){SavestateLoader<ChunkType> l;return l.Load(s.data(),s.size(),*this);}
};
static void txChecks(){
    for(unsigned ep:{1u,6u,11u})for(unsigned length:{32u,64u,128u}){
        Fixture f;const unsigned line=ep==1?25:ep==6?30:35;
        f.pattern(0x1000,length,ep);f.start(line,15,0xa0001000,0x40600000+fifoAddress[ep],length,false);
        assert(f.request(15));f.ticks(length/32);
        assert(!f.left(15) && dmaIrq && !(f.read(0x4000003c)&0x80000000));
        std::vector<uint8_t> actual;
        if(length==32){unsigned n;assert(pxa255UdcHostPeekIn(f.udc,ep,&n)==R::Nak);f.uw(0x10+4*ep,0x80);}
        for(unsigned i=0;i<(length+63)/64;++i){auto b=f.in(ep);actual.insert(actual.end(),b.begin(),b.end());}
        assert(actual==std::vector<uint8_t>(f.mem.ram.begin()+0x1000,f.mem.ram.begin()+0x1000+length));
    }
    Fixture f;f.pattern(0x1000,192,19);f.start(30,15,0xa0001000,0x40600600,192,false);
    f.ticks(4);assert(f.left(15)==64 && !f.request(15));
    f.ticks(3);assert(f.left(15)==64);auto first=f.in(6);assert(first.front()==19 && first.back()==uint8_t(63*37+19));
    assert(f.request(15));f.ticks(2);assert(!f.left(15));
    auto second=f.in(6),third=f.in(6);assert(second.front()==uint8_t(64*37+19) && third.back()==uint8_t(191*37+19));
    // TSP after a full packet appends the documented zero-length packet.
    f.start(30,15,0xa0001000,0x40600600,64,false);f.ticks(2);f.uw(0x28,0x80);
    assert(f.in(6).size()==64 && f.in(6).empty());
    // CPU and DMA share the same byte FIFO, including word-sized CPU stores.
    for(unsigned i=0;i<16;++i)f.uw(0x600,0xaabbcc00|i);
    f.start(30,15,0xa0001000,0x40600600,32,false);f.ticks(1);
    assert(!f.request(15));
    for(unsigned i=0;i<16;++i)f.uw(0x600,0x80+i);
    auto mixed=f.in(6);assert(mixed.size()==64);
    for(unsigned i=0;i<16;++i)assert(mixed[i]==i && mixed[48+i]==0x80+i);
    assert(std::memcmp(mixed.data()+16,f.mem.ram.data()+0x1000,32)==0);
    puts("PASS: real byte-width DMA bulk IN 1/6/11, 32/64/128 bytes, short/TSP/zero packets and double-buffer backpressure");
}
static void rxChecks(){
    for(unsigned ep:{2u,7u,12u})for(unsigned length:{32u,64u}){
        Fixture f;const unsigned line=ep==2?26:ep==7?31:36;
        f.pattern(0x1000,length,ep);f.uw(ep<8?0x50:0x54,255&~(1u<<(ep%8)));f.uw(0x10+4*ep,8);
        f.start(line,14,0x40600000+fifoAddress[ep],0xa0002000,length,true);
        assert(!f.request(14));assert(pxa255UdcHostEpOut(f.udc,ep,f.mem.ram.data()+0x1000,length)==R::Ack);
        assert(f.request(14) && !usbIrq);f.ticks(length/32);
        assert(!f.left(14) && !f.request(14) && usbIrq && dmaIrq);
        assert(std::memcmp(f.mem.ram.data()+0x1000,f.mem.ram.data()+0x2000,length)==0);
        assert(!(f.ur(0x10+4*ep)&0x40) && (f.ur(0x10+4*ep)&2));
        f.uw(0x10+4*ep,10);assert(!(f.ur(0x10+4*ep)&3));
    }
    Fixture f;f.pattern(0x1000,128,91);f.uw(0x50,0x7f);f.uw(0x2c,8);
    f.start(31,14,0x40600680,0xa0002000,128,true);
    assert(pxa255UdcHostEpOut(f.udc,7,f.mem.ram.data()+0x1000,64)==R::Ack);
    assert(pxa255UdcHostEpOut(f.udc,7,f.mem.ram.data()+0x1040,64)==R::Ack);
    assert(pxa255UdcHostEpOut(f.udc,7,nullptr,0)==R::Nak);
    f.ticks(2);assert(f.left(14)==64 && !f.request(14));f.ticks(2);assert(f.left(14)==64);
    f.uw(0x2c,10);assert(f.request(14));f.ticks(2);assert(!f.left(14));
    assert(std::memcmp(f.mem.ram.data()+0x1000,f.mem.ram.data()+0x2000,128)==0);
    f.uw(0x2c,10);f.uw(0x58,0x80);
    // DME controls IRQ timing. With DME off, the request still reflects full blocks.
    f.uw(0x2c,0);assert(pxa255UdcHostEpOut(f.udc,7,f.mem.ram.data()+0x1000,64)==R::Ack);
    assert(usbIrq && f.request(14));f.uw(0x2c,2);f.uw(0x58,0x80);
    for(unsigned length:{0u,1u,31u,33u,63u}){
        f.uw(0x2c,8);f.start(31,14,0x40600680,0xa0002000,64,true);
        assert(pxa255UdcHostEpOut(f.udc,7,f.mem.ram.data()+0x1000,length)==R::Ack);
        f.ticks(2);const unsigned dmaBytes=length>=32?32:0;
        assert(f.left(14)==64-dmaBytes && !f.request(14) && usbIrq);
        for(unsigned i=dmaBytes;i<length;++i)assert(f.ur(0x680)==f.mem.ram[0x1000+i]);
        f.uw(0x2c,10);f.uw(0x58,0x80);
    }
    assert(pxa255UdcHostEpOut(f.udc,7,f.mem.ram.data()+0x1000,64)==R::Ack);
    for(unsigned i=0;i<16;++i)assert(f.ur(0x680)==f.mem.ram[0x1000+i]);
    f.start(31,14,0x40600680,0xa0002000,32,true);f.ticks(1);
    assert(std::memcmp(f.mem.ram.data()+0x1010,f.mem.ram.data()+0x2000,32)==0 && !f.request(14));
    for(unsigned i=48;i<64;++i)assert(f.ur(0x680)==f.mem.ram[0x1000+i]);
    f.uw(0x2c,10);
    puts("PASS: real DMA bulk OUT 2/7/12, exact bytes, RPC ownership, DME IRQ timing and CPU short/zero tails");
}
static void mappingAndState(){
    Fixture f;f.pattern(0x1000,128,53);
    // The IN request is already high before a channel is mapped.
    f.map(30,15);assert(f.request(15));f.map(30,14);assert(!f.request(15) && f.request(14));
    f.write(0x40000178,0);assert(!f.request(14));
    f.start(30,15,0xa0001000,0x40600600,128,false);f.ticks(1);
    auto partial=f.save();const auto source=f.mem.ram;
    Fixture fresh;fresh.mem.ram=source;assert(fresh.load(partial));fresh.ticks(3);
    auto a=fresh.in(6),b=fresh.in(6);a.insert(a.end(),b.begin(),b.end());
    assert(a==std::vector<uint8_t>(source.begin()+0x1000,source.begin()+0x1080));
    // A partial receive and its second queued packet resume through the same FIFO.
    Fixture rx;rx.pattern(0x1000,128,71);rx.uw(0x2c,8);
    rx.start(31,14,0x40600680,0xa0002000,128,true);
    assert(pxa255UdcHostEpOut(rx.udc,7,rx.mem.ram.data()+0x1000,64)==R::Ack);
    assert(pxa255UdcHostEpOut(rx.udc,7,rx.mem.ram.data()+0x1040,64)==R::Ack);
    rx.ticks(1);auto partialRx=rx.save();
    Fixture rxFresh;rxFresh.mem.ram=rx.mem.ram;assert(rxFresh.load(partialRx));
    rxFresh.ticks(1);assert(rxFresh.left(14)==64 && !rxFresh.request(14));
    rxFresh.uw(0x2c,10);rxFresh.ticks(2);assert(!rxFresh.left(14));
    assert(std::memcmp(rxFresh.mem.ram.data()+0x1000,rxFresh.mem.ram.data()+0x2000,128)==0);
    // Bus reset discards pending received data and its request, without faking DMA completion.
    rxFresh.uw(0x2c,10);rxFresh.start(31,14,0x40600680,0xa0002000,64,true);
    assert(pxa255UdcHostEpOut(rxFresh.udc,7,rxFresh.mem.ram.data()+0x1000,64)==R::Ack);
    assert(rxFresh.request(14) && pxa255UdcHostReset(rxFresh.udc) && !rxFresh.request(14));
    rxFresh.ticks(2);assert(rxFresh.left(14)==64);
    // Real chained descriptors move a full packet, then write TSP for its ZLP.
    f.uw(0x28,4);f.write(0x4000003c,0x17);
    const uint32_t chain[]={0xa0004010,0xa0001000,0x40600600,0x90234040,1,0xa0004100,0x40600028,0x8021c004};
    std::memcpy(f.mem.ram.data()+0x4000,chain,sizeof(chain));uint32_t tsp=0x80;std::memcpy(f.mem.ram.data()+0x4100,&tsp,4);
    f.write(0x400002f0,0xa0004000);f.write(0x4000003c,0x80000000);f.ticks(3);
    assert(f.in(6).size()==64 && f.in(6).empty() && dmaIrq);
    // Controller reset removes its levels, without completing a waiting transfer.
    f.start(30,15,0xa0001000,0x40600600,128,false);f.ticks(1);
    f.uw(0,0);assert(!f.request(15));const auto left=f.left(15);f.ticks(3);assert(f.left(15)==left);
    uint32_t value=0x12;assert(!memAccess(&f.mem,0x40600210,4,true,&value));
    puts("PASS: request mapping/rebind, fresh partial FIFO+DMA restore, real descriptor chain, reset and unchanged reserved-register refusal");
}
int main(){txChecks();rxChecks();mappingAndState();}
