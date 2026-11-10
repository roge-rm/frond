// A NAND whose backing refuses a write (disk full, folder read-only) keeps running: the program or
// erase reports fail in the status byte, as a worn chip does, and the next program that stores
// clears it. nandWrite only fails for bus misuse, which ends the core.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "CardBacking.h"
#include "memory_buffer.h"
#include "nand.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

class FailingBacking : public RamCardBacking {
   public:
    using RamCardBacking::RamCardBacking;
    bool fail = false;

   protected:
    bool WriteBytes(size_t offset, size_t length, const void* src) override {
        return !fail && RamCardBacking::WriteBytes(offset, length, src);
    }
};

void noReschedule(void*, uint32_t) {}

// Tungsten E2 geometry: 528-byte pages, 32 to a block, 2048 blocks.
const NandSpecs specs = {528, 2048, 5, NAND_FLAG_SAMSUNG_ADDRESSED_VIA_AREAS, 2, {0xec, 0x75}};
constexpr uint32_t kPages = 2048 << 5;

bool cmd(NAND* n, uint8_t v) { return nandWrite(n, true, false, v); }
bool addr(NAND* n, uint8_t v) { return nandWrite(n, false, true, v); }
bool data(NAND* n, uint8_t v) { return nandWrite(n, false, false, v); }

void settle(NAND* n) {
    for (int i = 0; i < 1000 && !nandIsReady(n); i++) nandPeriodic(n);
}

uint8_t status(NAND* n) {
    uint8_t s = 0xee;
    cmd(n, 0x70);
    nandRead(n, false, false, &s);
    return s;
}

bool program(NAND* n, uint32_t page, uint8_t value) {
    bool ok = cmd(n, 0x00) && cmd(n, 0x80) && addr(n, 0) && addr(n, page & 0xff) && addr(n, page >> 8);
    for (int i = 0; i < 16; i++) ok = ok && data(n, value);
    ok = ok && cmd(n, 0x10);
    settle(n);
    return ok;
}

bool erase(NAND* n, uint32_t page) {
    bool ok = cmd(n, 0x60) && addr(n, page & 0xff) && addr(n, page >> 8) && cmd(n, 0xd0);
    settle(n);
    return ok;
}

uint8_t readFirst(NAND* n, uint32_t page) {
    uint8_t v = 0xee;
    cmd(n, 0x00);
    addr(n, 0);
    addr(n, page & 0xff);
    addr(n, page >> 8);
    settle(n);
    nandRead(n, false, false, &v);
    return v;
}

}  // namespace

int main() {
    MemoryBuffer pageBuf{};
    check(memoryBufferAllocate(&pageBuf, MEMORY_BUFFER_GRANULARITY), "page buffer");
    if (!pageBuf.buffer) return 1;
    NAND* n = nandInit(nullptr, &pageBuf, Reschedule{noReschedule, nullptr}, size_t(kPages) * 528, &specs,
                       nullptr, nullptr);
    check(n, "nandInit");
    if (!n) return 1;
    auto backing = std::make_unique<FailingBacking>(size_t(kPages) * 528);
    FailingBacking* store = backing.get();
    check(nandAdoptBacking(n, std::move(backing), uint64_t(kPages) * 512), "adopt backing");
    settle(n);
    // A program only clears bits, so start from an erased block 0 (pages 0-31).
    check(erase(n, 0), "erase block 0");
    check(status(n) == 0x40, "status after a stored erase is ready, pass");

    check(program(n, 5, 0x12), "program page 5");
    check(status(n) == 0x40, "status after a stored program is ready, pass");
    check(readFirst(n, 5) == 0x12, "page 5 reads back");

    store->fail = true;
    check(program(n, 6, 0x34), "a program the backing refuses doesn't fail the bus");
    check(status(n) == 0x41, "status after a refused program is ready, fail");
    check(erase(n, 64), "an erase the backing refuses doesn't fail the bus");
    check(status(n) == 0x41, "status after a refused erase is ready, fail");

    store->fail = false;
    check(program(n, 7, 0x56), "program page 7 after the failures");
    check(status(n) == 0x40, "the next stored program clears fail");
    check(readFirst(n, 7) == 0x56, "page 7 reads back");
    check(readFirst(n, 5) == 0x12, "page 5 still reads back");

    if (failures) return 1;
    std::printf("nand-store-fail: ok\n");
    return 0;
}
