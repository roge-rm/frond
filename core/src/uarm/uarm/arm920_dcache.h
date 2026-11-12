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
    // The line each segment's last access hit, looked at first. A segment never holds two valid lines
    // with one address, as a line is only filled after a search found none. Not saved.
    uint8_t recent[8];
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
