#ifndef _MAIN_LOOP_H_
#define _MAIN_LOOP_H_

#include <cstdint>

#include "Average.h"
#include "SoC.h"

#define MAIN_LOOP_FPS 60

class MainLoop {
   public:
    MainLoop(SoC* soc);

    double Cycle(uint64_t now, bool paused = false);
    void PauseAt(uint64_t now);
    void SetTimesliceUsec(uint64_t usec);

    uint64_t GetTimesliceSizeUsec() const;
    int64_t GetSuggestedDelayUsec(uint64_t now) const;

    uint32_t GetCurrentIps() const;
    uint64_t GetCurrentIpsMax() const;

    void SetMaxLoad(uint32_t maxLoad);
    void SetCyclesPerSecondLimit(uint32_t cyclesPerSecondLimit);
    // Runs every guest second at the full cycles per second limit, whatever the host manages, so
    // a slow host runs slower in wall time with the same guest result.
    void SetEmulatedTime(bool emulatedTime) { this->emulatedTime = emulatedTime; }

   private:
    uint64_t CalculateCyclesPerSecond(uint64_t safetyMargin);

   private:
    SoC* soc{nullptr};

    uint64_t realTimeUsec{0};
    uint64_t timesliceUsec{1000000 / MAIN_LOOP_FPS};
    uint64_t lastCyclesPerSecond{0};

    double virtualTimeUsec{0};
    double lastDeltaUsec{0};

    uint32_t currentIps{0};
    uint64_t currentIpsMax{0};

    Average<uint64_t> cyclesPerSecondAverage;

    uint32_t maxLoad{0};
    uint32_t cyclesPerSecondLimit{0};
    bool emulatedTime{false};
};

#endif  // _MAIN_LOOP_H_
