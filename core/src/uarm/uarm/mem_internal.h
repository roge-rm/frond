// ArmMem layout and an inlined RAM-first access, shared with the CPU's memory path.
#ifndef FROND_MEM_INTERNAL_H
#define FROND_MEM_INTERNAL_H

#include <cstdint>

#include "RAM_internal.h"
#include "cputil.h"
#include "mem.h"

#define NUM_MEM_REGIONS 128

#define REGION_RAM 0
#define REGION_ROM 1
#define REGION_BASE 2

struct ArmMemRegion {
    uint32_t pa;
    uint32_t ub;
    ArmMemAccessF aF;
    void *uD;
};

// Host-pointer TLB for the CPU's RAM and ROM accesses, indexed by virtual page. An entry
// is only valid for the gen it was filled in; gen moves on every TLB flush or refill,
// domain change and memory map change, and is never 0.
#define HOST_TLB_BITS 10
#define HOST_TLB_SIZE (1u << HOST_TLB_BITS)

struct HostTlbEntry {
    uint64_t key;      // gen << 32 | page << 1 | privileged
    uintptr_t addend;  // reads: host address - va; writes: pa - va
};

// The Fast engine's device pages: a virtual page the MMU TLB maps to a device region, with
// that region's bounds and handler, so a JIT callback to it skips the translation, the
// region search and the host TLB fill. Keyed and dropped like the host TLB entries, and
// emptied whenever a region is added, removed or replaced.
#define HOST_TLB_IO_BITS 7
#define HOST_TLB_IO_SIZE (1u << HOST_TLB_IO_BITS)

struct HostTlbIoEntry {
    uint64_t key;    // as HostTlbEntry's
    uint32_t delta;  // pa - va
    uint32_t pa, ub;  // the region's bounds
    ArmMemAccessF aF;
    void *uD;
};

static inline uint32_t hostTlbIoIndex(uint32_t va) { return ((va >> 12) * 0x9e3779b1u) >> (32 - HOST_TLB_IO_BITS); }

struct HostTlb {
    uint32_t gen;
    // Moves on with every flush but a domain change, so the JIT can tell a domain change
    // (where only pages in changed domains answer differently) from the rest.
    uint32_t epoch;
    // Moves on with a domain change that only adds rights, which keeps every entry.
    uint32_t gains;
    struct ArmRam *ram;  // primary RAM when the write entries were filled
    struct HostTlbEntry read[HOST_TLB_SIZE];
    struct HostTlbEntry write[HOST_TLB_SIZE];
    struct HostTlbIoEntry ioRead[HOST_TLB_IO_SIZE];
    struct HostTlbIoEntry ioWrite[HOST_TLB_IO_SIZE];
};

static inline void hostTlbForgetIo(struct HostTlb *tlb) {
    for (auto &e : tlb->ioRead) e = {};
    for (auto &e : tlb->ioWrite) e = {};
}

static inline uint64_t hostTlbKey(uint32_t gen, uint32_t va, bool privileged) {
    return (uint64_t(gen) << 32) | ((va >> 12) << 1) | (privileged ? 1 : 0);
}

static inline void hostTlbFlushDomains(struct HostTlb *tlb) {
    if (++tlb->gen == 0) {
        for (auto &e : tlb->read) e = {};
        for (auto &e : tlb->write) e = {};
        hostTlbForgetIo(tlb);
        tlb->gen = 1;
        tlb->epoch++;
    }
}

static inline void hostTlbFlush(struct HostTlb *tlb) {
    tlb->epoch++;
    hostTlbFlushDomains(tlb);
}

struct ArmMem {
    bool instructionFetch;
    struct ArmMemRegion regions[NUM_MEM_REGIONS];
    // Primary RAM while memHoldRam has it out of the address map, and the slot that
    // stands in for it.
    struct ArmMemRegion heldRam;
    int heldSlot;
    struct HostTlb hostTlb;
};

// memAccess with its first branch (primary RAM) inlined. Other addresses and sizes
// go through memAccess, which repeats the RAM test and then checks ROM and devices.
template <int size, bool write>
static FORCE_INLINE bool memAccessRamInline(struct ArmMem* mem, uint32_t addr, void* buf) {
    if constexpr (size <= 8) {
        const uint32_t ub = addr + size;
        const struct ArmMemRegion& ram = mem->regions[REGION_RAM];
        // ub < addr means the access wrapped past 0xffffffff; memAccess rejects it.
        if (ub >= addr && ram.pa <= addr && ram.ub >= ub)
            return ramAccessInline<size, write>(ram.uD, addr, buf);
    }
    return memAccess<size, write>(mem, addr, buf);
}

#endif
