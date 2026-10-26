#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "CPU.h"
#include "ReadOnlyGuest.h"
#include "pace.h"
#include "soc_PXA.h"
#include "syscall_68k.h"

class TimeoutSoc : public SocPXA {
 public:
    using SocPXA::SocPXA;
    int suspended = 0;
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
    { ReadOnlyGuest checkpoint(soc); }
    cpuClearSlowPath(cpu, SLOW_PATH_REASON_PACE_SYSCALL_BREAK);
    assert(!soc.RunToPaceSyscall(SYSCALL_68K_EVT_GET_EVENT, 0, 100000000));
    puts("PASS: export timeout and success restore guest RAM, NAND, CPU and pacing state");
}
