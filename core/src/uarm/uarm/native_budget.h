#ifndef FROND_NATIVE_BUDGET_H
#define FROND_NATIVE_BUDGET_H
#include <algorithm>
#include <cstdint>
// Time a native session's database import and export calls get for the data they move. Each
// top-level call gets 10 slices of 50M cycles plus 16 per MiB the host moved in its own callbacks,
// at most 1000. A 2 MB PRC with 521 small resources needs over 22 slices on the T|X, since NVFS
// stores each record separately. A call that moves nothing stops at 10 slices however much earlier calls moved. The
// session's limits grow by what finished calls were given plus the running call's share.
struct NativeBudget {
    static constexpr uint64_t baseSlices = 10, maxExtra = 1000;
    uint64_t moved{0}, callStart{0}, earned{0};
    static uint64_t extraFor(uint64_t bytes) { return std::min<uint64_t>(bytes * 16 >> 20, maxExtra); }
    void beginCall() { callStart = moved; }
    void add(uint64_t bytes) { moved += bytes; }
    uint64_t callExtra() const { return extraFor(moved - callStart); }
    void endCall() { earned = std::min<uint64_t>(earned + callExtra(), maxExtra); callStart = moved; }
    uint64_t sliceLimit() const { return baseSlices + callExtra(); }
    uint64_t sessionExtra() const { return std::min<uint64_t>(earned + callExtra(), maxExtra); }
};
#endif
