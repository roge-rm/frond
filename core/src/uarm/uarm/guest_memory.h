#ifndef FROND_GUEST_MEMORY_H
#define FROND_GUEST_MEMORY_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

namespace GuestMemory {

struct Backing {
    int fd = -1;
    size_t offset = 0;
};

// Keep the owner alive while using the borrowed descriptor from find().
#if defined(FROND_TEST_GUEST_MEMORY) || \
    (defined(__linux__) && ((defined(__aarch64__) && (!defined(__ANDROID__) || __ANDROID_API__ >= 26)) || \
                            (defined(__x86_64__) && !defined(__ANDROID__))))
#define FROND_GUEST_MEMORY_PLATFORM
bool verifyJitArena();
bool fastmemEnabled();
void* allocateShared(size_t size);
bool releaseShared(void* data);
Backing find(const void* data, size_t size);
#else
inline void* allocateShared(size_t) { return nullptr; }
inline bool releaseShared(void*) { return false; }
inline Backing find(const void*, size_t) { return {}; }
#endif

#if defined(FROND_GUEST_MEMORY_PLATFORM) && (defined(__aarch64__) || defined(FROND_TEST_GUEST_MEMORY))
#define FROND_GUEST_MEMORY_VECTOR
struct AllocatorOps {
    void* (*allocate)(size_t);
    bool (*release)(void*);
};

template <class T>
class Allocator {
   public:
    using value_type = T;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;

    Allocator() = default;
    explicit Allocator(const AllocatorOps* ops) noexcept : ops(ops) {}
    template <class U> Allocator(const Allocator<U>& other) noexcept : ops(other.ops) {}

    T* allocate(size_t count) {
        if (ops && alignof(T) <= alignof(std::max_align_t) && count <= SIZE_MAX / sizeof(T)) {
            if (void* data = ops->allocate(count * sizeof(T))) return static_cast<T*>(data);
        }
        return std::allocator<T>{}.allocate(count);
    }
    void deallocate(T* data, size_t count) noexcept {
        if (!ops || !ops->release(data)) std::allocator<T>{}.deallocate(data, count);
    }
    template <class U> bool operator==(const Allocator<U>& other) const noexcept { return ops == other.ops; }
    template <class U> bool operator!=(const Allocator<U>& other) const noexcept { return !(*this == other); }

   private:
    template <class> friend class Allocator;
    const AllocatorOps* ops = nullptr;
};

Allocator<uint8_t> byteAllocator();

#else
template <class T> using Allocator = std::allocator<T>;
inline Allocator<uint8_t> byteAllocator() { return {}; }
#endif

using Bytes = std::vector<uint8_t, Allocator<uint8_t>>;

}  // namespace GuestMemory

#endif
