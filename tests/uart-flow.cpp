#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>
#include "pxa_UART.h"

static ArmMemAccessF access;
static void* context;
static bool irq;
static uint_fast8_t expectedIrq=PXA_I_FFUART;
static unsigned reschedules;
extern "C" void uarmAbort(){std::abort();}
bool memRegionAdd(ArmMem*,uint32_t,uint32_t,ArmMemAccessF f,void* p){access=f;context=p;return true;}
void pxaIcInt(PxaIc*,uint_fast8_t number,bool value){assert(number==expectedIrq);irq=value;}
struct Fixture {
    PxaUart* uart;
    uint32_t base;
    unsigned reads=0,ticks=0,active=0;
    std::deque<uint16_t> input;
    std::vector<uint16_t> output;
    Fixture(uint32_t address=PXA_FFUART_BASE,uint_fast8_t interrupt=PXA_I_FFUART):base(address){
        expectedIrq=interrupt;
        uart=pxaUartInit(nullptr,{[](void*,uint32_t id){assert(id==RESCHEDULE_TASK_UART);++reschedules;},nullptr},nullptr,base,interrupt,true);
        pxaUartSetFuncs(uart,[](void* p)->uint_fast16_t{
            auto& f=*static_cast<Fixture*>(p);++f.reads;
            if(f.input.empty())return UART_CHAR_NONE;
            auto c=f.input.front();f.input.pop_front();return c;
        },[](uint_fast16_t c,void* p){static_cast<Fixture*>(p)->output.push_back(c);},
        [](void* p){auto& f=*static_cast<Fixture*>(p);++f.active;return !f.input.empty();},
        [](void* p){++static_cast<Fixture*>(p)->ticks;},this);
        write(3,0x83);write(0,96);write(1,0);write(3,3);write(4,8);write(1,0x41);write(2,0x27);
    }
    ~Fixture(){free(uart);}
    void write(unsigned reg,uint32_t value,unsigned size=4){assert(access(context,base+4*reg,size,true,&value));}
    uint32_t read(unsigned reg){uint32_t value=0;assert(access(context,base+4*reg,4,false,&value));return value;}
    void step(unsigned n){for(unsigned i=0;i<n;++i)pxaUartProcess(uart);}
};
int main(){
    {
        Fixture f;f.write(4,0x2b); // AFE + RTS + DTR + OUT2
        assert(pxaUartSetModemInputs(f.uart,0));f.read(6);
        f.write(0,0x44434241);assert(!pxaUartTaskRequired(f.uart));f.step(1000);assert(f.output.empty());
        const auto before=reschedules;assert(pxaUartSetModemInputs(f.uart,PXA_UART_CTS));assert(reschedules>before);
        f.step(40);assert(pxaUartSetModemInputs(f.uart,0));
        f.step(66);assert(f.output.empty());f.step(1);assert(f.output==std::vector<uint16_t>{0x41});
        assert(!pxaUartTaskRequired(f.uart));f.step(1000);assert(f.output.size()==1);
        assert(pxaUartSetModemInputs(f.uart,PXA_UART_CTS));f.step(320);
        assert((f.output==std::vector<uint16_t>{0x41,0x42,0x43,0x44}));
        // Half auto-flow still gates CTS with manually inactive RTS.
        f.write(4,0x29);assert(pxaUartSetModemInputs(f.uart,0));f.write(0,0x55);f.step(1000);assert(f.output.size()==4);
        f.write(4,9);f.step(107);assert(f.output.back()==0x55); // no AFE: CTS does not gate
    }
    puts("PASS: CTS gates only new frames; current frame completes; rise wakes TX; half/manual flow modes");
    for(unsigned trigger=0;trigger<4;++trigger){
        Fixture f;f.write(2,0x27|(trigger<<6));f.write(4,0x2b);
        const unsigned levels[]={1,8,16,32};const auto count=levels[trigger];
        assert(pxaUartGetModemOutputs(f.uart)==(PXA_UART_DTR|PXA_UART_RTS));
        for(unsigned i=0;i<count;++i)f.input.push_back(i);f.step(count);
        assert(f.read(9)==count && pxaUartGetModemOutputs(f.uart)==PXA_UART_DTR);
        f.read(0);assert(pxaUartGetModemOutputs(f.uart)==(PXA_UART_DTR|PXA_UART_RTS));
        f.write(2,0x27|(trigger<<6));assert(f.read(9)==0 && pxaUartGetModemOutputs(f.uart)==3);
    }
    puts("PASS: automatic RTS threshold deassertion/reassertion at 1/8/16/32 bytes and FIFO reset");
    {
        Fixture f;f.write(1,0x48);assert(pxaUartSetModemInputs(f.uart,0));f.read(6);
        assert(pxaUartSetModemInputs(f.uart,0xf0));assert(irq && (f.read(2)&15)==0);
        assert(f.read(6)==0xfb && !irq); // RI rising does not set TERI
        assert(pxaUartSetModemInputs(f.uart,0xf0));assert(!irq);
        assert(pxaUartSetModemInputs(f.uart,0xb0));assert(irq && f.read(6)==0xb4 && !irq);
        f.write(1,0x4b);assert(pxaUartSetModemInputs(f.uart,0));assert((f.read(2)&15)==2);
        f.write(1,0x49);f.input={0x66};f.step(1);assert((f.read(2)&15)==4);
        assert(f.read(0)==0x66 && (f.read(2)&15)==0);f.read(6);assert(!irq);
        f.write(1,8);assert(pxaUartSetModemInputs(f.uart,PXA_UART_CTS));assert(!irq);
        f.write(1,0x48);assert(irq);f.read(6);assert(!irq);
        pxaUartBeginCheckpoint(f.uart);assert(!pxaUartSetModemInputs(f.uart,0));pxaUartEndCheckpoint(f.uart);
        assert((f.read(6)&0xf0)==PXA_UART_CTS);assert(!pxaUartSetModemInputs(f.uart,1));
    }
    puts("PASS: modem deltas, RI trailing edge, IRQ priority/acknowledgement, UUE mask and frozen pin updates");
    {
        Fixture f;f.write(1,0);f.write(4,0x1f);f.write(1,0x49);f.read(6);
        f.input={0xe1};const auto reads=f.reads,ticks=f.ticks,active=f.active;
        assert(pxaUartSetModemInputs(f.uart,PXA_UART_DSR));assert((f.read(6)&0xf0)==0xf0);
        assert(pxaUartGetModemOutputs(f.uart)==0);f.write(0,0xdead4241,2);f.step(214);
        assert(f.output.empty() && f.reads==reads && f.ticks==ticks && f.active==active);
        assert((f.read(2)&15)==4 && f.read(0)==0x4241);f.read(6);
        f.write(1,0);f.write(4,8);f.write(1,0x49);assert((f.read(6)&0xf0)==PXA_UART_DSR);
        f.step(1);assert(f.read(0)==0xe1 && f.input.empty());
        // In LOOP, OUT2 controls DCD only: interrupts reach the CPU even when OUT2=0.
        f.write(1,0);f.write(4,0x10);f.write(1,0x49);f.read(6);
        f.write(0,0x77,1);f.step(107);assert(irq && (f.read(2)&15)==4);
        assert(f.read(0)==0x77 && !irq);
    }
    puts("PASS: PXA27x loopback isolates external callbacks/pins, receives TX and restores live inputs on exit");
    {
        Fixture f(PXA_BTUART_BASE,PXA_I_BTUART);
        assert(pxaUartSetModemInputs(f.uart,PXA_UART_CTS));assert(f.read(6)==PXA_UART_CTS);
        assert(!pxaUartSetModemInputs(f.uart,PXA_UART_DSR|PXA_UART_CTS));
        f.write(4,0x2b);assert(pxaUartGetModemOutputs(f.uart)==PXA_UART_RTS);
        f.write(1,0);f.write(4,0x1f);f.write(1,0x48);assert(f.read(6)==PXA_UART_CTS);
        assert(pxaUartGetModemOutputs(f.uart)==0);
    }
    {
        Fixture f(PXA_STUART_BASE,PXA_I_STUART);
        assert(!pxaUartSetModemInputs(f.uart,PXA_UART_CTS));assert(f.read(6)==0);
        f.write(1,0);f.write(4,0x1f);f.write(1,0x49);assert(f.read(6)==0);
        assert(pxaUartGetModemOutputs(f.uart)==0);f.write(0,0x78,1);f.step(107);
        assert(irq && f.read(0)==0x78 && f.output.empty());
    }
    puts("PASS: documented BTUART CTS/RTS-only interface and STUART reserved modem pins/data loopback");
}
