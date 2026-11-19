// Code hooks on their own (imx1MachineSetCodeHooks), in Exact, lean Fast and Fast with Dynarmic blocks: hooks set on
// a Thumb function and an ARM loop head after both ran and were compiled stop every call and pass there, each
// hooked instruction runs once after a leave, and once the hooks are cleared the code runs lean and compiled again
// and ends as a run without hooks does.
#define ZODIAC_FAST_LEAN_MAIN fastLeanMain
#include "zodiac-fast-lean.cpp"

namespace {
// The pc of a word in the guest image, as it runs from the linked alias.
uint32_t imagePc(const std::vector<uint32_t>& words, uint32_t word, bool thumbHigh = false) {
    for (size_t i = 0; i < words.size(); ++i)
        if (thumbHigh ? (words[i] >> 16) == word : words[i] == word) return 0x20000000 + uint32_t(i) * 4 + (thumbHigh ? 2 : 0);
    assert(false);
    return 0;
}

struct HookRun {
    Outcome outcome;
    unsigned thumbHits = 0, armHits = 0;
    uint64_t jitBeforeClear = 0, leanBeforeClear = 0;
    bool cleared = false;
};

// The tlb guest: a Thumb loop calling count (adds r6, #1; bx lr) 200 times, then ARM loops counting r7 down
// from 200. Hooks go on count and the first ARM loop's head once the run is well into the Thumb loop, and come off
// once the ARM loop has passed its head 50 times.
HookRun hooked(const std::vector<uint32_t>& words, bool fast, uint32_t thumbPc, uint32_t armPc, uint64_t setAfter) {
    Imx1Machine* m = make(words, fast);
    HookRun out;
    const uint32_t hooks[] = {thumbPc | 1, armPc};
    bool set = false;
    uint32_t lastR6 = 0, lastR7 = 0;
    for (unsigned n = 0; n < 200000 && running(m); ++n) {
        if (!set && imx1MachineGetStatus(m).retired >= setAfter) {
            assert(imx1MachineSetCodeHooks(m, hooks, 2));
            set = true;
        }
        const auto run = imx1MachineRunBounded(m, {997, never, 100000});
        assert(run.reason != Imx1MachineYield::invalidBounds && run.reason != Imx1MachineYield::pcStop);
        if (run.reason != Imx1MachineYield::codeHook) continue;
        assert(set && !out.cleared && imx1MachineAtCodeHook(m));
        uint32_t r[16], cpsr = 0;
        assert(imx1MachineRegisters(m, r, &cpsr));
        if ((r[15] & ~1u) == thumbPc) {
            // Thumb state, and every call caught: r6 counts the calls made before this one.
            assert((r[15] & 1) && (cpsr & 0x20));
            assert(!out.thumbHits || r[6] == lastR6 + 1);
            lastR6 = r[6];
            ++out.thumbHits;
        } else {
            assert(r[15] == armPc && !(cpsr & 0x20));
            assert(!out.armHits || r[7] == lastR7 - 1);
            lastR7 = r[7];
            ++out.armHits;
        }
        imx1MachineLeaveCodeHook(m);
        if (out.armHits == 50) {
            out.jitBeforeClear = imx1MachineJitRetired(m);
            out.leanBeforeClear = imx1MachineLeanRetired(m);
            assert(imx1MachineSetCodeHooks(m, nullptr, 0));
            out.cleared = true;
        }
    }
    out.outcome = finish(m);
    return out;
}

void codeHooks() {
    uint32_t stopPc = 0, savePc = 0;
    const auto words = guest(Tail::tlb, stopPc, savePc);
    const uint32_t countPc = imagePc(words, 0x3601, true);  // count: adds r6, #1, then bx lr
    const uint32_t armPc = imagePc(words, 0xe5925000);      // the first ARM loop's ldr r5, [r2]
    assert((words[(countPc + 2 - 0x20000000) / 4] & 0xffff) == 0x4770);
    for (int engine = 0; engine < 3; ++engine) {
        const bool fast = engine > 0;
        imx1SetFastBlocksDefault(engine == 2);
        const auto plain = runGuest(words, fast);
        // Well into the Thumb loop (7 instructions a round), with its code compiled in Fast.
        const auto run = hooked(words, fast, countPc, armPc, plain.cheap.retired - 4400);
        const char* name = engine == 0 ? "Exact" : engine == 1 ? "lean Fast" : "Fast with blocks";
        std::printf("code hooks, %s: %u Thumb and %u ARM hits, %llu instructions lean and %llu in the JIT before the "
                    "clear, %llu and %llu at the end\n", name, run.thumbHits, run.armHits,
                    (unsigned long long)run.leanBeforeClear, (unsigned long long)run.jitBeforeClear,
                    (unsigned long long)run.outcome.lean, (unsigned long long)run.outcome.jit);
        assert(run.cleared && run.thumbHits >= 50 && run.thumbHits < 200 && run.armHits == 50);
        same(plain, run.outcome);
        assert(run.outcome.memory == plain.memory);
        if (engine == 1) assert(run.leanBeforeClear > 0 && run.outcome.lean > run.leanBeforeClear + 1000);
        if (engine == 2) assert(run.jitBeforeClear > 200 && run.outcome.jit > run.jitBeforeClear + 1000);
    }
    imx1SetFastBlocksDefault(true);
}
}  // namespace

int main() {
    unsetenv("FROND_JIT");
    cpuSetJitSync(true);
    setenv("FROND_JIT_HOT", "1", 1);
    codeHooks();
    std::puts("PASS: code hooks stop every pass in ARM and Thumb with code compiled before them, and code runs lean "
              "and compiled again once they're cleared");
    return 0;
}
