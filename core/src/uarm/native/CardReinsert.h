#ifndef FROND_CARD_REINSERT_H
#define FROND_CARD_REINSERT_H

#include <algorithm>
#include <cstdint>
#include <string>

#include "SoC.h"
#include "soc_PXA.h"

// A card put in right after an eject: the removal and the insert can both land before the guest
// runs (the frontend pauses the core while the card's files are edited), and a guest that never
// saw the card go keeps its old volume. The eject is noted, and the next insert first lets the
// guest run until it has had delay of awake time since the eject and, where the board can tell
// (SoC::SdRemovalHandled), its card driver has handled the removal, or limit in all. The T|X
// driver stops the MMC clock about 0.65 s after the eject at 312 MHz.
namespace frondNative {

struct EjectWatch {
    bool pending{false};
    uint64_t at{0};  // guest time of the eject, ns
};

inline EjectWatch& ejectWatch() {
    static EjectWatch watch;
    return watch;
}

inline void noteEjected(SoC& soc) { ejectWatch() = {true, soc.GetTime()}; }

inline bool guestAsleep(SoC& soc) { return soc.PowerSleeping(); }

// Runs the guest if an eject is still unseen, at the guest's own clock (100 MHz where it has
// none), as the removal's handling is CPU bound.
inline void settleEject(SoC& soc, uint64_t delay = 500000000, uint64_t limit = 5000000000) {
    auto& watch = ejectWatch();
    if (!watch.pending) return;
    watch.pending = false;
    const uint64_t cyclesPerSecond = soc.FollowHz() ? soc.FollowHz() : 100000000;
    const uint64_t slice = cyclesPerSecond / 50;  // 20 ms
    const uint64_t start = soc.GetTime();
    uint64_t awake = guestAsleep(soc) ? 0 : std::min(delay, start - watch.at);
    while ((awake < delay || !soc.SdRemovalHandled().value_or(true)) && soc.GetTime() - watch.at < limit) {
        const uint64_t before = soc.GetTime();
        soc.Run(slice, cyclesPerSecond);
        const uint64_t after = soc.GetTime();
        if (after == before) break;
        if (!guestAsleep(soc)) awake += after - before;
    }
}

inline bool insertsCard(const std::string& command) {
    return command == "sd-insert" || command == "sd-create" || command == "sd-put-in" || command == "sd-reinsert" ||
           command == "sd2-insert" || command == "sd2-create" || command == "sd2-put-in" || command == "sd2-reinsert" ||
           command == "ms-insert" || command == "ms-create" || command == "ms-put-in" || command == "ms-reinsert";
}

}  // namespace frondNative

#endif
