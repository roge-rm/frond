#ifndef _DELAY_LOOPS_H_
#define _DELAY_LOOPS_H_

#include <cstdint>

#include "xscale_cost.h"

// The ROM busy-wait delays the Fast engine skips (cpuPrvDelaySkip in CPU.cpp). Each pass loads OSCR
// (base register + 16), compares it with a target and loops while it is at or below it. The TG50's
// also leaves when OSCR drops below a low bound (r4), for a count that wrapped. A pass changes only
// the loaded register and the flags. Matched by these instructions, from the head (the load) to the
// branch back, wherever they sit.

enum class DelayLoopKind : uint8_t { e2, tg50, zire72 };

struct DelayLoop {
    DelayLoopKind kind;
    bool thumb;
    const uint32_t *code;  // ARM words, or Thumb halfwords, head first
    uint32_t count;
    uint8_t value, base, target, low;  // registers: loaded, timer base, target, low bound (kNoLow: none)
    uint8_t failing;                   // the instruction whose condition fails on every pass, kNoLow: none
};

constexpr uint8_t kNoLow = 0xff;

// Tungsten E2 at 2009ed2c (Thumb): ldr r1, [r2, #16]; cmp r1, r0; bls head.
constexpr uint32_t kDelayLoopE2[] = {0x6911, 0x4281, 0xd9fc};
// CLIÉ TG50 at 200348fc: ldr r1, [r6, #16]; cmp r1, r0; bhi out; cmp r1, r4; bcs head.
constexpr uint32_t kDelayLoopTg50[] = {0xe5961010, 0xe1510000, 0x8a000001, 0xe1510004, 0x2afffffa};
// Zire 72 at 200a0dec: ldr r0, [r1, #16]; cmp r0, r2; bls head.
constexpr uint32_t kDelayLoopZire72[] = {0xe5910010, 0xe1500002, 0x9afffffc};

constexpr DelayLoop kDelayLoops[] = {
    {DelayLoopKind::e2, true, kDelayLoopE2, 3, 1, 2, 0, kNoLow, kNoLow},
    {DelayLoopKind::tg50, false, kDelayLoopTg50, 5, 1, 6, 0, 4, 2},
    {DelayLoopKind::zire72, false, kDelayLoopZire72, 3, 0, 1, 2, kNoLow, kNoLow},
};

// What a pass that stays in the loop costs, and in *load what its load costs. on: the cost model's
// table for core, otherwise 1 an instruction.
inline uint32_t delayPassCost(const DelayLoop &l, bool on, cost::Core core, uint32_t *load) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < l.count; i++) {
        uint32_t c = 1;
        if (on) {
            if (i == l.failing) c = cost::conditionFailedCost(core);
            else c = l.thumb ? cost::thumbCost(l.code[i], core) : cost::armCost(l.code[i], core);
        }
        if (!i) *load = c;
        sum += c;
    }
    return sum;
}

// Whether a pass that loads v stays in the loop. r: the registers.
inline bool delayStays(const DelayLoop &l, uint32_t v, const uint32_t *r) {
    return v <= r[l.target] && (l.low == kNoLow || v >= r[l.low]);
}

// The flags after a pass that loaded v and stayed: the last compare's (v against the target, or the
// low bound on the TG50), as N, Z, C and V in bits 31 to 28.
inline uint32_t delayFlags(const DelayLoop &l, uint32_t v, const uint32_t *r) {
    const uint32_t b = r[l.low == kNoLow ? l.target : l.low], d = v - b;
    return (d & 0x80000000u) | (d ? 0 : 0x40000000u) | (v >= b ? 0x20000000u : 0) |
           ((((v ^ b) & (v ^ d)) >> 31) << 28);
}

#endif
