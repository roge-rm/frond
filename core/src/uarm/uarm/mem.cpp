//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "mem.h"
#include "mem_internal.h"

#include <cstdlib>
#include <cstring>

#include "RAM.h"
#include "ROM.h"
#include "cputil.h"

struct ArmMem *memInit(void) {
    struct ArmMem *mem = (struct ArmMem *)malloc(sizeof(*mem));

    if (!mem) ERR("cannot alloc MEM");

    memset(mem, 0, sizeof(*mem));
    mem->hostTlb.gen = 1;

    return mem;
}

void memDeinit(struct ArmMem *mem) { (void)mem; }

// A region's end is stored in 32 bits, with 0 meaning a free slot, so it has to end below 4 GiB.
static bool memRegionFits(uint32_t pa, uint32_t sz) { return uint64_t(pa) + sz <= 0xffffffffull; }

static bool checkForIntersection(struct ArmMem *mem, uint32_t pa, uint32_t sz) {
    uint_fast8_t i;

    for (i = 0; i < NUM_MEM_REGIONS; i++) {
        if (!mem->regions[i].ub) continue;
        if ((mem->regions[i].pa <= pa && mem->regions[i].ub > pa) ||
            (pa <= mem->regions[i].pa && pa + sz > mem->regions[i].pa))
            return true;  // intersection -> fail
    }

    return false;
}

static bool memRegionAddFixed(struct ArmMem *mem, uint8_t region, uint32_t pa, uint32_t sz,
                              ArmMemAccessF af, void *uD) {
    if (!memRegionFits(pa, sz)) return false;

    mem->regions[region].pa = pa;
    mem->regions[region].aF = af;
    mem->regions[region].uD = uD;
    mem->regions[region].ub = pa + sz;

    return true;
}

bool memRegionAdd(struct ArmMem *mem, uint32_t pa, uint32_t sz, ArmMemAccessF aF, void *uD) {
    uint_fast8_t i;

    if (!memRegionFits(pa, sz)) return false;
    if (checkForIntersection(mem, pa, sz)) return false;

    // find a free region and put it there

    for (i = REGION_BASE; i < NUM_MEM_REGIONS; i++) {
        if (mem->regions[i].ub == 0) {
            hostTlbFlush(&mem->hostTlb);
            mem->regions[i].pa = pa;
            mem->regions[i].aF = aF;
            mem->regions[i].uD = uD;
            mem->regions[i].ub = pa + sz;

            return true;
        }
    }

    // fail miserably

    return false;
}

bool memRegionAddRam(struct ArmMem *mem, uint32_t pa, uint32_t sz, ArmMemAccessF af, void *uD) {
    return memRegionAddFixed(mem, REGION_RAM, pa, sz, af, uD);
}

static bool memHeldRamAccess(void *, uint32_t, uint_fast8_t size, bool write, void *buf) {
    if (!write) memset(buf, 0, size);
    return true;
}

bool memRamHeld(const struct ArmMem *mem) { return mem->heldRam.ub != 0; }

// A device slot backed by the same RAM buffer, like the E2 RAM probe mirror.
static bool memIsRamMirror(const struct ArmMemRegion &region, const struct ArmMemRegion &ram) {
    return region.ub && region.aF == ram.aF && region.uD != ram.uD && region.uD &&
           ((struct ArmRam *)region.uD)->buf.buffer == ((struct ArmRam *)ram.uD)->buf.buffer;
}

bool memHoldRam(struct ArmMem *mem, bool held) {
    if (held == memRamHeld(mem)) return true;
    hostTlbFlush(&mem->hostTlb);
    if (!held) {
        // Held mirrors keep their ArmRam as uD; the stand-in slot has none.
        for (int i = REGION_BASE; i < NUM_MEM_REGIONS; i++)
            if (mem->regions[i].aF == memHeldRamAccess && mem->regions[i].uD)
                mem->regions[i].aF = mem->heldRam.aF;
        mem->regions[mem->heldSlot] = {};
        mem->regions[REGION_RAM] = mem->heldRam;
        mem->heldRam = {};
        return true;
    }
    // Swap the RAM region for a device slot over the same range. The RAM fast paths
    // only test the REGION_RAM bounds, so they see nothing while it is held.
    for (int i = REGION_BASE; i < NUM_MEM_REGIONS; i++) {
        if (mem->regions[i].ub) continue;
        const struct ArmMemRegion ram = mem->regions[REGION_RAM];
        for (int j = REGION_BASE; j < NUM_MEM_REGIONS; j++)
            if (memIsRamMirror(mem->regions[j], ram)) mem->regions[j].aF = memHeldRamAccess;
        mem->heldRam = ram;
        mem->heldSlot = i;
        mem->regions[i] = {ram.pa, ram.ub, memHeldRamAccess, nullptr};
        mem->regions[REGION_RAM] = {};
        return true;
    }
    return false;
}

bool memRegionRemove(struct ArmMem *mem, uint32_t pa, uint32_t size, void *context) {
    if (!context || !size || !memRegionFits(pa, size)) return false;
    for (unsigned i = REGION_BASE; i < NUM_MEM_REGIONS; ++i) {
        auto &region = mem->regions[i];
        if (region.pa != pa || region.ub != pa + size || region.uD != context) continue;
        region = {};
        return true;
    }
    return false;
}

bool memRegionAddRom(struct ArmMem *mem, uint32_t pa, uint32_t sz, ArmMemAccessF af, void *uD) {
    return memRegionAddFixed(mem, REGION_ROM, pa, sz, af, uD);
}

bool memIsBufferRange(ArmMem* mem, uint32_t addr, uint32_t size, bool write) {
    const uint64_t end = uint64_t(addr) + size;
    if (!size || end > 0x100000000ull) return false;
    const auto contains = [&](const ArmMemRegion& r) {
        return r.ub && r.pa <= addr && end <= r.ub;
    };
    return contains(mem->regions[REGION_RAM]) || (!write && contains(mem->regions[REGION_ROM]));
}

template <int size, bool write>
bool memAccess(struct ArmMem *mem, uint32_t addr, void *buf) {
    const uint32_t ub = addr + size;
    if (ub < addr) return false;  // wrapped past 0xffffffff, so in no region

    if (mem->regions[REGION_RAM].pa <= addr && mem->regions[REGION_RAM].ub >= ub) {
        return ramAccessF<size, write>(mem->regions[REGION_RAM].uD, addr, buf);
    }

    if (mem->regions[REGION_ROM].pa <= addr && mem->regions[REGION_ROM].ub >= ub)
        return romAccessF<size, write>(mem->regions[REGION_ROM].uD, addr, buf);

    bool ret = false;
    uint_fast8_t i;

    for (i = REGION_BASE; i < NUM_MEM_REGIONS; i++) {
        if (mem->regions[i].pa <= addr && mem->regions[i].ub >= ub) {
            ret = mem->regions[i].aF(mem->regions[i].uD, addr, size, write, buf);
            break;
        }
    }

    return ret;
}

#define DECLARE_MEM_ACCESS_SIZE(sz)                                                   \
    template bool memAccess<sz, true>(struct ArmMem * mem, uint32_t addr, void *buf); \
    template bool memAccess<sz, false>(struct ArmMem * mem, uint32_t addr, void *buf);

DECLARE_MEM_ACCESS_SIZE(1)
DECLARE_MEM_ACCESS_SIZE(2)
DECLARE_MEM_ACCESS_SIZE(4)
DECLARE_MEM_ACCESS_SIZE(8)
DECLARE_MEM_ACCESS_SIZE(16)
DECLARE_MEM_ACCESS_SIZE(32)
DECLARE_MEM_ACCESS_SIZE(64)

bool memAccess(struct ArmMem *mem, uint32_t addr, uint_fast8_t size, bool write, void *buf) {
    const uint32_t ub = addr + size;
    if (ub < addr) return false;  // wrapped past 0xffffffff, so in no region

    if (mem->regions[REGION_RAM].pa <= addr && mem->regions[REGION_RAM].ub >= ub) {
        return ramAccessF(mem->regions[REGION_RAM].uD, addr, size, write, buf);
    }

    if (mem->regions[REGION_ROM].pa <= addr && mem->regions[REGION_ROM].ub >= ub)
        return romAccessF(mem->regions[REGION_ROM].uD, addr, size, write, buf);

    bool ret = false;
    uint_fast8_t i;

    for (i = REGION_BASE; i < NUM_MEM_REGIONS; i++) {
        if (mem->regions[i].pa <= addr && mem->regions[i].ub >= ub) {
            ret = mem->regions[i].aF(mem->regions[i].uD, addr, size, write, buf);
            break;
        }
    }

    return ret;
}

bool memIsInstructionFetch(const ArmMem* mem) { return mem->instructionFetch; }

template <int size>
bool memInstructionFetch(struct ArmMem *mem, uint32_t addr, void *buf) {
    const uint32_t ub = addr + size;
    if (ub < addr) return false;  // wrapped past 0xffffffff, so in no region

    if (mem->regions[REGION_RAM].pa <= addr && mem->regions[REGION_RAM].ub >= ub)
        return ramAccessF<size, false>(mem->regions[REGION_RAM].uD, addr, buf);

    if (mem->regions[REGION_ROM].pa <= addr && mem->regions[REGION_ROM].ub >= ub)
        return romInstructionFetch<size>(mem->regions[REGION_ROM].uD, addr, buf);

    bool ret = false;
    uint_fast8_t i;

    for (i = REGION_BASE; i < NUM_MEM_REGIONS; i++) {
        if (mem->regions[i].pa <= addr && mem->regions[i].ub >= ub) {
            const bool previous = mem->instructionFetch;
            mem->instructionFetch = true;
            ret = mem->regions[i].aF(mem->regions[i].uD, addr, size, false, buf);
            mem->instructionFetch = previous;
            break;
        }
    }

    return ret;
}

bool memIsMemory(struct ArmMem *mem, uint32_t addr, uint32_t size) {
    const uint32_t ub = addr + size;
    if (ub < addr) return false;
    const ArmMemAccessF ramF = ramAccessF, romF = romAccessF;
    for (uint_fast8_t i = 0; i < NUM_MEM_REGIONS; i++) {
        const struct ArmMemRegion &r = mem->regions[i];
        if (r.pa <= addr && r.ub >= ub)
            return i == REGION_RAM || i == REGION_ROM || r.aF == ramF || r.aF == romF;
    }
    return false;
}

template bool memInstructionFetch<1>(struct ArmMem *mem, uint32_t addr, void *buf);
template bool memInstructionFetch<2>(struct ArmMem *mem, uint32_t addr, void *buf);
template bool memInstructionFetch<4>(struct ArmMem *mem, uint32_t addr, void *buf);
template bool memInstructionFetch<8>(struct ArmMem *mem, uint32_t addr, void *buf);
template bool memInstructionFetch<16>(struct ArmMem *mem, uint32_t addr, void *buf);
template bool memInstructionFetch<32>(struct ArmMem *mem, uint32_t addr, void *buf);
template bool memInstructionFetch<64>(struct ArmMem *mem, uint32_t addr, void *buf);

// Every RAM region over the same buffer (the RAM, its mirrors) shares one code flag array,
// indexed by 64-byte line of the buffer.
static size_t memRamCodeLineCount(const struct ArmRam *ram) {
    const size_t bytes = ram->buf.size > ram->sz ? ram->buf.size : ram->sz;
    return (bytes + 4095) >> 6;
}

template <typename F>
static void memForEachRam(struct ArmMem *mem, F f) {
    const ArmMemAccessF ramF = ramAccessF;
    for (int i = 0; i < NUM_MEM_REGIONS; i++)
        if (mem->regions[i].ub && mem->regions[i].aF == ramF && mem->regions[i].uD)
            f(mem->regions[i], (struct ArmRam *)mem->regions[i].uD);
    if (mem->heldRam.ub && mem->heldRam.uD) f(mem->heldRam, (struct ArmRam *)mem->heldRam.uD);
}

uint8_t *memWatchRamCode(struct ArmMem *mem, uint32_t pa, void (*written)(void *, uint8_t *, uint32_t, uint32_t), void *context,
                         uint8_t **lines) {
    struct ArmRam *ram = nullptr;
    memForEachRam(mem, [&](const struct ArmMemRegion &r, struct ArmRam *candidate) {
        if (!ram && r.pa <= pa && pa < r.ub && r.aF != nullptr) ram = candidate;
    });
    if (!ram || pa - ram->adr >= ram->sz) return nullptr;
    if (!ram->codeLines) {
        uint8_t *flags = (uint8_t *)calloc(memRamCodeLineCount(ram), 1);
        if (!flags) ERR("cannot alloc code page flags");
        memForEachRam(mem, [&](const struct ArmMemRegion &, struct ArmRam *other) {
            if (other->buf.buffer != ram->buf.buffer || other->codeLines) return;
            other->codeLines = flags;
            other->codeWritten = written;
            other->codeContext = context;
        });
    }
    const uint32_t offset = (pa - ram->adr) & ~0xfffu;
    *lines = ram->codeLines + (offset >> 6);
    return ram->buf.buffer + offset;
}

void memForgetRamCode(struct ArmMem *mem) {
    memForEachRam(mem, [&](const struct ArmMemRegion &, struct ArmRam *ram) {
        if (ram->codeLines) memset(ram->codeLines, 0, memRamCodeLineCount(ram));
    });
}
