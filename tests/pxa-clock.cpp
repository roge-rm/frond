// Original follows the core clock: CCCR and CLKCFG to Hz on both chips, a change taking effect at the
// CLKCFG write in the middle of a run, and a session saved in turbo loading at the same clock.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "CPU.h"
#include "mem.h"
#include "soc_PXA.h"
#include "xscale_cost.h"

static constexpr uint32_t CCCR = 0x41300000;
static constexpr uint32_t WRITE_CLKCFG = 0xee060e10;

static void write(SocPXA& soc, uint32_t address, uint32_t value) {
    assert(memAccess(cpuGetMem(soc.GetCpu()), address, 4, true, &value));
}

// One CLKCFG write at the start of RAM with r0 = value.
static void clkcfg(SocPXA& soc, uint32_t value) {
    auto cpu = soc.GetCpu();
    write(soc, 0xa0000000, WRITE_CLKCFG);
    write(soc, 0xa0000004, 0xeafffffe);
    cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu, ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC | ARM_SR_I | ARM_SR_F);
    cpuSetReg(cpu, 15, 0xa0000000);
    cpuSetReg(cpu, 0, value);
    cpuCyclePure(cpu);
}

// 100000 nops (0.32 ms at 312 MHz), a CLKCFG write of r0, then a branch to itself.
static void program(SocPXA& soc, uint32_t clkcfgValue) {
    auto cpu = soc.GetCpu();
    for (uint32_t i = 0; i < 100000; ++i) write(soc, 0xa0000000 + 4 * i, 0xe1a00000);
    write(soc, 0xa0000000 + 400000, WRITE_CLKCFG);
    write(soc, 0xa0000000 + 400004, 0xeafffffe);
    cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu, ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC | ARM_SR_I | ARM_SR_F);
    cpuSetReg(cpu, 15, 0xa0000000);
    cpuSetReg(cpu, 0, clkcfgValue);
}

static void conversions() {
    using namespace cost;
    // PXA25x: 3.6864 MHz x L x M x N, turbo only with the T bit.
    assert(pxa25xCoreHz(0x122, false) == 99532800);   // reset: L 27, M 1, N 1
    assert(pxa25xCoreHz(0x141, true) == 199065600);   // E2: L 27, M 2, N 1
    assert(pxa25xCoreHz(0x221, true) == 199065600);   // TG50: L 27, M 1, N 2
    assert(pxa25xCoreHz(0x221, false) == 99532800);
    assert(pxa25xCoreHz(0x1a3, true) == 3686400ull * 36 * 3 / 2);  // L 36, M 1, N 1.5
    assert(pxa25xCoreHz(0x1a3, false) == 3686400ull * 36);
    // PXA27x: 13 MHz x L is the run clock, turbo x N, half-turbo x N / 2.
    assert(pxa27xCoreHz(0x190, 0x8) == 208000000);
    assert(pxa27xCoreHz(0x190, 0x9) == 312000000);
    assert(pxa27xCoreHz(0x190, 0xb) == 312000000);
    assert(pxa27xCoreHz(0x190, 0x4) == 156000000);
    assert(pxa27xCoreHz(0x190, 0x5) == 156000000);
    assert(pxa27xCoreHz(0x210, 0x1) == 13000000ull * 16 * 4 / 2);  // 2N 4
}

struct Rig {
    std::vector<uint8_t> rom, nand;
    std::unique_ptr<SocPXA> soc;
    Rig(DeviceType5 type, uint8_t rev) : rom((type == deviceTypeTX ? 16u : 8u) << 20),
        nand(type == deviceTypeTX ? 132u << 20 : 34603008, 0xff) {
        const uint32_t branch = 0xeafffffe;
        memcpy(rom.data(), &branch, 4);
        soc = std::make_unique<SocPXA>(type, 32u << 20, rom.data(), rom.size(), nand.data(), nand.size(), -1, rev);
    }
};

static std::vector<uint8_t> snapshot(SocPXA& soc) {
    assert(soc.Save());
    auto data = soc.GetSavestate();
    return {static_cast<uint8_t*>(data.data), static_cast<uint8_t*>(data.data) + data.size};
}

static void socClocks() {
    // T|X: reset is 0x190 in run mode (208 MHz); CLKCFG turbo gives 312 MHz, the write sets it.
    Rig tx(deviceTypeTX, 2);
    assert(tx.soc->CoreHz() == 208000000);
    clkcfg(*tx.soc, 0xb);
    assert(tx.soc->CoreHz() == 312000000);
    clkcfg(*tx.soc, 0x8);
    assert(tx.soc->CoreHz() == 208000000);
    clkcfg(*tx.soc, 0x9);
    // CCCR alone does nothing until the next CLKCFG write.
    write(*tx.soc, CCCR, 0x210);
    assert(tx.soc->CoreHz() == 312000000);
    clkcfg(*tx.soc, 0xb);
    assert(tx.soc->CoreHz() == 416000000);
    write(*tx.soc, CCCR, 0x190);
    clkcfg(*tx.soc, 0xb);
    clkcfg(*tx.soc, 0x6);  // half-turbo
    assert(tx.soc->CoreHz() == 156000000);
    clkcfg(*tx.soc, 0x9);

    // Saved in turbo, loaded into another machine at the same clock; a reset goes back to the reset one.
    auto saved = snapshot(*tx.soc);
    Rig other(deviceTypeTX, 2);
    assert(other.soc->CoreHz() == 208000000);
    assert(other.soc->Load(saved.size(), saved.data()));
    assert(other.soc->CoreHz() == 312000000);
    other.soc->Reset();
    assert(other.soc->CoreHz() == 208000000);

    // E2 (PXA255): CCCR 0x141 and the turbo bit.
    Rig e2(deviceTypeE2, 0);
    assert(e2.soc->CoreHz() == 99532800);
    write(*e2.soc, CCCR, 0x141);
    clkcfg(*e2.soc, 0x3);
    assert(e2.soc->CoreHz() == 199065600);
    clkcfg(*e2.soc, 0x0);
    assert(e2.soc->CoreHz() == 199065600);  // N is 1: turbo makes no difference
    write(*e2.soc, CCCR, 0x221);  // TG50
    clkcfg(*e2.soc, 0x0);
    assert(e2.soc->CoreHz() == 99532800);
    clkcfg(*e2.soc, 0x3);
    assert(e2.soc->CoreHz() == 199065600);
    auto savedE2 = snapshot(*e2.soc);
    Rig e2b(deviceTypeE2, 0);
    assert(e2b.soc->Load(savedE2.size(), savedE2.data()) && e2b.soc->CoreHz() == 199065600);
}

// A run of 10 ms of guest time from turbo, where the program leaves turbo after 100000 instructions: the
// guest time is 10 ms either way, the cycles run are not, and a fixed rate runs them all at the same speed.
static void midRun() {
    for (const bool follow : {true, false}) {
        Rig tx(deviceTypeTX, 2);
        auto& soc = *tx.soc;
        clkcfg(soc, 0xb);
        program(soc, 0x8);
        soc.SetFollowCoreClock(follow);
        const uint64_t before = soc.GetTime(), instructions = soc.GetRunInstructions();
        const uint64_t cycles = soc.Run(3120000, 312000000);  // 10 ms at 312 MHz
        const uint64_t ns = soc.GetTime() - before, ran = soc.GetRunInstructions() - instructions;
        assert(soc.CoreHz() == 208000000);
        printf("%s: returned %llu, ran %llu clocks, %llu ns\n", follow ? "follow" : "fixed", (unsigned long long)cycles,
               (unsigned long long)ran, (unsigned long long)ns);
        assert(ns >= 9990000 && ns <= 10010000);  // 10 ms, to the rounding of each batch
        if (follow) {
            // 208 MHz after the first 0.32 ms: about 2.11 M clocks, 3.12 M returned in 312 MHz units.
            assert(ran > 2100000 && ran < 2125000);
            assert(cycles >= 3120000 && cycles < 3125000);
        } else {
            assert(ran >= 3120000 && ran < 3125000);
        }
    }

    // The same through a save and load while still in turbo: it runs on as the straight run does, to the
    // few nanoseconds a load costs the scheduler's remainder (the same with a fixed rate).
    for (const bool follow : {true, false}) {
        Rig a(deviceTypeTX, 2), b(deviceTypeTX, 2);
        for (auto* r : {&a, &b}) {
            clkcfg(*r->soc, 0xb);
            program(*r->soc, 0x8);
            r->soc->SetFollowCoreClock(follow);
            r->soc->Run(31200, 312000000);  // 100 us: still before the write
        }
        auto saved = snapshot(*b.soc);
        Rig c(deviceTypeTX, 2);
        c.soc->SetFollowCoreClock(follow);
        // The RAM is not in the state.
        memcpy(c.soc->GetMemoryData().data, b.soc->GetMemoryData().data, b.soc->GetMemoryData().size);
        assert(c.soc->Load(saved.size(), saved.data()) && c.soc->CoreHz() == 312000000);
        a.soc->Run(3120000, 312000000);
        c.soc->Run(3120000, 312000000);
        const int64_t apart = int64_t(a.soc->GetTime()) - int64_t(c.soc->GetTime());
        printf("%s: loaded run %lld ns from the straight one, clocks %llu against %llu\n", follow ? "follow" : "fixed", (long long)apart,
               (unsigned long long)a.soc->GetRunInstructions(), (unsigned long long)c.soc->GetRunInstructions());
        assert(a.soc->CoreHz() == 208000000 && c.soc->CoreHz() == 208000000);
        assert(apart > -1000 && apart < 1000);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    conversions();
    socClocks();
    midRun();
    printf("pxa-clock ok\n");
    return 0;
}
