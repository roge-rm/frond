#include "jit_fastmem_arena.h"

#ifdef FROND_GUEST_MEMORY_PLATFORM
#include <sys/mman.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <new>

namespace {
constexpr size_t arenaSize = JitFastmemArena::addressSpace + JitFastmemArena::guard;
constexpr int anonMapping = MAP_PRIVATE | MAP_ANONYMOUS;
}

JitFastmemArena::JitFastmemArena(JitFastmemArenaStats& stats) : stats(stats) {
    if (sysconf(_SC_PAGESIZE) != pageSize) {
        disable("host page size");
        return;
    }
    void* area = mmap(nullptr, arenaSize, PROT_NONE, anonMapping, -1, 0);
    if (area == MAP_FAILED) {
        disable("reserve");
        return;
    }
    start = static_cast<uint8_t*>(area);
    stats.reservations++;
}

JitFastmemArena::~JitFastmemArena() {
    if (start) munmap(start, arenaSize);
}

void JitFastmemArena::disable(const char* operation) {
    stats.failures++;
    static std::atomic_flag logged = ATOMIC_FLAG_INIT;
    if (!logged.test_and_set(std::memory_order_relaxed))
        std::fprintf(stderr, "jit: fastmem arena unavailable (%s), using page tables\n", operation);
    if (start) munmap(start, arenaSize);
    start = nullptr;
    mapped.clear();
}

bool JitFastmemArena::Page::sameBacking(const Page& other) const {
    return host == other.host && backing.fd == other.backing.fd && backing.offset == other.backing.offset;
}

JitFastmemArena::Page JitFastmemArena::fromEntry(const uint8_t* entry) {
    const auto bits = reinterpret_cast<uintptr_t>(entry);
    const auto* host = reinterpret_cast<const uint8_t*>(bits & ~uintptr_t(1));
    auto backing = host ? GuestMemory::find(host, pageSize) : GuestMemory::Backing{};
    if (backing.offset % pageSize) backing = {};
    return {host, backing, PROT_READ | (bits & 1 ? 0 : PROT_WRITE)};
}

void JitFastmemArena::setPage(uint32_t page, const uint8_t* entry) {
    if (!start) return;
    if (page >= pageCount) {
        disable("page range");
        return;
    }
    const auto old = mapped.find(page);
    const Page target = fromEntry(entry);
    if (target.backing.fd < 0) {
        if (entry) stats.unbacked++;
        if (old == mapped.end()) return;
        if (mmap(address(page), pageSize, PROT_NONE, anonMapping | MAP_FIXED, -1, 0) == MAP_FAILED) {
            disable("drop");
            return;
        }
        mapped.erase(old);
        stats.drops++;
        return;
    }
    if (old != mapped.end() && old->second.sameBacking(target)) {
        if (old->second.protection == target.protection) return;
        if (mprotect(address(page), pageSize, target.protection) != 0) {
            disable("protect");
            return;
        }
        old->second.protection = target.protection;
        stats.protects++;
        return;
    }
    if (mmap(address(page), pageSize, target.protection, MAP_SHARED | MAP_FIXED,
             target.backing.fd, target.backing.offset) == MAP_FAILED) {
        disable("map");
        return;
    }
    try {
        mapped.insert_or_assign(page, target);
    } catch (const std::bad_alloc&) {
        disable("record mapping");
        return;
    }
    stats.maps++;
}

void JitFastmemArena::clear() {
    if (!start || mapped.empty()) return;
    if (mmap(start, arenaSize, PROT_NONE, anonMapping | MAP_FIXED, -1, 0) == MAP_FAILED) {
        disable("clear");
        return;
    }
    mapped.clear();
    stats.clears++;
}

bool JitFastmemArena::allows(uint32_t page, bool write) const {
    const auto found = mapped.find(page);
    return found != mapped.end() && (found->second.protection & (write ? PROT_WRITE : PROT_READ));
}

bool JitFastmemArena::verifyPage(uint32_t page, const uint8_t* entry, size_t& checked) const {
    const auto found = mapped.find(page);
    const Page expected = fromEntry(entry);
    if (expected.backing.fd < 0) return found == mapped.end();
    if (found == mapped.end() || !found->second.sameBacking(expected) || found->second.protection != expected.protection)
        return false;
    checked++;
    return std::memcmp(address(page), expected.host, pageSize) == 0;
}

bool JitFastmemArena::verifyCount(size_t checked) const {
    stats.checks++;
    stats.checkedPages += checked;
    return checked == mapped.size();
}

#endif
