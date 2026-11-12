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
};

static inline uint64_t hostTlbKey(uint32_t gen, uint32_t va, bool privileged) {
    return (uint64_t(gen) << 32) | ((va >> 12) << 1) | (privileged ? 1 : 0);
}

static inline void hostTlbFlushDomains(struct HostTlb *tlb) {
    if (++tlb->gen == 0) {
        for (auto &e : tlb->read) e = {};
        for (auto &e : tlb->write) e = {};
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
