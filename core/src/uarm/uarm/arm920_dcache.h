#ifndef FROND_ARM920_DCACHE_H
#define FROND_ARM920_DCACHE_H

#include <cstdint>

struct ArmMem;
// A successful check promises ordinary, infallible backing accesses, without issuing one.
using Arm920MemoryCheck = bool (*)(void*, uint32_t, uint32_t, bool);
enum class Arm920DataResult { bypass, ok, unsupported };
struct Arm920DataGuard {
    uint32_t address, size;
    bool write, active;
};
struct Arm920DataCache {
    struct Line {
        uint32_t va, pa, words[8];
        uint8_t dirty;
        bool valid, bufferable;
    } lines[8][64];
    uint8_t victim[8];
    uint32_t random;
    // The lines each segment's last accesses hit, newest first, looked at before the whole segment. A segment
    // never holds two valid lines with one address, as a line is only filled after a search found none, so the
    // order lines are looked at in doesn't change what is found. Not saved.
    uint8_t recent[8][4];
    // The index a line with bits 15:5 of its address was last found or filled at, looked at after recent and
    // before the whole segment (bits 7:5 are the segment). Only a hint: it is checked against the line. Not saved.
    uint8_t hint[2048];
    ArmMem* mem;
    Arm920MemoryCheck check;
    void* context;
    Arm920DataGuard guard;
    // Fast engine: no lines are kept, cacheable accesses go straight to memory after the same support check
    // a miss makes. Not saved.
    bool lean;
    void reset();
    bool supported(uint32_t pa, uint32_t size, bool write);
    bool reject(uint32_t pa, uint32_t size, bool write);
    bool clean(Line& line);
    void invalidate();
    bool maintain(uint32_t operand, bool index, bool clean, bool invalidate);
    Arm920DataResult access(uint32_t va, uint64_t translation, unsigned size, bool write,
                           void* data, bool enabled, bool roundRobin);
};

#endif
