// The classic Memory Stick in the v2 .frondms layout (LogicalMsMedia under MemoryStickMedia) against
// the flat physical model it replaced: random reads, programs, erases and extra-byte changes, the
// TG50 driver's mount and block commits cut off at random points, checkpoints and reopens. Every
// read must give the same bytes, always, and every claimed L must sit in its slot.
//
// Usage: test-ms-logical-equivalence [sequences [operations]]

#include "FileCardBacking.h"
#include "MemoryStickImage.h"
#include "MemoryStickMedia.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <unistd.h>
#include <vector>

namespace {
using M = MemoryStickMedia;
using G = frondMsGeometry::Geometry;
constexpr uint32_t R = M::recordBytes;

// The physical model as it was: a flat image, AND on program, 0xff on erase.
struct Flat {
    G g;
    std::vector<uint8_t> bytes;
    uint8_t* at(uint32_t b, uint32_t p) { return bytes.data() + (size_t(b) * g.pagesPerBlock + p) * R; }
    void program(uint32_t b, uint32_t p, const M::Page& in, bool data, bool extra) {
        auto* r = at(b, p);
        if (data)
            for (unsigned i = 0; i < 512; ++i) r[i] &= in[i];
        if (extra)
            for (unsigned i = 512; i < R; ++i) r[i] &= in[i];
    }
    void erase(uint32_t b) { std::fill_n(at(b, 0), size_t(g.pagesPerBlock) * R, 0xff); }
};

uint64_t operations = 0, reads = 0, fullChecks = 0, swaps = 0;

struct Run {
    std::mt19937_64 rng;
    Flat flat;
    M media;
    std::shared_ptr<FileCardBacking> file;  // when the stored bytes are file-backed
    std::filesystem::path path;

    uint32_t below(uint32_t n) { return uint32_t(rng() % n); }
    bool chance(unsigned percent) { return below(100) < percent; }

    void attach(bool onFile) {
        if (!onFile) {
            assert(media.Attach(flat.bytes, false) == M::Result::ok);
            return;
        }
        std::vector<uint8_t> stored;
        assert(frondMs::storedOf(flat.bytes, stored));
        std::vector<uint8_t> out(frondSdFile::headerBytes, 0);
        out.insert(out.end(), stored.begin(), stored.end());
        {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
            assert(f.good());
        }
        FileCardOptions options;
        options.cacheBytes = 1u << 20;  // small, so pages go out to .gen files and back
        file = std::shared_ptr<FileCardBacking>(
            FileCardBacking::Open(path, stored.size(), frondSdFile::headerBytes, options).release());
        assert(file);
        auto* raw = file.get();
        M::Store store;
        store.read = [raw](uint64_t off, uint64_t n, void* dest) { return raw->Read(off, n, dest); };
        store.write = [raw](uint64_t off, uint64_t n, const void* src) { return raw->Write(off, n, src); };
        store.snapshot = [raw] { return raw->Snapshot(); };
        store.restore = [raw](const std::shared_ptr<const CardSnapshot>& pin) { return raw->Restore(*pin); };
        assert(media.AttachStore(flat.g, std::move(store), false) == M::Result::ok);
    }

    void same(uint32_t b, uint32_t p) {
        M::Page got;
        assert(media.Read(b, p, got) == M::Result::ok);
        assert(std::equal(got.begin(), got.end(), flat.at(b, p)));
        ++reads;
    }
    void sameAll() {
        std::vector<uint8_t> image;
        assert(media.CopyImage(image) == M::Result::ok);
        assert(image == flat.bytes);
        assert(media.Logical().Consistent());
        ++fullChecks;
    }

    void program(uint32_t b, uint32_t p, const M::Page& in, bool data, bool extra) {
        assert(media.Program(b, p, in, data, extra) == M::Result::ok);
        flat.program(b, p, in, data, extra);
        ++operations;
        if (chance(30)) same(b, p);
    }
    void erase(uint32_t b) {
        assert(media.Erase(b) == M::Result::ok);
        flat.erase(b);
        ++operations;
        if (chance(30)) same(b, below(flat.g.pagesPerBlock));
    }

    M::Page randomPage(unsigned onesPercent) {
        M::Page page;
        for (auto& v : page) v = chance(onesPercent) ? 0xff : uint8_t(rng());
        return page;
    }
    M::Page extraPage(uint8_t overwrite, uint8_t management, uint16_t logical) {
        M::Page page;
        page.fill(0xff);
        page[512] = overwrite;
        page[513] = management;
        page[514] = uint8_t(logical >> 8);
        page[515] = uint8_t(logical);
        return page;
    }

    const frondMsLayout::Layout& layout() const { return *frondMs::layoutOf(flat.g); }
    uint32_t blockIn(uint32_t segment) {
        const uint32_t first = std::max<uint32_t>(2, segment * 512);
        return first + below(std::min(flat.g.blocks, (segment + 1) * 512) - first);
    }
    // An L, mostly from a few per segment so blocks fight over them.
    uint32_t pickLogical(uint32_t& segment) {
        segment = below(layout().Segments());
        const uint32_t first = layout().SegmentFirst(segment), end = layout().SegmentEnd(segment);
        return chance(70) ? first + below(std::min<uint32_t>(6, end - first)) : first + below(end - first);
    }
    bool blank(uint32_t b) {
        const auto* p = flat.at(b, 0);
        return std::all_of(p, p + size_t(flat.g.pagesPerBlock) * R, [](uint8_t v) { return v == 0xff; });
    }
    int32_t claimantOf(uint32_t logical, uint32_t segment) {
        for (uint32_t b = std::max<uint32_t>(2, segment * 512); b < std::min(flat.g.blocks, (segment + 1) * 512); ++b)
            if (frondMsLayout::claimOf(layout(), b, flat.at(b, 0) + 512).logical == int32_t(logical)) return int32_t(b);
        return -1;
    }

    // The driver's block commit for L, cut off at a random page or step: UDST clear on the old
    // block, the new one written in page order, then the old one erased.
    void commit() {
        uint32_t segment;
        const uint32_t logical = pickLogical(segment);
        const int32_t old = claimantOf(logical, segment);
        uint32_t fresh = blockIn(segment);
        for (int tries = 0; tries < 20 && !blank(fresh); ++tries) fresh = blockIn(segment);
        if (!blank(fresh)) return;
        const bool cut = chance(15);
        const uint32_t stopAt = cut ? below(flat.g.pagesPerBlock + 2) : ~0u;
        if (old >= 0) program(uint32_t(old), 0, extraPage(0xef, 0xff, 0xffff), false, true);
        for (uint32_t p = 0; p < flat.g.pagesPerBlock; ++p) {
            if (p == stopAt) return;
            M::Page page = randomPage(10);
            const bool copied = old >= 0 && chance(40);
            if (copied) {
                std::copy_n(flat.at(uint32_t(old), p), 512, page.begin());
                std::copy_n(flat.at(uint32_t(old), p) + 512, 9, page.begin() + 512);
                page[512] |= 0x10;
            } else {
                const auto e = extraPage(0xf8, 0xff, uint16_t(logical));
                std::copy(e.begin() + 512, e.end(), page.begin() + 512);
            }
            program(fresh, p, page, true, true);
        }
        if (stopAt == flat.g.pagesPerBlock) return;
        if (old >= 0) erase(uint32_t(old));
        ++swaps;
    }

    // Mount: an unmapped L gets a block with only page 0 extra bytes.
    void mapAtMount() {
        uint32_t segment;
        const uint32_t logical = pickLogical(segment);
        const uint32_t b = blockIn(segment);
        if (blank(b)) program(b, 0, extraPage(0xf8, 0xff, uint16_t(logical)), false, true);
    }

    // Page 0 extra bytes of a written block changed on their own: obsolete, bad, another L, junk.
    void changeExtra() {
        const uint32_t b = 2 + below(flat.g.blocks - 2);
        M::Page page;
        page.fill(0xff);
        switch (below(6)) {
            case 0: page[512] = 0xef; break;   // UDST clear
            case 1: page[512] = 0x7f; break;   // marked bad
            case 2: page[512] = 0x9f; break;   // PGST not 11
            case 3: page[513] = 0xf7; break;   // ATFLG clear
            case 4: page[514] = uint8_t(rng()); page[515] = uint8_t(rng()); break;  // L ANDed down
            default:
                for (unsigned i = 512; i < R; ++i) page[i] = uint8_t(rng() | rng());
        }
        program(b, 0, page, false, true);
    }

    // Plain data with no claim, enough of it to fill the spill area and borrow logical slots.
    void scribble() {
        const uint32_t b = 2 + below(flat.g.blocks - 2);
        program(b, below(flat.g.pagesPerBlock), randomPage(20), true, chance(30));
    }

    void reopen() {
        // The stored bytes read back from scratch give the same stick.
        if (file) {
            std::vector<uint8_t> stored(size_t(file->Size()));
            assert(file->Read(0, stored.size(), stored.data()));
            LogicalMsMedia again;
            assert(frondMs::openStored(again, flat.g, [&](uint64_t off, uint64_t n, void* dest) {
                std::memcpy(dest, stored.data() + off, size_t(n));
                return true;
            }));
            std::vector<uint8_t> image;
            assert(frondMs::physicalOf(again, flat.g, image) && image == flat.bytes && again.Consistent());
        } else {
            std::vector<uint8_t> stored;
            assert(media.CopyStored(stored));
            M copy;
            auto ram = std::make_shared<RamCardBacking>(std::move(stored), 4096);
            M::Store store;
            store.read = [ram](uint64_t off, uint64_t n, void* dest) { return ram->Read(off, n, dest); };
            store.write = [ram](uint64_t off, uint64_t n, const void* src) { return ram->Write(off, n, src); };
            store.snapshot = [ram] { return ram->Snapshot(); };
            store.restore = [ram](const std::shared_ptr<const CardSnapshot>& pin) { return ram->Restore(*pin); };
            assert(copy.AttachStore(flat.g, std::move(store), false) == M::Result::ok);
            std::vector<uint8_t> image;
            assert(copy.CopyImage(image) == M::Result::ok && image == flat.bytes);
        }
        // And Build of the flat image lays out a stick that reads the same.
        std::vector<uint8_t> rebuilt;
        assert(frondMs::storedOf(flat.bytes, rebuilt));
        LogicalMsMedia built;
        assert(frondMs::openStored(built, flat.g, [&](uint64_t off, uint64_t n, void* dest) {
            std::memcpy(dest, rebuilt.data() + off, size_t(n));
            return true;
        }));
        std::vector<uint8_t> image;
        assert(frondMs::physicalOf(built, flat.g, image) && image == flat.bytes && built.Consistent());
    }

    void sequence(const G& g, uint64_t seed, uint64_t steps, bool onFile, const std::filesystem::path& dir) {
        rng.seed(seed);
        flat.g = g;
        flat.bytes = frondMs::create(g);
        path = dir / ("eq-" + std::to_string(seed) + ".frondms");
        // Some sticks start from the import layout, every L at home, others blank.
        if (chance(30)) {
            std::vector<uint8_t> raw(size_t(layout().PayloadBytes()));
            for (auto& v : raw) v = uint8_t(rng());
            flat.bytes = frondMs::fromLogical(raw.data(), raw.size(), g);
        }
        attach(onFile);
        sameAll();
        std::vector<std::pair<M::Checkpoint, std::vector<uint8_t>>> pins;
        const unsigned scribbleBias = below(3) == 0 ? 60 : 10;
        for (uint64_t i = 0; i < steps; ++i) {
            const unsigned roll = below(100);
            if (roll < scribbleBias) scribble();
            else if (roll < scribbleBias + 20) commit();
            else if (roll < scribbleBias + 28) changeExtra();
            else if (roll < scribbleBias + 33) mapAtMount();
            else if (roll < scribbleBias + 38) erase(2 + below(g.blocks - 2));
            else if (roll < scribbleBias + 39) erase(below(2));
            else if (roll < scribbleBias + 41) {
                if (pins.size() < 3) pins.emplace_back(media.SaveCheckpoint(), flat.bytes);
            } else if (roll < scribbleBias + 42 && !pins.empty()) {
                const size_t k = below(uint32_t(pins.size()));
                assert(media.RestoreCheckpoint(pins[k].first) == M::Result::ok);
                flat.bytes = pins[k].second;
                pins.erase(pins.begin() + std::ptrdiff_t(k));
                sameAll();
            } else {
                same(below(g.blocks), below(g.pagesPerBlock));
            }
            if (i % 500 == 499) sameAll();
            if (i % 2500 == 2499) reopen();
        }
        sameAll();
        reopen();
        pins.clear();
        media.Detach();
        if (file) {
            file.reset();
            FileCardBacking::DiscardGenerations(path);
            std::filesystem::remove(path);
        }
    }
};
}  // namespace

int main(int argc, char** argv) {
    const uint64_t sequences = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 90;
    const uint64_t steps = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 2000;
    const auto dir = std::filesystem::path("build") / "ms-logical-equivalence";
    std::filesystem::create_directories(dir);
    uint64_t done = 0;
    for (uint64_t s = 0; s < sequences; ++s) {
        // Mostly the small sizes, where spill fills and borrowing happens; every size some of the time.
        static const unsigned sizes[] = {4, 4, 4, 8, 8, 16, 32, 64, 128};
        const auto& g = *frondMsGeometry::byMegabytes(sizes[s % 9]);
        const uint64_t n = g.megabytes >= 64 ? steps / 3 : steps;
        Run run;
        run.sequence(g, 0x5eed0000 + s, n, s % 3 == 1, dir);
        ++done;
    }
    std::filesystem::remove_all(dir);
    std::printf("Memory Stick v2 layout equivalence passed: %llu sequences, %llu operations, %llu page reads, %llu "
                "full compares, %llu block commits\n",
                (unsigned long long)done, (unsigned long long)operations, (unsigned long long)reads,
                (unsigned long long)fullChecks, (unsigned long long)swaps);
}
