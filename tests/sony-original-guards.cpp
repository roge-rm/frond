#include <cassert>
#include <cstdio>
#include <vector>
#include "tg50-audio-fixture.h"
#include "CPU.h"
#include "SonyDacPreview.h"
#include "MMU.h"
#include "RAM.h"
#include "pxa_I2S.h"
#include "mem_internal.h"

struct Instructions {
    unsigned calls{};
    uint32_t before{};
    static void Observe(ArmCpu* cpu,void* context,uint32_t) {
        auto& self=*static_cast<Instructions*>(context);
        assert(cpuGetRegisters(cpu)[0] == self.before++);
        ++self.calls;
    }
};
struct Writes {
    uintptr_t backing{};
    unsigned calls{};
    static void Observe(void* context,uintptr_t backing,uint32_t bytes) {
        auto& self=*static_cast<Writes*>(context);
        assert(bytes == 4);
        assert(*reinterpret_cast<const uint32_t*>(backing) == self.calls);
        if (self.backing) assert(backing == self.backing);
        self.backing = backing;
        ++self.calls;
    }
};
int main() {
    SonyDacPreview ledger;
    const uint32_t gain[2]={65536,65536}, zeros[2]={}, words[2]={0x12345678,0x23456789};
    assert(ledger.Start());
    assert(ledger.Submit({1,2,3,0x1000,2},zeros,true));
    assert(ledger.Render(0,{},gain) == 0);
    const auto old = ledger.Dma(0x1000,4,0);
    ledger.Fence();
    const auto before=ledger.Generations()[0];
    assert(ledger.Start());
    const auto after=ledger.Generations()[0];
    assert(after.references == before.references && after.delivered == before.delivered &&
           !after.preview && after.previewNext == after.identity.frames);
    assert(ledger.Submit({4,5,3,0x2000,2},words,true));
    assert(ledger.Render(0,{},gain) == words[0]);
    assert(ledger.Serial(old)); // Prior zero credit still spends exactly once.
    const auto oldLast=ledger.Dma(0x1004,4,0);
    ledger.Complete(1,2);
    assert(!ledger.Serial(oldLast));
    ledger.Fence();
    assert(!ledger.Start()); // Retained new nonzero source cannot be skipped.

    AudioMachine machine;
    auto& soc=machine.soc;
    assert(!soc.EnableSonyPreview()); // Unrecognised synthetic firmware.
    machine.loop();
    auto* cpu=soc.GetCpu();
    machine.wr(0xa0003000,0xe2800001); // ADD r0,r0,#1
    machine.wr(0xa0003004,0xeafffffd); // B 0xa0003000
    cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu,0,0);
    Instructions instructions;
    cpuSetInstructionObserver(cpu,Instructions::Observe,&instructions,0xa0003000,0xa0003000);
    cpuCycle<ARM_MEMORY_SYSTEM_MMU,false>(cpu,20);
    assert(instructions.calls && cpuGetRegisters(cpu)[0] == instructions.calls);
    cpuSetReg(cpu,15,0xa0003000);
    cpuCyclePure(cpu);
    assert(cpuGetRegisters(cpu)[0] == instructions.calls);
    cpuSetReg(cpu,15,0xa0003000);
    cpuCycle<ARM_MEMORY_SYSTEM_MMU,true>(cpu,1);
    assert(cpuGetRegisters(cpu)[0] == instructions.calls);
    cpuSetInstructionObserver(cpu,nullptr,nullptr,0,0);

    auto* mem=cpuGetMem(cpu);
    Writes writes;
    for (auto& region:mem->regions) if (region.aF == static_cast<ArmMemAccessF>(ramAccessF))
        ramSetWriteObserver(static_cast<ArmRam*>(region.uD),Writes::Observe,&writes);
    // Both addresses resolve to the same backing. The callback sees the old word.
    machine.wr(0xa0001000,1);
    machine.wr(0xa1101000,2);
    assert(writes.calls == 2);
    for (auto& region:mem->regions) if (region.aF == static_cast<ArmMemAccessF>(ramAccessF))
        ramSetWriteObserver(static_cast<ArmRam*>(region.uD),nullptr,nullptr);

    PxaI2s* i2s{};
    for (auto& region:mem->regions) if (region.pa == 0x40400000)
        i2s=static_cast<PxaI2s*>(region.uD);
    assert(i2s);
    unsigned controls{};
    pxaI2sSetTxObserver(i2s,&controls,[](void* context,PxaI2sTxEvent event,uint32_t) {
        if (event == PxaI2sTxEvent::control) ++*static_cast<unsigned*>(context);
    });
    auto read32=[&](uint32_t address) {uint32_t value{};assert(memAccess(mem,address,4,false,&value));return value;};
    const auto control=read32(0x40400000), divisor=read32(0x40400060);
    machine.wr(0x40400004,0); machine.wr(0x40400004,0);
    machine.wr(0x40400000,control); machine.wr(0x40400060,divisor);
    assert(controls == 0);
    machine.wr(0x40400004,1); assert(controls == 1);
    machine.wr(0x40400004,1); assert(controls == 1);
    machine.wr(0x40400004,0x10); const auto flushed=controls;
    machine.wr(0x40400004,0x10); assert(controls > flushed);
    machine.wr(0x40400000,control | 8); const auto reset=controls;
    machine.wr(0x40400000,control | 8); assert(controls > reset);
    pxaI2sSetTxObserver(i2s,nullptr,nullptr);

    uint32_t physical{};
    assert(cpuInspectMapping(cpu,0xa0001ff0,0x2020,physical) && physical == 0xa0001ff0);
    assert(!cpuInspectMapping(cpu,0xfffffff0,32,physical));
    assert(!cpuInspectMapping(cpu,0xa0001000,0,physical));
    assert(!cpuInspectMapping(cpu,0x40400080,4,physical)); // MMIO.
    auto* mmu=cpuGetMMU(cpu);
    machine.wr(0xa0004800,0xa0008001); // VA20000000 coarse table.
    for (unsigned page=0;page<3;++page)
        machine.wr(0xa0008000+page*4,0xa0010ffe + page*0x1000);
    mmuSetDomainCfg(mmu,1);
    mmuSetTTP(mmu,0xa0004000);
    assert(cpuInspectMapping(cpu,0x20000010,0x2fe0,physical) && physical == 0xa0010010);
    machine.wr(0xa0008004,0xa0020ffe); // The middle page changes, no TLB flush.
    assert(!cpuInspectMapping(cpu,0x20000010,0x2fe0,physical));
    machine.wr(0xa0008004,0);
    assert(!cpuInspectMapping(cpu,0x20000010,0x2fe0,physical));
    machine.wr(0xa0008004,0x40400ffe);
    assert(!cpuInspectMapping(cpu,0x20000010,0x2fe0,physical));
    puts("Sony original guard mapping/instruction/write-order negatives passed");
}
