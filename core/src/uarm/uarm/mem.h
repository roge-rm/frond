//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _MEM_H_
#define _MEM_H_

#include <cstdint>

struct ArmMem;

typedef bool (*ArmMemAccessF)(void* userData, uint32_t pa, uint_fast8_t size, bool write,
                              void* buf);

struct ArmMem* memInit(void);
void memDeinit(struct ArmMem* mem);

bool memRegionAdd(struct ArmMem* mem, uint32_t pa, uint32_t sz, ArmMemAccessF af, void* uD);
// Quiesced bus only, removes one matching device region without freeing its context.
bool memRegionRemove(struct ArmMem* mem, uint32_t pa, uint32_t sz, void* uD);
bool memRegionAddRam(struct ArmMem* mem, uint32_t pa, uint32_t sz, ArmMemAccessF af, void* uD);
bool memRegionAddRom(struct ArmMem* mem, uint32_t pa, uint32_t sz, ArmMemAccessF af, void* uD);

// While held, primary RAM and its mirrors reach no memory: reads return 0 and writes are
// dropped, as with SDRAM still in self-refresh. Returns false if no slot is free.
bool memHoldRam(struct ArmMem* mem, bool held);
bool memRamHeld(const struct ArmMem* mem);
// For the JIT: the host address of the RAM page holding pa, or null when pa isn't RAM, and
// in *lines the page's code flags, one per 64 bytes. A CPU, DMA or ramMarkDirty write to a
// line whose flag is set clears the page's flags and calls written(context, page, offset,
// size). memForgetRamCode clears every flag.
uint8_t* memWatchRamCode(struct ArmMem* mem, uint32_t pa, void (*written)(void*, uint8_t*, uint32_t, uint32_t), void* context,
                         uint8_t** lines);
void memForgetRamCode(struct ArmMem* mem);

// True only for a complete RAM/ROM range, never MMIO. DMA tails may split these accesses.
bool memIsBufferRange(ArmMem* mem, uint32_t addr, uint32_t size, bool write);

template <int size, bool write>
bool memAccess(struct ArmMem* mem, uint32_t addr, void* buf);

bool memAccess(struct ArmMem* mem, uint32_t addr, uint_fast8_t size, bool write, void* buf);

template <int size>
bool memInstructionFetch(struct ArmMem* mem, uint32_t addr, void* buf);
// Inside a device callback, identifies instruction fetches rather than data reads.
bool memIsInstructionFetch(const ArmMem* mem);
// Whether size bytes at addr are all in RAM or ROM (or a mirror), so reading them has no
// effect on a device.
bool memIsMemory(struct ArmMem* mem, uint32_t addr, uint32_t size);

#endif
