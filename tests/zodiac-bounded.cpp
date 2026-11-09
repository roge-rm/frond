#include "zodiac-machine-parity.h"
#define main originalIrqMain
#include "zodiac-irq.cpp"
#undef main
#include <limits>

namespace {
using T=Imx1ReferenceTime;
void clockCharges() {
    Imx1Bootstrap b;T expected;
    auto charge=[&](uint32_t cycles,uint32_t mode) {
        const auto period=b.cpuReferencePeriod(mode);
        assert(expected.add(uint64_t(cycles)*period.numerator,period.denominator));
        b.advanceCpuCycles(cycles,mode,true);
        assert(expected.compare(b.referenceTime())==0);
    };
    charge(17,0);charge(113,3);
    const auto bus=b.busReferencePeriod();
    b.advanceHclkCycles(73);assert(expected.add(73*bus.numerator,bus.denominator));
    assert(expected.compare(b.referenceTime())==0);
    // Retuning preserves reference time while the bus and CPU change rates.
    b=Imx1Bootstrap{};expected={};b.setSystemPllScenario(true);
    auto write=[&](uint32_t offset,uint32_t value,uint32_t mode=0) {
        assert(b.access(0x0021b000+offset,4,true,value,mode)==Imx1BootstrapAccess::ok);
    };
    write(0,0x57000003);write(4,0x03e717bf);write(0,0x57200003);charge(6000,0);
    write(12,0x123638ad,3);write(0,0x57400003,3);
    charge(56,3);const auto period=b.cpuReferencePeriod(3);
    const auto stalled=b.finishSystemPllBusStall();assert(stalled);
    assert(expected.add(stalled,period.denominator) && expected.compare(b.referenceTime())==0);
    assert(!b.finishSystemPllBusStall() && expected.compare(b.referenceTime())==0);
    write(0,0x5700ac03,3);charge(51,0);charge(127,3);
}
void firstAsynchronousRetuneCharge() {
    Imx1Bootstrap b;b.setMcuRetuneScenario(true);
    auto write=[&](uint32_t offset,uint32_t value,uint32_t mode=0) {
        assert(b.access(0x0021b000+offset,4,true,value,mode)==Imx1BootstrapAccess::ok);
    };
    write(0,0x57000003);write(4,0x03e717bf);write(0,0x57200003);
    b.advanceFastBusCycles(6000);
    auto before=b.referenceTime();const auto period=b.cpuReferencePeriod(3);
    b.advanceCpuCycles(56,3,true);
    assert(before.add(56*period.numerator,period.denominator) && before.compare(b.referenceTime())==0);
    write(4,0x03e717bf,3);
}
void referenceOverflowPreflight() {
    Imx1Bootstrap b;
    const auto period=b.cpuReferencePeriod(3);
    // Place this owned fixture at the last representable fraction without a production time setter.
    auto& time=const_cast<T&>(b.referenceTime());
    time={UINT64_MAX,period.denominator-1,period.denominator};
    const auto before=time;
    b.advanceCpuCycles(56,3,true);
    assert(time.compare(before)==0 && b.clockStatus()==Imx1BootstrapAccess::sideEffect);
    uint32_t cscr=0;assert(b.access(0x0021b000,4,false,cscr)==Imx1BootstrapAccess::ok);
    uint32_t powerOff=cscr&~1u;
    assert(b.access(0x0021b000,4,true,powerOff)==Imx1BootstrapAccess::sideEffect);
    Imx1Bootstrap qualified;qualified.advanceCpuCycles(1,3,true);
    assert(qualified.access(0x0021b000,4,true,powerOff)==Imx1BootstrapAccess::ok);
}
void times() {
    T split, whole; assert(whole.add(1000000, 1234567));
    for(unsigned n=0;n<1000000;++n)assert(split.add(1,1234567));
    assert(split.compare(whole)==0);
    assert(split.add(3,7) && whole.add(3,7) && split.compare(whole)==0);
    uint64_t ns=0;assert(split.nanoseconds(ns));
    assert(ns==uint64_t((__uint128_t(1000000)*7+3*1234567)*1000000000/(uint64_t(1234567)*7*32768)));
    const auto before=split;assert(!split.add(0,0) && split.compare(before)==0);
    T overflow{UINT64_MAX,0,1};assert(!overflow.add(1,1) && overflow.ticks==UINT64_MAX);
    T denominator{0,1,UINT64_MAX};const auto saved=denominator;
    assert(!denominator.add(1,2) && denominator.compare(saved)==0);
    assert(!overflow.nanoseconds(ns));
    T carry{3,1,3},start=carry;assert(carry.add(3,4));
    assert(carry.since(start).compare(T{0,3,4})==0);
    const T wideEnd{1,UINT64_MAX-1,UINT64_MAX},wideStart{0,UINT64_MAX-2,UINT64_MAX-1};
    const auto wide=wideEnd.intervalSince(wideStart);
    assert(wide.ticks==1 && wide.numerator==1 && wide.denominator==__uint128_t(UINT64_MAX)*(UINT64_MAX-1));
}
struct Outcome { Imx1MachineCheapStatus total; Imx1ProbeResult result; uint64_t hash,count; bool idleOnly=false,overshot=false; };
Outcome execute(const std::vector<uint32_t>& words, Imx1ProbeOptions options, bool fast, bool split, bool wakeGpio=false) {
    machine_test::Capture capture;capture.original=options;options=capture.options();
    auto* m=imx1MachineCreate(reinterpret_cast<const uint8_t*>(words.data()),words.size()*4,
        options.graphicsProfile!=Imx1GraphicsProfile::strict ? Imx1ProbeEntry::linkedAlias : Imx1ProbeEntry::resetAlias,fast,options,machine_test::Capture::trace,&capture);
    assert(m);bool idleOnly=false,overshot=false,delivered=false;
    auto before=imx1MachineGetStatus(m);
    auto zero=imx1MachineRunBounded(m,{0,{UINT64_MAX,0,1},100});
    assert(zero.reason==Imx1MachineYield::retired && !zero.workDelta && !zero.retiredDelta);
    zero=imx1MachineRunBounded(m,{1,{UINT64_MAX,0,1},0});assert(zero.reason==Imx1MachineYield::work);
    zero=imx1MachineRunBounded(m,{1,before.time,1});assert(zero.reason==Imx1MachineYield::deadline);
    auto invalid=imx1MachineRunBounded(m,{1,{0,0,0},1});assert(invalid.reason==Imx1MachineYield::invalidBounds);
    invalid=imx1MachineRunBounded(m,{1,{0,1,1},1});assert(invalid.reason==Imx1MachineYield::invalidBounds);
    invalid=imx1MachineRunBounded(m,{imx1ProbeMaximumSteps+1,{1,0,1},1});assert(invalid.reason==Imx1MachineYield::invalidBounds);
    invalid=imx1MachineRunBounded(m,{1,{1,0,1},UINT64_MAX});assert(invalid.reason==Imx1MachineYield::invalidBounds);
    const auto unchanged=imx1MachineGetStatus(m);
    assert(unchanged.time.compare(before.time)==0 && unchanged.work==before.work && unchanged.retired==before.retired);
    T sum;uint64_t work=0,retired=0;
    for(unsigned iteration=0;;++iteration) {
        assert(iteration<200000);
        before=imx1MachineGetStatus(m);
        const auto hash=capture.hash,count=capture.count;
        const auto inspected=imx1MachineInspect(m);
        const auto passive=imx1MachineGetStatus(m);
        assert(passive.time.compare(before.time)==0 && passive.work==before.work && passive.retired==before.retired);
        assert(hash==capture.hash && count==capture.count && passive.pc==inspected.result.registers[15]);
        T deadline{UINT64_MAX,0,1};
        if(split && iteration%3==0) {
            constexpr uint64_t prime=UINT64_MAX-58;
            const auto next=__uint128_t(before.time.numerator)*prime/before.time.denominator+1;
            deadline={before.time.ticks+uint64_t(next==prime),uint64_t(next%prime),prime};
        }
        const Imx1MachineBounds bound{split && iteration%4==1 ? 1u : 50000u,deadline,split ? uint64_t(iteration%7+1) : uint64_t(10000000)};
        const auto run=imx1MachineRunBounded(m,bound);
        assert(run.total.time.compare(before.time)>=0 && run.total.work>=before.work);
        uint64_t beforeNs=0,afterNs=0;
        assert(before.time.nanoseconds(beforeNs) && run.total.time.nanoseconds(afterNs) && afterNs>=beforeNs);
        if(run.reason==Imx1MachineYield::retired)assert(run.retiredDelta==bound.retired);
        if(run.reason==Imx1MachineYield::work)assert(run.workDelta>=bound.work);
        if(run.reason==Imx1MachineYield::deadline)assert(run.total.time.compare(bound.deadline)>=0);
        auto reconstructed=before.time;assert(reconstructed.add(run.elapsed.ticks,1));
        assert(reconstructed.add(run.elapsed.numerator,run.elapsed.denominator));assert(reconstructed.compare(run.total.time)==0);
        assert(sum.add(run.elapsed.ticks,1) && sum.add(run.elapsed.numerator,run.elapsed.denominator));
        work+=run.workDelta;retired+=run.retiredDelta;
        assert(work==run.total.work && retired==run.total.retired && sum.compare(run.total.time)==0);
        idleOnly|=!run.retiredDelta && run.workDelta && run.total.idleHclk>before.idleHclk;
        overshot|=run.workOvershoot>0 || run.timeOvershoot.positive();
        assert(run.workOvershoot==(run.workDelta>bound.work ? run.workDelta-bound.work : 0));
        if(run.total.time.compare(bound.deadline)>0) {
            const auto excess=run.total.time.intervalSince(bound.deadline);
            assert(run.timeOvershoot.ticks==excess.ticks && run.timeOvershoot.numerator==excess.numerator &&
                run.timeOvershoot.denominator==excess.denominator);
        }
        if(run.reason==Imx1MachineYield::waiting && wakeGpio && !delivered) {
            uint32_t values[4]{},masks[4]{0x80,0,0,0};
            assert(imx1MachineReplaceGpioInputs(m,values,masks,false)==Imx1BootstrapAccess::ok);
            auto same=imx1MachineGetStatus(m);assert(same.time.compare(run.total.time)==0 && same.work==run.total.work);
            assert(imx1MachineReplaceGpioInputs(m,values,masks)==Imx1BootstrapAccess::ok);
            delivered=true;continue;
        }
        if(run.reason==Imx1MachineYield::terminal || run.reason==Imx1MachineYield::waiting)break;
        assert(run.workDelta>0 && run.elapsed.compare(T{})>0);
    }
    const auto total=imx1MachineGetStatus(m);const auto result=imx1MachineInspect(m).result;
    const auto again=imx1MachineRunBounded(m,{1,{UINT64_MAX,0,1},1});
    assert(!again.workDelta && !again.retiredDelta && again.total.time.compare(total.time)==0);
    imx1MachineStop(m);
    assert(imx1MachineRunBounded(m,{1,{UINT64_MAX,0,1},1}).reason==Imx1MachineYield::stopped);
    return {total,result,capture.hash,capture.count,idleOnly,overshot};
}
void parity(const std::vector<uint32_t>& words, Imx1ProbeOptions options, bool fast,bool idle=false,bool cs5=false) {
    auto full=execute(words,options,fast,false), split=execute(words,options,fast,true);
    machine_test::same(full.result,split.result);
    assert(full.total.time.compare(split.total.time)==0 && full.total.time.denominator==split.total.time.denominator && full.total.work==split.total.work &&
           full.total.retired==split.total.retired && full.hash==split.hash && full.count==split.count);
    assert(split.overshot);if(idle)assert(split.idleOnly);if(cs5)assert(full.total.cs5WaitClocks==63);
    const auto legacy=imx1BootProbe(reinterpret_cast<const uint8_t*>(words.data()),words.size()*4,
        options.graphicsProfile!=Imx1GraphicsProfile::strict ? Imx1ProbeEntry::linkedAlias : Imx1ProbeEntry::resetAlias,50000,nullptr,nullptr,fast,&options);
    machine_test::same(legacy,full.result);
}
void timer(bool fast) {
    auto words=setup();load(words,0,0x00223000);store(words,0,8,58);
    load(words,0,0x00203000);store(words,0,8,200);store(words,0,0,0x13);
    words.push_back(0xe321f013);words.push_back(0xee070f90);words.push_back(0xe3a06055);words.push_back(0xe7f000f0);
    installTimerHandler(words);Imx1ProbeOptions o;o.bootstrapPeripherals=true;parity(words,o,fast,true);
}
void gpio(bool fast) {
    auto words=setup();load(words,0,0x00223000);store(words,0,8,11);
    load(words,0,0x0021c000);store(words,0,0x30,0x80);store(words,0,0x28,0x80);
    words.push_back(0xe321f013);words.push_back(0xee070f90);words.push_back(0xe7f000f0);
    Imx1ProbeOptions o;o.bootstrapPeripherals=true;o.gpioAInputKnown=true;o.gpioAInput=0;
    parity(words,o,fast);
}
void gpioWake(bool fast) {
    auto words=setup();load(words,0,0x0021c000);store(words,0,0x20,0x80);
    store(words,0,0x28,1u<<14);store(words,0,0x30,0x80);
    load(words,0,0x00223000);store(words,0,8,11);
    words.push_back(0xe321f013);words.push_back(0xee070f90);words.push_back(0xe3a06055);words.push_back(0xe7f000f0);
    words.resize(0x400/4,0xe1a00000);load(words,0,0x0021c000);store(words,0,0x30,0);store(words,0,0x34,0x80);
    words.push_back(0xe25ef004);
    Imx1ProbeOptions o;o.bootstrapPeripherals=true;o.gpioAInputKnown=true;o.gpioAInput=0x80;
    const auto full=execute(words,o,fast,false,true),split=execute(words,o,fast,true,true);
    machine_test::same(full.result,split.result);
    assert(full.total.time.compare(split.total.time)==0 && full.total.time.denominator==split.total.time.denominator && full.total.work==split.total.work &&
        full.hash==split.hash && full.count==split.count && full.result.irqEntries==1 && full.result.registers[6]==0x55 && !full.result.idleHclk);
}
void pll(bool fast) {
    std::vector<uint32_t> words(0x100/4,0xe1a00000);words[0]=0xea00003e;words[6]=0xe7f000f0;
    load(words,0,0x00223000);store(words,0,8,18);load(words,0,0x00204000);store(words,0,0x18,0x8000);
    for(unsigned n=0;n<15200;++n)words.push_back(0xe1a00000);
    load(words,0,0x0021b000);store(words,0,0,0x57000003);store(words,0,4,0x03e717bf);store(words,0,0,0x57200003);
    for(unsigned n=0;n<6000;++n)words.push_back(0xe1a00000);
    load(words,1,0xc0000078);words.push_back(0xee011f10);store(words,0,0x0c,0x123638ad);
    load(words,1,0x57400003);words.push_back(0xe321f013);words.push_back(0xe5801000);words.push_back(0xe7f000f0);
    Imx1ProbeOptions o;o.bootstrapPeripherals=true;o.rtcInitialTimeKnown=true;o.assumeMpllSettled=o.assumeSpllSettled=true;
    parity(words,o,fast);
}
void cs5(bool fast) {
    std::vector<uint32_t> words;load(words,0,0x00220028);store(words,0,0,0x3e00);store(words,0,4,0x11111e01);
    load(words,0,0x16000018);load(words,2,0x200bac84);words.push_back(0xe12fff12);
    words.resize(0xbac84/4+2,0xe1a00000);words[0xbac84/4]=0xe5d03000;words[0xbac84/4+1]=0xe7f000f0;
    Imx1ProbeOptions o;o.bootstrapPeripherals=true;o.graphicsProfile=Imx1GraphicsProfile::virtual5744Compatible8MiB;
    parity(words,o,fast,false,true);
}
}
int main() {
    unsetenv("FROND_JIT");cpuSetJitSync(true);setenv("FROND_JIT_HOT","1",1);times();clockCharges();firstAsynchronousRetuneCharge();referenceOverflowPreflight();
    for(bool fast:{false,true}){timer(fast);gpio(fast);gpioWake(fast);pll(fast);cs5(fast);}
    std::puts("PASS: bounded Exact/Fast timer WFI, GPIO waiting/wake, PLL stalls and CS5 match contiguous/legacy time and state");
}
