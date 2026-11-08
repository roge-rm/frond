#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "imx1_boot_probe.h"

namespace {
using A = Imx1BootstrapAccess;
constexpr uint32_t cgm = 0x21b000;
void w(Imx1Bootstrap& b, uint32_t a, uint32_t v, unsigned mode = 0, A status = A::ok) {
    assert(b.access(a, 4, true, v, mode) == status);
}
uint32_t r(Imx1Bootstrap& b, uint32_t a) { uint32_t v = 0; assert(b.access(a,4,false,v)==A::ok);return v; }
void clocks() {
    Imx1Bootstrap b;
    assert(b.setRtcInitialTime({1,12,34,56}));
    b.setSystemPllScenario(true); b.setMcuRetuneScenario(true);
    w(b,cgm,0x57000003);w(b,cgm+4,0x03e717bf);w(b,cgm,0x57200003);
    b.advanceFastBusCycles(6000);b.advanceCpuCycles(56,3,true);
    w(b,0x202004,7);w(b,0x202000,0x103);
    w(b,0x203004,3);w(b,0x203000,0x103);
    w(b,cgm,0x57003c03);
    auto twin=b;
    w(b,cgm,0x57003c02);
    assert(!b.cpuClockModeSupported(3,true) && b.cpuClockModeSupported(0,true));
    w(b,cgm,0x57003c02,3,A::sideEffect);
    w(b,cgm,0x57003c00,0,A::sideEffect);
    b.advanceHclkCycles(313); twin.advanceHclkCycles(313);
    for(auto a:{0x202010u,0x203010u,0x204000u,0x204004u}) assert(r(b,a)==r(twin,a));
    w(b,cgm,0x57003c03);
    assert(b.mcuWakePending() && !b.cpuClockModeSupported(3,true));
    auto split=b;
    b.advanceHclkCycles(1688);
    for(unsigned i=0;i<1688;++i)split.advanceHclkCycles(1);
    assert(!b.mcuWakePending() && !split.mcuWakePending());
    assert(b.cpuClockModeSupported(3,true) && split.cpuClockModeSupported(3,true));
    assert(r(b,cgm)==r(split,cgm));
}
void wakeQualification() {
    Imx1Bootstrap reset;
    reset.advanceCpuCycles(1,3,false);
    w(reset,cgm,0x0f00ac02);
    w(reset,cgm+4,0x003f1437,0,A::sideEffect);
    w(reset,cgm,0x0f00ac03);
    assert(!reset.cpuClockModeSupported(3,false));
    reset.advanceHclkCycles(3000);
    assert(reset.cpuClockModeSupported(3,true) && !reset.cpuClockModeSupported(3,false));

    Imx1Bootstrap b;
    b.connectCpuInterrupts();
    assert(b.setRtcInitialTime({1,12,34,56}));
    assert(!b.idleWakePossible());
    // Sampling and calendar enables must qualify only their own AITC source.
    w(b,0x204018,0x8000);w(b,0x223008,17);
    assert(!b.idleWakePossible());
    w(b,0x223008,18);assert(b.idleWakePossible());
    w(b,0x22300c,18);assert(!b.idleWakePossible());
    w(b,0x22300c,17);w(b,0x204018,8);w(b,0x223008,18);
    assert(!b.idleWakePossible());
    w(b,0x223008,17);assert(b.idleWakePossible());
    w(b,0x22300c,17);assert(!b.idleWakePossible());
    w(b,0x22300c,18);
    constexpr uint32_t bit=0x2000, gpio=0x21c000;
    assert(b.setGpioInputMasked(0,0,~bit) && b.reserveGpioInput(0,bit));
    assert(b.driveGpioInput(0,bit,bit,true,true)==A::ok);
    w(b,gpio+0x20,bit);w(b,gpio+0x28,3u<<26);w(b,gpio+0x30,bit);
    assert(!b.gpioWakePossible(0,0,bit));
    w(b,0x223008,11);
    assert(b.gpioWakePossible(0,0,bit));
    assert(b.interruptLines().irq==Imx1AitcLine::clear && r(b,gpio+0x34)==0);
    w(b,0x223004,0); // priority zero is masked.
    assert(!b.gpioWakePossible(0,0,bit));
    w(b,0x22301c,1u<<11); // FIQ bypasses the normal priority mask.
    assert(b.gpioWakePossible(0,0,bit));
    assert(b.interruptLines().fiq==Imx1AitcLine::clear);
    const uint32_t onlyWfi[]={0xee070f90,0xe7f000f0};
    const auto strict=imx1BootProbe(reinterpret_cast<const uint8_t*>(onlyWfi),sizeof(onlyWfi),
        Imx1ProbeEntry::physicalFlash,4);
    assert(strict.stop==Imx1ProbeStop::unsupportedCp15);
}
void literal(std::vector<uint32_t>& v, unsigned reg, uint32_t n) {
    v.push_back(0xe59f0000 | reg<<12);v.push_back(0xea000000);v.push_back(n);
}
struct Events { std::vector<Imx1ProbeInterrupt> irq; std::vector<Imx1ProbeAccess> bus; };
void interrupt(void* p,const Imx1ProbeInterrupt& e){static_cast<Events*>(p)->irq.push_back(e);}
void bus(void* p,const Imx1ProbeAccess& e){static_cast<Events*>(p)->bus.push_back(e);}
void sequence(bool fiq, bool masked, bool pending, bool timerEnabled, bool limited = false, bool repeat = false) {
    std::vector<uint32_t> v(64,0xe1a00000);
    v[0]=0xea00003e; // start at 0x100.
    v[6]=0xea000018;v[7]=0xea000017; // IRQ/FIQ vectors to 0x80.
    unsigned h=32;
    v[h++]=0xe5945000; // read timer status at r4.
    v[h++]=0xe3a05000;v[h++]=0xe5845000;v[h++]=0xe5825000;
    v[h++]=0xe25ef004;
    literal(v,0,cgm);literal(v,1,0x57000003);v.push_back(0xe5801000);
    literal(v,1,0x03e717bf);v.push_back(0xe5801004);
    literal(v,1,0x57200003);v.push_back(0xe5801000);
    v.push_back(0xe5902000);v.push_back(0xe3120602);v.push_back(0x1afffffc);
    literal(v,1,0xc0000078);v.push_back(0xee011f10);
    for(unsigned i=0;i<60;++i)v.push_back(0xe1a03003);
    literal(v,1,0x78);v.push_back(0xee011f10);
    literal(v,4,0x203014);
    literal(v,2,0x223000);literal(v,1,0x04000000);v.push_back(0xe5821010); // enable Timer2.
    if(fiq)v.push_back(0xe5821018);
    literal(v,2,0x203000);literal(v,1,pending?10:500);v.push_back(0xe5821008);
    literal(v,1,timerEnabled?0x113:0);v.push_back(0xe5821000);
    literal(v,1,0x57003c03);v.push_back(0xe5801000);
    literal(v,1,0x57003c02);v.push_back(0xe5801000);
    if(!masked && !pending) v.push_back(0xe321f013);
    uint32_t wfi=v.size()*4;
    v.push_back(0xee070f90);
    if(!masked && pending) v.push_back(0xe321f013);
    if(repeat) {
        // Start a second timed wait after acknowledging and restarting the same timer.
        v.push_back(0xe5945000);v.push_back(0xe3a05000);v.push_back(0xe5845000);
        v.push_back(0xe5825000);literal(v,5,0x113);v.push_back(0xe5825000);
        literal(v,1,0x57003c02);v.push_back(0xe5801000);
        wfi=v.size()*4;v.push_back(0xee070f90);
    }
    literal(v,1,0xc0000078);v.push_back(0xee011f10);
    literal(v,1,0x57000003);v.push_back(0xe5801000);
    v.push_back(0xe3a06055);v.push_back(0xe7f000f0);
    auto run=[&](bool fast,Events& events){
        Imx1ProbeOptions o{};o.bootstrapPeripherals=true;o.assumeMpllSettled=true;
        o.virtualMpllRetune=true;o.assumeSpllSettled=true;
        if(limited)o.idleHclkBudget=repeat?3000:16;
        o.interruptObserver=interrupt;o.interruptContext=&events;o.busObserver=bus;o.busContext=&events;
        return imx1BootProbe(reinterpret_cast<const uint8_t*>(v.data()),v.size()*4,
            Imx1ProbeEntry::resetAlias,30000,nullptr,nullptr,fast,&o);
    };
    Events ea,eb;const auto a=run(false,ea);
    if(limited){assert(a.stop==Imx1ProbeStop::idleBudget && a.idleHclk==(repeat?3000u:16u) && a.registers[15]==wfi+4);}
    else if(!timerEnabled){assert(a.stop==Imx1ProbeStop::idleNoWake && a.registers[15]==wfi+4);}
    else {
        if(a.stop!=Imx1ProbeStop::unsupportedInstruction)std::fprintf(stderr,"stop %s pc%08x step%u addr%08x status%s wfi%x\n",imx1ProbeStopName(a.stop),a.registers[15],a.trace.step,a.address,imx1BootstrapAccessName(a.peripheralStatus),wfi);
        assert(a.stop==Imx1ProbeStop::unsupportedInstruction && a.registers[6]==0x55);
        if(masked) assert(ea.irq.empty());
        else {assert(!ea.irq.empty());const auto& e=ea.irq.back();assert(e.fiq==fiq);assert(e.link==e.interruptedPc+4);if(!pending)assert(e.link==wfi+8);}
        assert(a.idleHclk || pending);
    }
#if FROND_TEST_FAST
    const auto b=run(true,eb);
    assert(a.stop==b.stop && a.trace.step==b.trace.step && a.cpsr==b.cpsr && a.spsr==b.spsr);
    assert(a.idleHclk==b.idleHclk && !std::memcmp(a.registers,b.registers,sizeof(a.registers)));
    assert(ea.bus.size()==eb.bus.size() && ea.irq.size()==eb.irq.size());
    for(size_t i=0;i<ea.bus.size();++i){const auto& x=ea.bus[i];const auto& y=eb.bus[i];assert(x.step==y.step && x.pc==y.pc && x.address==y.address && x.value==y.value && x.status==y.status);}
    assert(b.jitInstructions);
#endif
}
}
int main(){clocks();wakeQualification();for(bool fiq:{false,true})for(bool masked:{false,true})for(bool pending:{false,true})sequence(fiq,masked,pending,true);sequence(false,true,false,false);sequence(false,true,false,true,true);sequence(false,true,false,true,true,true);std::puts("Zodiac MCU shutdown, WFI, IRQ/FIQ wake and clock restore: PASS");}
