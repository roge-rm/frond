#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "CPU.h"
#include "ReadOnlyGuest.h"
#include "GuestExport.h"
#include "pace.h"
#include "soc_PXA.h"
#include "syscall_68k.h"

class TimeoutSoc : public SocPXA {
 public:
    using SocPXA::SocPXA;
    int suspended = 0;
    bool scriptedPreflight = false, preflightReady = false;
    unsigned preflightCalls = 0;
    bool PrepareM68kSupport() override {
        return scriptedPreflight || SocPXA::PrepareM68kSupport();
    }
    bool RunToPaceSyscall(uint16_t call, uint64_t cycles, uint64_t ips) override {
        if (!scriptedPreflight) return SocPXA::RunToPaceSyscall(call, cycles, ips);
        ++preflightCalls;
        // Ordinary startup work may advance devices, mutate RAM/NAND and change
        // CPU state before a suitable Palm API boundary is reached.
        SocPXA::Run(1000, 100000000);
        static_cast<uint8_t*>(GetMemoryData().data)[9000] = preflightCalls;
        static_cast<uint8_t*>(GetNandData().data)[123] = preflightCalls;
        SetNandDirty(true);
        cpuSetReg(GetCpu(), 0, preflightCalls);
        return preflightReady;
    }
    void SuspendTimerInterrupts(bool suspend) override {
        suspended += suspend ? 1 : -1;
        SocPXA::SuspendTimerInterrupts(suspend);
    }
    bool ExecuteInjected(uint64_t, uint64_t) override {
        // A timed-out call may already have modified every part of the guest.
        std::memset(GetMemoryData().data, 0x55, GetMemoryData().size);
        static_cast<uint8_t*>(GetNandData().data)[0] = 0;
        SetNandDirty(true);
        cpuSetReg(GetCpu(), 0, 0xdeadbeef);
        return false;
    }
};

int main() {
    std::vector<uint8_t> rom(16 * 1024 * 1024), nand(132 * 1024 * 1024, 0xff);
    TimeoutSoc soc(DeviceType5::deviceTypeTX, 32 * 1024 * 1024, rom.data(), rom.size(),
                   nand.data(), nand.size(), -1, 2);
    auto cpu = soc.GetCpu();
    cpuSetReg(cpu, 0, 123);
    cpuSetReg(cpu, 15, 0xa0003000);
    auto* ram = static_cast<uint8_t*>(soc.GetMemoryData().data);
    const uint32_t branch = 0xeafffffe;
    std::memcpy(ram + 0x3000, &branch, 4);
    paceSetPriviledged(true);
    paceSetStatePtr(0xa0001000);
    assert(paceLoad68kState());
    paceSetAreg(7, 0xa0002000);
    assert(paceSave68kState());
    const std::vector<uint8_t> original(ram, ram + soc.GetMemoryData().size);
    const auto time = soc.GetTime();
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        bool caught = false;
        try {
            ReadOnlyGuest checkpoint(soc);
            syscall68k_MemPtrNew(soc.GetSyscallDispatch(), SC_EXECUTE_FULL, 4);
        } catch (const InjectedCallError&) { caught = true; }
        assert(caught && soc.suspended == 0);
        assert(!syscallDispatchInProgress(soc.GetSyscallDispatch()));
        assert(!std::memcmp(original.data(), ram, original.size()));
        assert(static_cast<uint8_t*>(soc.GetNandData().data)[0] == 0xff);
        assert(!soc.IsNandDirty());
        assert(cpuGetRegExternal(cpu, 0) == 123);
        assert(soc.GetTime() == time);
    }
    // A caught timeout still leaves the transaction uncommittable; destruction
    // must roll it back and allow a fresh recovery scope afterwards.
    {
        ReadOnlyGuest checkpoint(soc);
        try { syscall68k_MemPtrNew(soc.GetSyscallDispatch(), SC_EXECUTE_FULL, 4); }
        catch (const InjectedCallError&) {}
        bool rejected = false;
        try { checkpoint.Commit(); } catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }
    assert(!std::memcmp(original.data(), ram, original.size()));
    assert(!syscallDispatchInProgress(soc.GetSyscallDispatch()));
    // Successful read-only operations restore their side effects too.
    {
        ReadOnlyGuest checkpoint(soc);
        ram[123] ^= 1;
        cpuSetReg(cpu, 0, 456);
        soc.Run(1000, 100000000);
    }
    assert(!std::memcmp(original.data(), ram, original.size()));
    assert(cpuGetRegExternal(cpu, 0) == 123 && soc.GetTime() == time);
    assert(soc.Run(1000, 100000000) >= 1000);
    cpuSetSlowPath(cpu, SLOW_PATH_REASON_PACE_SYSCALL_BREAK);
    assert(soc.RunToPaceSyscall(SYSCALL_68K_EVT_GET_EVENT, 0, 100000000));
    {
        ReadOnlyGuest checkpoint(soc);
        cpuClearSlowPath(cpu, SLOW_PATH_REASON_PACE_SYSCALL_BREAK);
        // Construction preserves the cached stopped boundary even without a
        // pending CPU break flag; no further guest execution is needed.
        assert(soc.RunToPaceSyscall(SYSCALL_68K_EVT_GET_EVENT, 0, 100000000));
    }
    cpuClearSlowPath(cpu, SLOW_PATH_REASON_PACE_SYSCALL_BREAK);
    assert(!soc.RunToPaceSyscall(SYSCALL_68K_EVT_GET_EVENT, 0, 100000000));
    // Normal execution consumes the boundary between socket requests.
    cpuSetSlowPath(cpu, SLOW_PATH_REASON_PACE_SYSCALL_BREAK);
    assert(soc.RunToPaceSyscall(SYSCALL_68K_EVT_GET_EVENT, 0, 100000000));
    cpuClearSlowPath(cpu, SLOW_PATH_REASON_PACE_SYSCALL_BREAK);
    soc.Run(1000, 100000000);
    assert(!soc.RunToPaceSyscall(SYSCALL_68K_EVT_GET_EVENT, 0, 100000000));

    soc.scriptedPreflight = true;
    // Failed preparation is not a speculative export. Repeated not-ready
    // attempts must retain progress rather than restoring the same busy state.
    for (unsigned attempt = 1; attempt <= 3; ++attempt) {
        const auto before = soc.GetTime();
        { GuestExport exportGuest(soc); assert(!exportGuest.Ready()); }
        assert(soc.preflightCalls == attempt && soc.GetTime() > before);
        assert(ram[9000] == attempt && cpuGetRegExternal(cpu, 0) == attempt);
        assert(static_cast<uint8_t*>(soc.GetNandData().data)[123] == attempt);
        assert(soc.IsNandDirty());
    }

    soc.preflightReady = true;
    uint64_t preparedTime = 0;
    std::vector<uint8_t> preparedRam;
    bool failed = false;
    try {
        GuestExport exportGuest(soc);
        assert(exportGuest.Ready() && soc.preflightCalls == 4);
        preparedTime = soc.GetTime();
        preparedRam.assign(ram, ram + soc.GetMemoryData().size);
        syscall68k_MemPtrNew(soc.GetSyscallDispatch(), SC_EXECUTE_FULL, 4);
    } catch (const InjectedCallError&) { failed = true; }
    // Export failure rolls back injected changes, preserving the successful
    // preflight's RAM/NAND/CPU state and its elapsed guest time.
    assert(failed && soc.GetTime() == preparedTime);
    assert(!std::memcmp(preparedRam.data(), ram, preparedRam.size()));
    assert(cpuGetRegExternal(cpu, 0) == 4);
    assert(static_cast<uint8_t*>(soc.GetNandData().data)[0] == 0xff);
    assert(static_cast<uint8_t*>(soc.GetNandData().data)[123] == 4 && soc.IsNandDirty());
    assert(!syscallDispatchInProgress(soc.GetSyscallDispatch()) && soc.suspended == 0);

    // Successful export work has the same rollback boundary.
    {
        GuestExport exportGuest(soc);
        assert(exportGuest.Ready() && soc.preflightCalls == 5);
        preparedTime = soc.GetTime();
        ram[9001] ^= 1;
        cpuSetReg(cpu, 0, 999);
        soc.Run(1000, 100000000);
    }
    assert(soc.GetTime() == preparedTime && cpuGetRegExternal(cpu, 0) == 5);
    assert(ram[9000] == 5 && ram[9001] == preparedRam[9001]);
    puts("PASS: live preflight progress survives not-ready; export timeout/success restore the prepared guest");
}
