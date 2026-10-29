// Exercise the PXA255's distinct register/FIFO behavior without firmware.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "pxa255_UDC.cpp"
static ArmMemAccessF mmio;
static bool interrupt;
extern "C" void uarmAbort(){std::abort();}
bool memRegionAdd(ArmMem*,uint32_t base,uint32_t size,ArmMemAccessF fn,void*){
    assert(base==0x40600000 && size==0x1000);mmio=fn;return true;
}
void pxaIcInt(PxaIc*,uint_fast8_t n,bool level){assert(n==PXA_I_USB);interrupt=level;}
struct Fixture {
    Pxa255Udc* u=pxa255UdcInit(nullptr,nullptr,nullptr);
    ~Fixture(){delete u;}
    void write(unsigned off,uint32_t val,unsigned size=4){assert(mmio(u,0x40600000+off,size,true,&val));}
    uint32_t read(unsigned off,unsigned size=4){uint32_t val=0;assert(mmio(u,0x40600000+off,size,false,&val));return val;}
    void setup(uint8_t type,uint8_t request,uint16_t value=0,uint16_t index=0,uint16_t length=0){
        uint8_t s[]={type,request,uint8_t(value),uint8_t(value>>8),uint8_t(index),uint8_t(index>>8),uint8_t(length),uint8_t(length>>8)};
        assert(pxa255UdcHostSetup(u,s)==R::Ack);
    }
    template<class T>void Save(T& state){pxa255UdcSave(u,state);}
    void Load(SavestateLoader<ChunkType>& loader){pxa255UdcLoad(u,loader);}
    std::vector<uint8_t> save(){Savestate<ChunkType> state;assert(state.Save(*this));auto p=static_cast<uint8_t*>(state.GetBuffer());return {p,p+state.GetSize()};}
    bool load(std::vector<uint8_t>& bytes){SavestateLoader<ChunkType> loader;return loader.Load(bytes.data(),bytes.size(),*this);}
};
struct Legacy {
    template<class T>void Save(T& state){auto* c=state.GetChunk(ChunkType::pxa255udc,0);assert(c);c->Put32(123);c->Put32(0x00ffffa1);}
};
int main(){
    Fixture f;uint8_t data[64]{};unsigned count=99;
    assert(f.read(0)==0xa0 && f.read(0x50)==255 && !interrupt);
    assert(pxa255UdcHostIn(f.u,data,&count)==R::Disabled);
    f.write(0,1);assert(pxa255UdcHostEnabled(f.u));
    assert(!pxa255UdcHostListening(f.u));f.write(0x50,0xfe);assert(pxa255UdcHostListening(f.u));
    assert(pxa255UdcHostReset(f.u) && interrupt && (f.read(0)&0x40));
    f.write(0,0x41);assert(!interrupt && !(f.read(0)&0x40));
    f.write(0x50,255);f.setup(0x80,6,0x100,0,18);
    assert(!f.read(0x58) && !interrupt);f.write(0x50,0xfe);assert(!interrupt);
    f.setup(0x80,6,0x100,0,18);assert(f.read(0x10)==0xc1 && interrupt);
    assert(f.read(0x80)==0x80 && f.read(0x80,1)==6);
    auto partial=f.save();assert(f.read(0x80)==0 && f.load(partial) && f.read(0x80)==0);
    f.write(0x10,0x81);f.write(0x58,1);assert(!interrupt);
    // Each word access moves one byte; full EP0 packets commit automatically.
    for(unsigned i=0;i<16;++i)f.write(0x80,0xabcdef00+i);
    assert(!(f.read(0x10)&2)); // IPR always reads zero.
    assert(pxa255UdcHostIn(f.u,data,&count)==R::Ack && count==16 && data[15]==15);
    f.write(0x58,1);f.write(0x10,2);
    assert(pxa255UdcHostIn(f.u,data,&count)==R::Ack && count==0);
    assert(pxa255UdcHostOut(f.u,nullptr,0)==R::Ack);f.write(0x10,1);f.write(0x58,1);
    // Hardware SET_ADDRESS does not reach the guest and applies after status IN.
    f.setup(0,5,2);assert(!interrupt && !pxa255UdcHostAddress(f.u));
    assert(pxa255UdcHostIn(f.u,data,&count)==R::Ack && count==0 && pxa255UdcHostAddress(f.u)==2);
    f.setup(0,5,0);assert(pxa255UdcHostIn(f.u,data,&count)==R::Ack && !pxa255UdcHostAddress(f.u));
    f.setup(0,5,2);assert(pxa255UdcHostIn(f.u,data,&count)==R::Ack && pxa255UdcHostAddress(f.u)==2);
    f.write(8,4);f.setup(0,9,1);assert(interrupt && f.read(0x10)==0xc1);
    for(unsigned i=0;i<8;++i)f.read(0x80);f.write(0x10,0x81);f.write(0x58,1);
    assert(pxa255UdcHostIn(f.u,data,&count)==R::Nak);f.write(8,0x84);
    assert(pxa255UdcHostIn(f.u,data,&count)==R::Ack);
    // Fixed bulk IN 1 and OUT 2; two packets, short packets, zero-length packets.
    f.write(0x50,0xf8);
    for(unsigned i=0;i<128;++i)f.write(0x100,i);
    assert(!(f.read(0x14)&1));assert(pxa255UdcHostEpIn(f.u,0x81,data,&count)==R::Ack && count==64 && data[63]==63);
    assert(f.read(0x14)&3);f.write(0x14,2);f.write(0x58,2);
    assert(pxa255UdcHostEpIn(f.u,1,data,&count)==R::Ack && data[0]==64);
    f.write(0x14,0x80);assert(pxa255UdcHostEpIn(f.u,1,data,&count)==R::Ack && count==0);
    const uint8_t bytes[]={42,43,44};
    assert(pxa255UdcHostEpOut(f.u,2,bytes,3)==R::Ack);
    assert(pxa255UdcHostEpOut(f.u,2,nullptr,0)==R::Ack);
    assert(pxa255UdcHostEpOut(f.u,2,bytes,3)==R::Nak);
    assert(f.read(0x68)==2 && (f.read(0x18)&0xc3)==0xc3);
    assert(f.read(0x180)==42);auto state=f.save();
    assert(f.read(0x180)==43 && f.load(state) && f.read(0x180)==43 && f.read(0x68)==0);
    assert(f.read(0x180)==44 && !(f.read(0x18)&0x40));f.write(0x18,2);
    assert((f.read(0x18)&0x83)==0x83);f.write(0x18,2);assert(!(f.read(0x18)&3));
    f.setup(0x02,3,0,0x81);assert(pxa255UdcHostEpIn(f.u,1,data,&count)==R::Stall);
    f.setup(0x02,1,0,0x81);assert(pxa255UdcHostEpIn(f.u,1,data,&count)==R::Nak);
    f.setup(1,11,1);assert(pxa255UdcHostIn(f.u,data,&count)==R::Stall);
    f.write(0,0xa0);assert(pxa255UdcHostEpIn(f.u,1,data,&count)==R::Disabled && !f.u->fifo[1].count);
    // A pending SOF must not hold the interrupt line high after UDE=0.
    f.write(0,1);f.write(0x60,0);assert(pxa255UdcHostFrame(f.u) && interrupt);
    f.write(0,0xa0);assert(!interrupt && f.read(0)==0xa0 && f.read(0x60)==0x40);
    assert(f.read(0x50)==255 && f.read(0x54)==255);
    Legacy old;Savestate<ChunkType> legacy;assert(legacy.Save(old));
    auto p=static_cast<uint8_t*>(legacy.GetBuffer());std::vector<uint8_t> legacyBytes(p,p+legacy.GetSize());
    assert(f.load(legacyBytes) && f.u->reg4==123 && !f.u->fifo[2].count);
    f.u->fifo[2].head=9;auto corrupt=f.save();f.u->fifo[2].head=0;
    assert(!f.load(corrupt) && f.u->fifo[2].head==0);
    puts("PASS: PXA255 EP0 hardware requests, fixed FIFOs, IRQ masks/W1C, backpressure, legacy/partial restore and corrupt-state refusal");
}
