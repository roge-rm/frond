// The cost table (core/src/uarm/uarm/xscale_cost.h): every class has a cost and is reached by some
// word, manual words give the manual's figure, the region map and charges, and a hash of the costs of
// every Thumb halfword and a fixed sweep of ARM words, the same on every host.
#include <cstdio>
#include <cstdlib>
#include <initializer_list>

#include "dcache_cost.h"
#include "xscale_cost.h"

#include <cstring>
#include <type_traits>

using namespace cost;

static int failures = 0;
static bool reached[classCount];

static void expect(bool ok, const char *what, unsigned long long got, unsigned long long want) {
    if (ok) return;
    std::printf("FAIL %s: got %llu, want %llu\n", what, got, want);
    failures++;
}

static void arm(Core core, uint32_t word, Class wantClass, uint32_t wantCost, const char *what) {
    unsigned words = 0;
    const Class c = armClass(word, core, words);
    reached[c] = true;
    expect(c == wantClass, what, c, wantClass);
    expect(armCost(word, core) == wantCost, what, armCost(word, core), wantCost);
}

static void thumb(Core core, uint32_t half, Class wantClass, uint32_t wantCost, const char *what) {
    unsigned words = 0;
    const Class c = thumbClass(half, core, words);
    reached[c] = true;
    expect(c == wantClass, what, c, wantClass);
    expect(thumbCost(half, core) == wantCost, what, thumbCost(half, core), wantCost);
}

// Checked at compile time too, so the table is a constant expression.
static_assert(armCost(0xe0810002u, Core::pxa27x) == 1, "ADD");
static_assert(thumbPairCost(0xf000f800u, Core::pxa27x) == 5, "Thumb BL");
static_assert(regionCost(Core::pxa27x, sdram, false, kClocksTx) == 0, "T|X SDRAM load: the cache model prices it");


// The data cache model (dcache_cost.h).
static_assert(std::is_trivially_copyable<DataCacheState>::value && std::is_standard_layout<DataCacheState>::value,
              "cache state is plain data");
static_assert(kDataCacheSaveBytesV1 == 4144 && kDataCacheSaveBytes == 4240, "cache state save size");

// An access at core clock now, queued on the bus at the T|X's costs as the CPU does.
static CacheEvents caccess(DataCacheState &s, Core core, uint32_t va, Policy p, bool store, uint64_t now, bool rr) {
    const CacheEvents e = dataCacheAccess(s, core, va, p, store, now, rr);
    cacheStall(s, core, e, cacheCosts(core, sdram, kClocksTx, true), now);
    return e;
}
static CacheEvents cload(DataCacheState &s, Core core, uint32_t va, Policy p = Policy::writeBack, bool rr = true) {
    return caccess(s, core, va, p, false, 0, rr);
}
static CacheEvents cstore(DataCacheState &s, Core core, uint32_t va, Policy p = Policy::writeBack, uint64_t now = 0) {
    return caccess(s, core, va, p, true, now, true);
}
static bool same(const CacheEvents &a, const CacheEvents &b) {
    return a.fills == b.fills && a.writebacks == b.writebacks && a.buffered == b.buffered &&
           a.uncachedLoads == b.uncachedLoads && a.uncachedStores == b.uncachedStores && a.hits == b.hits;
}
static CacheEvents ev(unsigned fills, unsigned wb, unsigned buffered, unsigned ul, unsigned us, unsigned hits) {
    CacheEvents e;
    e.fills = uint8_t(fills); e.writebacks = uint8_t(wb); e.buffered = uint8_t(buffered);
    e.uncachedLoads = uint8_t(ul); e.uncachedStores = uint8_t(us); e.hits = uint8_t(hits);
    return e;
}
static void expectEvents(const CacheEvents &got, const CacheEvents &want, const char *what) {
    if (same(got, want)) return;
    std::printf("FAIL %s: got fills %u wb %u buf %u ul %u us %u hits %u, want %u %u %u %u %u %u\n", what, got.fills,
                got.writebacks, got.buffered, got.uncachedLoads, got.uncachedStores, got.hits, want.fills,
                want.writebacks, want.buffered, want.uncachedLoads, want.uncachedStores, want.hits);
    failures++;
}

static void mixHash(uint64_t &hash, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        hash ^= (v >> (8 * i)) & 0xff;
        hash *= 0x100000001b3ull;
    }
}

static void cacheTests(uint64_t &hash, void (*mix)(uint64_t &, uint32_t)) {
    const Core x = Core::pxa27x, a = Core::arm920t;
    static DataCacheState s, t;

    // Policies from the MMU, the control register's C bit and the page's C, B and X bits.
    expect(policyOf(x, false, true, true, true, false) == Policy::uncached, "MMU off", 0, 0);
    expect(policyOf(x, true, false, true, true, false) == Policy::buffered, "cache off, B", 0, 0);
    expect(policyOf(x, true, false, true, false, false) == Policy::uncached, "cache off, no B", 0, 0);
    expect(policyOf(x, true, true, false, false, false) == Policy::uncached, "CB 00", 0, 0);
    expect(policyOf(x, true, true, false, true, false) == Policy::buffered, "CB 01", 0, 0);
    expect(policyOf(x, true, true, true, false, false) == Policy::writeThrough, "CB 10", 0, 0);
    expect(policyOf(x, true, true, true, true, false) == Policy::writeBack, "CB 11", 0, 0);
    expect(policyOf(x, true, true, true, true, true) == Policy::writeBackAllocate, "X CB 11", 0, 0);
    expect(policyOf(x, true, true, true, false, true) == Policy::writeBack, "X CB 10 (mini-data cache)", 0, 0);
    expect(policyOf(a, true, true, true, true, true) == Policy::writeBack, "ARM920T has no X", 0, 0);

    // XScale: a miss fills, then hits; 32 ways a set, round-robin from way 31 after reset.
    dataCacheReset(s, x);
    expectEvents(cload(s, x, 0xa0001000u), ev(1, 0, 0, 0, 0, 0), "load miss fills");
    expectEvents(cload(s, x, 0xa000101cu), ev(0, 0, 0, 0, 0, 1), "same line hits");
    expectEvents(cload(s, x, 0xa0001020u), ev(1, 0, 0, 0, 0, 0), "next line misses");
    expect(s.line[0 * 32 + 31] == (kLineValid | (0xa0001000u >> 5)), "first fill in way 31", s.line[31], 0);
    expect(s.line[1 * 32 + 31] == (kLineValid | (0xa0001020u >> 5)), "set 1's first fill in way 31", s.line[63], 0);
    // 32 more lines in set 0 (1 KB apart): the 32nd evicts the first, round-robin.
    for (uint32_t i = 1; i < 32; i++) cload(s, x, 0xa0001000u + i * 1024);
    expectEvents(cload(s, x, 0xa0001000u), ev(0, 0, 0, 0, 0, 1), "32 lines fit a set");
    expectEvents(cload(s, x, 0xa0001000u + 32 * 1024), ev(1, 0, 0, 0, 0, 0), "33rd line fills");
    expectEvents(cload(s, x, 0xa0001000u), ev(1, 0, 0, 0, 0, 0), "first line evicted");
    expectEvents(cload(s, x, 0xa0001000u + 2 * 1024), ev(0, 0, 0, 0, 0, 1), "third line kept");
    expectEvents(cload(s, x, 0xa0001000u + 1 * 1024), ev(1, 0, 0, 0, 0, 0), "second line evicted next");

    // Write-back: store hits mark a half dirty, the eviction writes each dirty half back.
    dataCacheReset(s, x);
    cload(s, x, 0xa0000000u);
    expectEvents(cstore(s, x, 0xa0000004u), ev(0, 0, 0, 0, 0, 1), "store hit");
    expectEvents(cstore(s, x, 0xa0000018u), ev(0, 0, 0, 0, 0, 1), "store hit, high half");
    for (uint32_t i = 1; i < 32; i++) cload(s, x, 0xa0000000u + i * 1024);
    expectEvents(cload(s, x, 0xa0000000u + 32 * 1024), ev(1, 2, 0, 0, 0, 0), "dirty line written back");
    // A store miss without write allocate goes to the write buffer; the same 16 bytes coalesce.
    expectEvents(cstore(s, x, 0xa0100000u), ev(0, 0, 1, 0, 0, 0), "store miss buffered");
    expectEvents(cstore(s, x, 0xa010000cu), ev(0, 0, 0, 0, 0, 0), "store coalesces");
    expectEvents(cstore(s, x, 0xa0100010u), ev(0, 0, 1, 0, 0, 0), "next 16 bytes buffered");
    expectEvents(cload(s, x, 0xa0100000u), ev(1, 0, 0, 0, 0, 0), "store miss didn't allocate");
    dataCacheDrain(s);
    expectEvents(cstore(s, x, 0xa0100014u, Policy::buffered), ev(0, 0, 1, 0, 0, 0), "drain stops coalescing");
    // Coalescing with any of the eight entries that hasn't started draining. At the T|X's costs a 16-bit
    // entry drains in 27 clocks: from an idle bus at 1000 the first entry starts at once, the next waits.
    dataCacheReset(s, x);
    expectEvents(cstore(s, x, 0xa0500000u, Policy::buffered, 1000), ev(0, 0, 1, 0, 0, 0), "A to an idle bus");
    expectEvents(cstore(s, x, 0xa0500004u, Policy::buffered, 1000), ev(0, 0, 0, 0, 0, 0), "A again at the same clock");
    expectEvents(cstore(s, x, 0xa0500008u, Policy::buffered, 1001), ev(0, 0, 1, 0, 0, 0), "A draining: a new entry");
    expectEvents(cstore(s, x, 0xa0600000u, Policy::buffered, 1002), ev(0, 0, 1, 0, 0, 0), "B");
    expectEvents(cstore(s, x, 0xa050000cu, Policy::buffered, 1003), ev(0, 0, 0, 0, 0, 0), "A joins its waiting entry");
    expectEvents(cstore(s, x, 0xa0600004u, Policy::buffered, 1004), ev(0, 0, 0, 0, 0, 0), "B joins, not the newest");
    expectEvents(cstore(s, x, 0xa0600008u, Policy::buffered, 1055), ev(0, 0, 1, 0, 0, 0), "B started at 1054");
    // Write-backs take entries no store joins.
    dataCacheReset(s, x);
    for (uint32_t i = 0; i < 32; i++) cstore(s, x, 0xa0000000u + i * 1024, Policy::writeBackAllocate);
    for (uint32_t i = 0; i < 8; i++) cload(s, x, 0xa0000000u + (32 + i) * 1024);
    expectEvents(cstore(s, x, 0xa0000000u, Policy::buffered), ev(0, 0, 1, 0, 0, 0), "a written back block takes an entry");
    // Write-through: hits stay clean and still go to the buffer.
    dataCacheReset(s, x);
    cload(s, x, 0xa0000000u, Policy::writeThrough);
    expectEvents(cstore(s, x, 0xa0000000u, Policy::writeThrough), ev(0, 0, 1, 0, 0, 1), "write-through hit");
    expectEvents(dataCacheLineOp(s, x, 0xa0000000u, true, false), ev(0, 0, 0, 0, 0, 0), "write-through line clean");
    // Write allocate (X=1): a store miss fills and leaves the line dirty.
    expectEvents(cstore(s, x, 0xa0200010u, Policy::writeBackAllocate), ev(1, 0, 0, 0, 0, 0), "write allocate");
    expectEvents(dataCacheLineOp(s, x, 0xa0200000u, true, false), ev(0, 1, 0, 0, 0, 0), "clean writes one half");
    expectEvents(dataCacheLineOp(s, x, 0xa0200000u, true, false), ev(0, 0, 0, 0, 0, 0), "cleaned line is clean");
    expectEvents(cload(s, x, 0xa0200000u), ev(0, 0, 0, 0, 0, 1), "clean keeps the line");
    dataCacheLineOp(s, x, 0xa0200000u, false, true);
    expectEvents(cload(s, x, 0xa0200000u), ev(1, 0, 0, 0, 0, 0), "invalidated line misses");
    // Uncacheable accesses never touch the lines.
    expectEvents(cload(s, x, 0xa0300000u, Policy::uncached), ev(0, 0, 0, 1, 0, 0), "uncached load");
    expectEvents(cstore(s, x, 0xa0300000u, Policy::uncached), ev(0, 0, 0, 0, 1, 0), "uncached store");
    expectEvents(cload(s, x, 0xa0300000u, Policy::buffered), ev(0, 0, 0, 1, 0, 0), "buffered load");
    expectEvents(cstore(s, x, 0xa0300000u, Policy::buffered), ev(0, 0, 1, 0, 0, 0), "buffered store");
    expectEvents(cload(s, x, 0xa0300000u), ev(1, 0, 0, 0, 0, 0), "uncached access left no line");
    // Invalidate all drops dirty lines without writing them back; line allocate takes a line with no fill.
    cstore(s, x, 0xa0300000u);
    dataCacheInvalidateAll(s);
    expectEvents(cload(s, x, 0xa0300000u), ev(1, 0, 0, 0, 0, 0), "invalidate all");
    dataCacheReset(s, x);
    for (uint32_t i = 0; i < 32; i++) cstore(s, x, 0xa0000000u + i * 1024, Policy::writeBackAllocate);
    expectEvents(dataCacheAllocateLine(s, x, 0xa0400000u), ev(0, 1, 0, 0, 0, 0), "allocate evicts a dirty line");
    expectEvents(cload(s, x, 0xa0400000u), ev(0, 0, 0, 0, 0, 1), "allocated line hits");
    expectEvents(dataCacheAllocateLine(s, x, 0xa0400000u), ev(0, 0, 0, 0, 0, 0), "allocate of a present line");

    // The write buffer: depth entries absorb a burst, the next ones wait for a drain; sparse stores never stall.
    {
        uint64_t emptyAt = 0;
        expect(writeBufferStall(emptyAt, 0, 8, 10, 8) == 0, "8 entries fit", 0, 0);
        expect(writeBufferStall(emptyAt, 0, 1, 10, 8) == 10, "the 9th waits a drain", 0, 10);
        expect(writeBufferStall(emptyAt, 10, 2, 10, 8) == 20, "two more wait two", 0, 20);
        expect(emptyAt == 110, "drained at 110", emptyAt, 110);
        uint64_t sparse = 0;
        uint32_t total = 0;
        for (uint64_t t = 1000; t < 2000; t += 10) total += writeBufferStall(sparse, t, 1, 10, 8);
        expect(total == 0, "one entry per drain time never stalls", total, 0);
        uint64_t bus = 100;
        expect(fillStall(bus, 50, 30) == 80, "a fill waits for the bus", 0, 80);
        expect(fillStall(bus, 200, 30) == 30, "a fill on a free bus", 0, 30);
        expect(writeBufferDepth(x) == 8 && writeBufferDepth(a) == 4, "buffer depths", 0, 0);
    }

    // ARM920T: 8 segments of 64 ways, one round-robin victim counter for every segment.
    dataCacheReset(s, a);
    cload(s, a, 0x08000000u);
    cload(s, a, 0x08000020u);
    expect(s.line[0 * 64 + 0] == (kLineValid | (0x08000000u >> 5)), "920T first fill way 0", s.line[0], 0);
    expect(s.line[1 * 64 + 1] == (kLineValid | (0x08000020u >> 5)), "920T segment 1 fill way 1", s.line[65], 0);
    for (uint32_t i = 1; i < 63; i++) cload(s, a, 0x08000000u + i * 256);
    expectEvents(cload(s, a, 0x08000000u), ev(0, 0, 0, 0, 0, 1), "920T 64 lines fit a segment");
    // Segment 0's way 1 went to segment 1, so the counter wraps to way 0 with way 1 still empty.
    expectEvents(cload(s, a, 0x08000000u + 63 * 256), ev(1, 0, 0, 0, 0, 0), "920T line 63 evicts way 0");
    expectEvents(cload(s, a, 0x08000000u), ev(1, 0, 0, 0, 0, 0), "920T first line evicted, refilled in way 1");
    expectEvents(cload(s, a, 0x08000000u + 1 * 256), ev(0, 0, 0, 0, 0, 1), "920T way 2 kept");
    cstore(s, a, 0x08000004u);
    expectEvents(dataCacheIndexOp(s, a, 1u << 26, true, true), ev(0, 1, 0, 0, 0, 0), "920T clean and invalidate by index");
    expectEvents(cload(s, a, 0x08000000u), ev(1, 0, 0, 0, 0, 0), "920T index op invalidated");
    // Random replacement: deterministic.
    dataCacheReset(s, a);
    dataCacheReset(t, a);
    for (uint32_t i = 0; i < 1000; i++) {
        cload(s, a, 0x08000000u + i * 256, Policy::writeBack, false);
        cload(t, a, 0x08000000u + i * 256, Policy::writeBack, false);
    }
    expect(std::memcmp(&s, &t, sizeof s) == 0, "920T random replacement repeats", 0, 0);

    // Save and load mid-run: the copy runs on exactly as the original; the bytes are little-endian.
    dataCacheReset(s, x);
    uint32_t r = 0x9e3779b9u;
    auto next = [&r] {
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        return r;
    };
    for (int i = 0; i < 100000; i++) caccess(s, x, 0xa0000000u + (next() & 0xffffc), Policy::writeBack, next() & 1, uint64_t(i) * 3, true);
    for (int i = 0; i < 20; i++) caccess(s, x, 0xa0000000u + (next() & 0xffffc), Policy::buffered, true, 300000, true);
    static uint8_t bytes[kDataCacheSaveBytes], bytes2[kDataCacheSaveBytes];
    dataCacheSaveBytes(s, bytes);
    expect(bytes[0] == uint8_t(s.line[0]) && bytes[3] == uint8_t(s.line[0] >> 24), "line 0 little-endian", bytes[0], 0);
    expect(bytes[4096] == s.victim[0] && bytes[4128] == uint8_t(s.random), "victims and random after lines", bytes[4096], 0);
    expect(bytes[4136] == uint8_t(s.busFreeAt), "bus time", bytes[4136], 0);
    expect(bytes[4144] == uint8_t(s.bufferBlock[0]) && bytes[4176] == uint8_t(s.bufferStart[0]),
           "write buffer slots last", bytes[4144], 0);
    std::memset(&t, 0x5a, sizeof t);
    dataCacheLoadBytes(t, bytes);
    dataCacheSaveBytes(t, bytes2);
    expect(std::memcmp(bytes, bytes2, sizeof bytes) == 0, "save, load, save", 0, 0);
    bool sameRun = true;
    for (int i = 0; i < 100000; i++) {
        const uint32_t va = 0xa0000000u + (next() & 0xffffc);
        const bool st = next() & 1;
        const Policy p = (i & 7) ? Policy::writeBack : Policy::buffered;
        if (!same(caccess(s, x, va, p, st, 300000 + uint64_t(i), true), caccess(t, x, va, p, st, 300000 + uint64_t(i), true)))
            sameRun = false;
    }
    expect(sameRun, "loaded state runs as the saved one", 0, 1);
    // Version 1 bytes (no write buffer slots) load with nothing to join.
    dataCacheSaveBytes(s, bytes);
    std::memset(&t, 0x5a, sizeof t);
    dataCacheLoadBytes(t, bytes, 1);
    bool emptyBuffer = t.bufferNext == 0;
    for (unsigned i = 0; i < kWriteBufferSlots; i++) emptyBuffer = emptyBuffer && !t.bufferBlock[i] && !t.bufferStart[i];
    expect(emptyBuffer && t.busFreeAt == s.busFreeAt && std::memcmp(t.line, s.line, sizeof t.line) == 0,
           "version 1 loads lines and bus time, empty buffer", 0, 1);

    // One hash over a fixed mixed run on both cores (every policy, maintenance) and the final state bytes.
    for (Core core : {x, a}) {
        dataCacheReset(s, core);
        for (int i = 0; i < 400000; i++) {
            const uint32_t n = next(), va = (n & 3) == 0 ? 0xa0000000u + (n & 0x3ffff8) : 0xa0000000u + (n & 0x7ff8);
            const Policy p = Policy((next() >> 7) % 5);
            const CacheEvents e = dataCacheAccess(s, core, va, p, n & 4, uint64_t(i) * 9, n & 8);
            mix(hash, uint32_t(e.fills) | uint32_t(e.writebacks) << 8 | uint32_t(e.buffered) << 16 |
                          uint32_t(e.uncachedLoads) << 24);
            mix(hash, uint32_t(e.uncachedStores) | uint32_t(e.hits) << 8);
            mix(hash, cacheStall(s, core, e, cacheCosts(core, sdram, kClocksTx, i & 1), uint64_t(i) * 9));
            if ((n & 0xfff) == 0) mix(hash, dataCacheLineOp(s, core, va, true, (n >> 12) & 1).writebacks);
            if ((n & 0xffff) == 1) dataCacheInvalidateAll(s);
        }
        dataCacheSaveBytes(s, bytes);
        for (uint8_t b : bytes) mix(hash, b);
    }

    // The charges in core clocks: SDRAM against the memory clock on the T|X and Zire 72 (x3) and E2 (x2), the
    // PXbus on the TG50 (x4, the slower side there). The T|X and E2 have 16-bit SDRAM, the Zire 72 and TG50 32-bit.
    {
        const CacheCosts tx = cacheCosts(x, sdram, kClocksTx, true), e2 = cacheCosts(Core::pxa25x, sdram, kClocksE2, true),
                         tg = cacheCosts(Core::pxa25x, sdram, kClocksTg50, false), z72 = cacheCosts(x, sdram, kClocksTx, false);
        expect(tx.fill == 84 && tx.writeback == 57 && tx.buffered == 27, "T|X SDRAM charges", tx.fill, 84);
        expect(e2.fill == 56 && e2.writeback == 38 && e2.buffered == 18, "E2 SDRAM charges", e2.fill, 56);
        expect(tg.fill == 112 && tg.writeback == 76 && tg.buffered == 36, "TG50 SDRAM charges", tg.fill, 112);
        expect(z72.fill == 60 && z72.writeback == 45 && z72.buffered == 15, "Zire 72 SDRAM charges", z72.fill, 60);
        const CacheCosts f16 = cacheCosts(x, flash, kClocksTx, true), f32 = cacheCosts(x, flash, kClocksTx, false);
        expect(f16.fill == f32.fill && f16.buffered == f32.buffered, "flash charges ignore the SDRAM width", f16.fill, f32.fill);

        // A fill on an idle bus costs its time; one behind a dirty eviction waits for the write-back.
        dataCacheReset(s, x);
        CacheEvents f;
        f.fills = 1;
        expect(cacheStall(s, x, f, tx, 1000) == 84, "fill on an idle bus", 0, 84);
        f.writebacks = 2;
        expect(cacheStall(s, x, f, tx, 5000) == 2 * 57 + 84, "fill after two write-backs", 0, 198);
        // Eight buffered stores in a row fit the buffer, the ninth waits a drain.
        CacheEvents b;
        b.buffered = 1;
        uint32_t st = 0;
        for (int i = 0; i < 8; i++) st += cacheStall(s, x, b, tx, 10000);
        expect(st == 0, "eight buffered stores", st, 0);
        expect(cacheStall(s, x, b, tx, 10000) == 27, "the ninth waits", 0, 27);
        CacheEvents u;
        u.uncachedLoads = 1;
        expect(cacheStall(s, x, u, tx, 100000) == 30, "uncached load", 0, 30);
    }

    // The charges in core clocks at each device's clocks.
    for (Core core : {Core::pxa25x, x, a})
        for (Region g : {sdram, sram, flash})
            for (Clocks c : {kClocksTx, kClocksE2, kClocksTg50, kClocksZodiac}) {
                for (bool w : {false, true}) {
                    const CacheCosts k = cacheCosts(core, g, c, w);
                    mix(hash, k.fill);
                    mix(hash, k.writeback);
                    mix(hash, k.buffered);
                    mix(hash, k.uncachedLoad);
                    mix(hash, k.uncachedStore);
                }
            }
}

int main() {
    const Core cores[] = {Core::pxa25x, Core::pxa27x, Core::arm920t};

    // Every class has a cost in every table, and every class is reached by some word on some core: a
    // sweep of every Thumb halfword and random ARM words, and the known words below.
    for (Core core : cores) {
        const Table &t = tableFor(core);
        for (unsigned c = 0; c < classCount; c++) {
            const uint32_t one = classCost(t, Class(c), 1);
            expect(one > 0, "class has a cost", c, 1);
        }
        unsigned words = 0;
        for (uint32_t h = 0; h < 0x10000; h++) reached[thumbClass(h, core, words)] = true;
        uint32_t x = 0x12345678;
        for (uint32_t i = 0; i < 1u << 22; i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            reached[armClass(x, core, words)] = true;
        }
    }
    // XScale manual figures (issue cycles) for passed instructions, and the fitted ones.
    const Core x = Core::pxa27x;
    arm(x, 0xe0810002u, dp, 1, "ADD r0, r1, r2");
    arm(x, 0xe0810312u, dpRegShift, 2, "ADD r0, r1, r2, LSL r3");
    arm(x, 0xe3a00001u, dp, 1, "MOV r0, #1");
    arm(x, 0xe1500001u, dp, 1, "CMP r0, r1");
    arm(x, 0xe1a0f00eu, dpPc, 5, "MOV pc, lr");
    arm(x, 0xe1b0f00eu, dpPc, 5, "MOVS pc, lr");
    arm(x, 0xe24ff004u, dpPc, 5, "SUB pc, pc, #4");
    arm(x, 0xe0000291u, mul, 1, "MUL r0, r1, r2");
    arm(x, 0xe0100291u, mulS, 3, "MULS r0, r1, r2");
    arm(x, 0xe0203291u, mul, 1, "MLA r0, r1, r2, r3");
    arm(x, 0xe0810392u, mulLong, 1, "UMULL r0, r1, r2, r3");
    arm(x, 0xe0a10392u, mulLongAcc, 2, "UMLAL r0, r1, r2, r3");
    arm(x, 0xe0f10392u, mulLongS, 4, "SMLALS r0, r1, r2, r3");
    arm(x, 0xe0d10392u, mulLongS, 4, "SMULLS r0, r1, r2, r3");
    arm(x, 0xe1000281u, dspMul, 1, "SMLABB r0, r1, r2, r0");
    arm(x, 0xe1600281u, dspMul, 1, "SMULBB r0, r1, r2");
    arm(x, 0xe1210281u, dspMul, 1, "SMLAWB r1, r1, r2, r0");
    arm(x, 0xe1410281u, dspMulLong, 2, "SMLALBB r0, r1, r1, r2");
    arm(x, 0xe16f0f11u, clzSat, 1, "CLZ r0, r1");
    arm(x, 0xe1020051u, clzSat, 1, "QADD r0, r1, r2");
    arm(x, 0xe5910000u, load, 2, "LDR r0, [r1]");
    arm(x, 0xe5d10000u, load, 2, "LDRB r0, [r1]");
    arm(x, 0xe1d100b0u, load, 2, "LDRH r0, [r1]");
    arm(x, 0xe1d100f0u, load, 2, "LDRSH r0, [r1]");
    arm(x, 0xe49df004u, loadPc, 8, "LDR pc, [sp], #4");
    arm(x, 0xe5810000u, store, 1, "STR r0, [r1]");
    arm(x, 0xe1c100b0u, store, 1, "STRH r0, [r1]");
    arm(x, 0xe1c120d0u, loadDouble, 2, "LDRD r2, [r1]");
    arm(x, 0xe1c120f0u, storeDouble, 2, "STRD r2, [r1]");
    arm(x, 0xe1010092u, swp, 5, "SWP r0, r2, [r1]");
    arm(x, 0xe8910003u, ldm, 2 + 2 * kXscale.ldmWord, "LDMIA r1, {r0, r1}");
    arm(x, 0xe8bd00f0u, ldm, 2 + 4 * kXscale.ldmWord, "LDMIA sp!, {r4-r7}");
    arm(x, 0xe92d40f0u, stm, 2 + 5 * kXscale.ldmWord, "STMDB sp!, {r4-r7, lr}");
    arm(x, 0xe8bd8000u, ldmPc, 10, "LDMIA sp!, {pc}");
    arm(x, 0xe8bd80f0u, ldmPc, 7 + 5 * kXscale.ldmWord, "LDMIA sp!, {r4-r7, pc}");
    arm(x, 0xea000000u, branch, kXscale.cls[branch], "B");
    arm(x, 0xeb000000u, branch, kXscale.cls[branch], "BL");
    arm(x, 0xe12fff1eu, branchExchange, 5, "BX lr");
    arm(x, 0xe12fff33u, branchExchange, 5, "BLX r3");
    arm(x, 0xfa000000u, branchExchange, 5, "BLX immediate");
    arm(x, 0xe10f0000u, mrs, 1, "MRS r0, cpsr");
    arm(x, 0xe14f0000u, mrs, 1, "MRS r0, spsr");
    arm(x, 0xe128f000u, msr, 2, "MSR cpsr_f, r0");
    arm(x, 0xe121f000u, msrControl, 6, "MSR cpsr_c, r0");
    arm(x, 0xe161f000u, msr, 2, "MSR spsr_c, r0");
    arm(x, 0xe321f0d3u, msrControl, 6, "MSR cpsr_c, #0xd3");
    arm(x, 0xee070f9au, cp15, 2, "MCR p15, 0, r0, c7, c10, 4");
    arm(x, 0xee110f10u, cp15Read, 4, "MRC p15, 0, r0, c1, c0, 0");
    arm(x, 0xee010f10u, cp15, 2, "MCR p15, 0, r0, c1, c0, 0");
    arm(x, 0xee160e10u, cp14, 8, "MRC p14, 0, r0, c6, c0, 0 (CCLKCFG)");
    arm(x, 0xee060e10u, cp14, 8, "MCR p14, 0, r0, c6, c0, 0 (CLKCFG)");
    arm(x, 0xef000000u, swi, 6, "SWI 0");
    arm(x, 0xe7f000f0u, exception, 6, "undefined");
    arm(x, 0xe1200070u, exception, 6, "BKPT");
    arm(x, 0xf5d1f000u, dp, 1, "PLD [r1]");
    // iWMMXt (PXA27x), the CP0 accumulator (PXA25x).
    arm(x, 0xee000180u, wmmx, 1, "iWMMXt data processing (CDP p1)");
    arm(x, 0xee100110u, wmmx, 1, "TMRC");
    arm(x, 0xec410000u, wmmx, 1, "TMCRR");
    arm(x, 0xedd10100u, wmmxLoadStore, 1, "WLDRD");
    arm(x, 0xedc10100u, wmmxLoadStore, 1, "WSTRD");
    arm(Core::pxa25x, 0xee200010u, dspAcc, 1, "MIA (PXA25x)");
    arm(Core::pxa25x, 0xec410000u, dspAccWrite, 2, "MAR (PXA25x)");
    arm(Core::pxa25x, 0xec510000u, dspAcc, 1, "MRA (PXA25x)");
    arm(Core::pxa25x, 0xee000180u, exception, 6, "CDP p1 on the PXA25x");
    arm(Core::pxa25x, 0xedd10100u, exception, 6, "LDC p1 on the PXA25x");
    expect(conditionFailedCost(x) == 1, "XScale condition failed", conditionFailedCost(x), 1);
    expect(paceOpCost(x) == kXscale.paceOp, "XScale PACE op", paceOpCost(x), kXscale.paceOp);

    // Thumb, priced as its ARM equivalent; a BL or BLX pair is the sum of its halves.
    thumb(x, 0x1888u, dp, 1, "ADDS r0, r1, r2");
    thumb(x, 0x2001u, dp, 1, "MOVS r0, #1");
    thumb(x, 0x4088u, dpRegShift, 2, "LSLS r0, r1");
    thumb(x, 0x4348u, mulS, 3, "MULS r0, r1");
    thumb(x, 0x4770u, branchExchange, 6, "BX lr (Thumb, one more)");
    thumb(x, 0x4798u, branchExchange, 6, "BLX r3");
    thumb(x, 0x46f7u, dpPc, 6, "MOV pc, lr");
    thumb(x, 0x4800u, load, 2, "LDR r0, [pc]");
    thumb(x, 0x6808u, load, 2, "LDR r0, [r1]");
    thumb(x, 0x5888u, load, 2, "LDR r0, [r1, r2]");
    thumb(x, 0x5288u, store, 1, "STRH r0, [r1, r2]");
    thumb(x, 0x8008u, store, 1, "STRH r0, [r1]");
    thumb(x, 0x9800u, load, 2, "LDR r0, [sp]");
    thumb(x, 0xb082u, dp, 1, "SUB sp, #8");
    thumb(x, 0xb510u, stm, 2 + 2 * kXscale.ldmWord, "PUSH {r4, lr}");
    thumb(x, 0xbd10u, ldmPc, 11, "POP {r4, pc}");
    thumb(x, 0xbdf0u, ldmPc, 7 + 5 + 1, "POP {r4-r7, pc}");
    thumb(x, 0xbd00u, ldmPc, 11, "POP {pc}");
    thumb(x, 0xc903u, ldm, 2 + 2 * kXscale.ldmWord, "LDMIA r1!, {r0, r1}");
    thumb(x, 0xd0feu, branch, kXscale.cls[branch], "BEQ");
    thumb(x, 0xe7feu, branch, kXscale.cls[branch], "B");
    thumb(x, 0xdf00u, swi, 6, "SWI 0");
    thumb(x, 0xde00u, exception, 6, "undefined");
    thumb(x, 0xbe00u, exception, 6, "BKPT");
    thumb(x, 0xf000u, thumbBlPrefix, 1, "BL first half");
    thumb(x, 0xf800u, branch, kXscale.cls[branch], "BL second half");
    thumb(x, 0xe800u, branchExchange, 6, "BLX second half");
    expect(thumbPairCost(0xf000f800u, x) == 1 + kXscale.cls[branch], "BL pair", thumbPairCost(0xf000f800u, x),
           1 + kXscale.cls[branch]);
    expect(thumbPairCost(0xf000e800u, x) == 7, "BLX pair", thumbPairCost(0xf000e800u, x), 7);

    // ARM920T (ARMv4T): TRM figures; v5TE-only words are undefined there.
    const Core a = Core::arm920t;
    arm(a, 0xe0810002u, dp, 1, "920T ADD");
    arm(a, 0xe0810312u, dpRegShift, 2, "920T ADD, LSL r3");
    arm(a, 0xe1a0f00eu, dpPc, 3, "920T MOV pc, lr");
    arm(a, 0xe0000291u, mul, 4, "920T MUL");
    arm(a, 0xe0810392u, mulLong, 5, "920T UMULL");
    arm(a, 0xe8bd8000u, ldmPc, 5, "920T LDMIA sp!, {pc}");
    arm(a, 0xee110f10u, cp15Read, 1, "920T MRC p15");
    arm(a, 0xe5910000u, load, 1, "920T LDR");
    arm(a, 0xe49df004u, loadPc, 5, "920T LDR pc");
    arm(a, 0xe8910001u, ldm, 2, "920T LDMIA r1, {r0}");
    arm(a, 0xe8bd00f0u, ldm, 4, "920T LDMIA sp!, {r4-r7}");
    arm(a, 0xe8bd80f0u, ldmPc, 9, "920T LDMIA sp!, {r4-r7, pc}");
    arm(a, 0xea000000u, branch, 3, "920T B");
    arm(a, 0xe12fff1eu, branchExchange, 3, "920T BX lr");
    arm(a, 0xe121f000u, msrControl, 3, "920T MSR cpsr_c");
    arm(a, 0xe16f0f11u, exception, 3, "920T CLZ");
    arm(a, 0xfa000000u, exception, 3, "920T BLX immediate");
    arm(a, 0xe1c120d0u, exception, 3, "920T LDRD");
    arm(a, 0xee000180u, exception, 3, "920T CDP p1");
    thumb(a, 0x4770u, branchExchange, 3, "920T Thumb BX lr");
    thumb(a, 0x4798u, exception, 3, "920T Thumb BLX r3");
    thumb(a, 0xe800u, exception, 3, "920T Thumb BLX second half");
    expect(thumbPairCost(0xf000f800u, a) == 4, "920T BL pair", thumbPairCost(0xf000f800u, a), 4);
    expect(paceOpCost(a) == kArm920.paceOp, "920T PACE op", paceOpCost(a), kArm920.paceOp);
    expect(maxInstructionCost(x) == 7 + 16u * kXscale.ldmWord, "XScale largest cost", maxInstructionCost(x),
           7 + 16u * kXscale.ldmWord);
    expect(maxInstructionCost(a) == 20, "920T largest cost", maxInstructionCost(a), 20);

    for (unsigned c = 0; c < classCount; c++) expect(reached[c], "class reached by a word", c, 1);

    // Regions.
    expect(pxaRegion(0xa0000000u) == sdram, "PXA SDRAM", pxaRegion(0xa0000000u), sdram);
    expect(pxaRegion(0xa3ffffffu) == sdram, "PXA SDRAM end", pxaRegion(0xa3ffffffu), sdram);
    expect(pxaRegion(0x5c000000u) == sram, "PXA SRAM", pxaRegion(0x5c000000u), sram);
    expect(pxaRegion(0x5c040000u) == device, "PXA past SRAM", pxaRegion(0x5c040000u), device);
    expect(pxaRegion(0x00000000u) == flash, "PXA flash", pxaRegion(0), flash);
    expect(pxaRegion(0x04000000u) == staticBus, "PXA CS1", pxaRegion(0x04000000u), staticBus);
    expect(pxaRegion(0x14000000u) == staticBus, "PXA CS5", pxaRegion(0x14000000u), staticBus);
    expect(pxaRegion(0x40a00010u) == device, "PXA OSCR", pxaRegion(0x40a00010u), device);
    expect(pxaRegion(0x48000000u) == device, "PXA memory controller", pxaRegion(0x48000000u), device);
    expect(imx1Region(0x08000000u) == sdram, "i.MX1 SDRAM", imx1Region(0x08000000u), sdram);
    expect(imx1Region(0x0c000000u) == sdram, "i.MX1 CSD1", imx1Region(0x0c000000u), sdram);
    expect(imx1Region(0x00300000u) == sram, "i.MX1 eSRAM", imx1Region(0x00300000u), sram);
    expect(imx1Region(0x10000000u) == flash, "i.MX1 CS0", imx1Region(0x10000000u), flash);
    expect(imx1Region(0x12000000u) == staticBus, "i.MX1 CS1", imx1Region(0x12000000u), staticBus);
    expect(imx1Region(0x16000000u) == modelled, "i.MX1 CS5", imx1Region(0x16000000u), modelled);
    expect(imx1Region(0x00209000u) == device, "i.MX1 AIPI", imx1Region(0x00209000u), device);

    // Clocks from CCCR and CLKCFG (PXA25x table 3-1, PXA27x figure 3-1's equations).
    expect(kClocksTx.core == 312000 && kClocksTx.bus == 208000 && kClocksTx.memory == 104000, "T|X clocks",
           kClocksTx.core, 312000);
    expect(kClocksE2.core == 199066 && kClocksE2.bus == 99533 && kClocksE2.memory == 99533, "E2 clocks",
           kClocksE2.core, 199066);
    expect(kClocksTg50.core == 199066 && kClocksTg50.bus == 49766 && kClocksTg50.memory == 99533, "TG50 clocks",
           kClocksTg50.core, 199066);
    const Clocks txRun = pxa27xClocks(0x190, 0x8);
    expect(txRun.core == 208000 && txRun.bus == 208000, "T|X run mode", txRun.core, 208000);
    const Clocks txSlowBus = pxa27xClocks(0x190, 0x1);
    expect(txSlowBus.core == 312000 && txSlowBus.bus == 104000, "T|X turbo without fast bus", txSlowBus.bus, 104000);
    const Clocks pxa25xReset = pxa25xClocks(0x121, false);
    expect(pxa25xReset.core == 99533 && pxa25xReset.bus == 49766, "PXA25x reset", pxa25xReset.core, 99533);

    // Region charges in core clocks: {device clocks, region, load cost, store cost}.
    struct Charge {
        Core core;
        Clocks clocks;
        Region region;
        uint32_t load, store;
        const char *what;
    } charges[] = {
        {Core::pxa27x, kClocksTx, sdram, 0, 0, "T|X SDRAM"},
        {Core::pxa27x, kClocksTx, flash, 0, 0, "T|X flash"},
        {Core::pxa27x, kClocksTx, staticBus, 144, 144, "T|X static"},
        {Core::pxa27x, kClocksTx, device, 6, 6, "T|X device"},
        {Core::pxa27x, kClocksTx, sram, 0, 0, "T|X SRAM"},
        {Core::pxa25x, kClocksE2, sdram, 0, 0, "E2 SDRAM"},
        {Core::pxa25x, kClocksE2, staticBus, 96, 96, "E2 static"},
        {Core::pxa25x, kClocksE2, device, 8, 8, "E2 device"},
        {Core::pxa25x, kClocksTg50, sdram, 0, 0, "TG50 SDRAM"},
        {Core::pxa25x, kClocksTg50, staticBus, 96, 96, "TG50 static"},
        {Core::pxa25x, kClocksTg50, device, 16, 16, "TG50 device"},
        {Core::pxa27x, kClocksTx, modelled, 0, 0, "PXA modelled"},
        {Core::arm920t, kClocksZodiac, sdram, 0, 0, "Zodiac SDRAM"},
        {Core::arm920t, kClocksZodiac, sram, 0, 0, "Zodiac eSRAM"},
        {Core::arm920t, kClocksZodiac, flash, 0, 0, "Zodiac flash"},
        {Core::arm920t, kClocksZodiac, staticBus, 8, 8, "Zodiac CS1-CS4"},
        {Core::arm920t, kClocksZodiac, device, 8, 8, "Zodiac device"},
        {Core::arm920t, kClocksZodiac, modelled, 0, 0, "Zodiac CS5"},
    };
    for (const Charge &c : charges) {
        const uint32_t l = regionCost(c.core, c.region, false, c.clocks), s = regionCost(c.core, c.region, true, c.clocks);
        expect(l == c.load, c.what, l, c.load);
        expect(s == c.store, c.what, s, c.store);
    }

    // One hash over every Thumb halfword's and a fixed ARM sweep's costs on every core, and the
    // region charges at the device clocks: the same number on every host.
    uint64_t hash = 0xcbf29ce484222325ull;
    auto mix = [&hash](uint32_t v) {
        for (int i = 0; i < 4; i++) {
            hash ^= (v >> (8 * i)) & 0xff;
            hash *= 0x100000001b3ull;
        }
    };
    for (Core core : cores) {
        for (uint32_t h = 0; h < 0x10000; h++) mix(thumbCost(h, core));
        for (uint32_t w = 0; w < 1u << 20; w++) mix(armCost(w * 0x0fffu + (w << 20), core));
        for (unsigned r = 0; r < regionCount; r++)
            for (Clocks c : {kClocksTx, kClocksE2, kClocksTg50, kClocksZodiac}) {
                mix(regionCost(core, Region(r), false, c));
                mix(regionCost(core, Region(r), true, c));
            }
        mix(paceOpCost(core));
        mix(conditionFailedCost(core));
    }
    const uint64_t wantHash = 0x7e5683f894f93293ull;
    if (hash != wantHash) {
        std::printf("FAIL cost hash: got 0x%016llx, want 0x%016llx\n", (unsigned long long)hash,
                    (unsigned long long)wantHash);
        failures++;
    }

    uint64_t cacheHash = 0xcbf29ce484222325ull;
    cacheTests(cacheHash, mixHash);
    const uint64_t wantCacheHash = 0x51ff708747dbc0b5ull;
    if (cacheHash != wantCacheHash) {
        std::printf("FAIL cache hash: got 0x%016llx, want 0x%016llx\n", (unsigned long long)cacheHash,
                    (unsigned long long)wantCacheHash);
        failures++;
    }

    if (failures) {
        std::printf("test-xscale-cost: %d failures\n", failures);
        return 1;
    }
    std::printf("test-xscale-cost: ok (hash 0x%016llx, cache hash 0x%016llx)\n", (unsigned long long)hash,
                (unsigned long long)cacheHash);
    return 0;
}
