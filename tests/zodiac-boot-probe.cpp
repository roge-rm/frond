#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "CPU.h"
#include "MMU.h"
#include "imx1_boot_probe.h"
#include "patch_dispatch.h"
#include "system_state.h"

static Imx1ProbeResult run(std::initializer_list<uint32_t> words, uint32_t steps = 100, bool fast = false) {
    std::vector<uint8_t> rom(words.size() * 4);
    size_t offset = 0;
    for (uint32_t w : words) {
        for (unsigned byte = 0; byte < 4; ++byte) rom[offset++] = w >> (byte * 8);
    }
    const auto original = rom;
    const auto r = imx1BootProbe(rom.data(), rom.size(), Imx1ProbeEntry::physicalFlash, steps,
                                nullptr, nullptr, fast);
    assert(rom == original);
    return r;
}

static bool memory(void* data, uint32_t address, uint_fast8_t size, bool write, void* value) {
    if (uint64_t(address) + size > 0x10000) return false;
    if (write) std::memcpy(static_cast<uint8_t*>(data) + address, value, size);
    else std::memcpy(value, static_cast<uint8_t*>(data) + address, size);
    return true;
}

int main() {
    // Cache maintenance with disabled caches, ARM920 control reset and first MMIO store.
    auto r = run({0xe3a00000, 0xee070f16, 0xee070f15, 0xee111f10,
                  0xe59f0004, 0xe5801000, 0xeafffffe, 0x00205034});
    assert(r.stop == Imx1ProbeStop::unsupportedWrite && r.trace.pc == 0x10000014);
    assert(r.address == 0x00205034 && r.size == 4 && r.value == 0x78);
    r = run({0xe59f0000, 0xe5901000, 0x0021b000});
    assert(r.stop == Imx1ProbeStop::unsupportedRead && r.address == 0x0021b000 && r.size == 4);
    r = run({0xee110f10, 0xe3800004, 0xee010f10});
    assert(r.stop == Imx1ProbeStop::unsupportedCp15 && r.trace.operand == 0x7c);
    for (uint32_t instruction : {0xee100f10u, 0xee170f16u, 0xee070f36u, 0xee080f37u}) {
        r = run({instruction});  // IDs, read of write-only cache register, unsupported cache/TLB form.
        assert(r.stop == Imx1ProbeStop::unsupportedCp15);
    }
    for (uint32_t instruction : {0xe16f1f10u, 0xe12fff30u, 0xfa000000u, 0xe5d0f000u}) {
        r = run({instruction});  // CLZ, BLX register/immediate and unqualified byte load to PC.
        assert(r.stop == Imx1ProbeStop::unsupportedInstruction && r.trace.pc == 0x10000000);
    }
    r = run({0x016f1f10, 0xeafffffe}, 3);  // Failed condition skips even an unsupported instruction.
    assert(r.stop == Imx1ProbeStop::budget && r.trace.step == 2);
    r = run({0xe3a00000, 0xee080f17, 0xeafffffe}, 3);
    assert(r.stop == Imx1ProbeStop::budget);
    r = run({0xe3a00000, 0xe5800000});
    assert(r.stop == Imx1ProbeStop::unsupportedWrite && r.address == 0);
    r = run({0xeafffffe}, 5);
    assert(r.stop == Imx1ProbeStop::budget && r.trace.step == 4);
    for (bool fast : {false, true}) {
        if (fast && !cpuSetFastEngine(true)) continue;
        r = run({0xe3a000d7, 0xe121f000, 0xe3a0da01, 0xe3a000d3, 0xe121f000, 0xee100f10}, 20, fast);
        assert(r.stop == Imx1ProbeStop::unsupportedCp15 && r.trace.step == 5);
        assert((r.cpsr & ARM_SR_M) == ARM_SR_MODE_UND && r.spsr == 0xd3);
    }
    cpuSetFastEngine(false);

    // The reusable non-XScale MMU walks sections and reports ARMv4 permission/domain faults.
    std::vector<uint8_t> bytes(0x10000);
    const uint32_t section = 0x10000402;  // AP=1, domain 0, 1 MiB section.
    std::memcpy(bytes.data() + 0x4000, &section, 4);
    auto* mem = memInit();
    assert(memRegionAdd(mem, 0, bytes.size(), memory, bytes.data()));
    auto* mmu = mmuInit(mem, false);
    assert(MMU_TRANSLATE_RESULT_PA(mmuTranslate(mmu, 0x1234, false, true)) == 0x1234);
    mmuSetTTP(mmu, 0x4000);
    mmuSetDomainCfg(mmu, 1);
    auto t = mmuTranslate(mmu, 0x1234, true, true);
    assert(MMU_TRANSLATE_RESULT_OK(t) && MMU_TRANSLATE_RESULT_PA(t) == 0x10001234);
    t = mmuTranslate(mmu, 0x1234, false, true);
    assert(!MMU_TRANSLATE_RESULT_OK(t) && MMU_TRANSLATE_RESULT_FSR(t) == 0x0d);
    mmuSetDomainCfg(mmu, 0);
    t = mmuTranslate(mmu, 0x1234, true, false);
    assert(!MMU_TRANSLATE_RESULT_OK(t) && MMU_TRANSLATE_RESULT_FSR(t) == 9);

    const uint32_t controlProgram[] = {0xe3a0007a, 0xee010f10, 0xee111f10};
    std::memcpy(bytes.data() + 0x1000, controlProgram, sizeof(controlProgram));
    auto* patches = initPatchDispatch();
    auto* pace = new PacePatch{};
    auto* cpu = cpuInit(0x1000, mem, ARM_MEMORY_SYSTEM_MMU, false, false, -1, 0, 0,
                        patches, pace, createSystemState(), ArmCpuModel::arm920t);
    patchDispatchSetCpu(patches, cpu);
    assert(cpuGetModel(cpu) == ArmCpuModel::arm920t);
    for (unsigned i = 0; i < 3; ++i) cpuCyclePure(cpu);
    assert(cpuGetRegExternal(cpu, 1) == 0x7a);
    cpuReset(cpu, 0x1008);
    cpuCyclePure(cpu);
    assert(cpuGetRegExternal(cpu, 1) == 0x78);
    std::puts("PASS: ARM920 bootstrap CP15, instruction limits, read/write stops and non-XScale section faults");
}
