#include "arm920_dcache.h"

#include <cstring>
#include "MMU.h"
#include "mem.h"

void Arm920DataCache::reset() {
    for (auto& segment : lines) for (auto& line : segment) line.valid = false;
    std::memset(victim, 0, sizeof(victim));
    random = 1;
    guard = {};
}

bool Arm920DataCache::reject(uint32_t pa, uint32_t size, bool write) {
    if (!guard.active) guard = {pa, size, write, true};
    return false;
}

bool Arm920DataCache::supported(uint32_t pa, uint32_t size, bool write) {
    if (guard.active) return false;
    const bool ok = check ? check(context, pa, size, write) :
        memIsBufferRange(mem, pa, size, write);
    return ok || reject(pa, size, write);
}

bool Arm920DataCache::clean(Line& line) {
    if (!line.valid || !line.dirty) return true;
    // PA TAG is retained at allocation, independently of later page-table changes.
    for (unsigned half = 0; half < 2; ++half)
        if ((line.dirty & (1u << half)) && !supported(line.pa + half * 16, 16, true))
            return false;
    for (unsigned i = 0; i < 8; ++i) {
        if (!(line.dirty & (1u << (i / 4)))) continue;
        if (!memAccess<4, true>(mem, line.pa + i * 4, &line.words[i]))
            return reject(line.pa + i * 4, 4, true);
    }
    line.dirty = 0;
    return true;
}

void Arm920DataCache::invalidate() {
    for (auto& segment : lines) for (auto& line : segment) line.valid = false;
}

bool Arm920DataCache::maintain(uint32_t operand, bool index, bool cleaning, bool invalidating) {
    if (guard.active) return false;
    auto& segment = lines[(operand >> 5) & 7];
    // An index op names one line, bits 31:26.
    if (index) {
        auto& line = segment[operand >> 26];
        if (cleaning && !clean(line)) return false;
        if (invalidating) line.valid = false;
        return true;
    }
    for (unsigned i = 0; i < 64; ++i) {
        auto& line = segment[i];
        if (!line.valid || line.va != (operand & ~31u)) continue;
        if (cleaning && !clean(line)) return false;
        if (invalidating) line.valid = false;
    }
    return true;
}

// Puts index first in a segment's recent lines.
static inline void remember(uint8_t (&last)[4], uint8_t index) {
    if (last[0] == index) return;
    unsigned r = 1;
    while (r < 3 && last[r] != index) ++r;
    for (; r > 0; --r) last[r] = last[r - 1];
    last[0] = index;
}

Arm920DataResult Arm920DataCache::access(uint32_t va, uint64_t translation, unsigned size,
                                        bool write, void* data, bool enabled, bool roundRobin) {
    if (guard.active) return Arm920DataResult::unsupported;
    const uint32_t pa = MMU_TRANSLATE_RESULT_PA(translation);
    const bool cacheable = enabled && MMU_TRANSLATE_RESULT_CACHEABLE(translation);
    const bool bufferable = MMU_TRANSLATE_RESULT_BUFFERABLE(translation);
    Line* hit = nullptr;
    if (enabled && !lean) {
        auto& segment = lines[(va >> 5) & 7];
        auto& last = recent[(va >> 5) & 7];
        const uint32_t base = va & ~31u;
        unsigned found = 64;
        for (unsigned r = 0; r < 4; ++r)
            if (segment[last[r] & 63].valid && segment[last[r] & 63].va == base) { found = last[r] & 63; break; }
        if (found == 64) {
            uint8_t& h = hint[(va >> 5) & 2047];
            if (segment[h & 63].valid && segment[h & 63].va == base) found = h & 63;
            else
                for (unsigned index = 0; index < 64; ++index)
                    if (segment[index].valid && segment[index].va == base) { found = index; h = uint8_t(index); break; }
        }
        if (found != 64) {
            hit = &segment[found];
            remember(last, uint8_t(found));
        }
    }
    // DDI0151C 4.3.2: changing C/B while a line is retained is a software error.
    if (hit && (!cacheable || hit->bufferable != bufferable)) {
        reject(pa, size, write);
        return Arm920DataResult::unsupported;
    }
    if (!cacheable && !bufferable) return Arm920DataResult::bypass;
    if (!(hit && !write) &&
        !supported(hit && write && bufferable ? hit->pa + (va & 31) : pa, size, write))
        return Arm920DataResult::unsupported;
    if (write) {
        uint64_t value;
        switch (size) {
            case 1: value = *static_cast<uint8_t*>(data); break;
            case 2: value = *static_cast<uint16_t*>(data); break;
            case 4: value = *static_cast<uint32_t*>(data); break;
            default: value = *static_cast<uint64_t*>(data); break;
        }
        // Read allocation only. Buffered writes drain synchronously to proved backing memory.
        if (!hit || !bufferable) {
            if (!memAccess(mem, pa, size, true, data)) {
                reject(pa, size, true);
                return Arm920DataResult::unsupported;
            }
        }
        if (hit) {
            // Cache words have host integer order; guest data are little endian.
            for (unsigned i = 0; i < size; ++i) {
                const unsigned offset = (va & 31) + i;
                const unsigned shift = (offset & 3) * 8;
                auto& word = hit->words[offset / 4];
                word = (word & ~(0xffu << shift)) | (uint32_t((value >> (i * 8)) & 0xff) << shift);
                if (bufferable) hit->dirty |= 1u << (offset / 16);
            }
        }
        return Arm920DataResult::ok;
    }
    if (!cacheable) {
        if (!memAccess(mem, pa, size, false, data)) {
            reject(pa, size, false);
            return Arm920DataResult::unsupported;
        }
        return Arm920DataResult::ok;
    }
    if (!hit) {
        if (!supported(pa & ~31u, 32, false)) return Arm920DataResult::unsupported;
        if (lean) {
            if (!memAccess(mem, pa, size, false, data)) {
                reject(pa, size, false);
                return Arm920DataResult::unsupported;
            }
            return Arm920DataResult::ok;
        }
        const unsigned segment = (va >> 5) & 7;
        uint32_t nextRandom = random * 1664525u + 1013904223u;
        const unsigned index = roundRobin ? victim[segment] & 63 : (nextRandom >> 24) & 63;
        auto& line = lines[segment][index];
        if (!clean(line)) return Arm920DataResult::unsupported;
        uint32_t words[8];
        for (unsigned i = 0; i < 8; ++i) {
            if (!memAccess<4, false>(mem, (pa & ~31u) + i * 4, words + i)) {
                reject((pa & ~31u) + i * 4, 4, false);
                return Arm920DataResult::unsupported;
            }
        }
        line.va = va & ~31u;
        line.pa = pa & ~31u;
        std::memcpy(line.words, words, sizeof(words));
        line.dirty = 0;
        line.bufferable = bufferable;
        line.valid = true;
        remember(recent[segment], uint8_t(index));
        hint[(va >> 5) & 2047] = uint8_t(index);
        if (roundRobin) ++victim[segment]; else random = nextRandom;
        hit = &line;
    }
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) {
        const unsigned offset = (va & 31) + i;
        value |= uint64_t(uint8_t(hit->words[offset / 4] >> ((offset & 3) * 8))) << (i * 8);
    }
    switch (size) {
        case 1: *static_cast<uint8_t*>(data) = value; break;
        case 2: *static_cast<uint16_t*>(data) = value; break;
        case 4: *static_cast<uint32_t*>(data) = value; break;
        default: *static_cast<uint64_t*>(data) = value; break;
    }
    return Arm920DataResult::ok;
}
