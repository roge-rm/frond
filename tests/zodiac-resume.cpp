// Zodiac save and resume on the real 1.1 ROM (ZODIAC_ROM) from a bring-up state (ZODIAC_STATE), through
// SocIMX1::Run with its 12 ms host limit on, as the app runs it. Skips without them. ZODIAC_TEST_FAST=1 runs
// the Fast engine with the JIT compiling in sync (FROND_JIT_SYNC=1), so its runs repeat.
// From a save, the machine that saved and the same save loaded again reach the same state byte for byte at
// the same guest point, each with its own random call sizes; and a load saved again at once gives the save
// back. Run twice, the whole thing gives the same states.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "audio_queue.h"
#include "soc_IMX1.h"

namespace {
std::vector<uint8_t> readFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

uint64_t hash(const std::vector<uint8_t>& bytes) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : bytes) h = (h ^ b) * 0x100000001b3ull;
    return h;
}

std::vector<uint8_t> save(SocIMX1& soc) {
    assert(soc.Save());
    const auto buffer = soc.GetSavestate();
    const auto* bytes = static_cast<const uint8_t*>(buffer.data);
    return {bytes, bytes + buffer.size};
}

// Runs ms of guest time in calls of 1 to 1000 CLK32 ticks. Run ends only at slice ends, so where the run
// stops depends on the guest alone.
void runFor(SocIMX1& soc, AudioQueue* queue, uint64_t ms, std::mt19937_64& rng) {
    const uint64_t until = soc.Status().time.ticks + ms * 32768 / 1000;
    uint32_t pcm[4096];
    for (;;) {
        const uint64_t now = soc.Status().time.ticks;
        if (now >= until || soc.Halted()) break;
        soc.Run(std::min<uint64_t>(1 + rng() % 1000, until - now), 32768);
        uint32_t first, last;
        if (soc.GetPendingFrame(first, last)) soc.ResetPendingFrame();
        while (audioQueuePopChunk(queue, pcm, 4096)) {}
    }
    assert(!soc.Halted());
}

// The section names of the two states that differ, for the failure message.
std::string differing(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    const auto sections = [](const std::vector<uint8_t>& s) {
        std::vector<std::pair<std::string, std::vector<uint8_t>>> out;
        uint32_t count;
        uint64_t table;
        std::memcpy(&count, s.data() + 28, 4);
        std::memcpy(&table, s.data() + 32, 8);
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* e = s.data() + table + i * 28;
            uint64_t offset, size;
            std::memcpy(&offset, e + 8, 8);
            std::memcpy(&size, e + 16, 8);
            out.push_back({std::string(reinterpret_cast<const char*>(e), 4),
                           std::vector<uint8_t>(s.begin() + offset, s.begin() + offset + size)});
        }
        return out;
    };
    std::string names;
    const auto x = sections(a), y = sections(b);
    for (const auto& [name, bytes] : x) {
        bool same = false;
        for (const auto& [other, otherBytes] : y) same |= other == name && otherBytes == bytes;
        if (!same) names += (names.empty() ? "" : " ") + name;
    }
    return names.empty() ? "(sizes or order)" : names;
}

void expectSame(const char* what, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a == b) return;
    std::fprintf(stderr, "zodiac resume: %s differ in %s\n", what, differing(a, b).c_str());
    std::exit(1);
}

// One pass: the bring-up state, 500 ms, a save, 1000 ms on; the save loaded again, 1000 ms on. Returns the end state.
std::vector<uint8_t> pass(SocIMX1& soc, AudioQueue* queue, const std::vector<uint8_t>& state, uint64_t seed) {
    std::mt19937_64 rng(seed);
    assert(soc.LoadBringUpState(state.data(), state.size()));
    runFor(soc, queue, 500, rng);
    const auto mid = save(soc);
    runFor(soc, queue, 1000, rng);
    const auto straight = save(soc);
    auto copy = mid;
    assert(soc.Load(copy.size(), copy.data()));
    expectSame("a load saved at once and its save", save(soc), mid);
    runFor(soc, queue, 1000, rng);
    const auto resumed = save(soc);
    expectSame("the run that saved and the save loaded", straight, resumed);
    return resumed;
}
}  // namespace

int main() {
    const char* romPath = std::getenv("ZODIAC_ROM");
    const char* statePath = std::getenv("ZODIAC_STATE");
    if (!romPath || !statePath || !*romPath || !*statePath) {
        std::printf("zodiac resume: skipped, needs ZODIAC_ROM and ZODIAC_STATE\n");
        return 0;
    }
    const char* fastEnv = std::getenv("ZODIAC_TEST_FAST");
    const bool fast = fastEnv && std::string(fastEnv) == "1";
    if (fast) setenv("FROND_JIT_SYNC", "1", 1);
    auto rom = readFile(romPath);
    const auto state = readFile(statePath);
    std::string why;
    SocIMX1* soc = SocIMX1::Create(rom.data(), rom.size(), why);
    if (!soc) { std::fprintf(stderr, "create: %s\n", why.c_str()); return 1; }
    if (fast) assert(soc->SetFastEngine(true));
    auto* queue = audioQueueCreate(1 << 14);
    soc->SetAudioQueue(queue);
    soc->SetPcmOutputEnabled(true);
    soc->SetPcmSuspended(false);
    const auto first = pass(*soc, queue, state, 1), second = pass(*soc, queue, state, 2);
    expectSame("two passes", first, second);
    std::printf("zodiac resume %s: PASS (end state %016llx)\n", fast ? "Fast" : "Exact", (unsigned long long)hash(first));
    return 0;
}
