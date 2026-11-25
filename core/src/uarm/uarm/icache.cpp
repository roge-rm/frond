//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "icache_internal.h"
#include "MMU_internal.h"

#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "CPU.h"
#include "arm920_state.h"
#include "cputil.h"
#include "xscale_cost.h"

void icacheConfigureArm920(struct icache* ic) {
    ic->arm920 = true;
    ic->arm920Enabled = false;
    ic->arm920RoundRobin = false;
    ic->arm920Random = 1;
    ic->arm920ContinuationCount = 0;
    std::memset(ic->arm920Victim, 0, sizeof(ic->arm920Victim));
}

// Drops every line of the decode cache, which only the ARM920T Fast engine fills for this CPU.
static void icachePrvArm920DropDecoded(struct icache* ic) {
    if (++ic->revision == 0) {
        ic->revision = 1;
        for (auto& line : ic->cache) line.revision = 0;
    }
}

void icacheArm920DropDecoded(struct icache* ic) { icachePrvArm920DropDecoded(ic); }

void icacheArm920SetFcsePid(struct icache* ic, const uint32_t* pid) { ic->fcsePid = pid; }

void icacheArm920SetLeanWord(struct icache* ic, bool (*word)(void*, uint32_t, unsigned, bool, uint32_t*), void* ctx) {
    ic->arm920LeanWord = word;
    ic->arm920LeanWordCtx = ctx;
}

void icacheArm920Control(struct icache* ic, bool enabled, bool roundRobin) {
    if (ic->arm920Enabled != enabled) {
        // Fast fills decode lines only while the I-cache is on; with it off, fetches read memory each time.
        icachePrvArm920DropDecoded(ic);
        ic->epoch++;
#ifdef FROND_DYNARMIC
        if (ic->invalHook) ic->invalHook(ic->invalHookCtx, 0, 0, false);
#endif
    }
    ic->arm920Enabled = enabled;
    ic->arm920RoundRobin = roundRobin;
}

// *from is the line the word was read from, or null when it came from elsewhere (prefetch with no line,
// continuation, lean word, uncached read).
static FORCE_INLINE bool icachePrvArm920Fetch(struct icache* ic, uint32_t va, unsigned size, bool privileged,
                                              uint_fast8_t* fsr, uint32_t* word, icache::Arm920Line** from) {
    *from = nullptr;
    if ((size != 2 && size != 4) || (va & (size - 1))) {
        *fsr = 3;
        return false;
    }
    if (ic->arm920Prefetched) {
        ic->arm920Prefetched = false;
        if (va == ic->arm920PrefetchVa && size == ic->arm920PrefetchSize && privileged == ic->arm920PrefetchPrivileged) {
            *word = ic->arm920PrefetchWord;
            *from = ic->arm920PrefetchLine;
            return true;
        }
    }
    const auto extract = [=](uint32_t value) {
        return size == 2 ? (value >> ((va & 2) * 8)) & 0xffff : value;
    };
    if (ic->arm920ContinuationCount && (va & ~3u) == ic->arm920ContinuationPc) {
        if (ic->arm920ContinuationFault[0]) {
            *fsr = ic->arm920ContinuationFault[0];
            return false;
        }
        *word = extract(ic->arm920Continuation[0]);
        return true;
    }
    // FCSE: the continuation above holds the VAs that followed an MMU switch; everything below is by MVA.
    if (va < 0x02000000u && ic->fcsePid) va |= *ic->fcsePid;
    if (ic->arm920LeanWord && ic->arm920LeanWord(ic->arm920LeanWordCtx, va, size, privileged, word)) return true;
    ArmMmu* const mmu = ic->memorySystem.mmu;
    auto& memo = ic->arm920Translation;
    uint32_t pa;
    bool cacheable;
    if (memo.valid && memo.page == va >> 12 && memo.privileged == privileged && memo.tlbWrites == mmu->tlbWrites &&
        memo.permGen == ic->permGen && memo.ttp == mmu->transTablPA) {
        pa = (memo.paPage << 12) | (va & 0xfff);
        cacheable = memo.cacheable;
    } else {
        const auto r = mmuTranslate(mmu, va, privileged, false);
        if (!MMU_TRANSLATE_RESULT_OK(r)) {
            *fsr = MMU_TRANSLATE_RESULT_FSR(r);
            return false;
        }
        pa = MMU_TRANSLATE_RESULT_PA(r);
        cacheable = !mmuIsOn(mmu) || MMU_TRANSLATE_RESULT_CACHEABLE(r);
        memo.valid = mmu->transTablPA != MMU_DISABLED_TTP && mmu->tlb[va >> 12].gen == mmu->gen;
        if (memo.valid) memo = {va >> 12, pa >> 12, mmu->tlbWrites, ic->permGen, mmu->transTablPA, privileged, cacheable, true};
    }
    const bool cached = ic->arm920Enabled && !ic->arm920LeanWord && cacheable;
    if (!cached) {
        if (size == 2) {
            uint16_t halfword;
            if (memInstructionFetch<2>(ic->mem, pa, &halfword)) {
                *word = halfword;
                return true;
            }
        } else if (memInstructionFetch<4>(ic->mem, pa, word)) return true;
        *fsr = 8;  // External abort on an instruction access.
        return false;
    }
    const unsigned segment = (va >> 5) & 7;
    auto& recent = ic->arm920Lines[segment][ic->arm920Recent[segment] & 63];
    if (recent.valid && recent.base == maskLine(va)) {
        *word = extract(recent.words[(va & 31) / 4]);
        *from = &recent;
        return true;
    }
    uint8_t& hint = ic->arm920Hint[(va >> 5) & 2047];
    auto& hinted = ic->arm920Lines[segment][hint & 63];
    if (hinted.valid && hinted.base == maskLine(va)) {
        ic->arm920Recent[segment] = uint8_t(hint & 63);
        *word = extract(hinted.words[(va & 31) / 4]);
        *from = &hinted;
        return true;
    }
    for (unsigned index = 0; index < 64; ++index) {
        auto& line = ic->arm920Lines[segment][index];
        if (line.valid && line.base == maskLine(va)) {
            ic->arm920Recent[segment] = uint8_t(index);
            hint = uint8_t(index);
            *word = extract(line.words[(va & 31) / 4]);
            *from = &line;
            return true;
        }
    }
    uint32_t words[8];
    for (unsigned i = 0; i < 8; ++i) {
        if (!memInstructionFetch<4>(ic->mem, maskLine(pa) + i * 4, words + i)) {
            *fsr = 8;
            return false;
        }
    }
    unsigned victim;
    if (ic->arm920RoundRobin) victim = ic->arm920Victim[segment]++ & 63;
    else {
        // A repeatable pseudo-random victim, without a claim about hardware cycle timing.
        ic->arm920Random = ic->arm920Random * 1664525u + 1013904223u;
        victim = (ic->arm920Random >> 24) & 63;
    }
    auto& line = ic->arm920Lines[segment][victim];
#ifdef FROND_DYNARMIC
    if (line.valid && ic->invalHook) ic->invalHook(ic->invalHookCtx, line.base, 32, true);
#endif
    line.base = maskLine(va);
    std::memcpy(line.words, words, sizeof(words));
    line.valid = true;
    line.armDecoded = 0;
    line.thumbDecoded = 0;
    ic->arm920Recent[segment] = uint8_t(victim);
    hint = uint8_t(victim);
    ic->epoch++;
    *word = extract(words[(va & 31) / 4]);
    *from = &line;
    return true;
}

bool icacheArm920Fetch(struct icache* ic, uint32_t va, unsigned size, bool privileged,
                       uint_fast8_t* fsr, uint32_t* word) {
    icache::Arm920Line* from;
    return icachePrvArm920Fetch(ic, va, size, privileged, fsr, word, &from);
}

bool icacheArm920Prefetch(struct icache* ic, uint32_t va, unsigned size, bool privileged,
                          uint_fast8_t* fsr, uint32_t* word) {
    ic->arm920Prefetched = false;
    if (!icachePrvArm920Fetch(ic, va, size, privileged, fsr, word, &ic->arm920PrefetchLine)) return false;
    ic->arm920Prefetched = true;
    ic->arm920PrefetchVa = va;
    ic->arm920PrefetchSize = uint8_t(size);
    ic->arm920PrefetchPrivileged = privileged;
    ic->arm920PrefetchWord = *word;
    return true;
}

void icacheArm920DropPrefetch(struct icache* ic) { ic->arm920Prefetched = false; }

bool icacheArm920Word(struct icache* ic, uint32_t va, bool privileged,
                      uint_fast8_t* fsr, uint32_t* word) {
    return icacheArm920Fetch(ic, va, 4, privileged, fsr, word);
}

void icacheArm920PrepareMmuSwitch(struct icache* ic, uint32_t nextPc, bool privileged) {
    uint32_t words[2]{};
    uint8_t faults[2]{};
    uint_fast8_t fsr = 0;
    unsigned count = 0;
    while (count < 2) {
        const bool ok = icacheArm920Word(ic, nextPc + count * 4, privileged, &fsr, words + count);
        faults[count] = ok ? 0 : fsr;
        ++count;
        if (!ok) break;
    }
    ic->arm920ContinuationPc = nextPc;
    std::memcpy(ic->arm920Continuation, words, count * sizeof(uint32_t));
    std::memcpy(ic->arm920ContinuationFault, faults, count);
    ic->arm920ContinuationCount = count;
}

bool icacheArm920ContinuationPending(const struct icache* ic) {
    return ic->arm920 && ic->arm920ContinuationCount;
}

void icacheArm920Branch(struct icache* ic) { icacheArm920BranchInline(ic); }

void icacheArm920Capture(const struct icache* ic, Arm920SavedState& s) {
    s.icacheEnabled = ic->arm920Enabled;
    s.icacheRoundRobin = ic->arm920RoundRobin;
    s.continuationPc = ic->arm920ContinuationPc;
    std::memcpy(s.continuation, ic->arm920Continuation, sizeof(s.continuation));
    std::memcpy(s.continuationFault, ic->arm920ContinuationFault, sizeof(s.continuationFault));
    s.continuationCount = ic->arm920ContinuationCount;
    s.icacheRandom = ic->arm920Random;
    std::memcpy(s.icacheVictim, ic->arm920Victim, sizeof(s.icacheVictim));
    for (unsigned segment = 0; segment < 8; ++segment)
        for (unsigned index = 0; index < 64; ++index) {
            const auto& from = ic->arm920Lines[segment][index];
            auto& to = s.instructionLines[segment][index];
            to.base = from.base;
            std::memcpy(to.words, from.words, sizeof(to.words));
            to.valid = from.valid;
        }
}

void icacheArm920Apply(struct icache* ic, const Arm920SavedState& s) {
    ic->arm920Enabled = s.icacheEnabled;
    ic->arm920RoundRobin = s.icacheRoundRobin;
    ic->arm920ContinuationPc = s.continuationPc;
    std::memcpy(ic->arm920Continuation, s.continuation, sizeof(s.continuation));
    std::memcpy(ic->arm920ContinuationFault, s.continuationFault, sizeof(s.continuationFault));
    ic->arm920ContinuationCount = s.continuationCount;
    ic->arm920Random = s.icacheRandom;
    std::memcpy(ic->arm920Victim, s.icacheVictim, sizeof(s.icacheVictim));
    for (unsigned segment = 0; segment < 8; ++segment)
        for (unsigned index = 0; index < 64; ++index) {
            const auto& from = s.instructionLines[segment][index];
            auto& to = ic->arm920Lines[segment][index];
            to.base = from.base;
            std::memcpy(to.words, from.words, sizeof(to.words));
            to.valid = from.valid;
            to.armDecoded = 0;
            to.thumbDecoded = 0;
        }
    ic->arm920Prefetched = false;
    ic->epoch++;
}

void icacheSetXscale(struct icache* ic, bool xscale) { ic->xscale = xscale; }

void icacheGetFetchState(struct icache* ic, uint32_t* state) {
    *state++ = ic->nextFetchBuffer;
    for (const auto& buffer : ic->fetchBuffers) {
        *state++ = buffer.base;
        std::memcpy(state, buffer.words, sizeof(buffer.words));
        state += 8;
    }
}

bool icacheSetFetchState(struct icache* ic, const uint32_t* state) {
    if (state[0] > 1) return false;
    for (unsigned i : {1u, 10u})
        if (state[i] != 0xffffffff && (state[i] & 31)) return false;
    ic->nextFetchBuffer = *state++;
    for (auto& buffer : ic->fetchBuffers) {
        buffer.base = *state++;
        std::memcpy(buffer.words, state, sizeof(buffer.words));
        buffer.fetchKey = 0;
#ifdef FROND_DYNARMIC
        buffer.pa = ~0u;
        buffer.host = nullptr;
#endif
        state += 8;
    }
#ifdef FROND_DYNARMIC
    ic->fetchUnwatched = ic->fetchWatch != nullptr;
#endif
    return true;
}

// pa is the physical line of an uncacheable fill, ~0u for a cacheable one.
static void icacheRememberFetch(struct icache* ic, uint32_t va, uint32_t pa, uint32_t key, const void* data) {
    if (!ic->xscale) return;
    auto& buffer = ic->fetchBuffers[ic->nextFetchBuffer];
    ic->nextFetchBuffer ^= 1;
    buffer.base = maskLine(va);
    buffer.fetchKey = key;
    std::memcpy(buffer.words, data, sizeof(buffer.words));
#ifdef FROND_DYNARMIC
    // Cacheable fills aren't watched: the I-cache line holds the same words anyway.
    buffer.pa = pa;
    buffer.host = ic->fetchWatch && pa != ~0u ? ic->fetchWatch(ic->fetchWatchCtx, pa) : nullptr;
#else
    (void)pa;
#endif
}

// The charge slot (icacheCostSlot) of an instruction: word is the ARM word or the Thumb halfword.
template <int sz>
static uint16_t icachePrvCost(struct icache* ic, uint32_t word) {
    if (!ic->costOn) return ICACHE_COST_ONE;
    const cost::Core core = cost::Core(ic->costCore);
    if constexpr (sz == 2) {
        const uint32_t half = word & 0xffff;
        // Only the conditional B has a condition (14 and 15 are UDF and SWI).
        const uint32_t cond = (half & 0xf000) == 0xd000 && ((half >> 8) & 15) < 14 ? (half >> 8) & 15 : 14;
        return icacheCostSlot(cost::thumbCostTable(core)[half], cond);
    } else {
        // Frond's own words (PACE entry and dispatch, peepholes) cost 1 here: a peephole charges the routine
        // it replaces itself.
        if ((word & 0xffff0000u) == 0xfff90000u) return ICACHE_COST_ONE;
        auto& m = ic->armCostMemo[(word ^ (word >> 10) ^ (word >> 20)) & 1023];
        if (m.word != word) {
            m.word = word;
            m.cost = uint8_t(cost::armCost(word, core));
        }
        const uint32_t cond = word >> 28;
        return icacheCostSlot(m.cost, cond >= 14 ? 14 : cond);
    }
}

uint16_t icacheCostOf(struct icache* ic, uint32_t word, bool thumb) {
    return thumb ? icachePrvCost<2>(ic, word) : icachePrvCost<4>(ic, word);
}

void icacheSetCost(struct icache* ic, bool on, uint8_t core) {
    ic->costOn = on;
    ic->costCore = core;
    for (auto& m : ic->armCostMemo) {
        m.word = 0;
        m.cost = uint8_t(on ? cost::armCost(0, cost::Core(core)) : 1);
    }
    ic->lastCost = ICACHE_COST_ONE;
    for (auto& m : ic->arm920DecodedArm) m.valid = false;
    for (auto& m : ic->arm920DecodedThumb) m.valid = false;
    for (auto& segment : ic->arm920Lines)
        for (auto& line : segment) line.armDecoded = line.thumbDecoded = 0;
    icacheInval(ic);  // lines hold slots charged under the old setting
}

template <int msys, int sz>
static void icacheDecodeFetch(struct icache* ic, uint32_t word, uint32_t va, uint32_t& instr, uint32_t& decoded) {
    if constexpr (sz == 4) {
        instr = word;
        decoded = cpuDecodeArm<msys>(instr);
        ic->lastCost = icachePrvCost<4>(ic, word);
    } else {
        const uint32_t half = (word >> ((va & 2) * 8)) & 0xffff;
        decoded = cpuDecodeThumb<msys>(half, instr);
        ic->lastCost = icachePrvCost<2>(ic, half);
    }
}

#ifdef FROND_DYNARMIC
bool icacheBufferedWord(const struct icache* ic, uint32_t va, uint32_t* word) {
    if (!ic->xscale) return false;
    for (const auto& buffer : ic->fetchBuffers)
        if (buffer.base == maskLine(va)) {
            *word = buffer.words[(va & 31) / 4];
            return true;
        }
    return false;
}

void icacheSetInvalHook(struct icache* ic, void (*hook)(void*, uint32_t, uint32_t, bool), void* ctx) {
    ic->invalHook = hook;
    ic->invalHookCtx = ctx;
}

void icacheSetFetchWatch(struct icache* ic, const uint8_t* (*watch)(void*, uint32_t), void* ctx) {
    ic->fetchWatch = watch;
    ic->fetchWatchCtx = ctx;
    ic->fetchUnwatched = watch != nullptr;
}

void icacheDropFetchedHost(struct icache* ic, const uint8_t* host, uint32_t size) {
    for (auto& buffer : ic->fetchBuffers)
        if (buffer.base != 0xffffffff && buffer.host && buffer.host < host + size &&
            host < buffer.host + sizeof(buffer.words)) {
            buffer.base = 0xffffffff;
            std::memset(buffer.words, 0, sizeof(buffer.words));
            buffer.fetchKey = 0;
            buffer.host = nullptr;
        }
}

void icacheWatchFetchBuffers(struct icache* ic) {
    ic->fetchUnwatched = false;
    if (!ic->fetchWatch || !ic->xscale) return;
    for (auto& buffer : ic->fetchBuffers) {
        if (buffer.base == 0xffffffff) continue;
        if (buffer.pa == ~0u) {
            // Loaded from a save: whether the line was uncacheable isn't known, so watch it
            // wherever the current mapping puts it.
            const MMUTranslateResult r = mmuTranslateInspect(ic->memorySystem.mmu, buffer.base, false);
            if (!MMU_TRANSLATE_RESULT_OK(r)) continue;
            buffer.pa = maskLine(MMU_TRANSLATE_RESULT_PA(r));
            buffer.host = ic->fetchWatch(ic->fetchWatchCtx, buffer.pa);
        } else if (buffer.host) {
            ic->fetchWatch(ic->fetchWatchCtx, buffer.pa);
        }
    }
}

bool icacheFetchUnwatched(const struct icache* ic) { return ic->fetchUnwatched; }
#endif

static void icacheInvalWhole(struct icache* ic, bool guest) {
    ic->arm920Prefetched = false;  // a word runStep saved is gone with the cache
#ifdef FROND_DYNARMIC
    if (ic->invalHook) ic->invalHook(ic->invalHookCtx, 0, 0, guest);
#else
    (void)guest;
#endif
    for (auto& segment : ic->arm920Lines)
        for (auto& line : segment) line.valid = false;
    ic->nextFetchBuffer = 0;
    for (auto& buffer : ic->fetchBuffers) {
        buffer.base = 0xffffffff;
        std::memset(buffer.words, 0, sizeof(buffer.words));
        buffer.fetchKey = 0;
    }
    ic->revision++;
    ic->epoch++;

    if (ic->revision == 0) {
        ic->revision = 1;
        for (size_t i = 0; i < (1 << CACHE_INDEX_BITS); i++) ic->cache[i].revision = 0;
    }
}

void icacheInval(struct icache* ic) { icacheInvalWhole(ic, false); }

void icacheInvalGuest(struct icache* ic) { icacheInvalWhole(ic, true); }

static icache* icacheInitCommon(struct ArmMem* mem) {
    struct icache* ic = (struct icache*)malloc(sizeof(*ic));

    if (!ic) ERR("cannot alloc icache");

    memset(ic, 0, sizeof(*ic));

    ic->mem = mem;
    ic->permGen = 1;

    icacheInval(ic);

    return ic;
}

static void icachePrvPermChanged(void* ctx) {
    struct icache* ic = (struct icache*)ctx;
    if (++ic->permGen >= 0x7fffffff) {
        // Keys would come round again: forget every line's.
        ic->permGen = 1;
        for (auto& line : ic->cache) line.fetchKey = 0;
        for (auto& buffer : ic->fetchBuffers) buffer.fetchKey = 0;
    }
    ic->epoch++;
}

struct icache* icacheInit(struct ArmMem* mem, struct ArmMmu* mmu) {
    struct icache* ic = icacheInitCommon(mem);
    ic->memorySystem.mmu = mmu;
    mmuSetPermHook(mmu, icachePrvPermChanged, ic);

    return ic;
}

struct icache* icacheInit(struct ArmMem* mem, struct ArmMpu* mpu) {
    struct icache* ic = icacheInitCommon(mem);
    ic->memorySystem.mpu = mpu;

    return ic;
}

void icacheInvalAddr(struct icache* ic, uint32_t va) {
#ifdef FROND_DYNARMIC
    if (ic->invalHook) ic->invalHook(ic->invalHookCtx, maskLine(va), 1u << CACHE_LINE_WIDTH_BITS, true);
#endif
    for (auto& buffer : ic->fetchBuffers)
        if (buffer.base == maskLine(va)) {
            buffer.base = 0xffffffff;
            std::memset(buffer.words, 0, sizeof(buffer.words));
#ifdef FROND_DYNARMIC
            buffer.host = nullptr;
#endif
        }
    if (ic->arm920) {
        for (auto& line : ic->arm920Lines[(va >> 5) & 7])
            if (line.valid && line.base == maskLine(va)) line.valid = false;
        // The Fast engine's decoded copy of the line.
        struct icacheline* decoded = ic->cache + calculateIndex(va);
        if (decoded->revision == ic->revision && decoded->tag == calculateTag(va)) decoded->revision = ic->revision - 1;
        ic->epoch++;
        return;
    }
    struct icacheline* line = ic->cache + calculateIndex(va);

    if (line->revision != ic->revision || line->tag != calculateTag(va)) return;

    line->revision = ic->revision - 1;
    ic->epoch++;
}

void icacheInvalRange(struct icache* ic, uint32_t addr, uint32_t size) {
    for (uint32_t line = maskLine(addr); line < addr + size; line += (1 << CACHE_LINE_WIDTH_BITS)) {
        icacheInvalAddr(ic, line);
    }
}

// Checks a fetch from a line or buffer that was fetched under another key: the
// translation and permissions for this mode, as a refill would.
template <int msys>
static bool icachePrvCheckFetch(struct icache* ic, uint32_t va, bool privileged, uint_fast8_t* fsrP) {
    if constexpr (armMemorySystemOf(msys) == ARM_MEMORY_SYSTEM_MMU) {
        const MMUTranslateResult r = mmuTranslate(ic->memorySystem.mmu, va, privileged, false);
        if (!MMU_TRANSLATE_RESULT_OK(r)) {
            *fsrP = MMU_TRANSLATE_RESULT_FSR(r);
            return false;
        }
    } else {
        if (!MPU_TEST_RESULT_OK(mpuTestAddress(ic->memorySystem.mpu, va, true, false))) {
            *fsrP = 1;
            return false;
        }
    }
    return true;
}

template <int msys, int sz, int tier>
bool icacheFetch(struct icache* ic, uint32_t va, bool privileged, uint_fast8_t* fsrP,
                 uint32_t& instr, uint32_t& decoded) {
    if (va & (sz - 1)) {  // alignment issue

        *fsrP = 3;
        return false;
    }

    // msys is the interpreter kind: only the ARM920T's has an ARM920T I-cache, and only the others have
    // the XScale fetch buffers.
    if constexpr (armKindIsArm920(msys)) {
        uint32_t word;
        if (ic->arm920ContinuationCount && (va & ~3u) != ic->arm920ContinuationPc)
            icacheArm920Branch(ic);
        icache::Arm920Line* from;
        if (!icachePrvArm920Fetch(ic, va, sz, privileged, fsrP, &word, &from)) return false;
        if (ic->arm920ContinuationCount) {
            --ic->arm920ContinuationCount;
            if (ic->arm920ContinuationCount) {
                ic->arm920Continuation[0] = ic->arm920Continuation[1];
                ic->arm920ContinuationFault[0] = ic->arm920ContinuationFault[1];
            }
            ic->arm920ContinuationPc += 4;
        }
        if (from) {
            // The line's words are what the fetch read, so its decode is this word's.
            if constexpr (sz == 2) {
                const unsigned slot = (va & 31) / 2;
                if (!(from->thumbDecoded & (1u << slot))) {
                    from->thumbExec[slot] = cpuDecodeThumb<msys>(word, from->thumbInstr[slot]);
                    from->thumbCost[slot] = icachePrvCost<2>(ic, word);
                    from->thumbDecoded |= uint16_t(1u << slot);
                }
                instr = from->thumbInstr[slot];
                decoded = from->thumbExec[slot];
                ic->lastCost = from->thumbCost[slot];
            } else {
                const unsigned slot = (va & 31) / 4;
                if (!(from->armDecoded & (1u << slot))) {
                    from->armExec[slot] = cpuDecodeArm<msys>(word);
                    from->armCost[slot] = icachePrvCost<4>(ic, word);
                    from->armDecoded |= uint8_t(1u << slot);
                }
                instr = word;
                decoded = from->armExec[slot];
                ic->lastCost = from->armCost[slot];
            }
            return true;
        }
        if constexpr (sz == 2) {
            auto& m = ic->arm920DecodedThumb[(word ^ (word >> 11)) & 2047];
            if (!m.valid || m.word != word) {
                m.decoded = cpuDecodeThumb<msys>(word, m.instr);
                m.cost = icachePrvCost<2>(ic, word);
                m.word = word;
                m.valid = true;
            }
            instr = m.instr;
            decoded = m.decoded;
            ic->lastCost = m.cost;
        } else {
            auto& m = ic->arm920DecodedArm[(word ^ (word >> 12) ^ (word >> 24)) & 4095];
            if (!m.valid || m.word != word) {
                m.decoded = cpuDecodeArm<msys>(word);
                m.cost = icachePrvCost<4>(ic, word);
                m.word = word;
                m.valid = true;
            }
            instr = word;
            decoded = m.decoded;
            ic->lastCost = m.cost;
        }
        return true;
    }
    const uint32_t key = icacheFetchKey(ic, privileged);

    // A buffer hit precedes translation. Enabling the MMU does not discard
    // instructions already fetched from the physical continuation address, so with
    // another key only domain and permission faults are taken, not translation faults.
    if (ic->xscale) {
        for (auto& buffer : ic->fetchBuffers)
            if (buffer.base == maskLine(va)) {
                if (buffer.fetchKey != key) {
                    uint_fast8_t fsr = 0;
                    if (!icachePrvCheckFetch<msys>(ic, va, privileged, &fsr) &&
                        armMemorySystemOf(msys) == ARM_MEMORY_SYSTEM_MMU && (fsr & 0x09) == 0x09) {
                        *fsrP = fsr;
                        return false;
                    }
                    buffer.fetchKey = key;
                }
                icacheDecodeFetch<msys, sz>(ic, buffer.words[(va & 31) / 4], va, instr, decoded);
                return true;
            }
    }

    struct icacheline* line = ic->cache + calculateIndex(va);
    const CACHE_TAG_TYPE tag = calculateTag(va);

    if (line->revision == ic->revision && line->tag == tag && line->fetchKey != key) {
        if (!icachePrvCheckFetch<msys>(ic, va, privileged, fsrP)) return false;
        line->fetchKey = key;
    }

    if (line->revision != ic->revision || line->tag != tag) {
        uint8_t data[sizeof(line->data)];
        bool cacheable;
        uint32_t pa = va;

        if constexpr (armMemorySystemOf(msys) == ARM_MEMORY_SYSTEM_MMU) {
            cacheable = mmuIsOn(ic->memorySystem.mmu);

            if (cacheable) {
                MMUTranslateResult translateResult =
                    mmuTranslate(ic->memorySystem.mmu, va, privileged, false);
                if (!MMU_TRANSLATE_RESULT_OK(translateResult)) {
                    *fsrP = MMU_TRANSLATE_RESULT_FSR(translateResult);
                    return false;
                }

                pa = MMU_TRANSLATE_RESULT_PA(translateResult);
                cacheable = MMU_TRANSLATE_RESULT_CACHEABLE(translateResult);
            }
        } else {
            MPUTestResult mpuTestResult = mpuTestAddress(ic->memorySystem.mpu, pa, true, false);

            if (!MPU_TEST_RESULT_OK(mpuTestResult)) {
                *fsrP = 1;
                return false;
            }

            cacheable = MPU_TEST_RESULT_CACHEABLE(mpuTestResult);
        }

        if (!cacheable && ic->xscale) {
            uint32_t words[8];
            if (!memInstructionFetch<sizeof(words)>(ic->mem, maskLine(pa), words)) {
                *fsrP = 0x0d;
                return false;
            }
            icacheRememberFetch(ic, va, maskLine(pa), key, words);
            icacheDecodeFetch<msys, sz>(ic, words[(va & 31) / 4], va, instr, decoded);
            return true;
        }

        if (!cacheable) {
            bool ok = memInstructionFetch<sz>(ic->mem, pa, &instr);

            if (!ok) {
                *fsrP = 0x0d;  // perm error
                return false;
            }

            if (sz == 4) {
                decoded = cpuDecodeArm<msys>(instr);
                ic->lastCost = icachePrvCost<4>(ic, instr);
            } else {
                uint32_t translatedInstr;

                decoded = cpuDecodeThumb<msys>(instr, translatedInstr);
                ic->lastCost = icachePrvCost<2>(ic, instr);
                instr = translatedInstr;
            }

            return true;
        }

        bool ok = memInstructionFetch<sizeof(data)>(
            ic->mem, pa & ((0xffffffff << CACHE_LINE_WIDTH_BITS)), data);
        if (!ok) {
            if (fsrP) *fsrP = 0x0d;  // perm error
            return false;
        };

        icacheRememberFetch(ic, va, ~0u, key, data);
        for (size_t i = 0; i < sizeof(data); i += 8) {
            const uint64_t d = *(uint64_t*)(data + i);
            if ((uint32_t)d != *(uint32_t*)(line->data + i))
                *((uint64_t*)(line->decoded + (i >> 1))) = 0;

            if ((d >> 32) != *(uint32_t*)(line->data + i + 4))
                *((uint64_t*)(line->decoded + (i >> 1) + 2)) = 0;

            *(uint64_t*)(line->data + i) = d;
        }

        line->revision = ic->revision;
        line->tag = tag;
        line->fetchKey = key;
        ic->epoch++;
    }

    switch (sz) {
        case 4: {
            const size_t i = calculateLineIndex(va);
            instr = *(uint32_t*)(line->data + i);

            const size_t iInst = i >> 1;
            if ((line->decoded[iInst] & DECODED_BITS) != DECODED_BITS_ARM) {
                // fprintf(stderr, "decode cache miss ARM\n");
                decoded = cpuDecodeArm<msys>(instr);
                line->decoded[iInst] = (decoded << DECODED_BITS_SHIFT) | DECODED_BITS_ARM;
                line->cost[iInst] = icachePrvCost<4>(ic, instr);
            } else {
                decoded = ((int32_t)line->decoded[iInst]) >> DECODED_BITS_SHIFT;
            }

            break;
        }

        case 2: {
            const size_t i = calculateLineIndex(va);

            const size_t iInst = i >> 1;
            if ((line->decoded[iInst] & DECODED_BITS) != DECODED_BITS_THUMB) {
                // fprintf(stderr, "decode cache miss thumb\n");
                const uint16_t instrThumb = *(uint16_t*)(line->data + i);

                decoded = cpuDecodeThumb<msys>(instrThumb, instr);
                line->decoded[iInst] = (decoded << DECODED_BITS_SHIFT) | DECODED_BITS_THUMB;
                line->translatedThumbInstructions[iInst] = instr;
                line->cost[iInst] = icachePrvCost<2>(ic, instrThumb);
            } else {
                decoded = ((int32_t)line->decoded[iInst]) >> DECODED_BITS_SHIFT;
                instr = line->translatedThumbInstructions[iInst];
            }

            break;
        }

        default:
            __builtin_unreachable();
    }
    ic->lastCost = line->cost[calculateLineIndex(va) >> 1];

    return true;
}

#define DEFINE_FETCH_MSYS_SZ_TIER(msys, sz, tier)                                                  \
    template bool icacheFetch<msys, sz, tier>(struct icache * ic, uint32_t va, bool privileged,   \
                                              uint_fast8_t* fsrP, uint32_t& instr,               \
                                              uint32_t& decoded);

#define DEFINE_FETCH_MSYS_SZ(msys, sz)      \
    DEFINE_FETCH_MSYS_SZ_TIER(msys, sz, 0); \
    DEFINE_FETCH_MSYS_SZ_TIER(msys, sz, 1);

#define DEFINE_FETCH_MSYS(msys)    \
    DEFINE_FETCH_MSYS_SZ(msys, 2); \
    DEFINE_FETCH_MSYS_SZ(msys, 4);

DEFINE_FETCH_MSYS(ARM_MEMORY_SYSTEM_MMU);
DEFINE_FETCH_MSYS(ARM_MEMORY_SYSTEM_MPU);
DEFINE_FETCH_MSYS(ARM_MEMORY_SYSTEM_ARM920T);
