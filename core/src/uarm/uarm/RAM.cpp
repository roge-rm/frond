//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "RAM.h"
#include "RAM_internal.h"

#include <cstdlib>
#include <cstring>

#include "CPEndian.h"
#include "SoC.h"
#include "cputil.h"
#include "memory_buffer.h"

void ramSetFramebufferDirty(struct ArmRam* ram, uint32_t pa, uint32_t size) {
    ram->soc->SetFramebufferDirty(pa, size);
}

template <int size, bool write>
bool ramAccessF(void* userData, uint32_t pa, void* bufP) {
    return ramAccessInline<size, write>(userData, pa, bufP);
}

#define DECLARE_ACCESS_F_SIZE(size)                                                \
    template bool ramAccessF<size, true>(void* userData, uint32_t pa, void* bufP); \
    template bool ramAccessF<size, false>(void* userData, uint32_t pa, void* bufP);

DECLARE_ACCESS_F_SIZE(1)
DECLARE_ACCESS_F_SIZE(2)
DECLARE_ACCESS_F_SIZE(4)
DECLARE_ACCESS_F_SIZE(8)
DECLARE_ACCESS_F_SIZE(16)
DECLARE_ACCESS_F_SIZE(32)
DECLARE_ACCESS_F_SIZE(64)

bool ramAccessF(void* userData, uint32_t pa, uint_fast8_t size, bool write, void* bufP) {
    if (write) {
        switch (size) {
            case 1:
                return ramAccessF<1, true>(userData, pa, bufP);

            case 2:
                return ramAccessF<2, true>(userData, pa, bufP);

            case 4:
                return ramAccessF<4, true>(userData, pa, bufP);

            case 8:
                return ramAccessF<8, true>(userData, pa, bufP);

            case 16:
                return ramAccessF<16, true>(userData, pa, bufP);

            case 32:
                return ramAccessF<32, true>(userData, pa, bufP);

            case 64:
                return ramAccessF<64, true>(userData, pa, bufP);

            default:
                return false;
        }
    } else {
        switch (size) {
            case 1:
                return ramAccessF<1, false>(userData, pa, bufP);

            case 2:
                return ramAccessF<2, false>(userData, pa, bufP);

            case 4:
                return ramAccessF<4, false>(userData, pa, bufP);

            case 8:
                return ramAccessF<8, false>(userData, pa, bufP);

            case 16:
                return ramAccessF<16, false>(userData, pa, bufP);

            case 32:
                return ramAccessF<32, false>(userData, pa, bufP);

            case 64:
                return ramAccessF<64, false>(userData, pa, bufP);

            default:
                return false;
        }
    }
}

void ramSetFramebuffer(struct ArmRam* ram, uint32_t base, uint32_t size) {
    if (size > 0) {
        // A write of n bytes from framebufferStart_n on can touch the frame buffer. Clamped at 0:
        // the Zire's SRAM frame buffer starts at offset 0, where start - n would wrap and no
        // wider write would mark it dirty.
        const auto before = [](uint32_t start, uint32_t n) { return start >= n ? start - n : 0; };
        ram->framebufferStart = base - ram->adr;
        ram->framebufferStart_2 = before(ram->framebufferStart, 2);
        ram->framebufferStart_4 = before(ram->framebufferStart, 4);
        ram->framebufferStart_8 = before(ram->framebufferStart, 8);
        ram->framebufferStart_16 = before(ram->framebufferStart, 16);
        ram->framebufferStart_32 = before(ram->framebufferStart, 32);
        ram->framebufferStart_64 = before(ram->framebufferStart, 64);
        ram->framebufferEnd = ram->framebufferStart + size;
    } else {
        ram->framebufferStart = base - ram->adr;
        ram->framebufferStart_2 = 0;
        ram->framebufferStart_4 = 0;
        ram->framebufferStart_8 = 0;
        ram->framebufferStart_16 = 0;
        ram->framebufferStart_32 = 0;
        ram->framebufferStart_64 = 0;
        ram->framebufferEnd = 0xffffffff;
    }
}

void ramSetNoFramebuffer(struct ArmRam* ram) {
    ram->framebufferStart = ram->framebufferStart_2 = ram->framebufferStart_4 = ram->framebufferStart_8 =
        ram->framebufferStart_16 = ram->framebufferStart_32 = ram->framebufferStart_64 = 0xffffffff;
    ram->framebufferEnd = 0;
}

struct ArmRam* ramInit(struct ArmMem* mem, class SoC* soc, uint32_t adr, uint32_t sz,
                       const struct MemoryBuffer* buf, bool primary) {
    struct ArmRam* ram = (struct ArmRam*)malloc(sizeof(*ram));

    if (!ram) ERR("cannot alloc RAM at 0x%08x", adr);

    memset(ram, 0, sizeof(*ram));

    ram->soc = soc;
    ram->adr = adr;
    ram->sz = sz;
    memcpy(&ram->buf, buf, sizeof(ram->buf));

    ramSetFramebuffer(ram, 0, 0);

    if (!(primary ? memRegionAddRam(mem, adr, sz, ramAccessF, ram)
                  : memRegionAdd(mem, adr, sz, ramAccessF, ram))) {
        ERR("cannot add RAM at 0x%08x to MEM\n", adr);
    }

    return ram;
}

void* ramResolveAddress(struct ArmRam* ram, uint32_t pv, uint32_t size) {
    if (pv < ram->adr || uint64_t(pv) + size > uint64_t(ram->adr) + ram->sz) return nullptr;

    return ram->buf.buffer + (pv - ram->adr);
}

void ramCodeWritten(struct ArmRam* ram, uint32_t offset, uint32_t size) {
    // The JIT drops every block from the page, so none of its lines hold code now. Cleared
    // first, so the callback can flag fetch buffer lines it keeps again.
    memset(ram->codeLines + ((offset & ~0xfffu) >> 6), 0, 0x1000 >> 6);
    ram->codeWritten(ram->codeContext, ram->buf.buffer + (offset & ~0xfffu), offset & 0xfffu, size);
}

// Lowest offset whose writes can mark the frame buffer dirty: the wider writes start
// below framebufferStart, and with no known size (framebufferStart_n 0) every write does.
static uint32_t ramFramebufferLow(const struct ArmRam* ram) {
    return ram->framebufferStart_64 < ram->framebufferStart ? ram->framebufferStart_64
                                                            : ram->framebufferStart;
}

uint8_t* ramDirectWritePage(struct ArmRam* ram, uint32_t pa) {
    const uint32_t offset = (pa - ram->adr) & ~0xfffu;
    if (uint64_t(offset) + 0x1000 > ram->buf.size) return nullptr;
    if (ram->codeLines) {
        const uint8_t* lines = ram->codeLines + (offset >> 6);
        for (uint32_t i = 0; i < (0x1000 >> 6); i++)
            if (lines[i]) return nullptr;
    }
    memoryBufferMarkRangeDirty(&ram->buf, offset, 0x1000);
    const uint32_t lo = ramFramebufferLow(ram);
    const uint64_t end = uint64_t(offset) + 0x1000;
    if (offset < ram->framebufferEnd && end > lo) {
        const uint32_t first = offset > lo ? offset : lo;
        const uint64_t last = end < ram->framebufferEnd ? end : ram->framebufferEnd;
        ramSetFramebufferDirty(ram, ram->adr + first, uint32_t(last - first));
    }
    return ram->buf.buffer + offset;
}

bool ramFramebufferHost(struct ArmRam* ram, uint8_t** host, uint32_t* size) {
    const uint32_t lo = ramFramebufferLow(ram);
    const uint64_t end = ram->framebufferEnd < ram->buf.size ? ram->framebufferEnd : ram->buf.size;
    if (lo >= end) return false;
    *host = ram->buf.buffer + lo;
    *size = uint32_t(end - lo);
    return true;
}

void ramMarkDirty(struct ArmRam* ram, uint32_t base, uint32_t size) {
    const uint32_t last = base + size;
    const uint32_t first = base < ram->adr ? ram->adr : base;

    if (last <= first) return;

    memoryBufferMarkRangeDirty(&ram->buf, first - ram->adr, last - first);
    // Host writes into RAM (device transfers) can land on translated code too.
    if (ram->codeLines)
        for (uint32_t line = (first - ram->adr) >> 6; line <= (last - 1 - ram->adr) >> 6; line++)
            if (ram->codeLines[line]) {
                // The rest of the write on this page.
                const uint32_t at = (line << 6) > first - ram->adr ? line << 6 : first - ram->adr;
                const uint32_t pageEnd = (at & ~0xfffu) + 0x1000;
                const uint32_t end = last - ram->adr < pageEnd ? last - ram->adr : pageEnd;
                ramCodeWritten(ram, at, end - at);
            }
}
