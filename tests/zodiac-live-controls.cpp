#include "zodiac-machine-parity.h"
#include "CPU.h"
#include <cstdlib>
#include <fstream>
#include <iterator>
using A = Imx1BootstrapAccess;
namespace {
void load(std::vector<uint32_t>& words, unsigned reg, uint32_t value) {
    words.push_back(0xe59f0000 | reg << 12); words.push_back(0xea000000); words.push_back(value);
}
void store(std::vector<uint32_t>& words, unsigned offset, uint32_t value) {
    load(words,1,value); words.push_back(0xe5801000 | offset);
}
Imx1Machine* create(const std::vector<uint32_t>& words, bool fast, const Imx1ProbeOptions& options) {
    auto* m=imx1MachineCreate(reinterpret_cast<const uint8_t*>(words.data()),words.size()*4,
        Imx1ProbeEntry::resetAlias,fast,options);
    assert(m); return m;
}
void pins(bool fast) {
    const std::vector<uint32_t> words={0xe59f0010,0xe5901000,0xe59f000c,0xe5902000,
                                      0xeafffffa,0xe1a00000,0x0021c024,0x0021c324};
    Imx1ProbeOptions o; o.bootstrapPeripherals=true;
    o.graphicsProfile=Imx1GraphicsProfile::virtual5744Compatible8MiB;o.graphicsIrqA13=true;
    o.graphicsRawMode1Known=true;o.graphicsRawMode1=712;
    o.graphicsRawAuxKnown=true;o.graphicsRawAux=900;o.graphicsMode2Route=Imx1GraphicsMode2Route::defaultAux;
    o.gpioAInputMaskedKnown=true;o.gpioAInputMask=0xffffdfff;o.gpioAInput=0x380;
    o.gpioDInputKnown=true;o.gpioDInput=0x5780;
    o.gpioEventAfterInstructions=1;o.gpioEventMasks[0]=0x400;o.gpioEventValues[0]=0x400;
    auto* m=create(words,fast,o);
    auto state=imx1MachineRunSlice(m,5,0);
    assert(state.status==Imx1MachineStatus::yielded && state.result.gpioEventApplied);
    const uint32_t originalA=state.result.registers[1],originalD=state.result.registers[2];
    assert((originalA & 0x380)==0x380 && originalD==0x5780);
    for (unsigned port : {0u,3u}) for (unsigned pin : {7u,8u,9u,10u,12u,14u}) {
        const uint32_t bit=1u<<pin;
        if (!(bit & (port==0 ? 0x380u : 0x5780u))) continue;
        uint32_t values[4]{},masks[4]{}; masks[port]=bit;
        const auto before=imx1MachineInspect(m);
        assert(imx1MachineReplaceGpioInputs(m,values,masks,false)==A::ok);
        assert(imx1MachineReplaceGraphicsAxes(m,512,512,false)==A::ok);
        assert(imx1MachineReplaceGraphicsAxes(m,1024,0)==A::sideEffect);
        machine_test::same(before.result,imx1MachineInspect(m).result);
        state=imx1MachineRunSlice(m,0,0);assert(state.retiredInstructions==before.retiredInstructions);
        assert(imx1MachineReplaceGpioInputs(m,values,masks)==A::ok);
        assert(imx1MachineReplaceGraphicsAxes(m,128,900)==A::ok);
        state=imx1MachineRunSlice(m,5,0);
        assert(state.result.registers[1]==(originalA & ~(port==0 ? bit : 0)));
        assert(state.result.registers[2]==(originalD & ~(port==3 ? bit : 0)));
        values[port]=bit;assert(imx1MachineReplaceGpioInputs(m,values,masks)==A::ok);
        state=imx1MachineRunSlice(m,5,0);
        assert(state.result.registers[1]==originalA && state.result.registers[2]==originalD);
    }
    uint32_t values[4]{},masks[4]{0x380,0,0,0x5780};
    assert(imx1MachineReplaceGpioInputs(m,values,masks)==A::ok);
    state=imx1MachineRunSlice(m,5,0);
    assert(state.result.registers[1]==(originalA&~0x380u) && state.result.registers[2]==0);
    values[0]=0x380;values[3]=0x5780;
    masks[2]=1; // A valid first port must roll back when a later unknown lane fails.
    assert(imx1MachineReplaceGpioInputs(m,values,masks)==A::sideEffect);
    state=imx1MachineRunSlice(m,5,0);
    assert(state.result.registers[1]==(originalA&~0x380u) && state.result.registers[2]==0);
    masks[2]=0;masks[0]|=0x2000;
    assert(imx1MachineReplaceGpioInputs(m,values,masks)==A::sideEffect);
    masks[0]=0x380;values[1]=1;
    assert(imx1MachineReplaceGpioInputs(m,values,masks)==A::sideEffect);
    values[1]=0;values[3]|=1;
    assert(imx1MachineReplaceGpioInputs(m,values,masks)==A::sideEffect);
    values[3]=0x5780;assert(imx1MachineReplaceGpioInputs(m,values,masks)==A::ok);
    state=imx1MachineRunSlice(m,5,0);
    assert(state.result.registers[1]==originalA && state.result.registers[2]==originalD && state.result.gpioEventApplied);
    imx1MachineStop(m);
    assert(imx1MachineReplaceGpioInputs(m,values,masks)==A::sequence);
    assert(imx1MachineReplaceGraphicsAxes(m,512,512,false)==A::sequence);
    assert(imx1MachineReplaceGpioInputs(nullptr,values,masks)==A::sequence);
    assert(imx1MachineReplaceGraphicsAxes(nullptr,0,0)==A::sequence);
    puts("PASS: live independent raw pins, atomic ports, passive preview, A13 ownership and consumed startup event");
}
void wake(bool fast) {
    std::vector<uint32_t> words(0x100/4,0xe1a00000);
    words[0]=0xea00003e;words[0x18/4]=0xea0000f8;
    load(words,0,0x0021b000);store(words,0,0x57000003);store(words,0x20,0);
    load(words,0,0x0021c000);store(words,0x20,0x80);store(words,0x28,1u<<14);store(words,0x30,0x80);
    load(words,0,0x00223000);store(words,8,11);
    words.push_back(0xe321f013);words.push_back(0xee070f90);
    words.push_back(0xe3a06055);words.push_back(0xe7f000f0);
    words.resize(0x400/4,0xe1a00000);
    load(words,0,0x0021c000);store(words,0x30,0);store(words,0x34,0x80);
    words.push_back(0xe25ef004);
    Imx1ProbeOptions o;o.bootstrapPeripherals=true;o.gpioAInputKnown=true;o.gpioAInput=0x80;
    auto* m=create(words,fast,o);auto waiting=imx1MachineRunSlice(m,500,1000);
    assert(waiting.status==Imx1MachineStatus::waiting && waiting.result.irqEntries==0);
    uint32_t values[4]{},masks[4]{0x80,0,0,0};
    assert(imx1MachineReplaceGpioInputs(m,values,masks,false)==A::ok);
    machine_test::same(waiting.result,imx1MachineRunSlice(m,1,1000).result);
    masks[3]=1;assert(imx1MachineReplaceGpioInputs(m,values,masks)==A::sideEffect);
    machine_test::same(waiting.result,imx1MachineRunSlice(m,1,1000).result);
    masks[3]=0;assert(imx1MachineReplaceGpioInputs(m,values,masks)==A::ok);
    const auto done=imx1MachineRunSlice(m,500,1000);
    assert(done.status==Imx1MachineStatus::terminal && done.result.registers[6]==0x55 && done.result.irqEntries==1);
    assert(done.result.idleHclk==0);
    assert(imx1MachineReplaceGpioInputs(m,values,masks,false)==A::sequence);
    assert(imx1MachineReplaceGraphicsAxes(m,512,512)==A::sequence);
    imx1MachineStop(m);
    puts("PASS: waiting raw GPIO wakes native IRQ once, rejected transactions and previews cannot wake");
}
}
int main(int argc, char** argv) {
    unsetenv("FROND_JIT");cpuSetJitSync(true);setenv("FROND_JIT_HOT","1",1);
    if (argc==3) {
        std::ifstream file(argv[1],std::ios::binary);
        const std::vector<uint8_t> rom{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
        assert(rom.size()==0x7d0000);
        Imx1ProbeOptions o;o.bootstrapPeripherals=true;
        o.graphicsProfile=Imx1GraphicsProfile::virtual5744Compatible8MiB;
        o.graphicsRawAxesKnown=true;o.graphicsRawFirst=128;o.graphicsRawSecond=900;
        o.graphicsRawMode1Known=true;o.graphicsRawMode1=712;
        o.graphicsRawAuxKnown=true;o.graphicsRawAux=900;o.graphicsMode2Route=Imx1GraphicsMode2Route::defaultAux;
        bool started=false,updated=false;
        o.busObserver=[](void* p,const Imx1ProbeAccess& e) {
            if(e.write && e.address==0x16010d0c && e.value==0x400 && e.status==A::ok)
                *static_cast<bool*>(p)=true;
        };o.busContext=&started;
        auto* m=imx1MachineCreate(rom.data(),rom.size(),Imx1ProbeEntry::linkedAlias,
                                  !std::strcmp(argv[2],"fast"),o);
        assert(m);
        Imx1MachineRun state=imx1MachineInspect(m);
        while(state.retiredInstructions<100000 && state.status!=Imx1MachineStatus::terminal) {
            state=imx1MachineRunSlice(m,1,0);
            if(started && !updated) {
                const auto before=imx1MachineInspect(m);
                assert(imx1MachineReplaceGraphicsAxes(m,23,24,false)==A::ok);
                assert(imx1MachineReplaceGraphicsAxes(m,1024,24)==A::sideEffect);
                assert(imx1MachineReplaceGraphicsAxes(m,23,24)==A::ok);
                machine_test::same(before.result,imx1MachineInspect(m).result);
                updated=true;
            }
        }
        assert(updated && state.status==Imx1MachineStatus::terminal &&
               state.result.stop==Imx1ProbeStop::unsupportedInstruction);
        const uint32_t expected[]={0x01000080,0x02000384,0x01000017,0x02000018,0x090002c8,0x0a000384};
        for(unsigned i=0;i<6;++i)assert(state.result.registers[i+4]==expected[i]);
        imx1MachineStop(m);
        puts("PASS: machine live axes retain START capture, next conversion samples update, battery and auxiliary stay independent");
        return 0;
    }
    assert(argc==1);
    for(bool fast : {false,true}) { pins(fast);wake(fast); }
    Imx1ProbeOptions o;const std::vector<uint32_t> words={0xeafffffe};
    auto* m=create(words,false,o);uint32_t values[4]{},masks[4]{1,0,0,0};
    assert(imx1MachineReplaceGpioInputs(m,values,masks)==A::sequence);
    assert(imx1MachineReplaceGraphicsAxes(m,0,0)==A::sequence);imx1MachineStop(m);
    o.bootstrapPeripherals=true;m=create(words,false,o);
    assert(imx1MachineReplaceGraphicsAxes(m,0,0)==A::sequence);imx1MachineStop(m);
    o.graphicsProfile=Imx1GraphicsProfile::virtual5744Compatible8MiB;
    o.graphicsMode2Route=static_cast<Imx1GraphicsMode2Route>(99);
    assert(!imx1MachineCreate(reinterpret_cast<const uint8_t*>(words.data()),4,Imx1ProbeEntry::resetAlias,false,o));
}
