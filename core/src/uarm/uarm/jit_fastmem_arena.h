#ifndef FROND_JIT_FASTMEM_ARENA_H
#define FROND_JIT_FASTMEM_ARENA_H

#include "guest_memory.h"

#ifdef FROND_GUEST_MEMORY_PLATFORM
#include <atomic>
#include <unordered_map>

struct JitFastmemArenaStats {
    std::atomic<uint64_t> reservations{0}, maps{0}, protects{0}, drops{0}, clears{0}, unbacked{0};
    std::atomic<uint64_t> failures{0}, checks{0}, checkedPages{0}, faults{0};
    std::atomic<uint64_t> retries{0}, slowSites{0}, cappedSites{0}, pendingSlowFaults{0};
};

// One per privilege level, owned and updated by the emulation thread.
class JitFastmemArena {
   public:
    static constexpr uint32_t pageSize = 4096;
    static constexpr size_t pageCount = uint64_t{1} << 20;
    static constexpr size_t addressSpace = pageCount * pageSize;
    static constexpr size_t guard = 64 * 1024;

    explicit JitFastmemArena(JitFastmemArenaStats& stats);
    ~JitFastmemArena();
    JitFastmemArena(const JitFastmemArena&) = delete;
    JitFastmemArena& operator=(const JitFastmemArena&) = delete;

    uint8_t* base() const { return start; }
    // The table's low bit means read-only. Unbacked pages have no direct arena mapping.
    void setPage(uint32_t page, const uint8_t* entry);
    void clear();
    bool allows(uint32_t page, bool write) const;
    bool verifyPage(uint32_t page, const uint8_t* entry, size_t& checked) const;
    bool verifyCount(size_t checked) const;

   private:
    struct Page {
        const uint8_t* host;
        GuestMemory::Backing backing;
        int protection;
        bool sameBacking(const Page& other) const;
    };

    static Page fromEntry(const uint8_t* entry);
    uint8_t* address(uint32_t page) const { return start + size_t(page) * pageSize; }
    void disable(const char* operation);

    JitFastmemArenaStats& stats;
    uint8_t* start = nullptr;
    std::unordered_map<uint32_t, Page> mapped;
};

#endif
#endif
