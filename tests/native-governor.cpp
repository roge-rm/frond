#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "MainLoop.h"
#include "soc_PXA.h"

static uint64_t wallUsec = 1000000;
extern "C" uint64_t timestampUsec() { return wallUsec; }
extern "C" void uarmAbort() { std::abort(); }

class TimedSoc : public SocPXA {
   public:
    using SocPXA::SocPXA;
    uint64_t capacity = 60000000;
    uint64_t descheduledUsec = 0;
    uint64_t lastBudget = 0;
    uint64_t lastCycles = 0;
    uint64_t Run(uint64_t cycles, uint64_t budget) override {
        lastBudget = budget;
        lastCycles = cycles;
        if (!cycles) { ++wallUsec; return 0; }
        const auto computeUsec = (cycles * 1000000 + capacity - 1) / capacity;
        wallUsec += computeUsec + descheduledUsec;
        descheduledUsec = 0;
        return cycles;
    }
};

int main() {
    std::vector<uint8_t> rom(16 * 1024 * 1024), nand(132 * 1024 * 1024, 0xff);
    TimedSoc soc(DeviceType5::deviceTypeTX, 32 * 1024 * 1024, rom.data(), rom.size(),
                 nand.data(), nand.size(), -1, 2);
    MainLoop loop(&soc);
    loop.SetCyclesPerSecondLimit(60000000);
    loop.Cycle(wallUsec);
    uint64_t deadline = wallUsec;
    auto cycle = [&]() {
        deadline += loop.GetTimesliceSizeUsec();
        wallUsec = std::max(wallUsec, deadline);
        loop.Cycle(wallUsec);
    };
    cycle();
    for (unsigned i = 0; i < 63; ++i) cycle();
    const auto settled = soc.lastBudget;
    assert(settled >= 55000000 && settled <= 59000000);
    assert(loop.GetSuggestedDelayUsec(wallUsec) > 0);
    loop.SetTimesliceUsec(1000000 / 120);
    for (unsigned i = 0; i < 64; ++i) cycle();
    assert(soc.lastBudget == settled);
    loop.SetTimesliceUsec(1000000 / 60);

    // Poll events between bounded catch-up requests, including after a frontend stall.
    wallUsec += 25000;
    cycle();
    assert(soc.lastCycles <= soc.lastBudget * loop.GetTimesliceSizeUsec() / 1000000);
    assert(loop.GetSuggestedDelayUsec(wallUsec) == 0);
    for (unsigned i = 0; i < 64; ++i) cycle();

    // Recover to available compute after a transient descheduling sample.
    soc.descheduledUsec = 40000;
    cycle();
    for (unsigned i = 0; i < 64; ++i) cycle();
    assert(soc.lastBudget >= 55000000 && soc.lastBudget <= 59000000);

    const auto prePauseBudget = soc.lastBudget;
    wallUsec += 5000000;
    loop.PauseAt(wallUsec);
    assert(loop.GetSuggestedDelayUsec(wallUsec) == int64_t(loop.GetTimesliceSizeUsec()));
    deadline = wallUsec;
    cycle();
    assert(soc.lastBudget == prePauseBudget);

    // Real compute-cost increases and the requested limit still constrain the clock.
    soc.capacity = 40000000;
    for (unsigned i = 0; i < 100; ++i) cycle();
    assert(soc.lastBudget >= 37000000 && soc.lastBudget <= 39000000);
    loop.SetCyclesPerSecondLimit(25000000);
    cycle();
    assert(soc.lastBudget == 25000000);
    puts("PASS: native pacing bounds catch-up work, recovers after stalls and honours pauses and ceiling");
}
