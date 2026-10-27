// Region bounds near the top of the address space: an access whose end passes 0xffffffff
// must not match RAM, ROM or a device. Guest memory sits in front of PROT_NONE pages, so
// an access that reaches host memory outside it faults. Each case runs in a child process.
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>

#include "RAM.h"
#include "ROM.h"
#include "mem.h"
#include "mem_internal.h"
#include "memory_buffer.h"

static int failures = 0;

// Reserve `guard` bytes of PROT_NONE and make the first `size` bytes usable.
static uint8_t* guarded(size_t size, size_t guard) {
    void* p = mmap(nullptr, guard, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        _exit(2);
    }
    const size_t page = sysconf(_SC_PAGESIZE);
    if (mprotect(p, (size + page - 1) / page * page, PROT_READ | PROT_WRITE)) {
        perror("mprotect");
        _exit(2);
    }
    return static_cast<uint8_t*>(p);
}

// RAM whose data and dirty-page bitmap are both followed by faulting pages for any offset
// below 4 GiB.
static ArmRam* guardedRam(ArmMem* mem, uint32_t adr, uint32_t sz) {
    MemoryBuffer buf{};
    buf.size = sz;
    buf.dirtyPagesSize = 1;
    buf.buffer = guarded(sz, (size_t(1) << 32) + 0x10000);
    buf.dirtyPages = reinterpret_cast<uint32_t*>(guarded(4, size_t(1) << 20));
    return ramInit(mem, nullptr, adr, sz, &buf, true);
}

static ArmRom* guardedRom(ArmMem* mem, uint32_t adr, uint32_t sz) {
    return romInit(mem, adr, guarded(sz, (size_t(1) << 32) + 0x10000), sz);
}

// Expect `access` to return `expected` without faulting.
static void expect(const char* name, bool expected, const std::function<bool()>& access) {
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) _exit(access() == expected ? 0 : 1);
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        printf("PASS: %s\n", name);
        return;
    }
    failures++;
    if (WIFSIGNALED(status))
        printf("FAIL: %s: signal %d, touched host memory outside the region\n", name,
               WTERMSIG(status));
    else
        printf("FAIL: %s: returned %s\n", name, expected ? "false" : "true");
}

static bool deviceCalled = false;
static bool device(void*, uint32_t, uint_fast8_t, bool, void*) {
    deviceCalled = true;
    return true;
}

int main() {
    uint8_t data[64] = {};

    // Primary RAM at the TX base: wrapped accesses must not reach the RAM branch.
    ArmMem* mem = memInit();
    ArmRam* ram = guardedRam(mem, 0xa0000000, 0x10000);
    expect("RAM read 4 at 0xa000fffc", true, [&] { return memAccess<4, false>(mem, 0xa000fffc, data); });
    expect("RAM read 4 across the end", false, [&] { return memAccess<4, false>(mem, 0xa000fffe, data); });
    expect("RAM template read 4 at 0xfffffffc", false, [&] { return memAccess<4, false>(mem, 0xfffffffc, data); });
    expect("RAM template write 4 at 0xfffffffc", false, [&] { return memAccess<4, true>(mem, 0xfffffffc, data); });
    expect("RAM template read 8 at 0xfffffffc", false, [&] { return memAccess<8, false>(mem, 0xfffffffc, data); });
    expect("RAM template read 2 at 0xffffffff", false, [&] { return memAccess<2, false>(mem, 0xffffffff, data); });
    expect("RAM template read 64 at 0xffffffd0", false, [&] { return memAccess<64, false>(mem, 0xffffffd0, data); });
    expect("RAM sized read 4 at 0xfffffffc", false, [&] { return memAccess(mem, 0xfffffffc, 4, false, data); });
    expect("RAM sized write 4 at 0xfffffffc", false, [&] { return memAccess(mem, 0xfffffffc, 4, true, data); });
    expect("RAM fetch 4 at 0xfffffffc", false, [&] { return memInstructionFetch<4>(mem, 0xfffffffc, data); });
    expect("RAM inline read 4 at 0xfffffffc", false, [&] { return memAccessRamInline<4, false>(mem, 0xfffffffc, data); });
    expect("RAM inline write 4 at 0xfffffffc", false, [&] { return memAccessRamInline<4, true>(mem, 0xfffffffc, data); });
    expect("RAM inline write 8 at 0xfffffffc", false, [&] { return memAccessRamInline<8, true>(mem, 0xfffffffc, data); });
    expect("RAM resolve 8 at 0xfffffffc", false, [&] { return ramResolveAddress(ram, 0xfffffffc, 8) != nullptr; });

    // RAM above the test addresses, so a wrapped access reaches the ROM branch.
    ArmMem* romMem = memInit();
    guardedRam(romMem, 0xfffffff8, 4);
    ArmRom* rom = guardedRom(romMem, 0xffff0000, 0x8000);
    expect("ROM read 4 at 0xffff7ffc", true, [&] { return memAccess<4, false>(romMem, 0xffff7ffc, data); });
    expect("ROM read 64 at 0xffffffd0", false, [&] { return memAccess<64, false>(romMem, 0xffffffd0, data); });
    expect("ROM sized read 64 at 0xffffffd0", false, [&] { return memAccess(romMem, 0xffffffd0, 64, false, data); });
    expect("ROM fetch 32 at 0xfffffff0", false, [&] { return memInstructionFetch<32>(romMem, 0xfffffff0, data); });
    expect("ROM resolve 8 at 0xfffffffc", false, [&] { return romResolveAddress(rom, 0xfffffffc, 8) != nullptr; });

    // RAM and ROM above the test addresses, so a wrapped access reaches the device scan.
    ArmMem* devMem = memInit();
    guardedRam(devMem, 0xfffffff8, 4);
    guardedRom(devMem, 0xfffffff4, 4);
    if (!memRegionAdd(devMem, 0x40000000, 0x1000, device, nullptr)) {
        printf("FAIL: cannot add device region\n");
        return 1;
    }
    expect("device read 4 at 0x40000ffc", true, [&] { return memAccess<4, false>(devMem, 0x40000ffc, data) && deviceCalled; });
    expect("device read 64 at 0xffffffd0", false, [&] { return memAccess<64, false>(devMem, 0xffffffd0, data) || deviceCalled; });
    expect("device sized read 64 at 0xffffffd0", false, [&] { return memAccess(devMem, 0xffffffd0, 64, false, data) || deviceCalled; });
    expect("device fetch 32 at 0xfffffff0", false, [&] { return memInstructionFetch<32>(devMem, 0xfffffff0, data) || deviceCalled; });

    // A region's end has to fit in 32 bits.
    ArmMem* addMem = memInit();
    expect("region ending at 4 GiB rejected", false, [&] { return memRegionAdd(addMem, 0xfffff000, 0x1000, device, nullptr); });
    expect("region past 4 GiB rejected", false, [&] { return memRegionAdd(addMem, 0xfffff000, 0x2000, device, nullptr); });
    expect("RAM region past 4 GiB rejected", false, [&] { return memRegionAddRam(addMem, 0xfffff000, 0x2000, device, nullptr); });
    expect("region ending at 0xffffffff accepted", true, [&] { return memRegionAdd(addMem, 0xfffff000, 0xfff, device, nullptr); });

    if (failures) {
        printf("%d memory region bounds checks failed\n", failures);
        return 1;
    }
    printf("PASS: memory region bounds near 4 GiB\n");
    return 0;
}
