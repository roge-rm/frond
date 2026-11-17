#ifndef _DCACHE_COST_H_
#define _DCACHE_COST_H_

#include <cstddef>
#include <cstdint>

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
//   page's C and B bits. An 8-entry, 16-byte coalescing write buffer.
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
// - Coalescing only looks at the newest write buffer entry: a store to the 16-byte block the last buffered
//   store went to joins it, any other takes a new entry.
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
};

// The model's whole state, a fixed layout with no pointers: the session saves it as saveBytes() gives.
struct DataCacheState {
    static constexpr unsigned kLines = 1024;  // the larger geometry: XScale 32 x 32 (ARM920T uses 8 x 64)
    // Per line: bits 0-26 the line address (VA >> 5), bit 29 the low half dirty, bit 30 the high half
    // dirty, bit 31 valid. Index set * ways + way.
    uint32_t line[kLines];
    uint8_t victim[32];  // XScale: the round-robin pointer per set; ARM920T: victim[0] for every set
    uint32_t random;     // ARM920T pseudo-random replacement
    uint32_t writeBlock; // the write buffer's coalescing entry: (VA >> 4) | 1 << 31 when it holds one, else 0
    uint64_t busFreeAt;  // core clock count at which the memory bus has finished what is queued
};

constexpr uint32_t kLineValid = 1u << 31, kLineDirtyHigh = 1u << 30, kLineDirtyLow = 1u << 29;
constexpr uint32_t kLineAddressMask = (1u << 27) - 1;
constexpr size_t kDataCacheSaveBytes = DataCacheState::kLines * 4 + 32 + 4 + 4 + 8;

inline void dataCacheReset(DataCacheState& s, Core core) {
    for (uint32_t& l : s.line) l = 0;
    // XScale: every set's pointer at way 31 after reset; ARM920T: the victim counter at 0.
    for (uint8_t& v : s.victim) v = core == Core::arm920t ? 0 : 31;
    s.random = 0;
    s.writeBlock = 0;
    s.busFreeAt = 0;
}

namespace detail {

inline unsigned dirtyHalves(uint32_t l) { return ((l & kLineDirtyLow) ? 1 : 0) + ((l & kLineDirtyHigh) ? 1 : 0); }

// The way holding va's line in its set, or -1.
inline int findWay(const DataCacheState& s, const CacheGeometry& g, uint32_t va) {
    const uint32_t tag = (va >> 5) | kLineValid;
    const unsigned set = (va >> 5) & ((1u << g.setBits) - 1);
    const uint32_t* base = s.line + set * g.ways;
    for (unsigned w = 0; w < g.ways; w++)
        if ((base[w] & (kLineValid | kLineAddressMask)) == tag) return int(w);
    return -1;
}

// Takes the next victim of va's set, writes it back if dirty and puts va's line there, clean.
inline uint32_t& allocate(DataCacheState& s, const CacheGeometry& g, uint32_t va, bool roundRobin, CacheEvents& e) {
    const unsigned set = (va >> 5) & ((1u << g.setBits) - 1);
    unsigned way;
    if (g.sharedVictim) {
        if (roundRobin) {
            way = s.victim[0] % g.ways;
            s.victim[0] = uint8_t((way + 1) % g.ways);
        } else {
            s.random = s.random * 1664525u + 1013904223u;
            way = (s.random >> 24) % g.ways;
        }
    } else {
        way = s.victim[set] % g.ways;
        s.victim[set] = uint8_t((way + 1) % g.ways);
    }
    uint32_t& l = s.line[set * g.ways + way];
    if (l & kLineValid) {
        const unsigned halves = dirtyHalves(l);
        if (halves) {
            e.writebacks = uint8_t(e.writebacks + halves);
            s.writeBlock = 0;  // the write-back takes buffer entries of its own
        }
    }
    l = ((va >> 5) & kLineAddressMask) | kLineValid;
    return l;
}

inline void bufferStore(DataCacheState& s, uint32_t va, CacheEvents& e) {
    const uint32_t block = ((va >> 4) & 0x0fffffffu) | (1u << 31);
    if (s.writeBlock != block) {
        e.buffered++;
        s.writeBlock = block;
    }
}

}  // namespace detail

// One load or store at va (any size up to 8, never crossing a line). roundRobin is the ARM920T's RR bit
// (the XScale is always round-robin).
inline CacheEvents dataCacheAccess(DataCacheState& s, Core core, uint32_t va, Policy p, bool store,
                                   bool roundRobin = true) {
    CacheEvents e;
    if (!policyCached(p)) {
        if (!store) e.uncachedLoads = 1;
        else if (p == Policy::buffered) detail::bufferStore(s, va, e);
        else {
            e.uncachedStores = 1;
            s.writeBlock = 0;  // an unbuffered store waits for the buffer to drain first
        }
        return e;
    }
    const CacheGeometry& g = cacheGeometry(core);
    const int way = detail::findWay(s, g, va);
    const unsigned set = (va >> 5) & ((1u << g.setBits) - 1);
    const uint32_t dirty = (va & 16) ? kLineDirtyHigh : kLineDirtyLow;
    if (way >= 0) {
        e.hits = 1;
        if (store) {
            if (p == Policy::writeThrough) detail::bufferStore(s, va, e);
            else s.line[set * g.ways + unsigned(way)] |= dirty;
        }
        return e;
    }
    if (!store) {
        detail::allocate(s, g, va, roundRobin, e);
        e.fills = 1;
        return e;
    }
    if (p == Policy::writeBackAllocate) {
        uint32_t& l = detail::allocate(s, g, va, roundRobin, e);
        l |= dirty;
        e.fills = 1;
        return e;
    }
    detail::bufferStore(s, va, e);  // a store miss without write allocate goes to memory
    return e;
}

// dataCacheAccess with the way each set hit last looked at first (hint, one byte per set, any start
// values). Gives the same events and state as dataCacheAccess: a set never holds two valid lines with one
// address, so the hinted way is the one the full search would find.
inline CacheEvents dataCacheAccessHinted(DataCacheState& s, Core core, uint32_t va, Policy p, bool store,
                                         uint8_t* hint, bool roundRobin = true) {
    const CacheGeometry& g = cacheGeometry(core);
    const unsigned set = (va >> 5) & ((1u << g.setBits) - 1);
    if (policyCached(p)) {
        uint32_t& l = s.line[set * g.ways + hint[set] % g.ways];
        if ((l & (kLineValid | kLineAddressMask)) == (((va >> 5) & kLineAddressMask) | kLineValid)) {
            CacheEvents e;
            e.hits = 1;
            if (store) {
                if (p == Policy::writeThrough) detail::bufferStore(s, va, e);
                else l |= (va & 16) ? kLineDirtyHigh : kLineDirtyLow;
            }
            return e;
        }
    }
    const CacheEvents e = dataCacheAccess(s, core, va, p, store, roundRobin);
    if (policyCached(p)) {
        const int way = detail::findWay(s, g, va);
        if (way >= 0) hint[set] = uint8_t(way);
    }
    return e;
}

// Cache maintenance (CP15 c7). Each returns the dirty halves it wrote back.
inline void dataCacheInvalidateAll(DataCacheState& s) {
    for (uint32_t& l : s.line) l = 0;  // dirty data is dropped, as the hardware does
}

inline CacheEvents dataCacheLineOp(DataCacheState& s, Core core, uint32_t va, bool clean, bool invalidate) {
    CacheEvents e;
    const CacheGeometry& g = cacheGeometry(core);
    const int way = detail::findWay(s, g, va);
    if (way < 0) return e;
    uint32_t& l = s.line[((va >> 5) & ((1u << g.setBits) - 1)) * g.ways + unsigned(way)];
    if (clean) {
        e.writebacks = uint8_t(detail::dirtyHalves(l));
        if (e.writebacks) s.writeBlock = 0;
        l &= ~(kLineDirtyLow | kLineDirtyHigh);
    }
    if (invalidate) l = 0;
    return e;
}

// ARM920T clean or clean and invalidate by index: way in bits 31:26, segment in bits 7:5.
inline CacheEvents dataCacheIndexOp(DataCacheState& s, Core core, uint32_t index, bool clean, bool invalidate) {
    CacheEvents e;
    const CacheGeometry& g = cacheGeometry(core);
    const unsigned set = (index >> 5) & ((1u << g.setBits) - 1), way = (index >> 26) % g.ways;
    uint32_t& l = s.line[set * g.ways + way];
    if (clean && (l & kLineValid)) {
        e.writebacks = uint8_t(detail::dirtyHalves(l));
        if (e.writebacks) s.writeBlock = 0;
        l &= ~(kLineDirtyLow | kLineDirtyHigh);
    }
    if (invalidate) l = 0;
    return e;
}

// XScale line allocate (MCR p15, 0, Rd, c7, c2, 5): takes a line for va without a fill, writing the
// victim back. The global clean routine (manual example 6-2) runs this over 1024 lines.
inline CacheEvents dataCacheAllocateLine(DataCacheState& s, Core core, uint32_t va) {
    CacheEvents e;
    const CacheGeometry& g = cacheGeometry(core);
    if (detail::findWay(s, g, va) >= 0) return e;
    detail::allocate(s, g, va, true, e);
    return e;
}

// Drain write buffer (c7, c10, 4): the next buffered store can't coalesce. The core waiting for the drain
// itself isn't charged.
inline void dataCacheDrain(DataCacheState& s) { s.writeBlock = 0; }

// Queues entries new write buffer entries at core clock now (the cycle count at the access, charges so far
// included). Each drains in drain clocks once the bus is free; a full buffer (depth entries) stalls the
// core until one leaves. Returns the stall in core clocks. emptyAt is DataCacheState::busFreeAt.
inline uint32_t writeBufferStall(uint64_t& emptyAt, uint64_t now, unsigned entries, uint32_t drain, unsigned depth) {
    uint64_t stall = 0;
    for (unsigned i = 0; i < entries; i++) {
        const uint64_t t = now + stall;
        const uint64_t end = (emptyAt > t ? emptyAt : t) + drain;
        const uint64_t limit = uint64_t(depth) * drain;
        if (end - t > limit) stall += end - t - limit;
        emptyAt = end;
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

constexpr unsigned writeBufferDepth(Core core) { return core == Core::arm920t ? 4 : 8; }

// The state as little-endian bytes, kDataCacheSaveBytes long, and back. loadBytes rejects nothing: any
// bytes give a valid state (a bad victim pointer is taken modulo the ways).
inline void dataCacheSaveBytes(const DataCacheState& s, uint8_t* out) {
    auto put = [&out](uint32_t v, unsigned n) {
        for (unsigned i = 0; i < n; i++) *out++ = uint8_t(v >> (8 * i));
    };
    for (uint32_t l : s.line) put(l, 4);
    for (uint8_t v : s.victim) put(v, 1);
    put(s.random, 4);
    put(s.writeBlock, 4);
    put(uint32_t(s.busFreeAt), 4);
    put(uint32_t(s.busFreeAt >> 32), 4);
}

inline void dataCacheLoadBytes(DataCacheState& s, const uint8_t* in) {
    auto get = [&in](unsigned n) {
        uint32_t v = 0;
        for (unsigned i = 0; i < n; i++) v |= uint32_t(*in++) << (8 * i);
        return v;
    };
    for (uint32_t& l : s.line) l = get(4);
    for (uint8_t& v : s.victim) v = uint8_t(get(1));
    s.random = get(4);
    s.writeBlock = get(4);
    s.busFreeAt = get(4);
    s.busFreeAt |= uint64_t(get(4)) << 32;
}

// What the events cost, per region a cached access can reach (SDRAM, SRAM, flash). Memory controller and
// bus clocks in sixteenths, turned into core clocks by the clock ratios and rounded once, as regionCost.
// What the bus does, in sixteenths of the slower of the memory and bus clocks (a line moves over both):
// the memory clock on the T|X, Zire 72 and E2, the 50 MHz PXbus on the TG50.
struct CacheCharge {
    uint16_t fill16;           // a 32-byte line fill, the core waiting for all of it
    uint16_t writeback16;      // one dirty 16-byte half written back through the write buffer
    uint16_t buffered16;       // a store taking a new write buffer entry
    uint16_t uncachedLoad16;   // an uncached load, the core waiting for it
    uint16_t uncachedStore16;  // an unbuffered store, the core waiting for it
};

// XScale SDRAM and flash: fill 28, dirty half 19, buffer entry 8.5 clocks; fitted to FrondBench Memory and
// Drawing and the Speedy runs (M1b and M5 notes). Uncached load and store 10 and 6: estimates, the runs make
// almost none. SRAM sits on the internal bus: estimates, no run touches it.
constexpr CacheCharge kXscaleCacheCharges[3] = {
    /* sdram */ {448, 304, 136, 160, 96},
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

constexpr uint32_t toCoreClocks(uint64_t slow16, Clocks c) {
    const uint64_t slow = c.memory < c.bus ? c.memory : c.bus;
    const uint64_t den = uint64_t(16) * slow;
    return den ? uint32_t((slow16 * c.core + den / 2) / den) : 0;
}

constexpr CacheCosts cacheCosts(Core core, Region r, Clocks c) {
    const unsigned i = r == sram ? 1 : r == flash ? 2 : 0;
    const CacheCharge& g = (core == Core::arm920t ? kArm920CacheCharges : kXscaleCacheCharges)[i];
    return {toCoreClocks(g.fill16, c), toCoreClocks(g.writeback16, c), toCoreClocks(g.buffered16, c),
            toCoreClocks(g.uncachedLoad16, c), toCoreClocks(g.uncachedStore16, c)};
}

// The core's stall for one access's events at core clock now (the cycle count at the access, charges
// so far included), on the state's bus: dirty halves queue in the write buffer, then a fill waits for
// the bus and takes its time, then buffered stores queue. Uncached loads and unbuffered stores wait
// like a fill. Hits cost nothing.
inline uint32_t cacheStall(DataCacheState& s, Core core, const CacheEvents& e, const CacheCosts& k, uint64_t now) {
    const unsigned depth = writeBufferDepth(core);
    uint64_t stall = 0;
    if (e.writebacks) stall += writeBufferStall(s.busFreeAt, now, e.writebacks, k.writeback, depth);
    for (unsigned i = 0; i < e.fills; i++) stall += fillStall(s.busFreeAt, now + stall, k.fill);
    for (unsigned i = 0; i < e.uncachedLoads; i++) stall += fillStall(s.busFreeAt, now + stall, k.uncachedLoad);
    for (unsigned i = 0; i < e.uncachedStores; i++) stall += fillStall(s.busFreeAt, now + stall, k.uncachedStore);
    if (e.buffered) stall += writeBufferStall(s.busFreeAt, now + stall, e.buffered, k.buffered, depth);
    return uint32_t(stall);
}

}  // namespace cost

#endif
