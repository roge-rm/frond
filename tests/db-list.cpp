#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "db_list.h"
#include "syscall_dispatch.h"

// Model Palm's scratch allocation exactly: every guest read/write must fit.
static constexpr uint32_t base = 0x1000;
static std::vector<uint8_t> scratch;
static bool failAllocation = false, freed = false;
static unsigned queries = 0;
static void bounds(uint32_t p, size_t n) {
    assert(p >= base && p - base <= scratch.size() && n <= scratch.size() - (p - base));
}
static void put(uint32_t p, uint32_t value, size_t n) {
    bounds(p, n);
    for (size_t i = 0; i < n; ++i) scratch[p - base + i] = value >> ((n - i - 1) * 8);
}
bool syscallDispatchM68kSupport(SyscallDispatch*) { return true; }
bool syscallDispatchPrepare(SyscallDispatch*) { return true; }
uint16_t syscall68k_MemNumCards(SyscallDispatch*, uint32_t) { return 1; }
uint16_t syscall68k_DmNumDatabases(SyscallDispatch*, uint32_t, uint16_t) { return 1; }
uint32_t syscall68k_MemPtrNew(SyscallDispatch*, uint32_t, uint32_t n) {
    scratch.assign(n, 0);
    return failAllocation ? 0 : base;
}
uint16_t syscall68k_MemPtrFree(SyscallDispatch*, uint32_t, uint32_t p) {
    assert(p == base); freed = true; return 0;
}
uint32_t syscall68k_DmGetDatabase(SyscallDispatch*, uint32_t, uint16_t, uint16_t) {
    return 0x80001234;
}
uint16_t syscall68k_DmDatabaseInfo(SyscallDispatch*, uint32_t, uint16_t, uint32_t,
    uint32_t name, uint32_t attr, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
    uint32_t, uint32_t, uint32_t type, uint32_t creator) {
    ++queries;
    bounds(name, 32);
    std::memcpy(scratch.data() + name - base, "AciduliteLog", 12);
    put(attr, 0x0008, 2); // Backup bit; not a resource database.
    put(type, 0x4c6f6720, 4);
    put(creator, 0x526d7266, 4);
    return 0;
}
bool syscallDispatch_strncpy_toHost(SyscallDispatch*, void* out, uint32_t p, size_t n) {
    bounds(p, n); std::memcpy(out, scratch.data() + p - base, n); return true;
}
uint32_t paceGet32(uint32_t p) {
    bounds(p, 4); uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) v = (v << 8) | scratch[p - base + i];
    return v;
}
uint16_t paceGet16(uint32_t p) {
    bounds(p, 2); return (scratch[p - base] << 8) | scratch[p - base + 1];
}
int main() {
    DbMetadataList list;
    assert(dbListGet(nullptr, list));
    assert(freed && queries == 1 && list.size() == 1);
    const auto& db = list.front();
    assert(!std::strcmp(db.name, "AciduliteLog"));
    assert(db.type == 0x4c6f6720 && db.creator == 0x526d7266 && db.attr == 8);
    assert(db.localID == 0x80001234 && !db.isRom);
    failAllocation = true; freed = false;
    assert(!dbListGet(nullptr, list));
    assert(!freed && queries == 1);
    puts("PASS: database enumeration stays within guest scratch allocation and reads 16-bit attributes");
}
