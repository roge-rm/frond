// Faster speed on the PXA: the core runs the clock CCCR and CLKCFG give times the multiplier, guest time
// and the scheduler stay on the guest's own clock, and Original runs as before. Also the speed setting's
// names and MainLoop's speed switch.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "CPU.h"
#include "MainLoop.h"
#include "mem.h"
#include "soc_PXA.h"
#include "speed_setting5.h"

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

// A loop of 1000 nops and a branch back, for ever.
static void program(SocPXA& soc) {
    auto cpu = soc.GetCpu();
    for (uint32_t i = 0; i < 1000; ++i) write(soc, 0xa0000000 + 4 * i, 0xe1a00000);
    write(soc, 0xa0000000 + 4000, 0xea000000 | ((uint32_t(-4000 - 8) / 4) & 0xffffff));
    cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu, ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC | ARM_SR_I | ARM_SR_F);
    cpuSetReg(cpu, 15, 0xa0000000);
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

struct Outcome { uint64_t ns, clocks, returned; };

// 10 ms of guest time asked for at the rate MainLoop gives a following run (the clock times the multiplier).
static Outcome run(SocPXA& soc, uint32_t num, uint32_t den, bool setMultiplier = true) {
    program(soc);
    soc.SetFollowCoreClock(true);
    if (setMultiplier) soc.SetClockMultiplier(num, den);
    const uint64_t rate = soc.FollowHz();
    assert(rate == soc.CoreHz() * num / den);
    const uint64_t before = soc.GetTime(), instructions = soc.GetRunInstructions();
    const uint64_t returned = soc.Run(rate / 100, rate);
    return {soc.GetTime() - before, soc.GetRunInstructions() - instructions, returned};
}

static void names() {
    SpeedSetting5 s;
    assert(SpeedSetting5::Parse("original", s) && s == SpeedSetting5::Original() && s.Name() == "original");
    assert(SpeedSetting5::Parse("uncapped", s) && s.kind == SpeedSetting5::Kind::uncapped && s.Name() == "uncapped");
    assert(SpeedSetting5::Parse("1.5x", s) && s.kind == SpeedSetting5::Kind::faster && s.num == 3 && s.den == 2 &&
           s.Name() == "1.5x" && s.Multiplier() == 1.5);
    for (const char* name : {"2x", "3x", "4x", "8x"}) {
        assert(SpeedSetting5::Parse(name, s) && s.kind == SpeedSetting5::Kind::faster && s.den == 1 && s.Name() == name);
    }
    assert(SpeedSetting5::Faster().Name() == "2x");
    const SpeedSetting5 before = s;
    for (const char* bad : {"", "5x", "2", "x", "0x", "1x", "16x", "Original", "2.0x", "fast"})
        assert(!SpeedSetting5::Parse(bad, s) && s == before);
}

// Each multiplier on the T|X at 312 MHz and the E2 at 199 MHz: the same guest time, N times the clocks.
static void faster() {
    for (const auto device : {deviceTypeTX, deviceTypeE2}) {
        const auto setUp = [&](Rig& rig) {
            if (device == deviceTypeE2) write(*rig.soc, CCCR, 0x141);
            clkcfg(*rig.soc, device == deviceTypeTX ? 0xb : 0x3);
        };
        Rig plain(device, device == deviceTypeTX ? 2 : 0);
        setUp(plain);
        const uint64_t hz = plain.soc->CoreHz();
        assert(hz == (device == deviceTypeTX ? 312000000u : 199065600u));
        const auto base = run(*plain.soc, 1, 1, false);
        // Original set explicitly runs as a machine that never heard of the multiplier.
        Rig original(device, device == deviceTypeTX ? 2 : 0);
        setUp(original);
        const auto same = run(*original.soc, 1, 1);
        assert(same.ns == base.ns && same.clocks == base.clocks && same.returned == base.returned);
        assert(base.ns >= 9990000 && base.ns <= 10010000);
        printf("%s original: %llu clocks in %llu ns\n", device == deviceTypeTX ? "T|X" : "E2",
               (unsigned long long)base.clocks, (unsigned long long)base.ns);
        for (const auto& m : {std::pair<uint32_t, uint32_t>{3, 2}, {2, 1}, {3, 1}, {4, 1}, {8, 1}}) {
            Rig rig(device, device == deviceTypeTX ? 2 : 0);
            setUp(rig);
            const auto r = run(*rig.soc, m.first, m.second);
            // The core clock itself is still the guest's; only the rate the run takes changes.
            assert(rig.soc->CoreHz() == hz && rig.soc->FollowsCoreClock());
            // Fast's memory charges stay on at Faster, as at Original.
            assert(cpuJitMemoryCost(rig.soc->GetCpu()));
            const double ratio = double(r.clocks) / base.clocks, want = double(m.first) / m.second;
            printf("  %u/%u: %llu clocks in %llu ns, %.5fx\n", m.first, m.second, (unsigned long long)r.clocks,
                   (unsigned long long)r.ns, ratio);
            assert(r.ns >= 9990000 && r.ns <= 10010000);
            assert(ratio > want * 0.999 && ratio < want * 1.001);
        }
    }
}

// A CLKCFG change mid-run at Faster: the multiplier goes with the new clock.
static void followsClock() {
    Rig tx(deviceTypeTX, 2);
    auto& soc = *tx.soc;
    clkcfg(soc, 0xb);
    soc.SetFollowCoreClock(true);
    soc.SetClockMultiplier(2, 1);
    assert(soc.FollowHz() == 624000000);
    clkcfg(soc, 0x8);
    assert(soc.CoreHz() == 208000000 && soc.FollowHz() == 416000000);
    program(soc);
    const uint64_t before = soc.GetTime(), instructions = soc.GetRunInstructions();
    soc.Run(4160000, 416000000);
    const uint64_t ns = soc.GetTime() - before, clocks = soc.GetRunInstructions() - instructions;
    assert(ns >= 9990000 && ns <= 10010000);
    assert(clocks >= 4160000 && clocks < 4165000);
}

// MainLoop with emulated time: each Cycle gives the guest its host time at the setting's rate.
static void mainLoop() {
    for (const auto& name : {"original", "2x", "8x", "uncapped"}) {
        SpeedSetting5 speed;
        assert(SpeedSetting5::Parse(name, speed));
        Rig tx(deviceTypeTX, 2);
        auto& soc = *tx.soc;
        clkcfg(soc, 0xb);
        program(soc);
        MainLoop loop(&soc);
        loop.SetEmulatedTime(true);
        loop.SetSpeed(speed, 100000000);
        assert(loop.GetSpeed() == speed);
        const bool uncapped = speed.kind == SpeedSetting5::Kind::uncapped;
        // Following the clock is what turns Fast's memory charges on (SoC::SetFollowCoreClock); Exact always has them.
        assert(soc.FollowsCoreClock() == !uncapped);
        assert(soc.ClockMultiplierNum() == (uncapped ? 1 : speed.num) && soc.ClockMultiplierDen() == (uncapped ? 1 : speed.den));
        const uint64_t start = 1000000000, guest = soc.GetTime(), instructions = soc.GetRunInstructions();
        loop.PauseAt(start);
        double seconds = 0;
        for (uint64_t now = start + 16000; now <= start + 48000; now += 16000) seconds += loop.Cycle(now);
        const uint64_t ns = soc.GetTime() - guest, clocks = soc.GetRunInstructions() - instructions;
        const double rate = uncapped ? 3000e6 : 312e6 * speed.Multiplier();
        printf("MainLoop %s: %.6f s returned, %llu ns of guest time, %llu clocks\n", name, seconds,
               (unsigned long long)ns, (unsigned long long)clocks);
        // 48 ms of host time is 48 ms of guest time at every setting.
        assert(ns >= 47990000 && ns <= 48010000);
        assert(clocks > rate * 0.04799 && clocks < rate * 0.04801);
        assert(loop.GetCurrentIps() == uint64_t(rate) && loop.SpeedName() == name);
    }
    // A fixed rate (set-mips, --mips) drops the multiplier and the clock following, and names itself until the
    // next speed setting.
    Rig tx(deviceTypeTX, 2);
    MainLoop loop(tx.soc.get());
    SpeedSetting5 speed;
    assert(SpeedSetting5::Parse("8x", speed));
    loop.SetSpeed(speed, 100000000);
    loop.SetFixedMips(100);
    assert(tx.soc->ClockMultiplierNum() == 1 && tx.soc->ClockMultiplierDen() == 1 && !tx.soc->FollowsCoreClock());
    assert(loop.SpeedName() == "fixed 100 MIPS");
    assert(SpeedSetting5::Parse("2x", speed));
    loop.SetSpeed(speed, 100000000);
    assert(loop.SpeedName() == "2x" && tx.soc->ClockMultiplierNum() == 2 && tx.soc->FollowsCoreClock());
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    names();
    faster();
    followsClock();
    mainLoop();
    printf("pxa-faster ok\n");
    return 0;
}
