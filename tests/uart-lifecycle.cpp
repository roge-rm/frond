// Firmware-free timing, disabled-unit and checkpoint callback contracts.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>
#include "pxa_UART.h"
#include "savestate/savestateAll.h"

static ArmMemAccessF access;
static void* context;
static bool irq;
static unsigned reschedules;
extern "C" void uarmAbort() { std::abort(); }
bool memRegionAdd(ArmMem*, uint32_t, uint32_t, ArmMemAccessF f, void* p) { access=f; context=p; return true; }
void pxaIcInt(PxaIc*, uint_fast8_t number, bool value) { assert(number==PXA_I_FFUART); irq=value; }
struct Fixture {
    PxaUart* uart;
    uint64_t now=0;
    unsigned reads=0, active=0, ticks=0;
    std::deque<uint16_t> incoming;
    std::vector<uint16_t> outgoing;
    std::vector<uint64_t> sentAt;
    Fixture() {
        uart=pxaUartInit(nullptr,{[](void*,uint32_t id){assert(id==RESCHEDULE_TASK_UART);++reschedules;},nullptr},
                         nullptr,PXA_FFUART_BASE,PXA_I_FFUART,true);
        pxaUartSetFuncs(uart,[](void* p)->uint_fast16_t {
            auto& f=*static_cast<Fixture*>(p);++f.reads;
            if(f.incoming.empty())return UART_CHAR_NONE;
            auto c=f.incoming.front();f.incoming.pop_front();return c;
        },[](uint_fast16_t c,void* p){auto& f=*static_cast<Fixture*>(p);f.outgoing.push_back(c);f.sentAt.push_back(f.now);},
        [](void* p){auto& f=*static_cast<Fixture*>(p);++f.active;return !f.incoming.empty();},
        [](void* p){++static_cast<Fixture*>(p)->ticks;},this);
        configure(96,3);
    }
    ~Fixture(){free(uart);}
    void write(unsigned reg,uint32_t value,unsigned size=4){assert(access(context,PXA_FFUART_BASE+4*reg,size,true,&value));}
    uint32_t read(unsigned reg){uint32_t value=0;assert(access(context,PXA_FFUART_BASE+4*reg,4,false,&value));return value;}
    void configure(unsigned divisor,unsigned lcr) {
        write(3,0x80|lcr);write(0,divisor&0xff);write(1,divisor>>8);write(3,lcr);
        write(4,8);write(1,0x41);write(2,0x27);
    }
    void step(unsigned count){for(unsigned i=0;i<count;++i){++now;pxaUartProcess(uart);}}
    template<class T> void Save(T& state){pxaUartSave(uart,state);}
    void Load(SavestateLoader<ChunkType>& state){pxaUartLoad(uart,state);}
};
static void paced(unsigned divisor,unsigned lcr,unsigned halfBits) {
    Fixture f;f.configure(divisor,lcr);
    // 64-byte FIFO plus shift register; queue valid full words without overflow.
    const unsigned mask=(1u<<(5+(lcr&3)))-1;
    for(unsigned i=0;i<64;i+=4)f.write(0,(i&mask)|(((i+1)&mask)<<8)|(((i+2)&mask)<<16)|(((i+3)&mask)<<24));
    const uint32_t units=divisor*halfBits;
    const unsigned duration=(64*units+17)/18;
    f.step(duration);
    assert(f.outgoing.size()==64 && (f.read(5)&0x40));
    for(unsigned i=0;i<64;++i){
        assert(f.outgoing[i]==(i&mask));
        if(f.sentAt[i]!=((i+1)*units+17)/18)std::fprintf(stderr,"cadence divisor=%u lcr=%x i=%u actual=%llu expected=%u\n",divisor,lcr,i,(unsigned long long)f.sentAt[i],((i+1)*units+17)/18);
        assert(f.sentAt[i]==((i+1)*units+17)/18); // no cumulative ceil-per-byte drift
    }
}
// RX bytes arrive one frame apart at the programmed baud and LCR, not all at once.
static void receivePaced(unsigned divisor,unsigned lcr,unsigned halfBits) {
    Fixture f;f.configure(divisor,lcr);
    for(unsigned i=0;i<40;++i)f.incoming.push_back(i&((1u<<(5+(lcr&3)))-1));
    std::vector<uint64_t> at;
    while(at.size()<40){
        f.step(1);for(auto n=f.read(9);at.size()<n;)at.push_back(f.now);
        assert(f.now<40ull*divisor*halfBits/18+100);
    }
    const double units=double(divisor)*halfBits/18;
    for(unsigned k=0;k<40;++k){
        const double expected=1+k*units;
        if(at[k]+2<expected||at[k]>expected+2)std::fprintf(stderr,"rx divisor=%u lcr=%x k=%u at=%llu expected=%.2f\n",divisor,lcr,k,(unsigned long long)at[k],expected);
        assert(at[k]+2>=expected && at[k]<=expected+2);
    }
    assert(!(f.read(5)&2)); // no overrun
}
int main() {
    receivePaced(96,3,20);    // 9600 8N1
    receivePaced(96,7,22);    // 9600 8N2
    receivePaced(96,0x0e,22); // 9600 7E2
    receivePaced(8,3,20);     // 115200 8N1
    receivePaced(1,0,14);     // fastest 5N1: sometimes two bytes per batch
    puts("PASS: RX paced at the programmed baud and frame size with fractional carry");
    {
        // ITL 8, RAVIE+RTOIE: three bytes raise no data IRQ; timeout after four character times.
        Fixture f;f.write(2,0x67);f.write(1,0x51);f.incoming={1,2,3};
        while(f.read(9)<3)f.step(1);
        const auto last=f.now;const unsigned timeout=(4*10*96+8)/9;
        while(f.now<last+timeout-1){f.step(1);assert((f.read(2)&15)==1 && !irq);}
        f.step(2);assert((f.read(2)&15)==0x0c && irq);
        uint32_t one=0;assert(access(context,PXA_FFUART_BASE,1,false,&one) && one==1);
        assert((f.read(2)&15)==1 && !irq && f.read(9)==2); // a FIFO read restarts the timeout
        f.step(timeout-2);assert((f.read(2)&15)==1);f.step(3);assert((f.read(2)&15)==0x0c);
        for(unsigned i=0;i<6;++i)f.incoming.push_back(0x10+i);
        while(f.read(9)<8)f.step(1);
        assert((f.read(2)&15)==4 && irq); // trigger level reached
    }
    puts("PASS: RX trigger level and character timeout follow guest time; FIFO read restarts the timeout");
    {
        // A guest that does not drain overruns: the FIFO keeps 64 bytes, OE clears on LSR read.
        Fixture f;f.configure(8,3);f.write(2,0xe7);f.write(1,0x45);
        for(unsigned i=0;i<70;++i)f.incoming.push_back(i);
        while(!f.incoming.empty())f.step(1);
        assert(f.read(9)==64 && (f.read(2)&15)==6 && irq);
        const auto lsr=f.read(5);assert((lsr&3)==3 && !(lsr&0x80));
        assert(!(f.read(5)&2) && (f.read(2)&15)==4);
        for(unsigned i=0;i<64;++i){uint32_t c=0;assert(access(context,PXA_FFUART_BASE,1,false,&c) && c==i);}
        assert(f.read(9)==0 && !(f.read(5)&1));
        // Non-FIFO mode: the new byte is lost and the holding register kept.
        f.write(2,0);f.incoming={0x61,0x62};while(!f.incoming.empty())f.step(1);
        assert((f.read(5)&3)==3 && (f.read(2)&15)==4 && f.read(0)==0x61);
    }
    puts("PASS: RX overrun when the guest does not drain; OE line status and clear on LSR read");
    {
        // Save mid-frame: restore gives the same arrival ticks as the uninterrupted run.
        Fixture f;f.incoming={0x31,0x32,0x33,0x34,0x35,0x36};
        while(f.read(9)<2)f.step(1);
        f.step(30);
        Savestate<ChunkType> saved;assert(saved.Save(f));
        const auto pending=f.incoming;const auto start=f.now;
        auto record=[&]{std::vector<uint32_t> levels;for(unsigned i=0;i<800;++i){f.step(1);levels.push_back(f.read(9)|(f.read(2)<<8));}return levels;};
        const auto first=record();
        assert(f.read(9)==6 && f.incoming.empty());
        SavestateLoader<ChunkType> loader;assert(loader.Load(saved.GetBuffer(),saved.GetSize(),f));
        f.incoming=pending;f.now=start;
        assert(record()==first && f.read(9)==6);
        for(unsigned i=0;i<6;++i){uint32_t c=0;assert(access(context,PXA_FFUART_BASE,1,false,&c) && c==0x31+i);}
    }
    puts("PASS: save/load mid-frame keeps RX pacing, FIFO and timeout phase");
    paced(96,7,22);       // real TX Serial Manager defaults: 9600 8N2
    paced(96,3,20);       // 9600 8N1
    paced(12,0x1f,24);    // 76800 8E2
    paced(96,4,15);       // 9600 5N1.5, exact half stop-bit
    paced(1,0,14);        // fastest 5N1 can finish two characters in one batch
    paced(65535,3,20);    // slowest divisor, no overflow
    puts("PASS: baud/framing TX cadence, fractional carry, maximum divisor and fast short frames");
    for(unsigned fcr : {0x25u,0u}) {
        Fixture f;f.write(0,0x04030201);f.step(40);
        f.write(2,fcr);assert(!(f.read(5)&0x40));
        f.step(66);assert(f.outgoing.empty());f.step(1);
        assert(f.outgoing==std::vector<uint16_t>{1});f.step(1000);
        assert(f.outgoing.size()==1);
    }
    puts("PASS: TX FIFO reset/mode change drops queued bytes while the current shift frame completes");
    {
        Fixture f;f.write(0,0x41);f.step(40);
        const auto reads=f.reads,ticks=f.ticks;
        f.write(1,0);f.incoming={0x80};const auto active=f.active;
        assert(!pxaUartTaskRequired(f.uart));f.step(200);
        assert(f.reads==reads && f.ticks==ticks && f.active==active && !irq && f.outgoing.empty());
        f.write(1,0x41);f.step(66);assert(f.outgoing.empty());f.step(1);
        assert(f.outgoing==std::vector<uint16_t>{0x41} && f.read(9)==1 && irq);
        f.write(1,0);assert(!irq && f.read(9)==1);
        f.write(1,0x41);assert(irq && f.read(0)==0x80 && !irq);
    }
    {
        Fixture f;f.write(0,0x5a);f.step(40);
        f.write(3,0x83);f.write(0,0);f.write(1,0);f.write(3,3);
        const auto reads=f.reads,ticks=f.ticks;f.incoming={0x33};
        assert(!pxaUartTaskRequired(f.uart));f.step(1000);
        assert(f.outgoing.empty() && f.reads==reads && f.ticks==ticks);
        f.write(3,0x83);f.write(0,96);f.write(3,3);
        f.step(66);assert(f.outgoing.empty());f.step(1);assert(f.outgoing.size()==1);
        assert(f.read(0)==0x33);
    }
    puts("PASS: disabled unit/zero divisor suppress callbacks and pause TX; pending RX/IRQ survives enable");
    {
        Fixture f;f.write(0,0x41);f.step(40);f.incoming={0x80};
        Savestate<ChunkType> saved;assert(saved.Save(f));
        const auto reads=f.reads,ticks=f.ticks,active=f.active;
        pxaUartBeginCheckpoint(f.uart);
        pxaUartBeginCheckpoint(f.uart);
        f.write(0,0x42);assert(!pxaUartTaskRequired(f.uart));f.step(10000);
        pxaUartEndCheckpoint(f.uart);f.step(10000);
        assert(f.reads==reads && f.ticks==ticks && f.active==active && f.outgoing.empty());
        SavestateLoader<ChunkType> loader;assert(loader.Load(saved.GetBuffer(),saved.GetSize(),f));
        assert(!pxaUartTaskRequired(f.uart));
        pxaUartEndCheckpoint(f.uart);
        f.step(66);assert(f.outgoing.empty());f.step(1);
        assert(f.outgoing==std::vector<uint16_t>{0x41} && f.read(0)==0x80);
        assert(f.incoming.empty());
    }
    puts("PASS: nested UART checkpoints suppress every callback and restore exact partial-frame phase");
}
