// PXA25x USB unit clock with real SoC DMA, host transactions and state rollback.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "CPU.h"
#include "soc_PXA.h"
#include "UsbHostBridge.h"
struct ClockSoc:SocPXA {using SocPXA::SocPXA;using SocPXA::BeginSerialCheckpoint;using SocPXA::EndSerialCheckpoint;};
using R=Pxa270UdcHostResult;
static constexpr uint32_t CKEN=0x41300004, USB=0x800;
static void write(SocPXA& s,uint32_t a,uint32_t v){assert(cpuMemOpExternal(s.GetCpu(),&v,a,4,true));}
static uint32_t read(SocPXA& s,uint32_t a){uint32_t v=0;assert(cpuMemOpExternal(s.GetCpu(),&v,a,4,false));return v;}
static std::vector<uint8_t> save(SocPXA& s){assert(s.Save());auto b=s.GetSavestate();return {static_cast<uint8_t*>(b.data),static_cast<uint8_t*>(b.data)+b.size};}
static bool sameChunk(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b,ChunkType type){
    auto* x=reinterpret_cast<const uint32_t*>(a.data());auto* y=reinterpret_cast<const uint32_t*>(b.data());
    assert(a.size()==b.size() && x[0]==y[0]);size_t at=1+2*x[0];
    for(unsigned i=0;i<x[0];++i){if(x[1+2*i]==uint32_t(type))return !std::memcmp(x+at,y+at,x[2+2*i]*4);at+=x[2+2*i];}assert(false);return false;
}
static void run(SocPXA& s){assert(s.Run(100000,100000000)>=100000);}
static void start(SocPXA& s,unsigned line,uint32_t source,uint32_t target,bool rx){
    write(s,0x40000100+4*line,0x8f);write(s,0x4000003c,0x40000017);
    write(s,0x400002f4,source);write(s,0x400002f8,target);
    write(s,0x400002fc,(rx?0x60234000:0x90234000)|64);write(s,0x4000003c,0xc0000000);
}
int main(){
    std::vector<uint8_t> rom(4096);const uint32_t bootLoop=0xeafffffe;std::memcpy(rom.data(),&bootLoop,4);ClockSoc s(deviceTypeTG50,16*1024*1024,rom.data(),rom.size(),nullptr,0,-1,0);
    auto* ram=static_cast<uint8_t*>(s.GetMemoryData().data);const uint32_t loop=0xeafffffe;
    std::memcpy(ram+0x3000,&loop,4);cpuSetReg(s.GetCpu(),ARM_REG_NUM_CPSR,ARM_SR_MODE_SVC|ARM_SR_I|ARM_SR_F);cpuSetReg(s.GetCpu(),15,0xa0003000);
    for(unsigned i=0;i<64;++i)ram[0x1000+i]=uint8_t(i*37+19);
    auto p=s.GetUsbPort();write(s,0x40600000,1);const uint8_t cfg[]={0,9,1,0,0,0,0,0};
    assert(p.Setup(cfg)==R::Ack);write(s,0x40600010,0x81);
    const uint32_t clocks=read(s,CKEN);assert(clocks&USB);
    write(s,CKEN,clocks&~USB);assert(!p.Enabled() && !p.Listening() && !p.Reset() && !p.Frame());
    uint8_t data[64]{};unsigned size=0;
    assert(p.Setup(cfg)==R::Disabled && p.Out(data,1)==R::Disabled && p.In(data,&size)==R::Disabled);
    assert(p.EpOut(7,data,64)==R::Disabled && p.EpIn(6,data,&size)==R::Disabled && p.PeekIn(6,&size)==R::Disabled);
    start(s,30,0xa0001000,0x40600600,false);assert(!(read(s,0x4000003c)&0x100));
    run(s);assert((read(s,0x400002fc)&0x1fff)==64);
    // Repeated unchanged clock writes do not disturb the saved scheduler phase.
    auto off=save(s);write(s,CKEN,clocks&~USB);auto repeated=save(s);
    for(auto type:{ChunkType::cpu,ChunkType::scheduler,ChunkType::pxaDma,ChunkType::pxa255udc})assert(sameChunk(off,repeated,type));
    off=repeated;
    // A candidate with the opposite clock is rejected after loading its clock chunk.
    write(s,CKEN,clocks);auto on=save(s);assert(s.Load(off.size(),off.data()));off=save(s);
    assert(!s.Load(on.size()-4,on.data()));assert(save(s)==off && !p.Enabled());
    std::vector<uint8_t> memory(ram,ram+16*1024*1024);
    ClockSoc fresh(deviceTypeTG50,16*1024*1024,rom.data(),rom.size(),nullptr,0,-1,0);
    std::memcpy(fresh.GetMemoryData().data,memory.data(),memory.size());assert(fresh.Load(off.size(),off.data()));auto q=fresh.GetUsbPort();
    assert(!q.Enabled());run(fresh);assert((read(fresh,0x400002fc)&0x1fff)==64);
    write(fresh,CKEN,clocks);assert(q.Enabled() && (read(fresh,0x4000003c)&0x100));run(fresh);
    assert(!(read(fresh,0x400002fc)&0x1fff));assert(q.EpIn(6,data,&size)==R::Ack && size==64);
    assert(std::memcmp(data,memory.data()+0x1000,64)==0);
    // OUT data remains queued while its unit clock is stopped.
    assert(q.EpOut(7,data,64)==R::Ack);write(fresh,CKEN,clocks&~USB);
    start(fresh,31,0x40600680,0xa0002000,true);run(fresh);assert((read(fresh,0x400002fc)&0x1fff)==64);
    write(fresh,CKEN,clocks);run(fresh);assert(!(read(fresh,0x400002fc)&0x1fff));
    assert(std::memcmp(static_cast<uint8_t*>(fresh.GetMemoryData().data)+0x2000,data,64)==0);
    // Sony's existing PWM observer still updates and validates its own clock state.
    write(fresh,CKEN,clocks&~1u);auto pwmOff=save(fresh);assert(fresh.Load(pwmOff.size(),pwmOff.data()));
    write(fresh,CKEN,clocks);auto pwmOn=save(fresh);assert(fresh.Load(pwmOn.size(),pwmOn.data()));
    UsbHostBridge bridge;std::string error;
    assert(bridge.Start(&fresh,{HostSerial::Kind::pty,0},error));const auto* host=bridge.Host();
    fresh.Run(600000,1000000);bridge.Service();run(fresh);
    assert(host->GetPhase()==UsbBusHost::Phase::waitEnable && host->NextWake()==UINT64_MAX);
    const auto wake=host->NextWake();const auto ticks=host->GetCounters().ticks;
    auto guarded=save(fresh);fresh.BeginSerialCheckpoint();
    write(fresh,CKEN,clocks&~USB);write(fresh,CKEN,clocks);bridge.Service();
    assert(host->NextWake()==wake && host->GetCounters().ticks==ticks);
    auto afterGuard=save(fresh);
    for(auto type:{ChunkType::cpu,ChunkType::scheduler})assert(sameChunk(guarded,afterGuard,type));
    fresh.EndSerialCheckpoint();bridge.Service();assert(host->NextWake()==wake);
    // A clock enable committed under the guard is noticed after releasing it.
    write(fresh,CKEN,clocks&~USB);bridge.Service();run(fresh);
    assert(host->NextWake()==UINT64_MAX);
    fresh.BeginSerialCheckpoint();write(fresh,CKEN,clocks);bridge.Service();
    assert(host->NextWake()==UINT64_MAX);fresh.EndSerialCheckpoint();bridge.Service();
    assert(host->NextWake()==0);bridge.Stop();
    // Real PXA25x sleep stops USB even when CKEN11 remains set.
    write(fresh,0x08000028,1);write(fresh,0x080000a8,0x100);write(fresh,0x08000128,0x100);
    write(fresh,0x40f0000c,1u<<14);write(fresh,0x40f00014,1u<<14);
    write(fresh,0xa0003000,0xee070e10);cpuInvalidateCodeCache(fresh.GetCpu());
    cpuSetReg(fresh.GetCpu(),15,0xa0003000);cpuSetReg(fresh.GetCpu(),0,3);cpuCyclePure(fresh.GetCpu());
    assert(fresh.PowerSleeping() && !q.Enabled());auto sleeping=save(fresh);
    assert(fresh.Load(sleeping.size(),sleeping.data()) && !q.Enabled());
    fresh.KeyDown(keyIdPower);fresh.Run(100000,1000000);assert(!fresh.PowerSleeping());
    assert(read(fresh,CKEN)&USB);write(fresh,0x40600000,1);assert(q.Enabled());
    write(fresh,CKEN,clocks&~USB);fresh.Reset();assert(read(fresh,CKEN)&USB);
    write(fresh,0x40600000,1);assert(q.Enabled());
    puts("PASS: CKEN11 gates USB host/DMA, preserves pending TX/RX and exact rollback/fresh state, Sony PWM observer retained");
}
