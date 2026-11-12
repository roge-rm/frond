//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _RAM_H_
#define _RAM_H_

#include <cstdint>

#include "mem.h"

struct ArmRam;
class SoC;
struct MemoryBuffer;

struct ArmRam* ramInit(struct ArmMem* mem, class SoC* soc, uint32_t adr, uint32_t sz,
                       const struct MemoryBuffer* buf, bool primary);

template <int size, bool write>
bool ramAccessF(void* userData, uint32_t pa, void* bufP);

bool ramAccessF(void* userData, uint32_t pa, uint_fast8_t size, bool write, void* bufP);

void ramSetFramebuffer(struct ArmRam* ram, uint32_t base, uint32_t size);
// No frame buffer in this RAM (it is in SRAM): writes never mark the screen dirty.
void ramSetNoFramebuffer(struct ArmRam* ram);

void* ramResolveAddress(struct ArmRam* ram, uint32_t pa, uint32_t size);

// Called with the host address of the page, on the first write to a flagged line (one the
// JIT has translated code from, or a watched fetch buffer line), with the written bytes'
// offset in the page and size. Every flag of the page is clear by then.
using RamCodeWritten = void (*)(void* context, uint8_t* hostPage, uint32_t offset, uint32_t size);
void ramMarkDirty(struct ArmRam* ram, uint32_t base, uint32_t size);
// For the JIT's direct stores. Host address of the 4 KiB page at pa, after marking it
// dirty and, where it overlaps the frame buffer, the frame buffer dirty, as if the whole page
// had been written. Null while the page has a code flag.
uint8_t* ramDirectWritePage(struct ArmRam* ram, uint32_t pa);
// Host range whose writes can mark the frame buffer dirty, if there is one.
bool ramFramebufferHost(struct ArmRam* ram, uint8_t** host, uint32_t* size);

#endif
