// ARM64 backend tests use an arena and split tables with deliberately different permissions.
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>

#include "dynarmic/interface/A32/a32.h"
#include "jit_fastmem_arena.h"

namespace {

namespace A32 = Dynarmic::A32;
using PageTable = std::array<uint8_t*, A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;

constexpr uint32_t pageSize = JitFastmemArena::pageSize;
constexpr uint32_t ram = pageSize * 2;
constexpr uint32_t flash = pageSize * 3;
constexpr uint32_t io = pageSize * 4;
constexpr uint32_t mmioValue = 0x13579bdf;

struct Callbacks final : A32::UserCallbacks {
    A32::Jit* jit = nullptr;
    uint32_t* data = nullptr;
    JitFastmemArena* arena = nullptr;
    unsigned reads = 0, writes = 0, faults = 0;
    uint64_t ticks = 0;
    unsigned slowSites = 0, retryFaults = 0, cappedSites = 0;
    bool disable = false, mapRamOnWrite = false, rejectEntry = false;

    void finishFault(A32::VAddr va, bool write) {
        if (!jit->FrondFastmemFaultPending()) return;
        const auto fault = jit->FrondFastmemFaultSlow(!arena->allows(va / pageSize, write));
        using Fault = A32::Jit::FastmemFault;
        assert(fault != Fault::None);
        faults++;
        slowSites += fault == Fault::Unmapped;
        retryFaults += fault == Fault::Retry;
        cappedSites += fault == Fault::Capped;
    }
    static constexpr std::array<uint32_t, 4> code{
        0xe5904000,  // ldr r4, [r0]
        0xe5814000,  // str r4, [r1]
        0xe5915000,  // ldr r5, [r1]
        0xeafffffe,  // b .
    };

    bool ShouldCompile(A32::VAddr pc, bool) override { return pc != 0 || !rejectEntry; }
    std::optional<uint32_t> MemoryReadCode(A32::VAddr va) override {
        if (va / 4 < code.size()) return code[va / 4];
        return std::nullopt;
    }
    uint8_t MemoryRead8(A32::VAddr) override { std::abort(); }
    uint16_t MemoryRead16(A32::VAddr) override { std::abort(); }
    uint64_t MemoryRead64(A32::VAddr) override { std::abort(); }
    uint32_t MemoryRead32(A32::VAddr va) override {
        reads++;
        finishFault(va, false);
        if (va == io && disable) jit->FrondDisableFastmem();
        if (va == ram) return data[0];
        if (va == flash) return data[pageSize / 4];
        return mmioValue;
    }
    void MemoryWrite8(A32::VAddr, uint8_t) override { std::abort(); }
    void MemoryWrite16(A32::VAddr, uint16_t) override { std::abort(); }
    void MemoryWrite64(A32::VAddr, uint64_t) override { std::abort(); }
    void MemoryWrite32(A32::VAddr va, uint32_t value) override {
        writes++;
        if (va == ram) {
            data[0] = value;
            if (mapRamOnWrite) arena->setPage(ram / pageSize, reinterpret_cast<uint8_t*>(data));
        }
        finishFault(va, true);
        // Flash takes the callback and keeps its bytes.
    }
    void InterpreterFallback(A32::VAddr, size_t) override { std::abort(); }
    void CallSVC(uint32_t) override { std::abort(); }
    void ExceptionRaised(A32::VAddr, A32::Exception) override { std::abort(); }
    void AddTicks(uint64_t used) override { ticks += used; }
    uint64_t GetTicksRemaining() override { return ticks >= 8 ? 0 : 8 - ticks; }
};

struct Fixture {
    JitFastmemArenaStats stats;
    JitFastmemArena arena{stats};
    GuestMemory::Bytes bytes{GuestMemory::byteAllocator()};
    std::unique_ptr<PageTable> book = std::make_unique<PageTable>();
    std::unique_ptr<PageTable> reads = std::make_unique<PageTable>();
    std::unique_ptr<PageTable> writes = std::make_unique<PageTable>();
    std::array<uint32_t, (20512 + 2048 * 8) / 4> costHints{};
    bool cost = false;
    Callbacks cb;
    std::unique_ptr<A32::Jit> jit;

    Fixture() {
        assert(arena.base());
        bytes.resize(pageSize * 2);
        cb.data = reinterpret_cast<uint32_t*>(bytes.data());
        cb.arena = &arena;
        assert(GuestMemory::find(bytes.data(), bytes.size()).fd >= 0);
        cb.data[0] = 0x40506070;
        cb.data[pageSize / 4] = 0x11223344;
    }
    void mapTables(uint32_t va, size_t byteOffset, bool write) {
        auto* host = bytes.data() + byteOffset;
        (*book)[va / pageSize] = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(host) | !write);
        (*reads)[va / pageSize] = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(host) - va);
        if (write) (*writes)[va / pageSize] = (*reads)[va / pageSize];
    }
    void makeJit() {
        A32::UserConfig conf;
        conf.callbacks = &cb;
        conf.arch_version = A32::ArchVersion::v5TE;
        conf.fastmem_pointer = reinterpret_cast<uintptr_t>(arena.base());
        conf.frond_fastmem_defer_faults = true;
        conf.recompile_on_fastmem_failure = false;
        conf.page_table = book.get();
        conf.frond_read_page_table = reads.get();
        conf.frond_write_page_table = writes.get();
        conf.frond_cost_hint = costHints.data();
        conf.frond_cost_hint_on = &cost;
        conf.page_table_pointer_mask_bits = 1;
        conf.page_table_write_protect_bit = true;
        conf.detect_misaligned_access_via_page_table = 16 | 32 | 64;
        conf.check_halt_on_memory_access = true;
        conf.check_halt_on_link = true;
        jit = std::make_unique<A32::Jit>(conf);
        cb.jit = jit.get();
    }
    Dynarmic::HaltReason run(uint32_t from, uint32_t to) {
        cb.ticks = 0;
        jit->SetCpsr(0xd3);
        auto& r = jit->Regs();
        r[0] = from;
        r[1] = to;
        r[15] = 0;
        const auto reason = jit->Run();
        assert(r[15] == (Callbacks::code.size() - 1) * 4);
        return reason;
    }
};

void slowSiteAndFlash() {
    Fixture f;
    f.mapTables(ram, 0, true);
    f.mapTables(flash, pageSize, false);
    f.arena.setPage(flash / pageSize, (*f.book)[flash / pageSize]);
    f.makeJit();
    const auto result = f.run(ram, flash);
    assert(!Dynarmic::Has(result, Dynarmic::HaltReason::CacheInvalidation));
    assert(f.cb.reads == 1 && f.cb.writes == 1 && f.cb.faults == 2 && f.cb.slowSites == 2);
    assert(f.jit->Regs()[4] == f.cb.data[0] && f.jit->Regs()[5] == 0x11223344);
    f.cb.rejectEntry = true;
    f.run(ram, flash);
    assert(f.cb.reads == 1 && f.cb.writes == 2 && f.cb.faults == 2);
    assert(f.cb.data[pageSize / 4] == 0x11223344);
}

void transientFaultCap() {
    Fixture f;
    f.mapTables(ram, 0, true);
    f.cb.mapRamOnWrite = true;
    f.makeJit();
    auto* readonly = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(f.cb.data) | 1);
    for (unsigned run = 1; run <= 32; run++) {
        f.arena.setPage(ram / pageSize, readonly);
        f.run(ram, ram);
        assert(f.cb.faults == run && f.cb.writes == run && f.cb.slowSites == 0);
    }
    assert(f.cb.retryFaults == 31 && f.cb.cappedSites == 1);
    f.arena.setPage(ram / pageSize, readonly);
    f.run(ram, ram);
    assert(f.cb.faults == 32 && f.cb.writes == 32);
    assert(f.jit->Regs()[5] == f.cb.data[0]);
}

void disableDuringBlock() {
    Fixture f;
    f.mapTables(ram, 0, true);
    f.arena.setPage(ram / pageSize, (*f.book)[ram / pageSize]);
    f.cb.disable = true;
    f.makeJit();
    f.run(io, ram);
    assert(f.cb.reads == 2 && f.cb.writes == 1 && f.cb.faults == 1);
    assert(f.cb.data[0] == mmioValue && f.jit->Regs()[5] == mmioValue);
    f.jit->Reset();
    f.run(ram, ram);
    assert(f.cb.reads == 2 && f.cb.writes == 1 && f.cb.faults == 1);
    assert(f.jit->Regs()[5] == mmioValue);
}

void alignmentAndCost() {
    Fixture f;
    f.mapTables(ram, 0, true);
    f.mapTables(flash, pageSize, false);
    f.arena.setPage(ram / pageSize, (*f.book)[ram / pageSize]);
    f.arena.setPage(flash / pageSize, (*f.book)[flash / pageSize]);
    f.cost = true;
    f.makeJit();
    f.run(flash - 2, ram);
    // The unaligned crossing is sent to the callback before the host load, as are hint misses.
    assert(f.cb.reads == 2 && f.cb.writes == 1 && f.cb.faults == 0);
    assert(f.jit->Regs()[4] == mmioValue && f.jit->Regs()[5] == mmioValue);
    f.costHints[(ram >> 5) & 1023] = ram;
    f.costHints[1024 + ((ram >> 4) & 2047)] = ram;
    f.run(ram, ram);
    assert(f.cb.reads == 2 && f.cb.writes == 1 && f.cb.faults == 0);
    assert(f.jit->Regs()[5] == mmioValue);
}

}  // namespace

int main() {
    slowSiteAndFlash();
    transientFaultCap();
    disableDuringBlock();
    alignmentAndCost();
    std::puts("ARM64 fastmem fault policy, cap, fallback, alignment and cost checks passed");
}
