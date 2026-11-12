// ArmRam layout and the RAM access body, shared with the CPU's inlined RAM path.
#ifndef FROND_RAM_INTERNAL_H
#define FROND_RAM_INTERNAL_H

#include <cstdint>

#include "CPEndian.h"
#include "RAM.h"
#include "cputil.h"
#include "memory_buffer.h"

class SoC;

// Out of line so callers don't need SoC.h. Calls soc->SetFramebufferDirty.
void ramSetFramebufferDirty(struct ArmRam* ram, uint32_t pa, uint32_t size);
// Clears the line's code flag and tells the JIT. Out of line, as it's rare.
void ramCodeWritten(struct ArmRam* ram, uint32_t offset, uint32_t size);

struct ArmRam {
    uint32_t adr;
    uint32_t sz;
    struct MemoryBuffer buf;
    class SoC* soc;
    // One byte per 64-byte line of buf, set while the JIT holds code translated from that
    // line. Null without the JIT. RAM regions over the same buffer share one array.
    uint8_t* codeLines;
    RamCodeWritten codeWritten;
    void* codeContext;

    uint32_t framebufferStart;
    uint32_t framebufferStart_2;
    uint32_t framebufferStart_4;
    uint32_t framebufferStart_8;
    uint32_t framebufferStart_16;
    uint32_t framebufferStart_32;
    uint32_t framebufferStart_64;
    uint32_t framebufferEnd;
};

template <int size, bool write>
static FORCE_INLINE bool ramAccessInline(void* userData, uint32_t pa, void* bufP) {
    struct ArmRam* ram = (struct ArmRam*)userData;
    const uint32_t offset = pa - ram->adr;
    const uint8_t* addr = (uint8_t*)ram->buf.buffer + offset;

    if constexpr (write) {
        MEMORY_BUFFER_MARK_DIRTY(ram->buf, offset);
        if (__builtin_expect(ram->codeLines != nullptr, 0) && ram->codeLines[offset >> 6])
            ramCodeWritten(ram, offset, size);

        switch (size) {
            case 1:
                if (offset < ram->framebufferEnd && offset >= ram->framebufferStart)
                    ramSetFramebufferDirty(ram, pa, 1);

                *((uint8_t*)addr) = *(uint8_t*)bufP;  // our memory system is little-endian
                break;

            case 2:
                if (offset < ram->framebufferEnd && offset >= ram->framebufferStart_2)
                    ramSetFramebufferDirty(ram, pa, 2);

                *((uint16_t*)addr) =
                    htole16(*(uint16_t*)bufP);  // our memory system is little-endian
                break;

            case 4:
                if (offset < ram->framebufferEnd && offset >= ram->framebufferStart_4)
                    ramSetFramebufferDirty(ram, pa, 4);

                *((uint32_t*)addr) = htole32(*(uint32_t*)bufP);
                break;

            case 64:
                if (offset < ram->framebufferEnd && offset >= ram->framebufferStart_64)
                    ramSetFramebufferDirty(ram, pa, 64);

                if (offset & 0x3f) MEMORY_BUFFER_MARK_DIRTY(ram->buf, offset + 0x3f);

                *((uint64_t*)(addr + 0)) = htole64(((uint64_t*)bufP)[0]);
                *((uint64_t*)(addr + 8)) = htole64(((uint64_t*)bufP)[1]);
                *((uint64_t*)(addr + 16)) = htole64(((uint64_t*)bufP)[2]);
                *((uint64_t*)(addr + 24)) = htole64(((uint64_t*)bufP)[3]);
                *((uint64_t*)(addr + 32)) = htole64(((uint64_t*)bufP)[4]);
                *((uint64_t*)(addr + 40)) = htole64(((uint64_t*)bufP)[5]);
                *((uint64_t*)(addr + 48)) = htole64(((uint64_t*)bufP)[6]);
                *((uint64_t*)(addr + 56)) = htole64(((uint64_t*)bufP)[7]);
                break;

            case 32:
                if (offset < ram->framebufferEnd && offset >= ram->framebufferStart_32)
                    ramSetFramebufferDirty(ram, pa, 32);

                if (offset & 0x1f) MEMORY_BUFFER_MARK_DIRTY(ram->buf, offset + 0x1f);

                *((uint64_t*)(addr + 0)) = htole64(((uint64_t*)bufP)[0]);
                *((uint64_t*)(addr + 8)) = htole64(((uint64_t*)bufP)[1]);
                *((uint64_t*)(addr + 16)) = htole64(((uint64_t*)bufP)[2]);
                *((uint64_t*)(addr + 24)) = htole64(((uint64_t*)bufP)[3]);
                break;

            case 16:
                if (offset < ram->framebufferEnd && offset >= ram->framebufferStart_16)
                    ramSetFramebufferDirty(ram, pa, 16);

                if (offset & 0x0f) MEMORY_BUFFER_MARK_DIRTY(ram->buf, offset + 0x0f);

                *((uint64_t*)(addr + 0)) = htole64(((uint64_t*)bufP)[0]);
                *((uint64_t*)(addr + 8)) = htole64(((uint64_t*)bufP)[1]);
                break;

            case 8:
                if (offset < ram->framebufferEnd && offset >= ram->framebufferStart_8)
                    ramSetFramebufferDirty(ram, pa, 8);

                *((uint64_t*)(addr + 0)) = htole64(((uint64_t*)bufP)[0]);
                break;

            default:
                return false;
        }
    } else {
        switch (size) {
            case 1:

                *(uint8_t*)bufP = *((uint8_t*)addr);
                break;

            case 2:

                *(uint16_t*)bufP = le16toh(*((uint16_t*)addr));
                break;

            case 4:

                *(uint32_t*)bufP = le32toh(*((uint32_t*)addr));
                break;

            case 64:
                ((uint64_t*)bufP)[4] = le64toh(*((uint64_t*)(addr + 32)));
                ((uint64_t*)bufP)[5] = le64toh(*((uint64_t*)(addr + 40)));
                ((uint64_t*)bufP)[6] = le64toh(*((uint64_t*)(addr + 48)));
                ((uint64_t*)bufP)[7] = le64toh(*((uint64_t*)(addr + 56)));
                // fallthrough
            case 32:

                ((uint64_t*)bufP)[2] = le64toh(*((uint64_t*)(addr + 16)));
                ((uint64_t*)bufP)[3] = le64toh(*((uint64_t*)(addr + 24)));
                // fallthrough
            case 16:

                ((uint64_t*)bufP)[1] = le64toh(*((uint64_t*)(addr + 8)));
                // fallthrough
            case 8:
                ((uint64_t*)bufP)[0] = le64toh(*((uint64_t*)(addr + 0)));
                break;

            default:

                return false;
        }
    }

    return true;
}

#endif
