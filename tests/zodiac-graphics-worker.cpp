// The fill worker thread (Imx1GraphicsWorker) against fills run on the calling thread: the same bank bytes, dirty
// state and saved state at every sync point, with fills queued back to back, reading what earlier queued fills
// wrote, and CPU accesses inside and outside the queued ranges. Also the memory going away with jobs queued, the
// worker moving between memories and a full ring. Built with -fsanitize=thread too (test-zodiac-graphics-worker).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#include "zodiac-graphics-setup.h"
#include "imx1_graphics_commands.h"

using A = Imx1BootstrapAccess;
using C = Imx1GraphicsCommands;
using M = Imx1GraphicsMemory;

struct Record {
    std::vector<uint64_t> v;
    Record& Do32(uint32_t& x) { v.push_back(x); return *this; }
    Record& Do64(uint64_t& x) { v.push_back(x); return *this; }
    Record& DoBool(bool& x) { v.push_back(x); return *this; }
    template <class T> Record& DoVector(std::vector<T>& x, size_t) {
        v.push_back(x.size()); v.insert(v.end(), x.begin(), x.end()); return *this;
    }
    Record& Require(bool, const char*) { return *this; }
    bool ok() const { return true; }
};

static Imx1Eim eim;
static std::unique_ptr<Imx1Cs5Configuration> state;
static uint64_t seed = 0x9e3779b97f4a7c15ull;
static uint32_t next(uint32_t n) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return uint32_t(seed % n); }
static constexpr uint32_t region = 0x100000, regionBytes = 0x8000;

// A random fill of one of the kinds fastFill and fastTransform take (and now and then one they don't: a ROP fill),
// inside the region, its source anywhere in it, often over what the last fills wrote.
static C::Fill randomFill() {
    for (;;) {
        const unsigned kind = next(129);
        const bool turn = kind == 128;
        const bool pixels = kind & 1, copy = kind & 2, keyed = copy && (kind & 4), doubled = copy && (kind & 8);
        const bool invert = kind & 16, a4 = kind & 32, painted = keyed && (kind & 64);
        if ((!copy && (kind & (4 | 8 | 64))) || (a4 && doubled) || (pixels && copy)) continue;
        C::Fill f{};
        f.width = 1 + next(40); f.height = 1 + next(20);
        f.pitch = next(4) ? f.width * 2 + next(40) * 2 : 2 + next(f.width) * 2;
        f.base = region + 64 + next(0x2000) * 2;
        f.keyed = keyed; f.plain = copy && !keyed; f.a4 = a4; f.invert = invert; f.doubled = doubled;
        f.painted = painted; f.keyEqual = keyed && next(2);
        f.paint = !doubled ? next(0x10000) : painted || next(2) ? 0 : (0x40 + next(0xc1)) | (0x40 + next(0xc1)) << 16;
        const bool a4Pixels = a4 && next(2);
        f.colour = a4Pixels ? next(17) : a4 ? next(16) | next(16) << 8 | next(16) << 16 : keyed ? (next(2) ? 0 : next(0x10000)) : next(0x10000);
        if (copy || a4) {
            f.sourcePitch = f.width * 2 + next(30) * 2;
            f.sourceX = doubled ? next(5) : 0; f.sourceY = doubled ? next(5) : 0;
            f.sourceBase = next(3) ? region + next(0x2000) * 2 : f.base - 64 + next(64) * 2;
        }
        if (turn) {
            f.plain = f.keyEqual = true;
            const int32_t column = (1 + int32_t(next(32))) * 2, row = (1 + int32_t(next(64))) * 2;
            f.colour = uint32_t(next(2) ? -column : column);
            f.sourcePitch = uint32_t(next(2) ? -row : row);
            f.base = region + 0x5000 + next(0x400) * 2;
            f.sourceBase = region + 0x6000 + next(0x400) * 2;
        }
        if (pixels || a4Pixels)
            for (uint32_t i = 0; i < f.width * f.height; ++i)
                f.pixels.push_back(uint16_t(a4 ? (next(3) ? next(2) * 15 : next(16)) : next(0x10000)));
        return f;
    }
}

static void fail(const char* what, unsigned step) {
    std::fprintf(stderr, "FAIL: %s at step %u\n", what, step);
    std::abort();
}
// Checks that run with or without NDEBUG, as several of them do the work they check.
static void require(bool ok, const char* what) {
    if (!ok) fail(what, 0);
}
#define REQUIRE(x) require((x), #x)
static void compareDirty(M& queued, M& direct, unsigned step, bool acknowledge) {
    for (unsigned c = 0; c < unsigned(M::Consumer::count); ++c)
        for (unsigned bank = 0; bank < 2; ++bank) {
            const auto a = queued.dirty(bank, M::Consumer(c)), b = direct.dirty(bank, M::Consumer(c));
            if (a.first != b.first || a.end != b.end || a.pending != b.pending || a.generation != b.generation)
                fail("dirty state", step);
            if (acknowledge) {
                queued.acknowledgeDirty(bank, M::Consumer(c), a.generation);
                direct.acknowledgeDirty(bank, M::Consumer(c), b.generation);
            }
        }
}
static void compareBanks(M& queued, M& direct, unsigned step) {
    for (unsigned bank = 0; bank < 2; ++bank) {
        uint32_t a = 0, b = 0;
        const uint8_t* x = queued.diagnosticData(bank, a);
        const uint8_t* y = direct.diagnosticData(bank, b);
        if (a != b || std::memcmp(x, y, a)) fail("bank bytes", step);
    }
}
static void fillBoth(M& queued, M& direct, unsigned step) {
    const auto f = randomFill();
    if (C::completeFill(*state, eim, queued, f) != A::ok || C::completeFill(*state, eim, direct, f) != A::ok)
        fail("fill status", step);
}
static void seedBoth(M& queued, M& direct) {
    for (uint32_t i = 0; i < regionBytes; i += 2) {
        uint32_t word = next(4) ? (next(3) ? 0 : 0xffff) : next(0x10000);
        REQUIRE(queued.accessGpu(*state, eim, region + i, 2, true, word) == A::ok);
        REQUIRE(direct.accessGpu(*state, eim, region + i, 2, true, word) == A::ok);
    }
}

// Fills, CPU reads and writes, spans, copies, dirty reads and saved state in a random order.
static void checkMixed() {
    static M queued, direct;
    Imx1GraphicsWorker worker;
    worker.attach(&queued);
    seedBoth(queued, direct);
    const uint64_t jobsBefore = worker.jobs();
    unsigned counts[8] = {};
    for (unsigned step = 0; step < 60000; ++step) {
        const unsigned op = next(100);
        if (op < 70) {
            fillBoth(queued, direct, step);
            ++counts[0];
        } else if (op < 80) {
            uint32_t word = next(0x10000);
            const uint32_t at = region + next(regionBytes / 2) * 2;
            REQUIRE(queued.accessGpu(*state, eim, at, 2, true, word) == A::ok);
            REQUIRE(direct.accessGpu(*state, eim, at, 2, true, word) == A::ok);
            ++counts[1];
        } else if (op < 90) {
            uint32_t a = 0, b = 0;
            const uint32_t at = region + next(regionBytes / 4) * 4;
            REQUIRE(queued.accessGpu(*state, eim, at, 4, false, a) == A::ok);
            REQUIRE(direct.accessGpu(*state, eim, at, 4, false, b) == A::ok);
            if (a != b) fail("CPU read", step);
            ++counts[2];
        } else if (op < 94) {
            unsigned bankA = 0, bankB = 0;
            uint32_t offsetA = 0, offsetB = 0;
            const uint32_t length = 2 + next(0x400) * 2, at = region + next((regionBytes - length) / 2) * 2;
            const uint8_t* a = queued.gpuSpan(*state, eim, at, length, bankA, offsetA);
            const uint8_t* b = direct.gpuSpan(*state, eim, at, length, bankB, offsetB);
            if (!a || !b || std::memcmp(a, b, length)) fail("span", step);
            ++counts[3];
        } else if (op < 96) {
            uint8_t a[256], b[256];
            const uint32_t at = region + next((regionBytes - 256) / 2) * 2;
            if (!queued.copyGpuRange(*state, eim, at, 256, a) || !direct.copyGpuRange(*state, eim, at, 256, b) ||
                std::memcmp(a, b, 256)) fail("copy", step);
            ++counts[4];
        } else if (op < 99) {
            compareDirty(queued, direct, step, next(2));
            ++counts[5];
        } else {
            Record a, b;
            queued.state(a);
            direct.state(b);
            queued.stateBytes(a, 0);
            direct.stateBytes(b, 0);
            if (a.v != b.v) fail("saved state", step);
            ++counts[6];
        }
    }
    compareDirty(queued, direct, 60000, false);
    compareBanks(queued, direct, 60000);
    const uint64_t jobs = worker.jobs() - jobsBefore;
    REQUIRE(jobs > 30000);
    std::printf("PASS: %llu queued fills match the direct ones through %u CPU writes, %u reads, %u spans, %u copies, "
                "%u dirty reads and %u saved states\n", (unsigned long long)jobs, counts[1], counts[2], counts[3],
                counts[4], counts[5], counts[6]);
}

// Back to back fills with no access between: the ring fills and the producer waits for the worker.
static void checkFullRing() {
    static M queued, direct;
    Imx1GraphicsWorker worker;
    worker.attach(&queued);
    seedBoth(queued, direct);
    const uint64_t fullBefore = imx1GraphicsWorkerStats.fullWaits.load();
    // Queued first and run on the reference afterwards, with the worker held for the first 50 ms.
    Imx1GraphicsWorker::holdForTests = true;
    std::thread release([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        Imx1GraphicsWorker::holdForTests = false;
    });
    std::vector<C::Fill> fills;
    for (unsigned step = 0; step < 20000; ++step) {
        fills.emplace_back();
        // Only fills the worker takes: a ROP fill on this thread would drain it.
        do fills.back() = randomFill(); while (fills.back().plain && fills.back().invert && !fills.back().doubled &&
                                               !fills.back().keyEqual);
        if (C::completeFill(*state, eim, queued, fills.back()) != A::ok) fail("fill status", step);
    }
    release.join();
    for (const auto& f : fills) REQUIRE(C::completeFill(*state, eim, direct, f) == A::ok);
    compareDirty(queued, direct, 20000, false);
    compareBanks(queued, direct, 20000);
    const uint64_t full = imx1GraphicsWorkerStats.fullWaits.load() - fullBefore;
    REQUIRE(full > 0);
    std::printf("PASS: 20000 fills back to back match, %llu pushes found the ring full\n", (unsigned long long)full);
}

// A full ring whose jobs the worker has all done before the next push: the push takes their results and goes on.
static void checkFullRingDone() {
    static M queued, direct;
    Imx1GraphicsWorker worker;
    worker.attach(&queued);
    seedBoth(queued, direct);
    // One pixel each, so the jobs are published 128 at a time and the last one fills the ring.
    std::vector<C::Fill> fills(2049);
    for (unsigned i = 0; i < fills.size(); ++i) {
        C::Fill& f = fills[i];
        f.width = f.height = 1;
        f.pitch = 2;
        f.base = region + 64 + (i % 0x1000) * 2;
        f.colour = i * 7919 & 0xffff;
    }
    std::atomic<bool> done{false};
    std::thread watchdog([&] {
        for (unsigned i = 0; i < 1000 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!done) fail("push after the worker emptied a full ring", 2048);
    });
    Imx1GraphicsWorker::holdForTests = true;
    const uint64_t before = worker.jobs();
    for (unsigned i = 0; i < 2048; ++i) REQUIRE(C::completeFill(*state, eim, queued, fills[i]) == A::ok);
    // The last push published the whole ring: wait until the worker has run all of it.
    if (worker.jobs() - before != 2048) fail("jobs queued", unsigned(worker.jobs() - before));
    Imx1GraphicsWorker::holdForTests = false;
    while (worker.finished() != worker.jobs()) std::this_thread::yield();
    REQUIRE(C::completeFill(*state, eim, queued, fills[2048]) == A::ok);
    done = true;
    watchdog.join();
    for (const auto& f : fills) REQUIRE(C::completeFill(*state, eim, direct, f) == A::ok);
    compareDirty(queued, direct, 2049, false);
    compareBanks(queued, direct, 2049);
    std::printf("PASS: a push after the worker finished a full ring goes on\n");
}

// The memory destroyed with jobs queued, the worker moved to another memory with jobs queued, the worker destroyed
// with jobs queued, and attach(nullptr): each finishes every job first.
static void checkLifetimes() {
    static M reference;
    auto worker = std::make_unique<Imx1GraphicsWorker>();
    {
        auto first = std::make_unique<M>();
        worker->attach(first.get());
        REQUIRE(first->graphicsFence() == worker.get());
        seedBoth(*first, reference);
        for (unsigned step = 0; step < 3000; ++step) fillBoth(*first, reference, step);
        // Its destructor drains the worker before the bytes go.
        first.reset();
        REQUIRE(!worker->attachedMemory());
    }
    {
        static M reference2;
        auto second = std::make_unique<M>(), third = std::make_unique<M>();
        worker->attach(second.get());
        seedBoth(*second, reference2);
        for (unsigned step = 0; step < 3000; ++step) fillBoth(*second, reference2, step);
        // Moving on drains: the second memory holds every fill and lets the worker go.
        worker->attach(third.get());
        REQUIRE(!second->graphicsFence() && third->graphicsFence() == worker.get());
        compareDirty(*second, reference2, 0, false);
        compareBanks(*second, reference2, 0);
        static M reference3;
        seedBoth(*third, reference3);
        for (unsigned step = 0; step < 3000; ++step) fillBoth(*third, reference3, step);
        worker.reset();
        REQUIRE(!third->graphicsFence());
        compareDirty(*third, reference3, 0, false);
        compareBanks(*third, reference3, 0);
        worker = std::make_unique<Imx1GraphicsWorker>();
        worker->attach(third.get());
        for (unsigned step = 0; step < 3000; ++step) fillBoth(*third, reference3, step);
        worker->attach(nullptr);
        REQUIRE(!third->graphicsFence());
        compareDirty(*third, reference3, 0, false);
        compareBanks(*third, reference3, 0);
    }
    std::printf("PASS: destroying the memory, moving the worker and destroying it each finish the queued fills\n");
}

int main() {
    uint32_t value = 0x3e00;
    REQUIRE(eim.access(0x00220028, 4, true, value) == Imx1EimAccess::ok);
    value = 0x11111e01;
    REQUIRE(eim.access(0x0022002c, 4, true, value) == Imx1EimAccess::ok);
    state = std::make_unique<Imx1Cs5Configuration>(Imx1GraphicsProfile::virtual5744Compatible8MiB, false, false);
    configureGraphicsInterface(*state, eim);
    configureGraphicsController(*state, eim);
    checkMixed();
    checkFullRing();
    checkFullRingDone();
    checkLifetimes();
    std::printf("PASS: %llu jobs, %llu drains waited, %llu wakes\n",
                (unsigned long long)imx1GraphicsWorkerStats.jobs.load(),
                (unsigned long long)imx1GraphicsWorkerStats.waits.load(),
                (unsigned long long)imx1GraphicsWorkerStats.wakes.load());
}
