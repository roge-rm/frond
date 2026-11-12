//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr
#ifndef FROND_ICACHE_INTERNAL_H
#define FROND_ICACHE_INTERNAL_H

#include <cstring>

#include "icache.h"
#include "cputil.h"

#define CACHE_LINE_WIDTH_BITS 5
#define CACHE_INDEX_BITS 17

// THIS FILE IS ABSOLUTELY NOT ENDIAN SAFE!

#if (32 - CACHE_LINE_WIDTH_BITS - CACHE_INDEX_BITS <= 8)
    #define CACHE_TAG_TYPE uint_fast8_t
#else
    #define CACHE_TAG_TYPE uint_fast16_t
#endif

#define calculateIndex(va) ((va >> CACHE_LINE_WIDTH_BITS) & ~(0xffffffff << CACHE_INDEX_BITS))
#define calculateTag(va) (va >> (CACHE_LINE_WIDTH_BITS + CACHE_INDEX_BITS))
#define calculateLineIndex(va) (va & ~(0xffffffff << CACHE_LINE_WIDTH_BITS))
#define maskLine(va) (va & (0xffffffff << CACHE_LINE_WIDTH_BITS))

#ifdef __EMSCRIPTEN__
    #define DECODED_INSTRUCTION_TYPE uint16_t
    #define DECODED_BITS 0xc000
    #define DECODED_BITS_ARM 0x4000
    #define DECODED_BITS_THUMB 0xc000
    #define DECODED_BITS_SHIFT 0
#else
    #define DECODED_INSTRUCTION_TYPE uint32_t
    #define DECODED_BITS 0x03
    #define DECODED_BITS_ARM 0x02
    #define DECODED_BITS_THUMB 0x03
    #define DECODED_BITS_SHIFT 2
#endif

struct icacheline {
    uint8_t data[1 << CACHE_LINE_WIDTH_BITS];
    DECODED_INSTRUCTION_TYPE decoded[1 << (CACHE_LINE_WIDTH_BITS - 1)];
    uint32_t translatedThumbInstructions[1 << (CACHE_LINE_WIDTH_BITS - 1)];

    CACHE_TAG_TYPE tag;
    uint32_t revision;
    // icacheFetchKey when the line's fetch permission was last checked. A hit with another
    // key checks it again, so a line filled in one mode or under other permissions still
    // raises the prefetch abort a real fetch would.
    uint32_t fetchKey;
} __attribute__((aligned(8)));

struct icache {
    union {
        struct ArmMmu* mmu;
        struct ArmMpu* mpu;
    } memorySystem;
    struct ArmMem* mem;

    uint32_t revision;
    // Bumped whenever any line can change validity or contents (invalidation, refill), or
    // the fetch key may change (permissions, privilege). While it holds still, a line that
    // was valid stays valid and its fetch key current.
    uint32_t epoch;
    // Moves on when the MMU's TLB, domains, S or R change (mmuSetPermHook). Never 0.
    uint32_t permGen;
    bool xscale;
    bool arm920, arm920Enabled, arm920RoundRobin;
    uint32_t arm920ContinuationPc, arm920Continuation[2];
    uint8_t arm920ContinuationFault[2];
    uint8_t arm920ContinuationCount;
    uint32_t arm920Random;
    uint8_t arm920Victim[8];
    struct {
        uint32_t base;
        uint32_t words[8];
        bool valid;
    } arm920Lines[8][64];
    // The line each segment's last fetch hit, looked at first. A segment never holds two valid lines
    // with one base, as a line is only filled after a search found none. Not saved.
    uint8_t arm920Recent[8];
    // ARM920T Fast: answers a fetch from the lean decode cache or a plain host page, or returns false for
    // the model to read it uncached. While set the model fills no lines. Not saved.
    bool (*arm920LeanWord)(void* ctx, uint32_t va, unsigned size, bool privileged, uint32_t* word);
    void* arm920LeanWordCtx;
    uint32_t nextFetchBuffer;
    struct {
        uint32_t base;
        uint32_t words[8];
        uint32_t fetchKey;  // as icacheline's; 0 matches no key
#ifdef FROND_DYNARMIC
        // Fast only, not saved: the line's physical address (~0u when unknown, after a load)
        // and its host address while a store to it is watched (uncacheable RAM lines).
        uint32_t pa;
        const uint8_t* host;
#endif
    } fetchBuffers[2];
    struct icacheline cache[1 << CACHE_INDEX_BITS];
#ifdef FROND_DYNARMIC
    // Called on every invalidation; size 0 means everything.
    void (*invalHook)(void* ctx, uint32_t va, uint32_t size, bool guest);
    void* invalHookCtx;
    // Fast only: watches the RAM line at pa for stores, returning its host address, or null
    // when it isn't RAM. Called for each fetch buffer filled from an uncacheable line.
    const uint8_t* (*fetchWatch)(void* ctx, uint32_t pa);
    void* fetchWatchCtx;
    bool fetchUnwatched;  // buffers loaded from a save, not watched yet
#endif
};

// icacheArm920Branch for the interpreter's hot paths, inline so their callers need no call or stack frame.
static FORCE_INLINE void icacheArm920BranchInline(struct icache* ic) { ic->arm920ContinuationCount = 0; }

// Permissions and privilege a line's fetch was checked under.
static inline uint32_t icacheFetchKey(const struct icache* ic, bool privileged) {
    return (ic->permGen << 1) | (privileged ? 1 : 0);
}

#ifndef __EMSCRIPTEN__
// Misses, invalidations, alignment faults, new decodes and stale fetch keys use icacheFetch.
template <int size>
static FORCE_INLINE bool icacheFetchCached(struct icache* ic, uint32_t va, uint32_t key,
                                           uint32_t& instr, uint32_t& decoded) {
    if (ic->arm920 || (va & (size - 1))) return false;
    const auto* line = ic->cache + calculateIndex(va);
    if (line->revision != ic->revision || line->tag != calculateTag(va) || line->fetchKey != key)
        return false;
    const unsigned offset = calculateLineIndex(va);
    const uint32_t cached = line->decoded[offset >> 1];
    constexpr uint32_t kind = size == 4 ? DECODED_BITS_ARM : DECODED_BITS_THUMB;
    if ((cached & DECODED_BITS) != kind) return false;
    decoded = int32_t(cached) >> DECODED_BITS_SHIFT;
    if constexpr (size == 4) std::memcpy(&instr, line->data + offset, sizeof(instr));
    else instr = line->translatedThumbInstructions[offset >> 1];
    return true;
}
#endif

#endif
