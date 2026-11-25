#ifndef _DCACHE_COST_H_
#define _DCACHE_COST_H_

#include <cstddef>
#include <cstdint>
#include <type_traits>
#if defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "xscale_cost.h"

// A tag-only data cache model for the cost model: which loads and stores hit, which fill a line, which
// write a dirty line back and which go to the write buffer. It holds no data (the guest's memory is
// always current in Frond) and only decides what an access costs. Pure integer code with a fixed state
// layout, so every host gets the same hits and misses and the state saves as plain bytes.
//
// XScale (PXA25x, PXA27x), core developer's manual chapter 6 and 3.2.2:
//   32 KB, 32 sets (VA[9:5]) of 32 ways, 32-byte lines, one valid and two dirty bits (one per 16-byte
//   half) a line, virtually addressed and tagged. Round-robin replacement, one pointer per set, at way 31
//   after reset. Read allocate; write allocate only with X=1, C=1, B=1. Write-back or write-through by the
//   page's C and B bits. An 8-entry, 16-byte coalescing write buffer: a store may coalesce with any of
//   the eight entries (6.1.3).
// ARM920T (Zodiac), TRM chapter 4:
//   16 KB, 8 segments (VA[7:5]) of 64 ways, 32-byte lines, one valid and two dirty bits a line, virtually
//   addressed. Read allocate only. Round-robin or pseudo-random replacement (control register RR bit),
//   one victim counter for all segments. A 16-word, 4-address write buffer.
//
// What decides cacheability: the MMU is on, the control register's C bit (dcache enable) is set and the
// page's C bit is set. Then B picks write-back (1) or write-through (0). Not cacheable: B=1 is buffered
// (stores go to the write buffer), B=0 waits for each access.
//
// Simplifications, each a deliberate choice:
// - The XScale mini-data cache (X=1, C=1, B=0) is priced as the main data cache, read allocate.
// - A disabled cache isn't looked up (the XScale and ARM920T still hit lines left in it): with the cache
//   off every access is uncached.
// - Locked lines and the XScale's data RAM are ignored: every way is a cache way.
// - Write buffer coalescing: a buffered store joins any of the last depth entries that holds its 16-byte
//   block and hasn't started draining at the store's core clock; otherwise it takes a new entry. An entry
//   that went to an idle bus starts draining at once, so the next store to its block takes a new entry. Dirty
//   halves written back take entries no store joins. The auxiliary control register's K bit (coalescing
//   off) is ignored: no ROM sets it.
// - One memory bus, kept as the core clock it is next free at: write buffer entries (buffered stores and
//   dirty halves written back) take drain clocks of it each, in order, and a line fill waits for
//   everything queued before it, then takes fill clocks. The core stalls for the whole fill, and for a
//   store only when the buffer already holds depth entries (writeBufferStall, fillStall). Critical word
//   first, the fill and pend buffers and hit-under-miss aren't modelled: a fill costs its full time.
// - Draining the buffer for a CP15 drain or clean isn't waited for.
// - Unaligned and 64-bit accesses never cross a line (ARM rotates unaligned words; LDRD is 8-aligned), so
//   each access looks up one line.

namespace cost {

// How an access is cached, from the MMU, the control register and the page's C, B (and XScale X) bits.
enum class Policy : uint8_t {
    uncached,          // C=0, B=0 (or MMU or cache off): each load and store waits on the bus
    buffered,          // C=0, B=1: loads wait, stores go to the write buffer
    writeThrough,      // C=1, B=0: read allocate, every store also goes to the write buffer
    writeBack,         // C=1, B=1: read allocate, store hits only mark the line dirty
    writeBackAllocate  // XScale X=1, C=1, B=1: write-back, store misses fill a line too
};

constexpr Policy policyOf(Core core, bool mmuOn, bool cacheOn, bool c, bool b, bool x) {
    if (!mmuOn) return Policy::uncached;
    if (core != Core::arm920t && x) {
        if (c && b) return cacheOn ? Policy::writeBackAllocate : Policy::buffered;
        if (c) return cacheOn ? Policy::writeBack : Policy::buffered;  // mini-data cache, priced as the main one
        if (b) return Policy::buffered;
        return Policy::uncached;  // X=1, C=0, B=0 is unpredictable
    }
    if (c && cacheOn) return b ? Policy::writeBack : Policy::writeThrough;
    return b ? Policy::buffered : Policy::uncached;
}

constexpr bool policyCached(Policy p) { return p == Policy::writeThrough || p >= Policy::writeBack; }

struct CacheGeometry {
    uint8_t setBits;  // sets = 1 << setBits, indexed by VA[5 + setBits - 1 : 5]
    uint8_t ways;
    bool sharedVictim;  // one victim counter for every set (ARM920T) rather than one per set (XScale)
};

constexpr CacheGeometry kXscaleCache = {5, 32, false};
constexpr CacheGeometry kArm920Cache = {3, 64, true};
constexpr const CacheGeometry& cacheGeometry(Core core) { return core == Core::arm920t ? kArm920Cache : kXscaleCache; }

// What one access or cache operation did. Hits cost nothing on top of the instruction's issue cycles.
struct CacheEvents {
    uint8_t fills = 0;           // 32-byte line fills
    uint8_t writebacks = 0;      // dirty 16-byte halves written back
    uint8_t buffered = 0;        // stores (or write-through stores) that took a new write buffer entry
    uint8_t uncachedLoads = 0;   // loads that waited on the bus
    uint8_t uncachedStores = 0;  // stores that waited on the bus (C=0, B=0)
    uint8_t hits = 0;            // loads and stores that hit a line (counted, never charged)
    uint32_t bufferBlock = 0;    // the new write buffer entry's block, when buffered
    uint32_t evicted = 0;        // the valid line a fill or line allocate replaced (DataCacheState::line), or 0
};

// The model's whole state, a fixed layout with no pointers: the session saves it as saveBytes() gives.
constexpr unsigned kWriteBufferSlots = 8;  // the larger depth: XScale 8 (ARM920T uses 4)
struct DataCacheState {
    static constexpr unsigned kLines = 1024;  // the larger geometry: XScale 32 x 32 (ARM920T uses 8 x 64)
    // Per line: bits 0-26 the line address (VA >> 5), bit 29 the low half dirty, bit 30 the high half
    // dirty, bit 31 valid. Index set * ways + way.
    uint32_t line[kLines];
    uint8_t victim[32];  // XScale: the round-robin pointer per set; ARM920T: victim[0] for every set
    uint32_t random;     // ARM920T pseudo-random replacement
    uint32_t bufferNext;  // the write buffer slot the next entry takes
    uint64_t busFreeAt;   // core clock count at which the memory bus has finished what is queued
    // The last entries queued, oldest overwritten first: the 16-byte block ((VA >> 4) | 1 << 31, 0 for a
    // write-back or none) and the core clock its drain starts at. bufferPending is the slot a store that
    // took a new entry claimed; cacheStall stamps its start time when it queues it.
    uint32_t bufferBlock[kWriteBufferSlots];
    uint64_t bufferStart[kWriteBufferSlots];
};

constexpr uint32_t kLineValid = 1u << 31, kLineDirtyHigh = 1u << 30, kLineDirtyLow = 1u << 29;
constexpr uint32_t kLineAddressMask = (1u << 27) - 1;

// Each bucket holds the ways that can match a tag within one set. Hits still check the full tag.
// Derived from valid lines after a load and kept outside saved state.
struct CacheWayIndex {
    static constexpr unsigned kBuckets = 1024;
    uint64_t candidates[kBuckets];

    static constexpr unsigned bucket(const CacheGeometry& g, uint32_t line, unsigned set) {
        const uint32_t tag = (line & kLineAddressMask) >> g.setBits;
        const uint32_t folded = tag ^ (tag >> 5) ^ (tag >> 11);
        return ((folded << g.setBits) | set) & (kBuckets - 1);
    }

    void insert(const CacheGeometry& g, uint32_t line, unsigned set, unsigned way) {
        candidates[bucket(g, line, set)] |= uint64_t{1} << way;
    }

    void remove(const CacheGeometry& g, uint32_t line, unsigned set, unsigned way) {
        candidates[bucket(g, line, set)] &= ~(uint64_t{1} << way);
    }

    void rebuild(const DataCacheState& s, Core core) {
        *this = {};
        const CacheGeometry& g = cacheGeometry(core);
        for (unsigned set = 0; set < 1u << g.setBits; ++set) {
            for (unsigned way = 0; way < g.ways; ++way) {
                const uint32_t line = s.line[set * g.ways + way];
                if (line & kLineValid) insert(g, line, set, way);
            }
        }
    }
};
// Version 1 (no write buffer slots, a single coalescing block where bufferNext is) and version 2.
constexpr size_t kDataCacheSaveBytesV1 = DataCacheState::kLines * 4 + 32 + 4 + 4 + 8;
constexpr size_t kDataCacheSaveBytes = kDataCacheSaveBytesV1 + kWriteBufferSlots * (4 + 8);

template <typename Directory = std::nullptr_t>
inline void dataCacheReset(DataCacheState& s, Core core, Directory index = nullptr) {
    if constexpr (std::is_pointer_v<Directory>) {
        if (index) *index = {};
    }
    for (uint32_t& l : s.line) l = 0;
    // XScale: every set's pointer at way 31 after reset; ARM920T: the victim counter at 0.
    for (uint8_t& v : s.victim) v = core == Core::arm920t ? 0 : 31;
    s.random = 0;
    s.bufferNext = 0;
    s.busFreeAt = 0;
    for (unsigned i = 0; i < kWriteBufferSlots; i++) {
        s.bufferBlock[i] = 0;
        s.bufferStart[i] = 0;
    }
}

constexpr unsigned writeBufferDepth(Core core) { return core == Core::arm920t ? 4 : 8; }

namespace detail {

inline unsigned dirtyHalves(uint32_t l) { return ((l & kLineDirtyLow) ? 1 : 0) + ((l & kLineDirtyHigh) ? 1 : 0); }

// The way holding va's line in its set, or -1.
inline int findIndexedWay(const CacheWayIndex& index, const CacheGeometry& g, const uint32_t* base,
                          unsigned set, uint32_t va) {
    const uint32_t line = va >> 5;
    const uint32_t wanted = line | kLineValid;
    uint64_t candidates = index.candidates[CacheWayIndex::bucket(g, line, set)];
    while (candidates) {
        const unsigned way = __builtin_ctzll(candidates);
        if ((base[way] & (kLineAddressMask | kLineValid)) == wanted) return int(way);
        candidates &= candidates - 1;
    }
    return -1;
}

template <typename Directory = std::nullptr_t>
inline int findWay(const DataCacheState& s, const CacheGeometry& g, uint32_t va, Directory index = nullptr) {
    const uint32_t tag = (va >> 5) | kLineValid;
    const unsigned set = (va >> 5) & ((1u << g.setBits) - 1);
    const uint32_t* base = s.line + set * g.ways;
    if constexpr (std::is_pointer_v<Directory>) {
        if (index) return findIndexedWay(*index, g, base, set, va);
    }
    // Four ways at a time (ways is a multiple of 4); a set holds at most one match.
#if defined(__SSE2__)
    const __m128i m = _mm_set1_epi32(int(kLineValid | kLineAddressMask)), t = _mm_set1_epi32(int(tag));
    for (unsigned w = 0; w < g.ways; w += 4) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(base + w));
        const int hit = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_and_si128(v, m), t)));
        if (hit) return int(w + unsigned(__builtin_ctz(unsigned(hit))));
    }
    return -1;
#elif defined(__aarch64__)
    const uint32x4_t m = vdupq_n_u32(kLineValid | kLineAddressMask), t = vdupq_n_u32(tag);
    for (unsigned w = 0; w < g.ways; w += 4) {
        const uint32x4_t c = vceqq_u32(vandq_u32(vld1q_u32(base + w), m), t);
        if (vmaxvq_u32(c))
            for (unsigned i = 0; i < 4; i++)
                if ((base[w + i] & (kLineValid | kLineAddressMask)) == tag) return int(w + i);
    }
    return -1;
#else
    for (unsigned w = 0; w < g.ways; w++)
        if ((base[w] & (kLineValid | kLineAddressMask)) == tag) return int(w);
    return -1;
#endif
}

// Takes the next victim of va's set, writes it back if dirty and puts va's line there, clean.
template <typename Directory>
inline uint32_t& allocate(DataCacheState& s, const CacheGeometry& g, uint32_t va, bool roundRobin, CacheEvents& e,
                          Directory index) {
    const unsigned set = (va >> 5) & ((1u << g.setBits) - 1);
    unsigned way;
    if (g.sharedVictim) {
        if (roundRobin) {
            way = s.victim[0] & (g.ways - 1);
            s.victim[0] = uint8_t((way + 1) & (g.ways - 1));
        } else {
            s.random = s.random * 1664525u + 1013904223u;
            way = (s.random >> 24) & (g.ways - 1);
        }
    } else {
        way = s.victim[set] & (g.ways - 1);
        s.victim[set] = uint8_t((way + 1) & (g.ways - 1));
    }
    uint32_t& l = s.line[set * g.ways + way];
    if (l & kLineValid) {
        e.writebacks = uint8_t(e.writebacks + dirtyHalves(l));
        e.evicted = l;
        if constexpr (std::is_pointer_v<Directory>) {
            if (index) index->remove(g, l, set, way);
        }
    }
    l = (va >> 5) | kLineValid;
    if constexpr (std::is_pointer_v<Directory>) {
        if (index) index->insert(g, l, set, way);
    }
    return l;
}

// A buffered store at core clock now: joins an entry for its block that hasn't started draining (one
// starting at now hasn't: the accesses of one LDM/STM share a clock), or takes a new one (e.buffered, its
// block in e.bufferBlock for cacheStall to queue).
inline void bufferStore(DataCacheState& s, Core core, uint32_t va, uint64_t now, CacheEvents& e) {
    const uint32_t block = ((va >> 4) & 0x0fffffffu) | (1u << 31);
    const unsigned depth = writeBufferDepth(core);
    for (unsigned i = 0; i < depth; i++)
        if (s.bufferBlock[i] == block && s.bufferStart[i] >= now) return;
    e.buffered++;
    e.bufferBlock = block;
}

}  // namespace detail

// One load or store at va (any size up to 8, never crossing a line) at core clock now (the cycle count at
// the access, charges so far included, as cacheStall takes it). roundRobin is the ARM920T's RR bit (the
// XScale is always round-robin).
template <typename Directory = std::nullptr_t>
inline CacheEvents dataCacheAccess(DataCacheState& s, Core core, uint32_t va, Policy p, bool store, uint64_t now,
                                   bool roundRobin = true, uint8_t* hint = nullptr, Directory index = nullptr) {
    CacheEvents e;
    if (!policyCached(p)) {
        if (!store) e.uncachedLoads = 1;
        else if (p == Policy::buffered) detail::bufferStore(s, core, va, now, e);
        else e.uncachedStores = 1;  // waits for the buffer to drain first (cacheStall)
        return e;
    }
    const CacheGeometry& g = cacheGeometry(core);
    const unsigned set = (va >> 5) & ((1u << g.setBits) - 1);
    uint32_t* base = s.line + set * g.ways;
    const unsigned previous = hint ? hint[set] & (g.ways - 1) : 0;
    const int way = hint && (base[previous] & (kLineValid | kLineAddressMask)) == ((va >> 5) | kLineValid)
                        ? int(previous) : detail::findWay(s, g, va, index);
    const uint32_t dirty = (va & 16) ? kLineDirtyHigh : kLineDirtyLow;
    if (way >= 0) {
        if (hint) hint[set] = uint8_t(way);
        e.hits = 1;
        if (store) {
            if (p == Policy::writeThrough) detail::bufferStore(s, core, va, now, e);
            else s.line[set * g.ways + unsigned(way)] |= dirty;
        }
        return e;
    }
    if (!store) {
        auto& line = detail::allocate(s, g, va, roundRobin, e, index);
        if (hint) hint[set] = uint8_t(&line - base);
        e.fills = 1;
        return e;
    }
    if (p == Policy::writeBackAllocate) {
        uint32_t& l = detail::allocate(s, g, va, roundRobin, e, index);
        if (hint) hint[set] = uint8_t(&l - base);
        l |= dirty;
        e.fills = 1;
        return e;
    }
    detail::bufferStore(s, core, va, now, e);  // a store miss without write allocate goes to memory
    return e;
}

// dataCacheAccess with the way each set hit last looked at first (hint, one byte per set, any start
// values). Gives the same events and state as dataCacheAccess: a set never holds two valid lines with one
// address, so the hinted way is the one the full search would find.
template <typename Directory = std::nullptr_t>
inline CacheEvents dataCacheAccessHinted(DataCacheState& s, Core core, uint32_t va, Policy p, bool store,
                                         uint64_t now, uint8_t* hint, bool roundRobin = true, Directory index = nullptr) {
    return dataCacheAccess(s, core, va, p, store, now, roundRobin, hint, index);
}

// Cache maintenance (CP15 c7). Each returns the dirty halves it wrote back.
template <typename Directory = std::nullptr_t>
inline void dataCacheInvalidateAll(DataCacheState& s, Directory index = nullptr) {
    for (uint32_t& l : s.line) l = 0;  // dirty data is dropped, as the hardware does
    if constexpr (std::is_pointer_v<Directory>) {
        if (index) *index = {};
    }
}

template <typename Directory = std::nullptr_t>
inline CacheEvents dataCacheLineOp(DataCacheState& s, Core core, uint32_t va, bool clean, bool invalidate,
                                   Directory index = nullptr) {
    CacheEvents e;
    const CacheGeometry& g = cacheGeometry(core);
    const int way = detail::findWay(s, g, va, index);
    if (way < 0) return e;
    const unsigned set = (va >> 5) & ((1u << g.setBits) - 1);
    uint32_t& l = s.line[set * g.ways + unsigned(way)];
    if (clean) {
        e.writebacks = uint8_t(detail::dirtyHalves(l));
        l &= ~(kLineDirtyLow | kLineDirtyHigh);
    }
    if (invalidate) {
        if constexpr (std::is_pointer_v<Directory>) {
            if (index) index->remove(g, l, set, unsigned(way));
        }
        l = 0;
    }
    return e;
}

// ARM920T clean or clean and invalidate by index: way in bits 31:26, segment in bits 7:5.
template <typename Directory = std::nullptr_t>
inline CacheEvents dataCacheIndexOp(DataCacheState& s, Core core, uint32_t index, bool clean, bool invalidate,
                                    Directory ways = nullptr) {
    CacheEvents e;
    const CacheGeometry& g = cacheGeometry(core);
    const unsigned set = (index >> 5) & ((1u << g.setBits) - 1), way = (index >> 26) % g.ways;
    uint32_t& l = s.line[set * g.ways + way];
    if (clean && (l & kLineValid)) {
        e.writebacks = uint8_t(detail::dirtyHalves(l));
        l &= ~(kLineDirtyLow | kLineDirtyHigh);
    }
    if (invalidate) {
        if constexpr (std::is_pointer_v<Directory>) {
            if (ways && (l & kLineValid)) ways->remove(g, l, set, way);
        }
        l = 0;
    }
    return e;
}

// XScale line allocate (MCR p15, 0, Rd, c7, c2, 5): takes a line for va without a fill, writing the
// victim back. The global clean routine (manual example 6-2) runs this over 1024 lines.
template <typename Directory = std::nullptr_t>
inline CacheEvents dataCacheAllocateLine(DataCacheState& s, Core core, uint32_t va, Directory index = nullptr) {
    CacheEvents e;
    const CacheGeometry& g = cacheGeometry(core);
    if (detail::findWay(s, g, va, index) >= 0) return e;
    detail::allocate(s, g, va, true, e, index);
    return e;
}

// Drain write buffer (c7, c10, 4): no later store joins an entry queued before it. The core waiting for
// the drain itself isn't charged.
inline void dataCacheDrain(DataCacheState& s) {
    for (uint32_t& b : s.bufferBlock) b = 0;
}

// Queues entries new write buffer entries at core clock now (the cycle count at the access, charges so far
// included). Each drains in drain clocks once the bus is free; a full buffer (depth entries) stalls the
// core until one leaves. Returns the stall in core clocks. emptyAt is DataCacheState::busFreeAt. starts,
// when given, gets each entry's drain start.
inline uint32_t writeBufferStall(uint64_t& emptyAt, uint64_t now, unsigned entries, uint32_t drain, unsigned depth,
                                 uint64_t* starts = nullptr) {
    uint64_t stall = 0;
    for (unsigned i = 0; i < entries; i++) {
        const uint64_t t = now + stall;
        const uint64_t start = emptyAt > t ? emptyAt : t;
        const uint64_t end = start + drain;
        const uint64_t limit = uint64_t(depth) * drain;
        if (end - t > limit) stall += end - t - limit;
        emptyAt = end;
        if (starts) starts[i] = start;
    }
    return uint32_t(stall);
}

// A line fill at core clock now: waits for the bus (the write buffer ahead of it), then takes fill clocks.
// Returns the core's stall.
inline uint32_t fillStall(uint64_t& busFreeAt, uint64_t now, uint32_t fill) {
    const uint64_t start = busFreeAt > now ? busFreeAt : now;
    busFreeAt = start + fill;
    return uint32_t(busFreeAt - now);
}

// The state as little-endian bytes, kDataCacheSaveBytes long, and back. loadBytes rejects nothing: any
// bytes give a valid state (a bad victim pointer is taken modulo the ways).
inline void dataCacheSaveBytes(const DataCacheState& s, uint8_t* out) {
    auto put = [&out](uint32_t v, unsigned n) {
        for (unsigned i = 0; i < n; i++) *out++ = uint8_t(v >> (8 * i));
    };
    for (uint32_t l : s.line) put(l, 4);
    for (uint8_t v : s.victim) put(v, 1);
    put(s.random, 4);
    put(s.bufferNext, 4);
    put(uint32_t(s.busFreeAt), 4);
    put(uint32_t(s.busFreeAt >> 32), 4);
    for (uint32_t b : s.bufferBlock) put(b, 4);
    for (uint64_t t : s.bufferStart) {
        put(uint32_t(t), 4);
        put(uint32_t(t >> 32), 4);
    }
}

// version 1 bytes (kDataCacheSaveBytesV1) load with no write buffer entries to join.
inline void dataCacheLoadBytes(DataCacheState& s, const uint8_t* in, unsigned version = 2) {
    auto get = [&in](unsigned n) {
        uint32_t v = 0;
        for (unsigned i = 0; i < n; i++) v |= uint32_t(*in++) << (8 * i);
        return v;
    };
    for (uint32_t& l : s.line) l = get(4);
    for (uint8_t& v : s.victim) v = uint8_t(get(1));
    s.random = get(4);
    s.bufferNext = get(4) % kWriteBufferSlots;
    s.busFreeAt = get(4);
    s.busFreeAt |= uint64_t(get(4)) << 32;
    if (version < 2) {
        s.bufferNext = 0;
        for (unsigned i = 0; i < kWriteBufferSlots; i++) {
            s.bufferBlock[i] = 0;
            s.bufferStart[i] = 0;
        }
        return;
    }
    for (uint32_t& b : s.bufferBlock) b = get(4);
    for (uint64_t& t : s.bufferStart) {
        t = get(4);
        t |= uint64_t(get(4)) << 32;
    }
}

// What the events cost, per region a cached access can reach (SDRAM, SRAM, flash). Memory controller and
// bus clocks in sixteenths, turned into core clocks by the clock ratios and rounded once, as regionCost.
// SRAM, flash and the ARM920T: what the bus does in sixteenths of the slower of the memory and bus clocks.
struct CacheCharge {
    uint16_t fill16;           // a 32-byte line fill, the core waiting for all of it
    uint16_t writeback16;      // one dirty 16-byte half written back through the write buffer
    uint16_t buffered16;       // a store taking a new write buffer entry
    uint16_t uncachedLoad16;   // an uncached load, the core waiting for it
    uint16_t uncachedStore16;  // an unbuffered store, the core waiting for it
};

// XScale SDRAM: a fill, a dirty half and a write buffer entry cross both the internal bus (PXbus on the
// PXA25x, system bus on the PXA27x, 32 bits wide) and the SDRAM interface, and take as long as the slower
// side. Each side is a fixed part plus one clock a data beat: on the bus one beat a word, on the SDRAM one
// a word at 32 bits and two at 16 bits, the width the ROM wrote to MDCNFG (DWID0, PXA25x and PXA27x
// manuals 6.4 / 6.5). A fill moves 8 words, a dirty half and an entry 4 (the SDRAM's burst of four is
// always done in full, so a partly filled entry takes the whole burst). Uncached accesses are one burst
// on either width and stay flat, in slow clocks as CacheCharge.
struct SdramCharge {
    uint16_t fillMemory16, writebackMemory16, bufferedMemory16;  // fixed parts, memory clocks
    uint16_t fillBus16, writebackBus16, bufferedBus16;           // fixed parts, bus clocks
    uint16_t uncachedLoad16, uncachedStore16;                    // slow clocks
};

constexpr unsigned kFillWords = 8, kHalfWords = 4;

// Fitted to FrondBench Memory and Drawing and the Speedy runs (M1b, M5 and M5b notes). SDRAM fixed parts:
// memory side fill 12, dirty half 11, entry 1 clock; bus side 20, 15, 5. So the 16-bit T|X and E2 take
// 28, 19 and 9 memory clocks, the 32-bit Zire 72 20, 15 and 5, and the TG50 is set by its 50 MHz PXbus at
// 28, 19 and 9 bus clocks. Uncached load and store 10 and 6: estimates, the runs make almost
// none. Flash keeps the M1b figures (no width). SRAM sits on the internal bus: estimates, no run touches it.
constexpr SdramCharge kXscaleSdramCharge = {192, 176, 16, 320, 240, 80, 160, 96};
constexpr CacheCharge kXscaleCacheCharges[3] = {
    /* sdram: kXscaleSdramCharge */ {0, 0, 0, 0, 0},
    /* sram */ {128, 64, 32, 64, 32},
    /* flash */ {448, 304, 136, 160, 96},
};
// ARM920T (Zodiac, against HCLK): the XScale's figures, untuned until a Zodiac Speedy run.
constexpr CacheCharge kArm920CacheCharges[3] = {
    /* sdram */ {448, 304, 96, 160, 96},
    /* sram */ {128, 64, 32, 64, 32},
    /* flash */ {448, 304, 96, 160, 96},
};

// The charges in core clocks at one set of clocks.
struct CacheCosts {
    uint32_t fill, writeback, buffered, uncachedLoad, uncachedStore;
};

constexpr bool regionCached(Region r) { return r == sdram || r == sram || r == flash; }

// Sixteenths of a clock at kHz to core clocks, rounded half up.
constexpr uint32_t toCoreClocksAt(uint64_t x16, uint32_t kHz, uint32_t coreKHz) {
    const uint64_t den = uint64_t(16) * kHz;
    return den ? uint32_t((x16 * coreKHz + den / 2) / den) : 0;
}

constexpr uint32_t toCoreClocks(uint64_t slow16, Clocks c) {
    return toCoreClocksAt(slow16, c.memory < c.bus ? c.memory : c.bus, c.core);
}

// One SDRAM transfer of words: the slower of the bus side and the memory side.
constexpr uint32_t sdramTransfer(uint64_t memory16, uint64_t bus16, unsigned words, Clocks c, bool sdram16) {
    const uint32_t m = toCoreClocksAt(memory16 + words * (sdram16 ? 32u : 16u), c.memory, c.core);
    const uint32_t b = toCoreClocksAt(bus16 + words * 16u, c.bus, c.core);
    return m > b ? m : b;
}

constexpr CacheCosts sdramCosts(const SdramCharge& g, Clocks c, bool sdram16) {
    return {sdramTransfer(g.fillMemory16, g.fillBus16, kFillWords, c, sdram16),
            sdramTransfer(g.writebackMemory16, g.writebackBus16, kHalfWords, c, sdram16),
            sdramTransfer(g.bufferedMemory16, g.bufferedBus16, kHalfWords, c, sdram16),
            toCoreClocks(g.uncachedLoad16, c), toCoreClocks(g.uncachedStore16, c)};
}

// sdram16 is a 16-bit SDRAM bus (MDCNFG DWID0 set), else 32-bit.
constexpr CacheCosts cacheCosts(Core core, Region r, Clocks c, bool sdram16 = false) {
    if (core != Core::arm920t && r == sdram) return sdramCosts(kXscaleSdramCharge, c, sdram16);
    const unsigned i = r == sram ? 1 : r == flash ? 2 : 0;
    const CacheCharge& g = (core == Core::arm920t ? kArm920CacheCharges : kXscaleCacheCharges)[i];
    return {toCoreClocks(g.fill16, c), toCoreClocks(g.writeback16, c), toCoreClocks(g.buffered16, c),
            toCoreClocks(g.uncachedLoad16, c), toCoreClocks(g.uncachedStore16, c)};
}

// The core's stall for one access's events at core clock now (the cycle count at the access, charges
// so far included), on the state's bus: dirty halves queue in the write buffer, then a fill waits for
// the bus and takes its time, then buffered stores queue. Uncached loads and unbuffered stores wait
// like a fill. Hits cost nothing.
namespace detail {

// Puts n entries queued with their drain starts into the write buffer slots: block for a store's entry,
// 0 for a write-back (no store joins it).
inline void bufferQueue(DataCacheState& s, unsigned depth, uint32_t block, const uint64_t* starts, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        const unsigned slot = s.bufferNext % depth;
        s.bufferBlock[slot] = block;
        s.bufferStart[slot] = starts[i];
        s.bufferNext = (slot + 1) % depth;
    }
}

}  // namespace detail

inline uint32_t cacheStall(DataCacheState& s, Core core, const CacheEvents& e, const CacheCosts& k, uint64_t now) {
    const unsigned depth = writeBufferDepth(core);
    uint64_t stall = 0, starts[256];
    if (e.writebacks) {
        stall += writeBufferStall(s.busFreeAt, now, e.writebacks, k.writeback, depth, starts);
        detail::bufferQueue(s, depth, 0, starts, e.writebacks);
    }
    for (unsigned i = 0; i < e.fills; i++) stall += fillStall(s.busFreeAt, now + stall, k.fill);
    for (unsigned i = 0; i < e.uncachedLoads; i++) stall += fillStall(s.busFreeAt, now + stall, k.uncachedLoad);
    for (unsigned i = 0; i < e.uncachedStores; i++) stall += fillStall(s.busFreeAt, now + stall, k.uncachedStore);
    if (e.buffered) {
        stall += writeBufferStall(s.busFreeAt, now + stall, e.buffered, k.buffered, depth, starts);
        detail::bufferQueue(s, depth, e.bufferBlock, starts, e.buffered);
    }
    return uint32_t(stall);
}

}  // namespace cost

#endif
