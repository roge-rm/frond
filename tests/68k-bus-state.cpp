#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

#include "EmBusTimingSavestate.h"
#include "savestate/Savestate.h"

namespace {
    using namespace EmBusTiming;
    constexpr EmBusSteal model{20, 3, 11, 3};
    struct Target {
        template <typename T> void Save(T& state) { EmBusTiming::Save(state); }
        void Load(SavestateLoader<ChunkType>& state) { EmBusTiming::Load(state, 100); }
    } target;

    std::vector<uint8_t> Save() {
        Savestate<ChunkType> state;
        assert(state.Save(target));
        auto* data = static_cast<const uint8_t*>(state.GetBuffer());
        return {data, data + state.GetSize()};
    }
    bool Load(std::vector<uint8_t> bytes) {
        SavestateLoader<ChunkType> loader;
        return loader.Load(bytes.data(), bytes.size(), target);
    }
    void Rebuild() {
        Clear();
        EmSdram::Set(EmVzSdramTiming(0x4000, 0));
        Set(model, 100);
        Reset(100);
    }
    std::vector<uint64_t> Continue(uint64_t cycle) {
        std::vector<uint64_t> trace;
        for (unsigned i = 0; i < 24; ++i) {
            const unsigned wait = EmSdram::Access(0x1000 + (i % 3) * 0x100, i & 1, 1);
            const unsigned clocks = 12 + wait;
            windowCount += 0x10002;
            windowWait += wait;
            windowCycles += clocks;
            const auto end = cycle + clocks;
            const auto stolen = end >= EmBusTiming::next ? ChargeWindow(cycle, end) : 0;
            cycle = end + stolen;
            trace.insert(trace.end(), {cycle, wait, stolen, EmBusTiming::next});
        }
        return trace;
    }
}

int main() {
    // Register-only rebuilds retain a later fractional remainder from the abandoned branch.
    Clear();
    const EmBusSteal fractional{100, 1, 0, 0};
    Set(fractional, 100);
    const auto fractionCheckpoint = Capture();
    assert(Charge(199, 200, 2, 1, 0) == 0);
    Set({}, 0);
    Set(fractional, 100);
    assert(Charge(199, 200, 2, 1, 0) == 1);
    assert(Restore(fractionCheckpoint));
    assert(Charge(199, 200, 2, 1, 0) == 0);
    puts("PASS register-only rebuild reproduces +1 fractional clock; restoring history removes it");
    Rebuild();
    // Leave partial bus shares, both fractional charges and an open SDRAM page.
    auto cycle = Continue(100)[92];
    EmSdram::Access(0x1000, false, 1);
    auto savedState = Capture();
    assert(savedState.lcdFraction && savedState.refreshFraction);
    assert(savedState.openRow[0] != ~0u);
    windowCount = 0x20003;
    windowWait = 2;
    windowCycles = 17;
    const auto saved = Save();
    const auto expected = Continue(cycle);
    const auto expectedTail = Save();

    Rebuild();
    assert(Load(saved));
    const auto restored = Continue(cycle);
    assert(restored == expected);
    assert(Save() == expectedTail);
    puts("PASS bus deadlines, fractions, usage window and SDRAM rows continue at exact cycles");

    assert(Load(saved));
    const auto before = Save();
    // TOC is three words; chunk fields are version, model, SDRAM config, deadlines,
    // count, fractions, wait, cycles and four rows.
    for (auto [word, value] : std::vector<std::pair<unsigned, uint32_t>>{
             {0, 2}, {1, 0}, {5, 0}, {6, ~0u}, {7, ~0u},
             {8, ~0u}, {9, ~0u}, {12, 0x10000}, {13, 0x10000}, {14, 18}, {16, 0xfffffffe}}) {
        auto bad = saved;
        std::memcpy(bad.data() + 12 + 4 * word, &value, 4);
        // Each deadline needs both words changed to the disabled sentinel.
        if (word == 6 || word == 8) std::memcpy(bad.data() + 12 + 4 * (word + 1), &value, 4);
        if (word == 7 || word == 9) continue;
        assert(!Load(bad));
        assert(Save() == before);
    }
    auto trailing = saved;
    trailing.resize(trailing.size() + 4);
    ++reinterpret_cast<uint32_t*>(trailing.data())[2];
    assert(!Load(trailing));
    assert(Save() == before);
    auto truncated = saved;
    truncated.resize(truncated.size() - 4);
    assert(!Load(truncated));
    assert(Save() == before);
    puts("PASS malformed bus chunks leave all bus state unchanged");

    // An old session has no bus chunk; its register-derived model starts at now.
    std::vector<uint8_t> legacy(4, 0);
    assert(Load(legacy));
    const auto reset = Capture();
    assert(reset.steal == model && reset.nextLcd == 120 && reset.nextRefresh == 111);
    assert(!reset.lcdFraction && !reset.refreshFraction && !reset.count && !reset.wait && !reset.cycles);
    for (auto row : reset.openRow) assert(row == ~0u);
    const auto legacyState = Save();
    Continue(100);
    assert(Load(legacy));
    assert(Save() == legacyState);
    puts("PASS legacy sessions start deterministic bus phases with closed SDRAM pages");
    Clear();
    const auto clean = Capture();
    assert(clean.steal == EmBusSteal{} && clean.nextLcd == ~uint64_t(0) &&
           clean.nextRefresh == ~uint64_t(0) && clean.sdramConfig != reset.sdramConfig);
    assert(!clean.lcdFraction && !clean.refreshFraction && !clean.count && !clean.wait && !clean.cycles);
    for (auto row : clean.openRow) assert(row == ~0u);
    puts("PASS machine replacement clears bus model and history");
}
