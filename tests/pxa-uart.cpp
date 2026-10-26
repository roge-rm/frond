#include <cassert>
#include <cstdio>
#include <cstdlib>
#include "pxa_UART.h"
static ArmMemAccessF access;
static void* context;
static bool irq;
static unsigned reschedules;
static uint16_t incoming=UART_CHAR_NONE;
extern "C" void uarmAbort(){std::abort();}
bool memRegionAdd(ArmMem*,uint32_t,uint32_t,ArmMemAccessF f,void* ctx){access=f;context=ctx;return true;}
void pxaIcInt(PxaIc*,uint_fast8_t number,bool value){assert(number==PXA_I_STUART);irq=value;}
static void write(unsigned reg,uint32_t value){assert(access(context,PXA_STUART_BASE+reg*4,4,true,&value));}
static uint32_t read(unsigned reg){uint32_t value;assert(access(context,PXA_STUART_BASE+reg*4,4,false,&value));return value;}
int main(){
    auto* uart=pxaUartInit(nullptr,{[](void*,uint32_t task){assert(task==RESCHEDULE_TASK_UART);++reschedules;},nullptr},nullptr,PXA_STUART_BASE,PXA_I_STUART,true);
    pxaUartSetFuncs(uart,[](void*)->uint_fast16_t{auto value=incoming;incoming=UART_CHAR_NONE;return value;},nullptr,[](void*){return incoming!=UART_CHAR_NONE;},nullptr,nullptr);
    write(3,0x83);write(0,96);write(1,0);write(3,3); // start the baud clock
    assert(!pxaUartIrReceiveEnabled(uart));
    incoming=0x7b;write(8,3);assert(pxaUartIrReceiveEnabled(uart)&&reschedules);
    write(4,8);write(1,0x41); // OUT2 and UUE/RAVIE
    pxaUartProcess(uart);assert((read(5)&3)==1 && irq);assert(read(0)==0x7b && !irq); // first non-FIFO byte must be retained
    incoming=0x11;pxaUartProcess(uart);incoming=0x22;pxaUartProcess(uart);
    assert(read(5)&2);assert(read(0)==0x11); // unread data causes overrun rather than a false empty receive
    write(3,0x83);write(0,96);write(1,0);write(3,3); // 9600 baud, 8N1
    write(1,0x51);
    write(2,0x43); // FIFO enabled, RX reset, trigger eight bytes
    incoming=0x33;pxaUartProcess(uart);assert(read(9)==1);assert(!irq && pxaUartTaskRequired(uart));
    for(unsigned i=0;i<100;i++)pxaUartProcess(uart);
    assert(!irq && pxaUartTaskRequired(uart)); // a millisecond between IR bytes must not time out
    for(unsigned i=100;i<427;i++)pxaUartProcess(uart);
    assert(irq && (read(2)&0x0f)==0x0c);assert(!pxaUartTaskRequired(uart));
    write(1,0x41);assert(!irq); // receiver timeout interrupt has its own enable bit
    write(1,0x51);assert(irq);
    assert(read(0)==0x33 && !irq && read(9)==0);
    incoming=0x44;pxaUartProcess(uart);incoming=0x55;pxaUartProcess(uart);assert(read(9)==2);
    write(2,0x43);assert(read(9)==0 && !(read(5)&1) && !irq);
    write(1,0);assert(!pxaUartIrReceiveEnabled(uart));
    puts("PASS: PXA UART non-FIFO receive, overrun, short FIFO timeout scheduling and infrared enable");
}
