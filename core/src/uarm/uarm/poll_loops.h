#ifndef _POLL_LOOPS_H_
#define _POLL_LOOPS_H_

#include <cstdint>

#include "xscale_cost.h"

// The ROM busy-polls of a GPIO level the Fast engine skips (cpuPrvPollSkip in CPU.cpp). Each pass loads a
// word through a base register and tests one bit of it. The level only changes at scheduler events, so
// within a run every pass reads the same value. Some also count passes in a register and leave at a limit.
// A pass changes only the loaded register, the counter and the flags. Matched by these instructions, from
// the head (the load) to the branch back, wherever they sit.

enum class PollLoopKind : uint8_t { e2NandReady, e2NandBusy };

struct PollLoop {
    PollLoopKind kind;
    const uint32_t *code;  // ARM words, head first
    uint32_t count;
    uint8_t value, base;   // registers: loaded, address
    uint32_t mask;         // the bit tested
    bool staysSet;         // a pass stays while the bit is set (else while clear)
    uint8_t counter;       // register counting passes, kNoCounter: none
    uint32_t limit;        // the pass that makes counter reach limit leaves
};

constexpr uint8_t kNoCounter = 0xff;

// Tungsten E2 NAND driver at 201a25e4, wait for R/B (GPIO 79, GPLR2 bit 15) to go high:
// ldr r0, [r1]; tst r0, #0x8000; beq head.
constexpr uint32_t kPollLoopE2NandReady[] = {0xe5910000, 0xe3100c80, 0x0afffffc};
// The same at 201a25ac, wait up to 500 reads for R/B to go low (the chip starting a job), count in r0:
// ldr r1, [r2]; and r1, r1, #0x8000; cmp r1, #0; addne r0, r0, #1; cmpne r0, #500; bcc head.
constexpr uint32_t kPollLoopE2NandBusy[] = {0xe5921000, 0xe2011c80, 0xe3510000,
                                            0x12800001, 0x13500f7d, 0x3afffff9};

constexpr PollLoop kPollLoops[] = {
    {PollLoopKind::e2NandReady, kPollLoopE2NandReady, 3, 0, 1, 0x8000, false, kNoCounter, 0},
    {PollLoopKind::e2NandBusy, kPollLoopE2NandBusy, 6, 1, 2, 0x8000, true, 0, 500},
};

// What a pass that stays in the loop costs, and in *load what its load costs. Every instruction of a
// staying pass runs. on: the cost model's table for core, otherwise 1 an instruction.
inline uint32_t pollPassCost(const PollLoop &l, bool on, cost::Core core, uint32_t *load) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < l.count; i++) {
        const uint32_t c = on ? cost::armCost(l.code[i], core) : 1;
        if (!i) *load = c;
        sum += c;
    }
    return sum;
}

// Passes that stay, from the registers r, when each loads v: 0 if the first leaves, UINT32_MAX for no limit.
inline uint32_t pollStaying(const PollLoop &l, uint32_t v, const uint32_t *r) {
    if (!(v & l.mask) != !l.staysSet) return 0;
    if (l.counter == kNoCounter) return UINT32_MAX;
    // Pass k (from 0) stays while counter + k + 1 < limit.
    return r[l.counter] < l.limit - 1 ? l.limit - 1 - r[l.counter] : 0;
}

// Registers and flags after passes (at least 1) that stayed loading v. flags: N, Z, C and V in bits 31
// to 28, as before the passes.
inline uint32_t pollAfter(const PollLoop &l, uint32_t v, uint32_t passes, uint32_t *r, uint32_t flags) {
    switch (l.kind) {
        case PollLoopKind::e2NandReady:
            // tst with a rotated immediate: Z from the clear bit, C from the immediate's bit 31 (0), V kept.
            r[l.value] = v;
            return 0x40000000u | (flags & 0x10000000u);
        case PollLoopKind::e2NandBusy: {
            // The last compare: cmpne counter, #limit.
            r[l.value] = v & l.mask;
            r[l.counter] += passes;
            const uint32_t a = r[l.counter], b = l.limit, d = a - b;
            return (d & 0x80000000u) | (d ? 0 : 0x40000000u) | (a >= b ? 0x20000000u : 0) |
                   ((((a ^ b) & (a ^ d)) >> 31) << 28);
        }
    }
    return flags;
}

#endif
