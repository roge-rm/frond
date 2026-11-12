#ifndef FROND_TEST_MACHINE_PARITY_H
#define FROND_TEST_MACHINE_PARITY_H
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "imx1_boot_probe.h"

namespace machine_test {
struct Capture {
    uint64_t hash = 1469598103934665603ull, count = 0;
    Imx1ProbeOptions original{};
    Imx1ProbeObserver observer = nullptr;
    void* context = nullptr;
    bool forward = false;
    void add(std::initializer_list<uint64_t> words) {
        for (auto word : words) { hash = (hash ^ word) * 1099511628211ull; ++count; }
    }
    static void trace(void* p, const Imx1ProbeTrace& e) {
        auto& c = *static_cast<Capture*>(p);
        c.add({1,e.step,e.pc,e.instruction,e.cpsr,e.operand,e.instructionSize});
        if (c.forward && c.observer) c.observer(c.context,e);
    }
    static void bus(void* p, const Imx1ProbeAccess& e) {
        auto& c = *static_cast<Capture*>(p);
        c.add({2,e.step,e.pc,e.address,e.size,e.value,e.write,uint64_t(e.status),
            e.diagnosticCs4,e.diagnosticCs5Reset,e.diagnosticCs5Configuration,e.knownMask,e.cs5WaitClocks});
        if (c.forward && c.original.busObserver) c.original.busObserver(c.original.busContext,e);
    }
    static void irq(void* p, const Imx1ProbeInterrupt& e) {
        auto& c = *static_cast<Capture*>(p);
        c.add({3,e.step,e.interruptedPc,e.cpsr,e.vector,e.link,e.fiq,e.pending.normal,e.pending.fast,
               e.pending.unknownNormal,e.pending.unknownFast});
        if (c.forward && c.original.interruptObserver) c.original.interruptObserver(c.original.interruptContext,e);
    }
    static void i2c(void* p, const Imx1ProbeI2cEvent& e) {
        auto& c = *static_cast<Capture*>(p);
        c.add({4,e.step,e.pc,e.issueStep,e.issuePc,uint64_t(e.transfer.phase),e.transfer.address,
               e.transfer.byte,e.transfer.read,e.transfer.acknowledge,uint64_t(e.response),e.control,e.status,e.divider});
        if (c.forward && c.original.i2cObserver) c.original.i2cObserver(c.original.i2cContext,e);
    }
    static void cs4(void* p,const Imx1ProbeCs4Event& e) {
        auto& c=*static_cast<Capture*>(p);
        c.add({5,e.step,e.pc,e.address,e.size,e.value,e.write,uint64_t(e.status),uint64_t(e.index),uint64_t(e.bank)});
        if(c.forward && c.original.cs4Observer)c.original.cs4Observer(c.original.cs4Context,e);
    }
    static void reset(void* p,const Imx1ProbeCs5ResetEvent& e) {
        auto& c=*static_cast<Capture*>(p);
        c.add({6,e.step,e.pc,e.address,e.size,e.value,e.write,uint64_t(e.status),e.phase});
        if(c.forward && c.original.cs5ResetObserver)c.original.cs5ResetObserver(c.original.cs5ResetContext,e);
    }
    static void config(void* p,const Imx1ProbeCs5ConfigurationEvent& e) {
        auto& c=*static_cast<Capture*>(p);
        c.add({7,e.step,e.pc,e.address,e.size,e.value,e.write,uint64_t(e.status),e.defaultsKnown,e.knownMask});
        if(c.forward && c.original.cs5ConfigurationObserver)c.original.cs5ConfigurationObserver(c.original.cs5ConfigurationContext,e);
    }
    Imx1ProbeOptions options() {
        auto o=original;
        o.busObserver=bus;o.busContext=this;o.interruptObserver=irq;o.interruptContext=this;
        o.i2cObserver=i2c;o.i2cContext=this;o.cs4Observer=cs4;o.cs4Context=this;
        o.cs5ResetObserver=reset;o.cs5ResetContext=this;o.cs5ConfigurationObserver=config;o.cs5ConfigurationContext=this;
        if(!forward){o.frameObserver=nullptr;o.diagnosticSnapshot=nullptr;}
        return o;
    }
};
inline void same(const Imx1ProbeResult& a,const Imx1ProbeResult& b) {
    assert(a.stop==b.stop && a.trace.step==b.trace.step && a.trace.pc==b.trace.pc &&
           a.trace.instruction==b.trace.instruction && a.trace.cpsr==b.trace.cpsr &&
           a.trace.operand==b.trace.operand && a.trace.instructionSize==b.trace.instructionSize);
    assert(a.address==b.address && a.value==b.value && a.size==b.size && a.cpsr==b.cpsr && a.spsr==b.spsr);
    assert(!std::memcmp(a.registers,b.registers,sizeof(a.registers)));
    assert(a.jitBlocks==b.jitBlocks && a.jitInstructions==b.jitInstructions && a.busAccesses==b.busAccesses &&
           a.cs5WaitClocks==b.cs5WaitClocks && a.idleHclk==b.idleHclk && a.irqEntries==b.irqEntries && a.fiqEntries==b.fiqEntries);
    assert(a.graphicsFrameHash==b.graphicsFrameHash && a.graphicsFrameWidth==b.graphicsFrameWidth &&
           a.graphicsFrameHeight==b.graphicsFrameHeight && a.graphicsLcdEnabled==b.graphicsLcdEnabled);
    assert(a.peripheralStatus==b.peripheralStatus && a.gpioEventApplied==b.gpioEventApplied &&
           a.elapsedInterruptBoundary==b.elapsedInterruptBoundary &&
           a.possibleWatchdogTimeoutBoundary==b.possibleWatchdogTimeoutBoundary);
}
}
inline Imx1ProbeResult machineParityProbe(const uint8_t* rom,uint32_t bytes,Imx1ProbeEntry entry,
        uint32_t steps,Imx1ProbeObserver observer=nullptr,void* context=nullptr,bool fast=false,
        const Imx1ProbeOptions* options=nullptr) {
    using namespace machine_test;
    Capture baseline,split;
    baseline.original=split.original=options?*options:Imx1ProbeOptions{};
    baseline.observer=observer;baseline.context=context;baseline.forward=true;
    auto o=baseline.options();
    const auto expected=imx1BootProbe(rom,bytes,entry,steps,Capture::trace,&baseline,fast,&o);
    // Invalid wrapper requests retain their own pre-allocation validation.
    if(!steps || steps>imx1ProbeMaximumSteps || !imx1ProbeGpioEventValid(o,steps) ||
       o.idleHclkBudget>imx1ProbeMaximumIdleHclk || expected.stop==Imx1ProbeStop::engineUnavailable)return expected;
    o=split.options();Imx1ProbeResult error{};
    auto* machine=imx1MachineCreate(rom,bytes,entry,fast,o,Capture::trace,&split,&error);
    if(!machine){same(expected,error);return expected;}
    // A second live machine is refused without changing the first one's engine.
    assert(!imx1MachineCreate(rom,bytes,entry,!fast,o));
    auto state=imx1MachineInspect(machine);
    uint32_t round=0;
    while(state.retiredInstructions<steps) {
        const auto before=state;
        const auto hash=split.hash,count=split.count;
        same(before.result,imx1MachineRunSlice(machine,0,0).result);
        uint32_t w=0,h=0,first=0,last=0;
        imx1MachinePeekFrame(machine,w,h);
        imx1MachineAcquireFrame(machine,w,h,first,last);
        imx1MachineAcknowledgeFrame(machine);
        assert(hash==split.hash && count==split.count);
        const uint32_t budget=std::min<uint64_t>(steps-state.retiredInstructions,++round%7+1);
        state=imx1MachineRunSlice(machine,budget,o.idleHclkBudget);
        if(state.status==Imx1MachineStatus::terminal || state.status==Imx1MachineStatus::waiting ||
           state.result.stop==Imx1ProbeStop::idleBudget)break;
    }
    same(expected,state.result);
    assert(baseline.count==split.count && baseline.hash==split.hash);
    if(state.status==Imx1MachineStatus::terminal) {
        const auto hash=split.hash,count=split.count;
        same(state.result,imx1MachineRunSlice(machine,10,UINT64_MAX).result);
        assert(hash==split.hash && count==split.count);
    }
    imx1MachineStop(machine);
    assert(imx1MachineRunSlice(machine,10,UINT64_MAX).status==Imx1MachineStatus::stopped);
    same(state.result,imx1MachineInspect(machine).result);
    assert(imx1MachineReplacePenInputs(machine,{},false)==Imx1BootstrapAccess::sequence);
    return expected;
}
#endif
