#ifndef _MAIN_LOOP_H_
#define _MAIN_LOOP_H_

#include <cstdint>
#include <string>

#include "Average.h"
#include "SoC.h"
#include "speed_setting5.h"

#define MAIN_LOOP_FPS 60

class MainLoop {
   public:
    MainLoop(SoC* soc);

    double Cycle(uint64_t now, bool paused = false);
    void PauseAt(uint64_t now);
    void SetTimesliceUsec(uint64_t usec);

    uint64_t GetTimesliceSizeUsec() const;
    int64_t GetSuggestedDelayUsec(uint64_t now) const;

    uint64_t GetCurrentIps() const;
    uint64_t GetCurrentIpsMax() const;

    void SetMaxLoad(uint32_t maxLoad);
    void SetCyclesPerSecondLimit(uint32_t cyclesPerSecondLimit);
    // Runs every guest second at the full cycles per second limit, whatever the host manages, so
    // a slow host runs slower in wall time instead of giving the guest fewer cycles.
    // Original speed: the limit is the SoC's own core clock, as the guest changes it, where it has one;
    // the limit set above is the fallback. Off runs at the fixed limit.
    void SetFollowCoreClock(bool follow);
    // The machine's speed setting. Original and Faster follow the core clock, Faster with the SoC's clock
    // multiplier, and take originalHz as the limit where the SoC has no clock to follow. Uncapped is the
    // fixed UNCAPPED_MHZ.
    void SetSpeed(const SpeedSetting5& speed, uint32_t originalHz);
    const SpeedSetting5& GetSpeed() const { return speed; }
    // A fixed rate (set-mips, --mips) in place of a speed setting: clock multiplier 1, no core clock to
    // follow. Cleared by the next SetSpeed.
    void SetFixedMips(uint32_t mips);
    // The setting's name, or "fixed N MIPS" while a fixed rate is set.
    std::string SpeedName() const;
    static constexpr uint32_t UNCAPPED_MHZ = 3000;
    void SetEmulatedTime(bool emulatedTime) { this->emulatedTime = emulatedTime; }

   private:
    uint64_t CalculateCyclesPerSecond(uint64_t safetyMargin, uint64_t limit);
    uint64_t CurrentLimit() const;

   private:
    SoC* soc{nullptr};

    uint64_t realTimeUsec{0};
    uint64_t timesliceUsec{1000000 / MAIN_LOOP_FPS};
    uint64_t lastCyclesPerSecond{0};

    double virtualTimeUsec{0};
    double lastDeltaUsec{0};

    uint64_t currentIps{0};
    uint64_t currentIpsMax{0};

    Average<uint64_t> cyclesPerSecondAverage;

    uint32_t maxLoad{0};
    uint32_t cyclesPerSecondLimit{0};
    bool emulatedTime{false};
    bool followCoreClock{false};
    SpeedSetting5 speed{};
    uint32_t fixedMips{0};
};

#endif  // _MAIN_LOOP_H_
