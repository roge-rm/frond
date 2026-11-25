#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>

#include "guest_memory.h"
#include "machine_state_archive.h"
#include "memory_buffer.h"

namespace {

void expectReleased(uintptr_t address, int fd) {
    assert(GuestMemory::find(reinterpret_cast<void*>(address), 1).fd == -1);
    assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
    unsigned char contents;
    assert(mincore(reinterpret_cast<void*>(address), 1, &contents) == -1 && errno == ENOMEM);
}

void aliasCoherence() {
    MemoryBuffer ram{};
    assert(memoryBufferAllocate(&ram, 2 * MEMORY_BUFFER_GRANULARITY));
    MemoryBuffer sub{};
    assert(memoryBufferGetSubBuffer(&ram, &sub, MEMORY_BUFFER_GRANULARITY, MEMORY_BUFFER_GRANULARITY));
    const auto base = GuestMemory::find(ram.buffer, ram.size);
    const auto backing = GuestMemory::find(sub.buffer, sub.size);
    assert(base.fd >= 0 && base.offset == 0);
    assert(backing.fd == base.fd && backing.offset == MEMORY_BUFFER_GRANULARITY);
    assert(fcntl(base.fd, F_GETFD) & FD_CLOEXEC);
    assert(GuestMemory::find(sub.buffer, sub.size + 1).fd == -1);
    assert(GuestMemory::find(ram.dirtyPages, ram.dirtyPagesSize).fd == -1);

    void* mapping = mmap(nullptr, sub.size, PROT_READ | PROT_WRITE, MAP_SHARED, backing.fd, backing.offset);
    assert(mapping != MAP_FAILED);
    auto* alias = static_cast<uint8_t*>(mapping);
    assert(alias[0] == 0 && sub.buffer[1] == 0);
    sub.buffer[0] = 108;
    assert(alias[0] == 108);
    alias[1] = 211;
    assert(sub.buffer[1] == 211);
    MEMORY_BUFFER_MARK_DIRTY(sub, 0);
    assert(ram.dirtyPages[0] == 0 && ram.dirtyPages[1] == 1);

    memoryBufferRelease(&sub);
    assert(GuestMemory::find(sub.buffer, sub.size).fd == base.fd);
    assert(munmap(alias, sub.size) == 0);
    const auto address = reinterpret_cast<uintptr_t>(ram.buffer);
    memoryBufferRelease(&ram);
    assert(!memoryBufferValid(&ram));
    expectReleased(address, base.fd);
}

void vectorSwap() {
    GuestMemory::Bytes first(9000, 10, GuestMemory::byteAllocator());
    GuestMemory::Bytes second(29, 20);
    const auto address = reinterpret_cast<uintptr_t>(first.data());
    const auto backing = GuestMemory::find(first.data(), first.size());
    assert(backing.fd >= 0 && backing.offset == 0);

    first.swap(second);
    assert(first.size() == 29 && first.front() == 20);
    assert(second.size() == 9000 && second.back() == 10);
    assert(GuestMemory::find(first.data(), first.size()).fd == -1);
    assert(GuestMemory::find(second.data(), second.size()).fd == backing.fd);

    std::vector<uint8_t> serialized;
    StateWriter writer(serialized);
    writer.DoVector(second, 9000);
    std::vector<uint8_t> ordinary;
    StateReader ordinaryReader(serialized.data(), serialized.size());
    ordinaryReader.DoVector(ordinary, 9000);
    assert(ordinaryReader.complete() && ordinary.size() == 9000 && ordinary.back() == 10);
    GuestMemory::Bytes restored(GuestMemory::byteAllocator());
    StateReader sharedReader(serialized.data(), serialized.size());
    sharedReader.DoVector(restored, 9000);
    assert(sharedReader.complete() && restored == second);
    assert(GuestMemory::find(restored.data(), restored.size()).fd >= 0);

    second.reserve(18000);
    expectReleased(address, backing.fd);
    assert(GuestMemory::find(second.data(), second.size()).fd >= 0);
    assert(second.size() == 9000 && second.back() == 10);
}

void ordinaryFallback() {
    rlimit previous{};
    assert(getrlimit(RLIMIT_NOFILE, &previous) == 0);
    const rlimit unavailable{0, previous.rlim_max};
    assert(setrlimit(RLIMIT_NOFILE, &unavailable) == 0);
    GuestMemory::Bytes bytes(4096, 34, GuestMemory::byteAllocator());
    MemoryBuffer ram{};
    assert(memoryBufferAllocate(&ram, MEMORY_BUFFER_GRANULARITY));
    assert(setrlimit(RLIMIT_NOFILE, &previous) == 0);

    assert(GuestMemory::find(bytes.data(), bytes.size()).fd == -1 && bytes.back() == 34);
    assert(GuestMemory::find(ram.buffer, ram.size).fd == -1 && ram.buffer[ram.size - 1] == 0);
    memoryBufferRelease(&ram);
    assert(!memoryBufferValid(&ram));
}

}  // namespace

int main() {
    aliasCoherence();
    vectorSwap();
    ordinaryFallback();
    std::puts("Shared guest memory checks passed");
}
