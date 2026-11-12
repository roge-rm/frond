#include "native_boundary.h"
#ifndef __EMSCRIPTEN__
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <exception>
#include <map>
#include <set>
#include <stdexcept>
#include "CPU.h"
#include "Defer.h"
#include "SoC.h"
#include "savestate/ChunkTypeUarm.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"
#include "syscall_dispatch.h"

namespace NativeBoundary {
namespace {
constexpr uint32_t task = 0x1ff02218, nativeTable = 0x1ff05e3c;
constexpr uint32_t waitPc = 0x2004d536, allocationSize = 32 * 1024;
constexpr uint32_t dataSize = 8192, bootstrapBase = 0x7e000000;
constexpr uint64_t budget = 50000000;
uint32_t word(ArmCpu* cpu, uint32_t a) {
    uint32_t v;
    if (!cpuInspectRead(cpu,a,&v,4)) throw std::runtime_error("native metadata is not mapped RAM/ROM");
    return v;
}
bool bytes(ArmCpu* cpu, uint32_t a, const void* p, uint32_t n) {
    std::vector<uint8_t> data(n);
    return cpuInspectRead(cpu,a,data.data(),n) && !memcmp(data.data(),p,n);
}
bool owner(ArmCpu* cpu) {
    return word(cpu,0x1ff00630)==0x1ff020f8 &&
           word(cpu,0x1ff042b8)==task && word(cpu,0x1ff042bc)==task &&
           word(cpu,0x1ff042c8)==0 && // scheduler disable/lock bytes
           (word(cpu,task+0x18)&0xffffff00)==0x100;
}
// This profile deliberately excludes the pen/silk wait at 0x2004db92.
// The verified route is EvtGetSysEvent -> original event-manager wait,
// with the matching complete prologue and its saved return slot.
bool validate(SoC& soc) {
    auto* cpu=soc.GetCpu();
    if (soc.GetDeviceType()!=DeviceType5::deviceTypeTX || cpuIsModePace(cpu) ||
        cpuGetMemorySystemKind(cpu)!=ARM_MEMORY_SYSTEM_MMU) return false;
    auto* r=cpuGetRegisters(cpu);
    if (r[15]!=waitPc || !cpuIsThumb(cpu) ||
        (cpuGetRegExternal(cpu,ARM_REG_NUM_CPSR)&0xff)!=0x3f ||
        r[14]!=0x2004d533 || !owner(cpu)) return false;
    static const uint16_t prologue[]={0xb5f3,0xb08f,0x1c05,0x4649,0x6809,0x6849,0x484a,0x1840};
    static const uint16_t clear[]={0xf01f,0xfaf1,0x2100,0x7171,0x2800,0xd01f};
    static const uint32_t wrapper[]={0xe92d4278,0xe5999000,0xe5999020,0xe59f60e8,0xe1a05000,
        0xe0866009,0xe2864d40,0xe5942048,0xe1a0e00f,0xe12fff12,0xe5d40016};
    if (!bytes(cpu,0x2004d3aa,prologue,sizeof(prologue)) ||
        !bytes(cpu,0x2004d52e,clear,sizeof(clear)) ||
        !bytes(cpu,0x201de2dc,wrapper,sizeof(wrapper))) return false;
    if ((r[13]&3) || r[13]<0x10000 || !cpuInspectRamRange(cpu,r[13],112) ||
        word(cpu,r[13]+84)!=0x201de304 || word(cpu,r[13]+108)!=0x20037d55 ||
        word(cpu,r[13]+60)!=r[5]) return false;
    const uint32_t root=word(cpu,r[9]);
    if (root!=0x1ff00ffc || word(cpu,r[9]-8)!=nativeTable ||
        word(cpu,r[9]-4)!=0x1ff04e5c) return false;
    const uint32_t globals=word(cpu,root+4);
    if (!cpuInspectRamRange(cpu,globals,0x600)) return false;
    const uint32_t events=word(cpu,globals+0x4a4+0x3c);
    if (events!=r[4] || events+0x20!=r[6] || !cpuInspectRamRange(cpu,events,0x80)) return false;
    uint8_t flags[8];
    if (!cpuInspectRead(cpu,events+0x20,flags,sizeof(flags)) || flags[5]!=0 || flags[3]!=0) return false;
    // Original wait event group is task 3's saved blocking object, not another task's wait.
    if (word(cpu,r[13]+56)!=globals+0x4a4 || word(cpu,task+0x30)!=0x1ff042b8) return false;
    return true;
}
struct CpuCheckpoint {
    ArmCpu* cpu;
    Savestate<ChunkType> state;
    SavestateLoader<ChunkType> loader;
    void Save(Savestate<ChunkType>& s) { cpuPrepareInjectedCall(cpu,s); }
    void Save(SavestateProbe<ChunkType>& s) { cpuPrepareInjectedCall(cpu,s); }
    void Load(SavestateLoader<ChunkType>& l) { cpuFinishInjectedCall(cpu,l); }
    explicit CpuCheckpoint(ArmCpu* c):cpu(c) { state.Save(*this); }
    void Restore() { loader.Load(state.GetBuffer(),state.GetSize(),*this); }
};
void require(bool ok,const char* reason) { if (!ok) throw std::runtime_error(reason); }
// Logs each patched trap once per run.
void notePatched(uint16_t trap,uint32_t hook) {
    static std::set<uint16_t> noted;
    if (noted.insert(trap).second)
        fprintf(stderr,"native: trap %#06x is patched (hook %#010x), calling its ROM routine\n",trap,hook);
}

// The kernel's idle loop calls its doze routine (ARM, SVC mode, IRQs off) with the scheduler's idle
// flag set, which is where interrupt routines wake tasks. The wake calls EvtWakeup's ROM glue there
// (its Boot table entry without patches) instead of that one doze, as an interrupt routine would; the event
// manager then hands the UI task a nilEvent. Per firmware: the call site with the six words before
// it and the one after, the DAL and Boot tables r9 points at, and the EvtWakeup glue and code.
struct IdleWake {
    DeviceType5 device;
    uint32_t site, siteCode[8], dal, boot, stub, stubCode[4];
    uint32_t wake; uint16_t wakeCode[14]; uint32_t wakeNil; uint16_t wakeNilCode[14];
    uint32_t eventGlobals, eventOffset;
};
const IdleWake idleWakes[]={
    {DeviceType5::deviceTypeTX,0x200a5adc,
     {0xe5980014,0xe2800d80,0xe2800f74,0xe5d01000,0xe3811001,0xe5c01000,0xeb00199c,0xe5980014},
     0x1ff04e5c,0x1ff05e3c,0x20068cdc,{0xe59fc004,0xe08cc00f,0xe12fff1c,0xfffe5129},
     0x2004de10,{0xb580,0x4649,0x6809,0x6849,0x4864,0x1840,0x6bc1,0x2001,0x7008,0xf01e,0xfe87,0xbc80,0xbc08,0x4718},
     0x2004ddf4,{0xb580,0x4649,0x6809,0x6849,0x486b,0x1840,0x6f00,0x2101,0xf01e,0xfcb2,0xbc80,0xbc08,0x2000,0x4718},
     0x2004dfac,0x4a4},
    {DeviceType5::deviceTypeZire72,0x2009235c,
     {0xe594b010,0xe5950000,0xe2801a02,0xe5d101d0,0xe3800001,0xe5c101d0,0xebfffe8f,0xe5950000},
     0x1ff049c0,0x1ff05408,0x200601b8,{0xe59fc004,0xe08cc00f,0xe12fff1c,0xfffed135},
     0x2004d2f8,{0xb580,0x4649,0x6809,0x6849,0x483f,0x1840,0x6bc1,0x2001,0x7008,0xf015,0xef46,0xbc80,0xbc08,0x4718},
     0x2004d2dc,{0xb580,0x4649,0x6809,0x6849,0x4846,0x1840,0x6f00,0x2101,0xf011,0xee54,0xbc80,0xbc08,0x2000,0x4718},
     0x2004d400,0x420},
};
const IdleWake* idleWake(SoC& soc) {
    auto* cpu=soc.GetCpu();
    for (const auto& p:idleWakes) {
        if (soc.GetDeviceType()!=p.device) continue;
        // From six words before the call, which set the idle flag, to the word after it.
        if (!bytes(cpu,p.site-24,p.siteCode,sizeof(p.siteCode)) || !bytes(cpu,p.stub,p.stubCode,sizeof(p.stubCode)) ||
            !bytes(cpu,p.wake,p.wakeCode,sizeof(p.wakeCode)) || !bytes(cpu,p.wakeNil,p.wakeNilCode,sizeof(p.wakeNilCode)) ||
            word(cpu,p.eventGlobals)!=p.eventOffset) return nullptr;
        return &p;
    }
    return nullptr;
}
}

bool WakeIdle(SoC& soc) {
    try {
        if (syscallDispatchInProgress(soc.GetSyscallDispatch())) return false;
        const IdleWake* p=idleWake(soc);
        if (!p || !soc.RunToNativePc(p->site,0,budget,budget)) return false;
        auto* cpu=soc.GetCpu();
        auto* r=cpuGetRegisters(cpu);
        // ARM, SVC mode, IRQs off, at the call, with the system r9. The wake jumps to the EvtWakeup
        // glue checked above, so a system patch on its table entry is skipped like one on a host call.
        if (r[15]!=p->site || cpuIsThumb(cpu) || cpuIsModePace(cpu) ||
            (cpuGetRegExternal(cpu,ARM_REG_NUM_CPSR)&0xbf)!=0x93 ||
            word(cpu,r[9]-4)!=p->dal || word(cpu,r[9]-8)!=p->boot)
            return false;
        const uint32_t hook=word(cpu,p->boot+0x2e8);
        if (hook!=p->stub) notePatched(0x82e8,hook);
        cpuSetReg(cpu,14,p->site+4);
        cpuSetReg(cpu,15,p->stub);
        if (getenv("FROND_TRACE_WAKE")) fprintf(stderr,"native: host wake\n");
        return true;
    } catch (const std::runtime_error&) { return false; }
}

bool Prepare(SoC& soc, bool wake) {
    if (soc.GetDeviceType()!=DeviceType5::deviceTypeTX || syscallDispatchInProgress(soc.GetSyscallDispatch())) return false;
    try {
        // Reject different firmware before running. No guest API is used here.
        static const uint16_t signature[]={0x2100,0x7171,0x2800,0xd01f};
        if (!bytes(soc.GetCpu(),0x2004d532,signature,sizeof(signature))) return false;
        if (validate(soc)) return true;
        if (soc.RunToNativePc(waitPc|1,0,budget,budget)) return validate(soc);
        // A 68k app waiting with no timeout leaves the UI task blocked for good.
        return wake && WakeIdle(soc) && soc.RunToNativePc(waitPc|1,0,budget,budget) && validate(soc);
    } catch (const std::runtime_error&) { return false; }
}

std::string InspectUiApp(SoC& soc) {
    const std::string unavailable="{\"available\":false,\"reason\":\"unqualified-ui-root\"}";
    try {
        if (!validate(soc)) return unavailable;
        auto* cpu=soc.GetCpu();
        // Pinned current-task TLS lookup and app/root split, not a PACE context.
        static const uint16_t slot[]={0xb5b0,0x1c04,0xf000,0xf89b,0x1c05,0xd00a};
        static const uint16_t appfn[]={0xb5b0,0x1c04,0x1c0d,0xf007,0xf840,0x6bc1,0x2900,0xd104};
        static const uint16_t dbfn[]={0xb470,0x9c03,0x6885,0x2900,0xd001,0x68ae,0x600e};
        if (!bytes(cpu,0x20056046,slot,sizeof(slot)) ||
            !bytes(cpu,0x2004f008,appfn,sizeof(appfn)) ||
            !bytes(cpu,0x2002da40,dbfn,sizeof(dbfn))) return unavailable;
        static const uint8_t tlsLookup[]={0xb0,0xb5,0x04,0x1c,0x00,0xf0,0x9b,0xf8,0x05,0x1c,0x0a,0xd0,0x63,0x1c,0x03,0xd1,0x68,0x68,0xb0,0xbc,0x08,0xbc,0x18,0x47,0x20,0x1c,0x00,0xf0,0x9e,0xf8,0x00,0x28,0x01,0xd1,0x00,0x20,0xf5,0xe7,0x29,0x68,0x00,0x20,0x0a,0x68,0xa2,0x42,0xf0,0xd9,0xa0,0x00,0x40,0x18,0x40,0x68,0xec,0xe7,0x80,0xb5,0x20,0x28,0x03,0xd3,0x00,0x20,0x80,0xbc,0x08,0xbc,0x18,0x47,0xff,0xf7,0xdb,0xff,0xf9,0xe7,0x80,0xb5,0x00,0x20,0xc0,0x43,0xff,0xf7,0xd5,0xff,0x80,0xbc,0x08,0xbc,0x18,0x47};
        if (!bytes(cpu,0x20056046,tlsLookup,sizeof(tlsLookup))) return unavailable;
        static const uint8_t tlsInfo[]={0x80,0xb5,0x8a,0xb0,0x09,0xa8,0x15,0xf0,0x63,0xfe,0x69,0x46,0x09,0x98,0x16,0xf0,0x1b,0xfb,0x00,0x98,0x0a,0xb0,0x80,0xbc,0x08,0xbc,0x18,0x47};
        if (!bytes(cpu,0x20056184,tlsInfo,sizeof(tlsInfo))) return unavailable;
        static const uint8_t kalInfo[]={0x10,0x40,0x2d,0xe9,0x20,0xd0,0x4d,0xe2,0x01,0x40,0xa0,0xe1,0x00,0x08,0xa0,0xe1,0x40,0x08,0xa0,0xe1,0x0d,0x10,0xa0,0xe1,0x0d,0x00,0x00,0xef,0x00,0x00,0x50,0xe3,0x00,0x00,0x60,0x12,0x00,0x08,0xa0,0x11,0x20,0x08,0xa0,0x11,0xf0,0x0c,0x80,0x13,0x17,0x00,0x00,0x1a,0x00,0x00,0x9d,0xe5,0x00,0x10,0x90,0xe5,0x00,0x10,0x84,0xe5,0x04,0x00,0x90,0xe5,0x14,0x00,0x84,0xe5,0x08,0x00,0x9d,0xe5,0x04,0x00,0x84,0xe5,0x10,0x00,0x9d,0xe5,0x08,0x00,0x84,0xe5,0x0c,0x00,0x9d,0xe5,0x0c,0x00,0x84,0xe5,0x15,0x00,0xdd,0xe5,0x10,0x00,0x84,0xe5,0x1a,0x00,0xdd,0xe5,0x1e,0x00,0xc4,0xe5,0xf6,0x01,0xdd,0xe1,0x18,0x00,0x84,0xe5,0xb8,0x01,0xdd,0xe1,0xbc,0x01,0xc4,0xe1,0x1b,0x00,0xdd,0xe5,0x1f,0x00,0xc4,0xe5,0x1c,0x00,0xdd,0xe5,0x20,0x00,0xc4,0xe5,0x00,0x00,0xa0,0xe3,0x20,0xd0,0x8d,0xe2,0x10,0x40,0xbd,0xe8};
        if (!bytes(cpu,0x200a72b0,kalInfo,sizeof(kalInfo))) return unavailable;
        auto ptr=[&](uint32_t a) {
            uint32_t v=word(cpu,a);
            require(v && !(v&3) && cpuInspectRamRange(cpu,v,64),"invalid app link");
            return v;
        };
        uint32_t tcb=ptr(task), tls=ptr(tcb), app=ptr(tls+4);
        // Only a normal root launch at the already-qualified UI wait is supported.
        if ((word(cpu,app)&0xffff)!=0 || word(cpu,app+60)!=0 ||
            (word(cpu,app+8)&0xffff)!=0x000e) return unavailable;
        uint32_t ref=ptr(app+20), db=ptr(ref+8), id=word(cpu,db+8), header=word(cpu,db+12);
        if (!id || (id&3)) return unavailable;
        if (id&0x80000000u) {
            // Support only an already-resident header, with independent cache agreement.
            if (!cpuInspectRamRange(cpu,id&0x7fffffffu,4) || word(cpu,id&0x7fffffffu)!=header || !cpuInspectRamRange(cpu,header,80)) return unavailable;
            if ((header&3) || header<8 || !cpuInspectRamRange(cpu,header-8,8)) return unavailable;
            uint32_t chunk=word(cpu,header-8), flags=word(cpu,header-4);
            if (chunk&0x80000000u) return unavailable;
            uint32_t signed24=flags&0xffffff;
            if (signed24&0x800000) signed24|=0xff000000;
            uint32_t back=((chunk>>28)&3) | (signed24<<2);
            if (back ? header-8-back*4!=(id&0x7fffffffu) : (flags>>28)!=15) return unavailable;
            uint32_t total=chunk&0xffffff, padding=(chunk>>24)&15;
            if (total<8+padding+80) return unavailable;
        } else if (id!=header) return unavailable;
        uint8_t h[80];
        if (!cpuInspectRead(cpu,header,h,sizeof(h)) || !memchr(h,0,32)) return unavailable;
        auto u16=[&](unsigned o) {return uint32_t(h[o]) | (uint32_t(h[o+1])<<8);};
        auto u32=[&](unsigned o) {return u16(o) | (u16(o+2)<<16);};
        if (!(u16(32)&1) || u32(60)!=0x6170706c) return unavailable;
        auto hex=[](const uint8_t* b,unsigned n) {std::string s; const char* d="0123456789abcdef";
            for(unsigned i=0;i<n;++i){s+=d[b[i]>>4];s+=d[b[i]&15];}return s;};
        auto fourcc=[&](unsigned o) {uint8_t b[]={h[o+3],h[o+2],h[o+1],h[o]};return hex(b,4);};
        unsigned length=static_cast<const uint8_t*>(memchr(h,0,32))-h;
        return "{\"available\":true,\"verified\":true,\"source\":\"tx-qualified-ui-root-wait\",\"ui_task_id\":3,\"database\":{\"card\":0,\"local_id\":"+
            std::to_string(id)+",\"name_hex\":\""+hex(h,length)+"\",\"type_hex\":\""+fourcc(60)+
            "\",\"creator_hex\":\""+fourcc(64)+"\",\"version\":"+std::to_string(u16(34))+"}}";
    } catch (const std::runtime_error&) { return unavailable; }
}

struct Session::Impl {
    SoC& soc;
    ArmCpu* cpu;
    uint32_t allocation{0}, depth{0};
    bool closed{false}, failed{false};
    uint32_t calls{0}, callbacks{0};
    const std::chrono::steady_clock::time_point started=std::chrono::steady_clock::now();
    uint64_t startedGuest;
    void budgetCheck() {
        require(std::chrono::steady_clock::now()-started<std::chrono::seconds(30) &&
                soc.GetInjectedTimeNsec()-startedGuest<20000000000ull,"native session deadline exceeded");
    }
    std::vector<uint8_t> bootstrap;
    struct Callback { Impl* self; std::function<void(ArmCpu*)> fn; };
    std::map<uint32_t,Callback> handlers;
    std::exception_ptr callbackError;
    explicit Impl(SoC& soc):soc(soc),cpu(soc.GetCpu()),startedGuest(soc.GetInjectedTimeNsec()),bootstrap(128*1024,0xa5) {}
    bool copy(uint32_t a,void* p,uint32_t n,bool write) {
        if (n>16*1024*1024) return false;
        return cpuNativeBuffer(cpu,a,p,n,write);
    }
    static void callback(ArmCpu* cpu,void* opaque) {
        auto* c=static_cast<Callback*>(opaque);
        Impl* self=c->self;
        try {
            self->budgetCheck();
            require(++self->callbacks<=65536 && owner(cpu),"native callback/task guard failed");
            auto fn=c->fn; // self-unregistration is safe, even when fn throws
            fn(cpu);
        } catch (...) {
            self->callbackError=std::current_exception();
            self->failed=true;
            // Stop at this instruction so RunUntil can account completed guest
            // cycles before propagating the host exception to whole-guest recovery.
            cpuSetSlowPath(cpu,SLOW_PATH_REASON_NATIVE_FAULT);
        }
    }

    uint32_t invoke(uint16_t trap,std::array<uint32_t,4> args,const std::vector<uint32_t>& stackArgs,bool boot) {
        budgetCheck();
        require(!closed && !failed && syscallDispatchRecoveryActive(soc.GetSyscallDispatch()),"native recovery scope is not active");
        require(depth<4 && ++calls<=65536 && stackArgs.size()<=16,"native call bound exceeded");
        struct Entry { uint16_t trap; uint32_t address; uint32_t signature[4]; };
        static const Entry entries[]={
            {0x812c,0x200685ec,{0xe59fc004,0xe08cc00f,0xe12fff1c,0xfffc35d1}},
            {0x8148,0x2006865c,{0xe59fc004,0xe08cc00f,0xe12fff1c,0xfffc2801}},
            {0x815c,0x200686ac,{0xe59fc004,0xe08cc00f,0xe12fff1c,0xfffc1cad}},
            {0x81d4,0x2006888c,{0xe59fc004,0xe08cc00f,0xe12fff1c,0xfffc3473}},
            {0x8188,0x2006875c,{0xe59fc004,0xe08cc00f,0xe12fff1c,0xfffc3547}},
            {0x8304,0x204ae7a8,{0xe92d4ff0,0xe5999000,0xe24dd034,0xe599932c}},
            {0x82fc,0x20068d2c,{0xe59fc004,0xe08cc00f,0xe12fff1c,0xfffcf751}},
            {0x88f8,0x2006a51c,{0xe59fc004,0xe08cc00f,0xe12fff1c,0xfffe5c1d}},
            {0x8584,0x2006974c,{0xe59fc004,0xe08cc00f,0xe12fff1c,0xfffd8d73}},
            {0x84b8,0x2006941c,{0xe59fc004,0xe08cc00f,0xe12fff1c,0xfffd8afb}},
            // After an install: SysCurAppDatabase, DmGetNextDatabaseByTypeCreator, SysAppLaunch,
            // EvtSysEventAvail, and SysEventAvail, which PACE's handler for the 68k EvtEventAvail
            // (trap 0xa2cc) calls.
            {0x8800,0x2006a13c,{0xe59fc004,0xe08cc00f,0xe12fff1c,0xfffe6295}},
            {0x8198,0x2014bf7c,{0xe92d421e,0xe5999000,0xe5999338,0xe59f408c}},
            {0x87c8,0x20327a3c,{0xe92d4ff0,0xe5999000,0xe24dd02c,0xe5999224}},
            {0x82e0,0x20068cbc,{0xe59fc004,0xe08cc00f,0xe12fff1c,0xfffe48bb}},
            {0x881c,0x2006a1ac,{0xe59fc004,0xe08cc00f,0xe12fff1c,0xfffcdcf7}}
        };
        const Entry* expected=nullptr;
        for (const auto& candidate:entries) if (candidate.trap==trap) expected=&candidate;
        require(expected,"native trap is outside qualified database API set");
        require(owner(cpu),"native task changed before call");
        auto* r=cpuGetRegisters(cpu);
        require(word(cpu,r[9]-8)==nativeTable,"native syscall table changed");
        // A system patch (UDMH, etc.) points the table entry at its own code. The call then goes
        // straight to the ROM routine, which is still checked by its signature; the hook never runs.
        const uint32_t patched=word(cpu,nativeTable+(trap&0xfff)), entry=expected->address;
        if (!bytes(cpu,entry,expected->signature,sizeof(expected->signature))) {
            if (patched==entry) throw std::runtime_error("native syscall firmware signature mismatch");
            char reason[160];
            snprintf(reason,sizeof(reason),"native trap %#06x has a system patch (hook %#010x) "
                     "and its ROM routine doesn't match",trap,patched);
            throw std::runtime_error(reason);
        }
        if (patched!=entry) notePatched(trap,patched);
        CpuCheckpoint saved(cpu);
        uint8_t taskContext[0x34]; uint32_t userSp=word(cpu,task+0x24);
        require(copy(task+0x40,taskContext,sizeof(taskContext),false),"cannot snapshot task context");
        Defer restore([&] {
            cpuNativeBuffer(cpu,task+0x24,&userSp,4,true);
            cpuNativeBuffer(cpu,task+0x40,taskContext,sizeof(taskContext),true);
            saved.Restore();
        });
        uint32_t stack = boot ? bootstrapBase+bootstrap.size()-64 :
            (depth ? r[13] : allocation+allocationSize-64);
        stack = (stack - uint32_t(stackArgs.size()*4) - 8) & ~7u;
        require(boot || stack>allocation+dataSize+256,"native scratch stack exhausted");
        if (boot) {
            for (size_t i=0;i<stackArgs.size();++i)
                memcpy(bootstrap.data()+stack-bootstrapBase+i*4,&stackArgs[i],4);
        } else require(copy(stack,const_cast<uint32_t*>(stackArgs.data()),stackArgs.size()*4,true),"native stack arguments unmapped");
        std::copy(args.begin(),args.end(),r);
        r[13]=stack;
        const bool suspendTimers = depth == 0;
        if (suspendTimers) soc.SuspendTimerInterrupts(true);
        Defer resumeTimers([&] { if (suspendTimers) soc.SuspendTimerInterrupts(false); });
        ++depth;
        Defer leave([&] { --depth; });
        cpuSetNativeStackBounds(cpu,boot ? bootstrapBase+256 : allocation+dataSize+4,
                                boot ? bootstrapBase+bootstrap.size()-4 : allocation+allocationSize-4,0x1ff042bc,task);
        require(cpuStartNativeCall(cpu,entry),"native call nesting rejected");
        Defer finish([&] { cpuEndNativeCall(cpu); });
        // Installs and exports of large databases need more time; record databases
        // near 500 KB take over 100M cycles to export. Deadline checks run between slices.
        const unsigned callSlices = !boot && (trap == 0x82fc || trap == 0x8304) ? 10 : 1;
        bool completed=false;
        if (boot) {
            uint64_t cycles=0;
            Defer account([&] { soc.AccountInjectedCycles(cycles,budget); });
            while (cycles<budget) {
                cycles+=cpuCyclePure(cpu);
                const auto reason=cpuGetSlowPathReason(cpu);
                if (reason&SLOW_PATH_REASON_NATIVE_FAULT) break;
                if (reason&SLOW_PATH_REASON_INJECTED_CALL_DONE) { completed=true; break; }
            }
        } else for (unsigned slice = 0; slice < callSlices; ++slice) {
            completed = soc.ExecuteInjected(budget,budget);
            if (completed || (cpuGetSlowPathReason(cpu)&SLOW_PATH_REASON_NATIVE_FAULT)) break;
            if (slice + 1 < callSlices) budgetCheck();
        }
        if (callbackError) std::rethrow_exception(callbackError);
        require(!(cpuGetSlowPathReason(cpu)&SLOW_PATH_REASON_NATIVE_FAULT),"native call fault");
        require(completed,"native call cycle deadline");
        require(owner(cpu) && r[13]==stack,"native call changed task or stack");
        if (allocation) {
            const uint32_t marker=0xa55a3cc3;
            require(word(cpu,allocation+dataSize)==marker &&
                    word(cpu,allocation+allocationSize-4)==marker,"native scratch stack guard damaged");
        }
        return r[0];
    }
};

Session::Session(SoC& soc) {
    require(syscallDispatchRecoveryActive(soc.GetSyscallDispatch()) &&
            !syscallDispatchInProgress(soc.GetSyscallDispatch()),"native session requires top-level recovery checkpoint");
    require(validate(soc),"unqualified native boundary");
    auto candidate=std::make_unique<Impl>(soc);
    require(cpuAcquireNativeSession(candidate->cpu),"native session already active");
    Defer failure([&] { if (candidate) cpuClearNativeRuntime(candidate->cpu); });
    require(cpuSetNativeScratch(candidate->cpu,bootstrapBase,candidate->bootstrap.data(),candidate->bootstrap.size()),"bootstrap address is mapped");
    candidate->allocation=candidate->invoke(0x8584,{allocationSize,0,0,0},{},true);
    require(candidate->allocation && !(candidate->allocation&3) &&
            cpuNativeRamRange(candidate->cpu,candidate->allocation,allocationSize),("native scratch allocation is not RAM: "+std::to_string(candidate->allocation)).c_str());
    // The original UI call frame is never writable scratch.
    const uint32_t oldSp=cpuGetRegisters(candidate->cpu)[13];
    require(uint64_t(candidate->allocation)+allocationSize<=oldSp || candidate->allocation>oldSp+112,"scratch overlaps UI frame");
    require(cpuSetNativeScratch(candidate->cpu,0,nullptr,0),"cannot release bootstrap mapping");
    const uint32_t marker=0xa55a3cc3;
    require(candidate->copy(candidate->allocation+dataSize,const_cast<uint32_t*>(&marker),4,true) &&
            candidate->copy(candidate->allocation+allocationSize-4,const_cast<uint32_t*>(&marker),4,true),"cannot initialize stack guards");
    impl=std::move(candidate);
}
Session::~Session() { if (impl) cpuClearNativeRuntime(impl->cpu); }
uint32_t Session::Call(uint16_t trap,std::array<uint32_t,4> args,std::vector<uint32_t> stackArgs) {
    try { return impl->invoke(trap,args,stackArgs,false); }
    catch (...) { impl->failed=true; throw; }
}
bool Session::Read(uint32_t address,void* dest,uint32_t size) {
    return !impl->closed && !impl->failed && impl->copy(address,dest,size,false);
}
bool Session::Write(uint32_t address,const void* src,uint32_t size) {
    return !impl->closed && !impl->failed && impl->copy(address,const_cast<void*>(src),size,true);
}
void Session::RegisterCallback(uint32_t address,std::function<void(ArmCpu*)> fn) {
    require(!impl->closed && !impl->failed && fn && address>=DataAddress() &&
            uint64_t(address)+4<=uint64_t(DataAddress())+DataSize() && !(address&3),"invalid native callback address");
    auto result=impl->handlers.emplace(address,Impl::Callback{impl.get(),std::move(fn)});
    require(result.second,"duplicate native callback");
    if (!cpuRegisterNativeCallback(impl->cpu,address,Impl::callback,&result.first->second)) {
        impl->handlers.erase(result.first); throw std::runtime_error("native callback slots exhausted");
    }
}
void Session::UnregisterCallback(uint32_t address) {
    cpuUnregisterNativeCallback(impl->cpu,address); impl->handlers.erase(address);
}
uint32_t Session::DataAddress() const { return impl->allocation+64; }
uint32_t Session::DataSize() const { return dataSize-64; }
void Session::Close() {
    require(!impl->depth,"cannot close native session from callback");
    if (impl->closed) return;
    require(!impl->failed,"failed native session requires whole-guest rollback");
    for (const auto& h:impl->handlers) cpuUnregisterNativeCallback(impl->cpu,h.first);
    impl->handlers.clear();
    require(cpuSetNativeScratch(impl->cpu,bootstrapBase,impl->bootstrap.data(),impl->bootstrap.size()),"bootstrap address changed");
    const uint32_t allocation=impl->allocation;
    // Check guards before freeing; never dereference the freed block in invoke.
    const uint32_t marker=0xa55a3cc3;
    require(word(impl->cpu,allocation+dataSize)==marker && word(impl->cpu,allocation+allocationSize-4)==marker,"stack guards damaged before free");
    impl->allocation=0;
    try { require(impl->invoke(0x84b8,{allocation,0,0,0},{},true)==0,"native scratch free failed"); }
    catch (...) { impl->failed=true; throw; }
    impl->closed=true;
    cpuClearNativeRuntime(impl->cpu);
}
}
#endif
