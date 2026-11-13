#include "MainLoop.h"

#include <algorithm>
#include <cmath>
#include <ctime>

#include "cputil.h"

using namespace std;

namespace {
    constexpr int AVERAGE_TIMESLICES = 30;
    constexpr uint32_t BIN_SIZE = 1000000;
    constexpr uint32_t CYCLES_PER_SECOND_LIMIT_DEFAULT = 100000000;
    constexpr uint32_t MAX_LOAD_MAX = 97;
#ifdef __EMSCRIPTEN__
    constexpr uint32_t MAX_LOAD_CATCHUP_MARGIN = 20;
#else
    constexpr uint32_t MAX_LOAD_CATCHUP_MARGIN = 10;
#endif
    constexpr int PRESERVE_TIMESLICES_FOR_CATCHUP = 3;
}  // namespace

MainLoop::MainLoop(SoC* soc)
    : soc(soc),
      lastCyclesPerSecond(CYCLES_PER_SECOND_LIMIT_DEFAULT),
      cyclesPerSecondAverage(AVERAGE_TIMESLICES),
      maxLoad(MAX_LOAD_MAX),
      cyclesPerSecondLimit(CYCLES_PER_SECOND_LIMIT_DEFAULT) {
    uint64_t now = timestampUsec();

    realTimeUsec = now;
    virtualTimeUsec = now;
    cyclesPerSecondAverage.Add(cyclesPerSecondLimit);
}

void MainLoop::PauseAt(uint64_t now) {
    // Audio pacing intentionally pauses guest time; discard the resulting host-time lag.
    realTimeUsec = now;
    virtualTimeUsec = now;
    lastDeltaUsec = 0;
}

void MainLoop::SetTimesliceUsec(uint64_t usec) {
    timesliceUsec = std::clamp<uint64_t>(usec,1000,1000000);
}

double MainLoop::Cycle(uint64_t now, bool paused) {
    if (paused) {
        PauseAt(now);
        return 0;
    }

    const uint64_t lagCatchup = 3 * timesliceUsec / 2;
    const uint64_t lagSkip = 3 * timesliceUsec;
    // virtualTimeUsec += static_cast<double>(socGetInjectedTimeNsec(soc)) / 1000;
    soc->ResetInjectedTimeNsec();

    double deltaUsec = now - virtualTimeUsec;
    if (deltaUsec < 0) return 0;

    if (deltaUsec > lagSkip) {
        deltaUsec = timesliceUsec;
        virtualTimeUsec = now - deltaUsec;
        cyclesPerSecondAverage.Reset(1);

        // cerr << "too much lag, skipping forward" << endl << flush;
    } else if (lastDeltaUsec < lagCatchup &&
               deltaUsec >= lagCatchup) {
        cyclesPerSecondAverage.Reset(PRESERVE_TIMESLICES_FOR_CATCHUP);
    }

    lastDeltaUsec = deltaUsec;
    realTimeUsec = now;

    // With emulated time the lag handling above only paces the loop; the guest always gets the limit.
    double cyclesPerSecond = emulatedTime ? double(cyclesPerSecondLimit) : CalculateCyclesPerSecond(
        deltaUsec >= lagCatchup ? (maxLoad - MAX_LOAD_CATCHUP_MARGIN) : maxLoad);
#ifdef __EMSCRIPTEN__
    const double runUsec = deltaUsec;
#else
    const double runUsec = std::min(deltaUsec, double(timesliceUsec));
#endif
    const uint64_t cyclesEmulated =
        soc->Run(runUsec * cyclesPerSecond / 1E6, round(cyclesPerSecond));

    const double slizeSizeSeconds = cyclesEmulated / cyclesPerSecond;

    virtualTimeUsec += slizeSizeSeconds * 1E6;
    uint64_t now2 = timestampUsec();

    if (now2 != now) cyclesPerSecondAverage.Add((cyclesEmulated * 1000000) / (now2 - now));

    currentIps = cyclesPerSecond;
    currentIpsMax = cyclesPerSecondAverage.Calculate();

    return slizeSizeSeconds;
}

uint64_t MainLoop::GetTimesliceSizeUsec() const { return timesliceUsec; }

int64_t MainLoop::GetSuggestedDelayUsec(uint64_t now) const {
    // Catch up in short batches without sleeping away the remaining headroom.
    return std::clamp<double>(timesliceUsec - (double(now) - virtualTimeUsec), 0, timesliceUsec);
}

uint32_t MainLoop::GetCurrentIps() const { return currentIps; }

uint64_t MainLoop::GetCurrentIpsMax() const { return currentIpsMax; }

void MainLoop::SetMaxLoad(uint32_t maxLoad) {
    if (maxLoad < MAX_LOAD_CATCHUP_MARGIN) maxLoad = MAX_LOAD_CATCHUP_MARGIN;
    if (maxLoad > MAX_LOAD_MAX) maxLoad = MAX_LOAD_MAX;

    this->maxLoad = maxLoad;
}

void MainLoop::SetCyclesPerSecondLimit(uint32_t cyclesPerSecondLimit) {
    if (cyclesPerSecondLimit < 10) cyclesPerSecondLimit = 10;

    this->cyclesPerSecondLimit = cyclesPerSecondLimit;
}

uint64_t MainLoop::CalculateCyclesPerSecond(uint64_t safetyMargin) {
    const uint64_t avg = (cyclesPerSecondAverage.Calculate() * safetyMargin) / 100;
    const uint64_t avgBinned = max((avg / BIN_SIZE) * BIN_SIZE, static_cast<uint64_t>(BIN_SIZE));

    if (avgBinned < lastCyclesPerSecond || avg > lastCyclesPerSecond + BIN_SIZE + BIN_SIZE / 2)
        lastCyclesPerSecond = avgBinned;

    if (lastCyclesPerSecond > cyclesPerSecondLimit) lastCyclesPerSecond = cyclesPerSecondLimit;

    return lastCyclesPerSecond;
}
