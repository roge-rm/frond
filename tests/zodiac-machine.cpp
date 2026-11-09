#include "zodiac-machine-parity.h"
#define imx1BootProbe machineParityProbe
#define main originalIrqMain
#include "zodiac-irq.cpp"
#undef main
#undef imx1BootProbe
namespace {
Imx1AspPenInputs pen(bool down) {
    Imx1AspPenInputs p;
    p.contactKnown=p.normalKnown=p.referenceKnown=true;p.contactDown=down;
    p.normal={0x1100,0x5500,0xa900,0x1234};p.reference={0x0180,0x3280,0x7100};
    return p;
}
struct Diagnostic {
    uint64_t hash=0,words=0,recent=0;
    static void word(void* p,const char* name,const uint32_t* data,uint32_t count) {
        auto& d=*static_cast<Diagnostic*>(p);
        for(const char* n=name;*n;++n)d.hash=d.hash*131+uint8_t(*n);
        for(uint32_t i=0;i<count;++i){d.hash=d.hash*131+data[i];++d.words;}
    }
    static void bytes(void* p,const char*,const uint8_t* data,uint32_t count) {
        auto& d=*static_cast<Diagnostic*>(p);
        for(uint32_t i=0;i<count;++i)d.hash=d.hash*131+data[i];
    }
    static void counters(void* p,const char* name,const uint64_t* data,uint32_t count) {
        auto& d=*static_cast<Diagnostic*>(p);
        if(!std::strcmp(name,"machine-schema"))assert(count==1 && data[0]==2);
        if(!std::strcmp(name,"machine-recent"))++d.recent;
        for(uint32_t i=0;i<count;++i){d.hash=d.hash*131+data[i];++d.words;}
    }
};
void penWait(bool fast) {
    auto words=setup();
    load(words,0,0x00215000);store(words,0,0x18,0x10);
    load(words,0,0x00223000);store(words,0,8,46);
    words.push_back(0xe321f013);
    const auto wfi=words.size()*4;
    words.push_back(0xee070f90);words.push_back(0xe3a06055);words.push_back(0xe7f000f0);
    words.resize(0x400/4,0xe1a00000);
    load(words,0,0x00215000);store(words,0,0x18,0);store(words,0,0x1c,0x40);
    words.push_back(0xe25ef004);
    machine_test::Capture events;
    events.original.bootstrapPeripherals=true;events.original.aspPenInputs=pen(false);
    events.original.gpioBInputKnown=true;events.original.gpioBInput=0;
    events.original.gpioEventAfterInstructions=1;events.original.gpioEventMasks[1]=1;
    events.original.gpioEventValues[1]=1;
    auto options=events.options();
    const ArmDiagnosticSink retain{};options.diagnosticSnapshot=&retain;
    auto* m=imx1MachineCreate(reinterpret_cast<const uint8_t*>(words.data()),words.size()*4,
        Imx1ProbeEntry::resetAlias,fast,options,machine_test::Capture::trace,&events);
    assert(m);
    const auto waiting=imx1MachineRunSlice(m,500,10000);
    assert(waiting.status==Imx1MachineStatus::waiting && waiting.result.registers[15]==wfi+4);
    assert(waiting.result.gpioEventApplied && waiting.result.idleHclk==0);
    const auto hash=events.hash,count=events.count;
    Diagnostic a,b;
    imx1MachineEmitDiagnostic(m,{{&a,Diagnostic::word,Diagnostic::bytes},Diagnostic::counters,&a});
    imx1MachineEmitDiagnostic(m,{{&b,Diagnostic::word,Diagnostic::bytes},Diagnostic::counters,&b});
    assert(a.hash==b.hash && a.words==b.words && a.recent==waiting.retiredInstructions && a.recent==b.recent);
    for(unsigned i=0;i<3;++i) {
        const auto again=imx1MachineRunSlice(m,1,10000);
        assert(again.retiredInstructions==waiting.retiredInstructions);
        machine_test::same(waiting.result,again.result);
    }
    assert(imx1MachineReplacePenInputs(m,pen(true),false)==Imx1BootstrapAccess::ok);
    auto invalid=pen(true);invalid.contactKnown=false;
    assert(imx1MachineReplacePenInputs(m,invalid)==Imx1BootstrapAccess::externalInput);
    machine_test::same(waiting.result,imx1MachineInspect(m).result);
    assert(hash==events.hash && count==events.count);
    assert(imx1MachineReplacePenInputs(m,pen(true))==Imx1BootstrapAccess::ok);
    const auto done=imx1MachineRunSlice(m,500,10000);
    assert(done.status==Imx1MachineStatus::terminal && done.result.registers[6]==0x55 &&
           done.result.irqEntries==1 && done.result.idleHclk==0 && done.result.gpioEventApplied);
    imx1MachineStop(m);
    std::puts("PASS: waiting WFI preview/rejection is passive, explicit pen wakes once without retiring WFI twice");
}
void timedAsp(bool fast) {
    auto words=setup();
    load(words,0,0x00215000);store(words,0,0x2c,3);store(words,0,0x14,0x3814);
    store(words,0,0x10,0x0200e001);store(words,0,0x10,0x0200e003);
    const auto loop=words.size();
    words.push_back(0xe5901034);words.push_back(0xe201100f);words.push_back(0xe3510004);
    words.push_back(0x1afffffb);
    assert(words.size()==loop+4);
    for(unsigned n=0;n<4;++n)words.push_back(0xe5900000 | (n+4)<<12);
    words.push_back(0xe7f000f0);
    Imx1ProbeOptions options;options.bootstrapPeripherals=true;options.aspUSampleKnown=true;
    options.aspUSample=0x9b00;options.aspPenInputs=pen(true);
    const auto result=machineParityProbe(reinterpret_cast<const uint8_t*>(words.data()),words.size()*4,
        Imx1ProbeEntry::resetAlias,250000,nullptr,nullptr,fast,&options);
    if(result.stop!=Imx1ProbeStop::unsupportedInstruction)std::fprintf(stderr,"ASP stop=%s pc=%08x address=%08x status=%s\n",imx1ProbeStopName(result.stop),result.trace.pc,result.address,imx1BootstrapAccessName(result.peripheralStatus));
    assert(result.stop==Imx1ProbeStop::unsupportedInstruction);
    for(unsigned n=0;n<4;++n)assert(result.registers[n+4]==pen(true).normal[n]);
    std::puts("PASS: timed ASP capture/FIFO values survive slices and passive frame reads");
}
void timerWait(bool fast) {
    auto words=setup();
    load(words,0,0x00223000);store(words,0,8,58);
    load(words,0,0x00203000);store(words,0,8,200);store(words,0,0,0x13);
    words.push_back(0xe321f013);words.push_back(0xee070f90);
    words.push_back(0xe3a06055);words.push_back(0xe7f000f0);
    installTimerHandler(words);
    Imx1ProbeOptions options;options.bootstrapPeripherals=true;
    const auto result=machineParityProbe(reinterpret_cast<const uint8_t*>(words.data()),words.size()*4,
        Imx1ProbeEntry::resetAlias,500,nullptr,nullptr,fast,&options);
    assert(result.stop==Imx1ProbeStop::unsupportedInstruction && result.registers[6]==0x55 && result.idleHclk>0);
    auto* m=imx1MachineCreate(reinterpret_cast<const uint8_t*>(words.data()),words.size()*4,
        Imx1ProbeEntry::resetAlias,fast,options);
    assert(m);
    const auto paused=imx1MachineRunSlice(m,500,1);
    assert(paused.status==Imx1MachineStatus::yielded && paused.result.stop==Imx1ProbeStop::idleBudget && paused.result.idleHclk==1);
    const auto resumed=imx1MachineRunSlice(m,500,10000);
    machine_test::same(result,resumed.result);
    imx1MachineStop(m);
    std::puts("PASS: split/continuous WFI timer wake and resumed idle budget have identical clocks and IRQ context");
}
}
int main() {
    unsetenv("FROND_JIT"); cpuSetJitSync(true); setenv("FROND_JIT_HOT","1",1);
    static_assert(sizeof(Imx1ProbeTrace::step)==8 && sizeof(Imx1ProbeAccess::step)==8);
    Imx1ProbeOptions invalid;invalid.rtcInitialTimeKnown=true;invalid.rtcInitialTime.hour=99;
    assert(!imx1MachineCreate(nullptr,0,Imx1ProbeEntry::resetAlias,false,invalid));
    for(bool fast:{false,true}) { penWait(fast);timedAsp(fast);timerWait(fast); }
    thumbPrefix(); suspendedThumbPrefix(); sharedThumbPrefix(); stalledDelivery();
    std::puts("PASS: persistent Exact/Fast slices across interrupted Thumb BL, task switches and timed stalls");
}
