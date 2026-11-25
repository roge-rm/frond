#include "guest_memory.h"

#if defined(FROND_GUEST_MEMORY_PLATFORM)

#if defined(FROND_DYNARMIC) || defined(FROND_TEST_GUEST_MEMORY)
#include <sys/mman.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/sharedmem.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <new>

namespace {
struct Mapping {
    size_t size;
    int fd;
};
struct Registry {
    std::mutex mutex;
    std::map<uintptr_t, Mapping> mappings;
};

Registry& registry() {
    // Allocators in static objects can release mappings at process exit.
    alignas(Registry) static std::byte storage[sizeof(Registry)];
    static auto* const live = ::new (storage) Registry;
    return *live;
}

void* unavailable() {
    static std::atomic_flag logged = ATOMIC_FLAG_INIT;
    if (!logged.test_and_set(std::memory_order_relaxed))
        std::fputs("Shared guest memory unavailable, using ordinary memory\n", stderr);
    return nullptr;
}

int createBacking(size_t size) {
#ifdef __ANDROID__
    return ASharedMemory_create("Frond guest memory", size);
#else
    int fd = memfd_create("Frond guest memory", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(size)) == 0) return fd;
    close(fd);
    return -1;
#endif
}

#ifdef FROND_GUEST_MEMORY_VECTOR
const GuestMemory::AllocatorOps sharedOps{GuestMemory::allocateShared, GuestMemory::releaseShared};
#endif

bool sharedEnabled() {
#ifdef FROND_TEST_GUEST_MEMORY
    return true;
#else
    return GuestMemory::verifyJitArena() || GuestMemory::fastmemEnabled();
#endif
}
}  // namespace

namespace GuestMemory {

bool verifyJitArena() {
    static const bool verify = [] {
        const char* v = std::getenv("FROND_JIT_FASTMEM_VERIFY");
        return v && *v && std::strcmp(v, "0");
    }();
    return verify;
}

bool fastmemEnabled() {
#ifdef __aarch64__
    static const bool enabled = [] {
        const char* value = std::getenv("FROND_JIT_FASTMEM");
        return value && std::strcmp(value, "1") == 0 && sysconf(_SC_PAGESIZE) == 4096;
    }();
    return enabled;
#else
    return false;
#endif
}

void* allocateShared(size_t size) {
    if (!size || !sharedEnabled()) return nullptr;
    if (size > static_cast<size_t>(std::numeric_limits<off_t>::max())) return unavailable();

    int fd = createBacking(size);
    if (fd < 0) return unavailable();
    void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return unavailable();
    }
    try {
        auto& live = registry();
        std::lock_guard<std::mutex> lock(live.mutex);
        live.mappings.emplace(reinterpret_cast<uintptr_t>(data), Mapping{size, fd});
    } catch (const std::bad_alloc&) {
        munmap(data, size);
        close(fd);
        return unavailable();
    }
    return data;
}

bool releaseShared(void* data) {
    if (!data || !sharedEnabled()) return false;
    auto& live = registry();
    std::lock_guard<std::mutex> lock(live.mutex);
    auto it = live.mappings.find(reinterpret_cast<uintptr_t>(data));
    if (it == live.mappings.end()) return false;
    const Mapping mapping = it->second;
    live.mappings.erase(it);
    munmap(data, mapping.size);
    close(mapping.fd);
    return true;
}

Backing find(const void* data, size_t size) {
    if (!size || !sharedEnabled()) return {};
    auto& live = registry();
    std::lock_guard<std::mutex> lock(live.mutex);
    const uintptr_t address = reinterpret_cast<uintptr_t>(data);
    auto it = live.mappings.upper_bound(address);
    if (it == live.mappings.begin()) return {};
    --it;
    const size_t offset = address - it->first;
    if (offset > it->second.size || size > it->second.size - offset) return {};
    return {it->second.fd, offset};
}

#ifdef FROND_GUEST_MEMORY_VECTOR
Allocator<uint8_t> byteAllocator() { return Allocator<uint8_t>(&sharedOps); }
#endif

}  // namespace GuestMemory

#else

namespace GuestMemory {

bool verifyJitArena() { return false; }
bool fastmemEnabled() { return false; }
void* allocateShared(size_t) { return nullptr; }
bool releaseShared(void*) { return false; }
Backing find(const void*, size_t) { return {}; }
#ifdef FROND_GUEST_MEMORY_VECTOR
Allocator<uint8_t> byteAllocator() { return {}; }
#endif

}  // namespace GuestMemory
#endif

#endif
