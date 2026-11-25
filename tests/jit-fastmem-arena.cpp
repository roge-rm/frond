#include <sys/resource.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>

#include "jit_fastmem_arena.h"

namespace {

bool transfer(void* address, uint8_t& value, bool write) {
    const iovec local{&value, 1};
    const iovec remote{address, 1};
    const ssize_t count = write ? process_vm_writev(getpid(), &local, 1, &remote, 1, 0)
                                : process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    return count == 1;
}

const uint8_t* readOnly(const uint8_t* p) {
    return reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(p) | 1);
}

void reserveFailure() {
    rlimit previous{};
    assert(getrlimit(RLIMIT_AS, &previous) == 0);
    const rlimit capped{rlim_t{2} << 30, previous.rlim_max};
    assert(setrlimit(RLIMIT_AS, &capped) == 0);
    JitFastmemArenaStats stats;
    JitFastmemArena arena(stats);
    assert(setrlimit(RLIMIT_AS, &previous) == 0);
    assert(!arena.base() && stats.failures == 1 && stats.reservations == 0);
}

}  // namespace

int main() {
    using Arena = JitFastmemArena;
    const auto pageBytes = Arena::pageSize;
    assert(GuestMemory::verifyJitArena());
    reserveFailure();
    auto* data = static_cast<uint8_t*>(GuestMemory::allocateShared(pageBytes * 2));
    assert(data);
    JitFastmemArenaStats stats;
    Arena user(stats), kernel(stats);
    assert(user.base() && kernel.base() && user.base() != kernel.base());

    uint8_t value = 0;
    uint8_t* userPage = user.base() + pageBytes;
    uint8_t* kernelPage = kernel.base() + pageBytes;
    uint8_t* lastPage = user.base() + Arena::addressSpace - pageBytes;
    uint8_t* guard = user.base() + Arena::addressSpace;
    assert(!transfer(userPage, value, false) && !transfer(userPage, value, true));
    assert(!transfer(guard, value, false) && !transfer(guard + Arena::guard - 1, value, true));

    user.setPage(1, readOnly(data));
    kernel.setPage(1, data);
    value = 42;
    assert(!transfer(userPage, value, true) && transfer(kernelPage, value, true));
    assert(data[0] == 42 && transfer(userPage, value, false) && value == 42);

    user.setPage(1, data);
    value = 17;
    assert(transfer(userPage, value, true) && data[0] == 17);
    kernel.setPage(1, readOnly(data));
    assert(!transfer(kernelPage, value, true));

    user.setPage(1, data + pageBytes);
    data[pageBytes] = 8;
    assert(transfer(userPage, value, false) && value == 8);
    assert(transfer(kernelPage, value, false) && value == 17);
    user.setPage(Arena::pageCount - 1, data);
    assert(transfer(lastPage, value, false) && value == 17);
    assert(!transfer(guard, value, false));

    size_t checked = 0;
    assert(user.verifyPage(1, data + pageBytes, checked));
    assert(!user.verifyPage(1, readOnly(data + pageBytes), checked));
    assert(user.verifyPage(Arena::pageCount - 1, data, checked));
    assert(user.verifyCount(checked));

    uint8_t unbacked[4096]{};
    user.setPage(1, unbacked);
    assert(!transfer(userPage, value, false) && !transfer(userPage, value, true));
    user.setPage(1, data + 2);
    assert(!transfer(userPage, value, false));
    kernel.setPage(1, nullptr);
    assert(!transfer(kernelPage, value, false));
    user.clear();
    user.clear();
    assert(user.verifyCount(0) && !transfer(lastPage, value, false) && !transfer(guard, value, true));
    assert(stats.reservations == 2 && stats.maps == 4 && stats.protects == 2 && stats.drops == 2);
    assert(stats.clears == 1 && stats.unbacked == 2 && stats.failures == 0);

    user.setPage(Arena::pageCount, data);
    assert(!user.base() && stats.failures == 1);
    assert(!transfer(userPage, value, false));
    assert(kernel.base());
    assert(GuestMemory::releaseShared(data));
    std::puts("JIT fastmem arena checks passed");
}
