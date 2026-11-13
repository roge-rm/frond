#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "CPU.h"
#include "MMU.h"
#include "imx1_boot_probe.h"
#include "imx1_graphics_memory.h"
#include "patch_dispatch.h"
#include "system_state.h"

static Imx1ProbeResult run(std::initializer_list<uint32_t> words, uint32_t steps = 100, bool fast = false,
                             const Imx1ProbeOptions* options = nullptr) {
    std::vector<uint8_t> rom(words.size() * 4);
    size_t offset = 0;
    for (uint32_t w : words) {
        for (unsigned byte = 0; byte < 4; ++byte) rom[offset++] = w >> (byte * 8);
    }
    const auto original = rom;
    const auto r = imx1BootProbe(rom.data(), rom.size(), Imx1ProbeEntry::physicalFlash, steps,
                                nullptr, nullptr, fast, options);
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
    {
        Imx1ProbeOptions o{};
        o.bootstrapPeripherals = true;
        o.gpioAInputKnown = true; o.gpioAInput = 0x80;
        o.gpioDInputKnown = true; o.gpioDInput = 0x600;
        const auto read = [&](const Imx1ProbeOptions& options) {
            return run({0xe59f0010, 0xe5901000, 0xe5902000, 0xe59f0008, 0xe5903000,
                        0xeafffffe, 0x0021c024, 0x0021c324}, 8, false, &options);
        };
        auto r = read(o);
        assert(r.stop == Imx1ProbeStop::budget && !r.gpioEventApplied);
        assert(r.registers[1] == 0x80 && r.registers[2] == 0x80 && r.registers[3] == 0x600);
        o.gpioEventAfterInstructions = 2;
        o.gpioEventValues[0] = o.gpioEventMasks[0] = 0x400;
        o.gpioEventValues[3] = o.gpioEventMasks[3] = 0x1000;
        r = read(o);
        assert(r.stop == Imx1ProbeStop::budget && r.gpioEventApplied);
        assert(r.registers[1] == 0x80 && r.registers[2] == 0x480 && r.registers[3] == 0x1600);
        o.gpioEventAfterInstructions = 1;
        r = read(o);
        assert(r.gpioEventApplied && r.registers[1] == 0x480);
        for (auto count : {0u, 8u, imx1ProbeMaximumSteps}) {
            o.gpioEventAfterInstructions = count;
            r = read(o);
            assert(r.stop == Imx1ProbeStop::unsupportedPeripheral && !r.gpioEventApplied);
        }
        o.gpioEventAfterInstructions = 2;
        o.gpioAInputKnown = false; o.gpioAInputMaskedKnown = true; o.gpioAInputMask = 0xffffdfff;
        o.gpioEventValues[0] = o.gpioEventMasks[0] = 0x2000;
        r = read(o);
        assert(r.stop == Imx1ProbeStop::unsupportedPeripheral && !r.gpioEventApplied);
        o.gpioEventValues[0] = o.gpioEventMasks[0] = 0x400;
        o.bootstrapPeripherals = false;
        assert(read(o).stop == Imx1ProbeStop::unsupportedPeripheral);
    }

    {
        const Imx1GraphicsMemory memory;
        for (unsigned bank=0;bank<3;++bank) {
            const auto dirty=memory.dirty(bank);
            uint32_t bytes=123;
            const auto* data=memory.diagnosticData(bank,bytes);
            assert(bytes==(bank==0 ? 0x60000u : bank==1 ? 0x800000u : 0));
            assert(bool(data)==(bank<2));
            for (uint32_t i=0;i<bytes;++i) assert(data[i]==0);
            assert(memory.dirty(bank).generation==dirty.generation && memory.dirty(bank).pending==dirty.pending);
        }
    }
    struct Capture {
        std::vector<uint32_t> steps;
        uint32_t memoryBytes=0;
        static void words(void* ctx,const char* name,const uint32_t* w,uint32_t count) {
            auto& c=*static_cast<Capture*>(ctx);
            if (!std::strcmp(name,"recent")) {
                assert(count==10 && w[1]==0x10000000 && w[2]==0xeafffffe);
                c.steps.push_back(w[0]);
            }
        }
        static void bytes(void* ctx,const char* name,const uint8_t* bytes,uint32_t count) {
            if (!std::strcmp(name,"sram")) {
                assert(count==0x20000);
                for (uint32_t i=0;i<count;++i) assert(bytes[i]==0);
            }
            static_cast<Capture*>(ctx)->memoryBytes+=count;
        }
    };
    for (bool fast:{false,true}) for (uint32_t steps:{1u,4096u,5000u}) {
        Capture capture;
        ArmDiagnosticSink sink{&capture,Capture::words,Capture::bytes};
        Imx1ProbeOptions options;
        options.diagnosticSnapshot=&sink;
        const auto diagnostic=run({0xeafffffe},steps,fast,&options);
        const auto ordinary=run({0xeafffffe},steps,fast);
        assert(diagnostic.stop==ordinary.stop && diagnostic.trace.step==ordinary.trace.step &&
            diagnostic.busAccesses==ordinary.busAccesses && diagnostic.cpsr==ordinary.cpsr &&
            diagnostic.spsr==ordinary.spsr && diagnostic.idleHclk==ordinary.idleHclk &&
            !std::memcmp(diagnostic.registers,ordinary.registers,sizeof(ordinary.registers)));
        assert(capture.steps.size()==(steps<4096 ? steps : 4096) && capture.memoryBytes==0x20000);
        assert(capture.steps.front()==steps-capture.steps.size() && capture.steps.back()==steps-1);
    }
    // Guarded instructions are absent from the retired-context ring.
    Capture guarded;
    ArmDiagnosticSink guardedSink{&guarded,Capture::words,Capture::bytes};
    Imx1ProbeOptions guardedOptions;
    guardedOptions.diagnosticSnapshot=&guardedSink;
    run({0xe16f1f10},1,false,&guardedOptions);
    assert(guarded.steps.empty());
    for (uint32_t invalid : {0u, imx1ProbeMaximumSteps + 1, UINT32_MAX}) {
        const auto rejected=imx1BootProbe(nullptr,0,Imx1ProbeEntry::physicalFlash,invalid);
        assert(rejected.stop==Imx1ProbeStop::unsupportedPeripheral && rejected.peripheralStatus==Imx1BootstrapAccess::sequence);
    }

    assert(run({0xe16f1f10}, imx1ProbeMaximumSteps).stop == Imx1ProbeStop::unsupportedInstruction);
    // Invalid virtual profiles fail before CPU setup or bus activity.
    for (bool fast : {false, true}) {
        for (unsigned scenario = 0; scenario < 3; ++scenario) {
            Imx1ProbeOptions options;
            options.graphicsProfile = scenario == 0 ? static_cast<Imx1GraphicsProfile>(255) :
                Imx1GraphicsProfile::virtual5744Compatible8MiB;
            options.bootstrapPeripherals = scenario == 0 || scenario == 2;
            options.cs5ResetStatusInputKnown = scenario == 2;
            const auto result = imx1BootProbe(nullptr, 0, Imx1ProbeEntry::physicalFlash,
                                              1, nullptr, nullptr, fast, &options);
            assert(result.stop == Imx1ProbeStop::unsupportedPeripheral);
            assert(result.peripheralStatus == Imx1BootstrapAccess::sequence && !result.busAccesses);
            assert(!result.jitBlocks && !result.irqEntries && !result.fiqEntries);
        }
    }

    for (bool fast : {false,true}) {
        const auto strict = run({0xee070f55,0xeafffffe},4,fast);
        assert(strict.stop == Imx1ProbeStop::unsupportedCp15 && strict.trace.step == 0);
        Imx1ProbeOptions compatibility;
        compatibility.arm920IcacheIndexCompatibility = true;
        const auto accepted = run({0xee070f55,0xeafffffe},4,fast,&compatibility);
        assert(accepted.stop == Imx1ProbeStop::budget && accepted.trace.step == 3);
        assert(accepted.registers[15] == 0x10000004 && accepted.registers[0] == 0);
    }

    // Cache maintenance with disabled caches, ARM920 control reset and first MMIO store.
    auto r = run({0xe3a00000, 0xee070f16, 0xee070f15, 0xee111f10,
                  0xe59f0004, 0xe5801000, 0xeafffffe, 0x00205034});
    assert(r.stop == Imx1ProbeStop::unsupportedWrite && r.trace.pc == 0x10000014);
    assert(r.address == 0x00205034 && r.size == 4 && r.value == 0x78);
    r = run({0xe59f0000, 0xe5901000, 0x0021b000});
    assert(r.stop == Imx1ProbeStop::unsupportedRead && r.address == 0x0021b000 && r.size == 4);
    r = run({0xee110f10, 0xe3800004, 0xee010f10});
    assert(r.stop == Imx1ProbeStop::unsupportedCp15 && r.trace.operand == 0x7c);
    for (uint32_t instruction : {0xee100f50u, 0xee170f16u, 0xee070f56u, 0xee080f37u}) {
        r = run({instruction});  // Unknown ID register, read of write-only cache register, unsupported cache/TLB form.
        assert(r.stop == Imx1ProbeStop::unsupportedCp15);
    }
    r = run({0xee100f10, 0xeafffffe}, 3);  // The ARM920T main ID reads.
    assert(r.stop == Imx1ProbeStop::budget && r.registers[0] == 0x41129200);
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
        r = run({0xe3a000d7, 0xe121f000, 0xe3a0da01, 0xe3a000d3, 0xe121f000, 0xee100f50}, 20, fast);
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
