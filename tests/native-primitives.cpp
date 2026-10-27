#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "CPU.h"
#include "MMU.h"
#include "RAM.h"
#include "ROM.h"
#include "memory_buffer.h"
#include "patch_dispatch.h"
#include "system_state.h"
#include "soc_PXA.h"
#include "savestate/savestateAll.h"

// No firmware: every executed instruction and translation descriptor is supplied here.
constexpr uint32_t code = 0x10000, callbackPc = code + 0x100;
constexpr uint32_t done = SLOW_PATH_REASON_INJECTED_CALL_DONE;
constexpr uint32_t fault = SLOW_PATH_REASON_NATIVE_FAULT;
struct Region {
    uint32_t base;
    std::vector<uint8_t> bytes;
    unsigned reads = 0, writes = 0;
    Region(uint32_t b, unsigned n):base(b),bytes(n) {}
    static bool access(void* opaque, uint32_t a, uint_fast8_t n, bool w, void* p) {
        auto& r = *static_cast<Region*>(opaque);
        if (a < r.base || uint64_t(a) + n > uint64_t(r.base) + r.bytes.size()) return false;
        if (w) { ++r.writes; memcpy(r.bytes.data() + a-r.base, p, n); }
        else { ++r.reads; memcpy(p, r.bytes.data() + a-r.base, n); }
        return true;
    }
    void put(uint32_t a, uint32_t v) { assert(a >= base && a-base+4 <= bytes.size()); memcpy(bytes.data()+a-base,&v,4); }
};
struct Fixture {
    Region ram{code,0x40000};
    std::vector<uint32_t> dirty = std::vector<uint32_t>(8);
    Region rom{0x50000,0x1000}, io{0x60000,0x1000};
    PacePatch pace{};
    ArmMem* mem = memInit();
    ArmCpu* cpu;
    Fixture() {
        MemoryBuffer buffer{ram.bytes.size(),dirty.size()*sizeof(uint32_t),ram.bytes.data(),dirty.data(),false};
        assert(ramInit(mem,nullptr,ram.base,ram.bytes.size(),&buffer,true));
        assert(romInit(mem,rom.base,rom.bytes.data(),rom.bytes.size()));
        assert(memRegionAdd(mem,io.base,io.bytes.size(),Region::access,&io));
        auto pd = initPatchDispatch();
        cpu = cpuInit(code,mem,ARM_MEMORY_SYSTEM_MMU,true,false,-1,0x69054114,0x0b16a16a,pd,&pace,createSystemState());
        patchDispatchSetCpu(pd,cpu);
        ram.put(code,0xe12fff1e); // BX LR
        ram.put(callbackPc,0xe12fff1e);
        ram.put(callbackPc+4,0x47704770); // Thumb BX LR
    }
    uint32_t run(bool injected=true, unsigned cycles=32) {
        return injected ? cpuCycle<ARM_MEMORY_SYSTEM_MMU,true>(cpu,cycles)
                        : cpuCycle<ARM_MEMORY_SYSTEM_MMU,false>(cpu,cycles);
    }
    void start(uint32_t pc=code) { assert(cpuStartNativeCall(cpu,pc)); }
    // Identity section zero for code/tables; fine table maps adjacent virtual
    // 1KB pages to separate physical RAM regions, followed by an unmapped page.
    void mmu() {
        ram.put(0x14000,0x00000c02);
        ram.put(0x14004,0x00018003);
        ram.put(0x18000,0x30000|0x33);
        ram.put(0x18004,0x40000|0x33);
        mmuSetDomainCfg(cpuGetMMU(cpu),3);
        mmuSetTTP(cpuGetMMU(cpu),0x14000);
        mmuTlbFlush(cpuGetMMU(cpu));
        cpuInvalidateCodeCache(cpu);
    }
};
static void count(ArmCpu* c,void* p) { ++*static_cast<unsigned*>(p); cpuSetReg(c,0,0x1234); }

static void exactHooks() {
    Fixture f; auto c=f.cpu; unsigned calls=0;
    assert(!cpuSetNativePcBreak(c,code+2,0));
    assert(!cpuRegisterNativeCallback(c,code+2,count,&calls));
    assert(cpuRegisterNativeCallback(c,callbackPc,count,&calls));
    assert(!cpuRegisterNativeCallback(c,callbackPc,count,&calls));
    cpuSetReg(c,14,code); cpuSetReg(c,15,callbackPc);
    f.run(false,1); assert(calls==0); // registered hooks never intercept ordinary execution
    assert(cpuSetNativePcBreak(c,callbackPc|1,code));
    cpuSetReg(c,15,callbackPc); f.run(false,1);
    assert(!(cpuGetSlowPathReason(c)&SLOW_PATH_REASON_NATIVE_PC)); // ARM != Thumb
    cpuSetReg(c,15,callbackPc|1); f.run(false);
    assert(cpuGetRegisters(c)[15]==callbackPc);
    assert(cpuGetSlowPathReason(c)&SLOW_PATH_REASON_NATIVE_PC);
    cpuSetReg(c,15,code);
    assert(f.run(false)==0); // second PC stops before fetching BX LR
    assert(cpuGetRegisters(c)[15]==code);
    assert(cpuSetNativePcBreak(c,callbackPc,0));
    f.start(callbackPc);
    assert(!cpuSetNativePcBreak(c,0,0)); f.run();
    assert(calls==1 && cpuGetRegExternal(c,0)==0x1234);
    assert(cpuGetSlowPathReason(c)&done); cpuEndNativeCall(c);
    assert(cpuSetNativePcBreak(c,0,0));
    // Thumb entry must not hit the ARM callback at the same byte address.
    f.ram.put(callbackPc,0x47704770); cpuInvalidateCodeCache(c);
    f.start(callbackPc|1); f.run(); assert(calls==1); assert(cpuGetSlowPathReason(c)&done); cpuEndNativeCall(c);
    assert(cpuRegisterNativeCallback(c,callbackPc|1,count,&calls));
    f.start(callbackPc|1); f.run(); assert(calls==2); cpuEndNativeCall(c);
    cpuUnregisterNativeCallback(c,callbackPc|1);
    f.start(callbackPc|1); f.run(); assert(calls==2); cpuEndNativeCall(c);
}

// Use the public injection checkpoint protocol used by real nested native calls.
struct Checkpoint {
    ArmCpu* cpu; Savestate<ChunkType> state;
    void Save(Savestate<ChunkType>& s) { cpuPrepareInjectedCall(cpu,s); }
    void Save(SavestateProbe<ChunkType>& s) { cpuPrepareInjectedCall(cpu,s); }
    void Load(SavestateLoader<ChunkType>& l) { cpuFinishInjectedCall(cpu,l); }
    explicit Checkpoint(ArmCpu* c):cpu(c) { state.Save(*this); }
    void restore() { SavestateLoader<ChunkType> l; l.Load(state.GetBuffer(),state.GetSize(),*this); }
};
struct Nested { unsigned outer=0, inner=0; };
static void inner(ArmCpu*,void* p) { ++static_cast<Nested*>(p)->inner; }
static void outer(ArmCpu* c,void* p) {
    auto& n=*static_cast<Nested*>(p); ++n.outer;
    Checkpoint saved(c);
    assert(cpuStartNativeCall(c,callbackPc+4));
    cpuCycle<ARM_MEMORY_SYSTEM_MMU,true>(c,32);
    assert(cpuGetSlowPathReason(c)&done);
    cpuEndNativeCall(c); saved.restore();
    cpuUnregisterNativeCallback(c,callbackPc); // safe while this callback is executing
    cpuSetReg(c,0,0xabcdef);
}
static void nestedHooks() {
    Fixture f; Nested n;
    assert(cpuAcquireNativeSession(f.cpu)); assert(!cpuAcquireNativeSession(f.cpu));
    assert(cpuRegisterNativeCallback(f.cpu,callbackPc,outer,&n));
    assert(cpuRegisterNativeCallback(f.cpu,callbackPc+4,inner,&n));
    f.start(callbackPc); f.run();
    assert(n.outer==1 && n.inner==1 && cpuGetRegExternal(f.cpu,0)==0xabcdef);
    assert(cpuGetSlowPathReason(f.cpu)&done); cpuEndNativeCall(f.cpu);
    f.start(callbackPc+4); f.run(); assert(n.inner==2); cpuEndNativeCall(f.cpu);
    for (unsigned i=0;i<4;++i) f.start();
    assert(!cpuStartNativeCall(f.cpu,code));
    for (unsigned i=0;i<4;++i) cpuEndNativeCall(f.cpu);
    cpuClearNativeRuntime(f.cpu); assert(cpuAcquireNativeSession(f.cpu));
}

static void buffers() {
    Fixture f; auto c=f.cpu; uint32_t v=0x11223344;
    assert(cpuNativeBuffer(c,0xffffffff,nullptr,0,true));
    assert(cpuNativeBuffer(c,0xffffffff,nullptr,0,false));
    assert(cpuNativeRamRange(c,0xffffffff,0));
    assert(!cpuNativeBuffer(c,code,nullptr,1,false));
    assert(!cpuNativeRamRange(c,0xfffffffc,8));
    assert(!cpuNativeBuffer(c,f.io.base,&v,4,false));
    assert(!cpuNativeBuffer(c,f.io.base,&v,4,true));
    assert(f.io.reads==0 && f.io.writes==0);
    f.rom.put(f.rom.base,0xabcdef01);
    const auto originalRom=f.rom.bytes;
    assert(cpuNativeBuffer(c,f.rom.base,&v,4,false) && v==0xabcdef01);
    assert(!cpuNativeBuffer(c,f.rom.base,&v,4,true));
    assert(!cpuNativeRamRange(c,f.rom.base,4)); assert(f.rom.bytes==originalRom);
    f.mmu();
    assert(!cpuNativeBuffer(c,f.io.base,&v,4,false));
    assert(!cpuNativeBuffer(c,f.io.base,&v,4,true));
    assert(!cpuNativeBuffer(c,f.rom.base,&v,4,true));
    assert(f.io.reads==0 && f.io.writes==0 && f.rom.bytes==originalRom);
    std::vector<uint8_t> data(0x410), read(data.size());
    for(unsigned i=0;i<data.size();++i) data[i]=i*17+3;
    auto expected=f.ram.bytes;
    memcpy(expected.data()+0x303f0-code,data.data(),16);
    memcpy(expected.data()+0x40000-code,data.data()+16,data.size()-16);
    assert(cpuNativeRamRange(c,0x1003f0,data.size()));
    assert(cpuNativeBuffer(c,0x1003f0,data.data(),data.size(),true));
    assert(f.ram.bytes==expected); // physical fragments changed, intervening RAM untouched
    assert(cpuNativeBuffer(c,0x1003f0,read.data(),read.size(),false)); assert(read==data);
    const auto before=f.ram.bytes;
    assert(!cpuNativeBuffer(c,0x1007f0,data.data(),32,true));
    assert(f.ram.bytes==before); // validate the entire range before writing its prefix
    assert(!cpuNativeRamRange(c,0x1007f0,32));
    std::vector<uint8_t> untouched(32,0xa5), output=untouched;
    assert(!cpuNativeBuffer(c,0x1007f0,output.data(),output.size(),false));
    assert(output==untouched);
}

static void scratch() {
    Fixture f; auto c=f.cpu; std::vector<uint8_t> bytes(4096); uint32_t value=0x87654321,read=0;
    assert(!cpuSetNativeScratch(c,0x200000,bytes.data(),bytes.size())); // MMU disabled
    f.mmu();
    assert(!cpuSetNativeScratch(c,code,bytes.data(),bytes.size())); // existing RAM
    assert(!cpuSetNativeScratch(c,f.rom.base,bytes.data(),bytes.size()));
    assert(!cpuSetNativeScratch(c,f.io.base,bytes.data(),bytes.size()));
    assert(!cpuSetNativeScratch(c,0x200001,bytes.data(),bytes.size()));
    assert(!cpuSetNativeScratch(c,0x200000,bytes.data(),4095));
    assert(cpuSetNativeScratch(c,0x200000,bytes.data(),bytes.size()));
    assert(!cpuMemOpExternal(c,&read,0x200000,4,false));
    f.ram.put(code,0xe5801000); // STR r1,[r0]
    f.ram.put(code+4,0xe12fff1e);
    cpuInvalidateCodeCache(c);
    cpuSetReg(c,0,0x200000); cpuSetReg(c,1,value);
    f.start(); f.run();
    assert(cpuGetSlowPathReason(c)&done);
    memcpy(&read,bytes.data(),4); assert(read==value);
    assert(!cpuSetNativeScratch(c,0,nullptr,0));
    assert(cpuMemOpExternal(c,&value,0x200ffc,4,true));
    assert(cpuMemOpExternal(c,&read,0x200ffc,4,false) && read==value);
    assert(!cpuMemOpExternal(c,&read,0x201000,4,false));
    assert(!cpuNativeBuffer(c,0x200000,&read,4,false)); // scratch never becomes mapped guest RAM
    cpuEndNativeCall(c);
    assert(!cpuMemOpExternal(c,&read,0x200ffc,4,false));
    assert(cpuSetNativeScratch(c,0,nullptr,0));
    f.start(); assert(!cpuMemOpExternal(c,&read,0x200ffc,4,false)); cpuEndNativeCall(c);
}

static void stackOwner() {
    Fixture f; auto c=f.cpu;
    f.ram.put(code,0xeafffffe); // B .
    const uint32_t owner=code+0x200;
    cpuSetReg(c,ARM_REG_NUM_CPSR,ARM_SR_MODE_SYS);
    cpuSetNativeStackBounds(c,code+0x1000,code+0x2000,owner,7);
    f.ram.put(owner,7);
    for(auto sp:{code+0xffc,code+0x2004}) {
        cpuSetReg(c,13,sp); f.start(); f.run();
        assert(cpuGetSlowPathReason(c)&fault); assert(cpuGetRegisters(c)[15]==code); cpuEndNativeCall(c);
    }
    for(auto sp:{code+0x1000,code+0x2000}) {
        cpuSetReg(c,13,sp); f.start(); f.run(); assert(!(cpuGetSlowPathReason(c)&fault)); cpuEndNativeCall(c);
    }
    cpuSetReg(c,13,code+0x3000); f.ram.put(owner,8); // another scheduled task owns this stack
    f.start(); f.run(); assert(!(cpuGetSlowPathReason(c)&fault)); cpuEndNativeCall(c);
    cpuSetNativeStackBounds(c,code+0x1000,code+0x2000,0xdead0000,7);
    f.start(); f.run(); assert(cpuGetSlowPathReason(c)&fault); cpuEndNativeCall(c);
    cpuSetReg(c,ARM_REG_NUM_CPSR,ARM_SR_MODE_SVC); cpuSetReg(c,13,0);
    f.start(); f.run(); assert(!(cpuGetSlowPathReason(c)&fault)); cpuEndNativeCall(c);
}


struct FullCpuState {
    ArmCpu* cpu; Savestate<ChunkType> state;
    void Save(Savestate<ChunkType>& s) { cpuSave(cpu,s); }
    void Save(SavestateProbe<ChunkType>& s) { cpuSave(cpu,s); }
    void Load(SavestateLoader<ChunkType>& l) { cpuLoad(cpu,l); }
    explicit FullCpuState(ArmCpu* c):cpu(c) { state.Save(*this); }
    void restore() { SavestateLoader<ChunkType> l; l.Load(state.GetBuffer(),state.GetSize(),*this); }
};
static void runtimeReplacement() {
    for(unsigned replacement=0;replacement<2;++replacement) {
        Fixture f; auto c=f.cpu; f.mmu(); FullCpuState saved(c);
        unsigned calls=0; std::vector<uint8_t> scratch(4096,0x5a);
        assert(cpuAcquireNativeSession(c));
        assert(cpuRegisterNativeCallback(c,callbackPc,count,&calls));
        assert(cpuSetNativePcBreak(c,callbackPc,0));
        assert(cpuSetNativeScratch(c,0x200000,scratch.data(),scratch.size()));
        cpuSetNativeStackBounds(c,0x300000,0x301000);
        f.start();
        cpuSetSlowPath(c,fault|SLOW_PATH_REASON_NATIVE_PC);
        if(replacement==0) cpuReset(c,code); else saved.restore();
        assert(!(cpuGetSlowPathReason(c)&(fault|SLOW_PATH_REASON_NATIVE_PC)));
        assert(cpuAcquireNativeSession(c)); // session and depth cleared
        cpuSetReg(c,ARM_REG_NUM_CPSR,ARM_SR_MODE_SYS); cpuSetReg(c,13,0);
        f.start(callbackPc); f.run();
        assert(cpuGetSlowPathReason(c)&done); // stack bounds and callback cleared
        assert(calls==0);
        uint32_t value=0;
        assert(!cpuMemOpExternal(c,&value,0x200000,4,false)); // no stale overlay in a fresh call
        cpuEndNativeCall(c);
        cpuSetReg(c,15,callbackPc); cpuSetReg(c,14,code); f.run(false,1);
        assert(!(cpuGetSlowPathReason(c)&SLOW_PATH_REASON_NATIVE_PC));
    }
}

class TestSoc : public SocPXA {
 public:
    using SocPXA::SocPXA;
    uint64_t budget() const { return runningCyclesPerSecond; }
};
static void throws(ArmCpu*,void*) { throw std::runtime_error("synthetic callback failure"); }
static void nestedFailure(ArmCpu* c,void* opaque) {
    auto& s=*static_cast<TestSoc*>(opaque);
    assert(s.budget()==123456000);
    Checkpoint saved(c);
    assert(cpuStartNativeCall(c,0xa0003010));
    bool caught=false;
    try { s.ExecuteInjected(32,76543000); } catch(const std::runtime_error&) { caught=true; }
    assert(caught && s.budget()==123456000);
    cpuEndNativeCall(c); saved.restore();
    throw std::runtime_error("outer callback failure after nested recovery");
}
static void socLifecycle() {
    std::vector<uint8_t> rom(16*1024*1024),nand(132*1024*1024,0xff);
    TestSoc s(DeviceType5::deviceTypeTX,32*1024*1024,rom.data(),rom.size(),nand.data(),nand.size(),-1,2);
    auto c=s.GetCpu(); auto ram=static_cast<uint8_t*>(s.GetMemoryData().data);
    constexpr uint32_t pc=0xa0003000;
    uint32_t loop=0xeafffffe,ret=0xe12fff1e;
    memcpy(ram+0x3000,&loop,4); memcpy(ram+0x3010,&ret,4);
    cpuSetReg(c,15,pc);
    cpuSetSlowPath(c,SLOW_PATH_REASON_PACE_SYSCALL_BREAK);
    assert(s.RunToPaceSyscall(0xa11d,0,100000000));
    cpuClearSlowPath(c,SLOW_PATH_REASON_PACE_SYSCALL_BREAK);
    assert(s.RunToNativePc(pc,0,100,100000000));
    assert(!s.RunToPaceSyscall(0xa11d,0,100000000)); // stale cached marker consumed
    cpuSetSlowPath(c,SLOW_PATH_REASON_PACE_SYSCALL_BREAK);
    assert(s.RunToPaceSyscall(0xa11d,0,100000000));
    cpuClearSlowPath(c,SLOW_PATH_REASON_PACE_SYSCALL_BREAK);
    cpuSetReg(c,15,pc);
    assert(!s.RunToNativePc(pc+0x20,0,32,100000000));
    assert(!s.RunToPaceSyscall(0xa11d,0,100000000)); // timeout also consumes cached marker
    assert(cpuRegisterNativeCallback(c,pc+0x10,throws,nullptr));
    assert(cpuRegisterNativeCallback(c,pc+0x20,nestedFailure,&s));
    assert(cpuStartNativeCall(c,pc+0x20));
    bool caught=false;
    try { s.ExecuteInjected(100,123456000); } catch(const std::runtime_error&) { caught=true; }
    assert(caught && s.budget()==0);
    cpuEndNativeCall(c); cpuClearNativeRuntime(c);
    assert(cpuStartNativeCall(c,pc)); assert(!s.ExecuteInjected(32,100000000));
    cpuEndNativeCall(c); assert(!(cpuGetSlowPathReason(c)&(fault|done)));
    assert(cpuStartNativeCall(c,0x70000000)); assert(!s.ExecuteInjected(32,100000000));
    assert(cpuGetSlowPathReason(c)&fault); cpuEndNativeCall(c); cpuClearNativeRuntime(c);
    cpuSetReg(c,ARM_REG_NUM_CPSR,ARM_SR_MODE_SVC); // restore mode after synthetic fault
    assert(cpuStartNativeCall(c,pc+0x10)); assert(s.ExecuteInjected(32,100000000)); cpuEndNativeCall(c);
    // Reset and full savestate load discard host callbacks, depth, sessions and PC breaks.
    assert(s.Save()); auto saved=s.GetSavestate();
    std::vector<uint8_t> snapshot(static_cast<uint8_t*>(saved.data),static_cast<uint8_t*>(saved.data)+saved.size);
    unsigned calls=0;
    for(unsigned which=0;which<2;++which) {
        assert(cpuAcquireNativeSession(c));
        assert(cpuRegisterNativeCallback(c,pc+0x10,count,&calls));
        assert(cpuSetNativePcBreak(c,pc+0x10,0));
        assert(cpuStartNativeCall(c,pc));
        if(which==0) s.Reset(); else assert(s.Load(snapshot.size(),snapshot.data()));
        assert(cpuAcquireNativeSession(c));
        assert(cpuRegisterNativeCallback(c,pc+0x10,count,&calls)); // slot was cleared
        cpuUnregisterNativeCallback(c,pc+0x10);
        assert(cpuStartNativeCall(c,pc+0x10)); assert(s.ExecuteInjected(32,100000000));
        assert(calls==0); cpuEndNativeCall(c); cpuClearNativeRuntime(c);
        cpuSetReg(c,15,pc+0x10); cpuSetReg(c,14,pc);
        s.Run(1,100000000); assert(!(cpuGetSlowPathReason(c)&SLOW_PATH_REASON_NATIVE_PC));
    }
}
int main(int argc,char** argv) {
    assert(argc==2);
    const std::string name=argv[1];
    if(name=="exact-hooks") exactHooks();
    else if(name=="nested-hooks") nestedHooks();
    else if(name=="buffers") buffers();
    else if(name=="scratch") scratch();
    else if(name=="stack-owner") stackOwner();
    else if(name=="runtime-replacement") runtimeReplacement();
    else if(name=="soc-lifecycle") socLifecycle();
    else return 2;
    printf("PASS: %s\n",argv[1]);
}
