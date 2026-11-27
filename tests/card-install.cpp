// The card edit behind a Windows CE install (CardInstallCopy.h): a new file goes in, a file of the same name is
// replaced, and a copy that fails part way leaves the old file whole. Run on a raw image, where data writes go
// straight in.
#include "CardInstallCopy.h"
#include "WinCeAutorun.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace frondStorage;
namespace fs = std::filesystem;

#define CHECK(x)                                                                    \
    do {                                                                            \
        if (!(x)) {                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #x); \
            std::exit(1);                                                           \
        }                                                                           \
    } while (0)

namespace {
std::vector<uint8_t> pattern(size_t n, uint8_t seed) {
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = uint8_t(i * 13 + seed);
    return out;
}

// Gives data, or throws once failAt bytes have gone.
void copy(FatImage& fat, const std::string& name, const std::vector<uint8_t>& data, size_t failAt = SIZE_MAX) {
    size_t at = 0;
    frondNative::copyToCardRoot(fat, name, [&](char* b, int64_t n) {
        if (at >= failAt) throw std::runtime_error("read failed");
        const size_t m = std::min({size_t(n), data.size() - at, failAt - at});
        std::memcpy(b, data.data() + at, m);
        at += m;
        return int64_t(m);
    }, int64_t(data.size()));
}

std::vector<uint8_t> contents(FatImage& fat, const std::string& path) {
    const auto e = fat.find(path);
    CHECK(e && !e->directory);
    std::vector<uint8_t> out;
    fat.read(*e, [&](const char* b, int64_t n) { out.insert(out.end(), b, b + n); });
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    const fs::path image = fs::path(argc > 1 ? argv[1] : "build") / "test-card-install.img";
    {
        std::ofstream(image, std::ios::binary | std::ios::trunc).flush();
        fs::resize_file(image, 4u << 20);
    }
    const auto old = pattern(70000, 1), fresh = pattern(90000, 7), small = pattern(100, 3);
    {
        RawBlocks raw(image, true);
        FatImage::format(raw);
    }
    {
        RawBlocks raw(image, true);
        FatImage fat(raw);
        copy(fat, "setup.cab", old);
        CHECK(contents(fat, "/setup.cab") == old);
        // A copy over it that fails part way: the old file is still all there, and nothing else is used.
        bool threw = false;
        try { copy(fat, "setup.cab", fresh, 40000); } catch (const std::exception&) { threw = true; }
        CHECK(threw);
        CHECK(contents(fat, "/setup.cab") == old);
        CHECK(fat.check().empty());
    }
    {
        // Read again from the image, then replaced for real.
        RawBlocks raw(image, true);
        FatImage fat(raw);
        CHECK(contents(fat, "/setup.cab") == old);
        copy(fat, "setup.cab", fresh);
        CHECK(contents(fat, "/setup.cab") == fresh);
        copy(fat, "note.txt", small);
        CHECK(contents(fat, "/note.txt") == small && contents(fat, "/setup.cab") == fresh);
        CHECK(fat.check().empty());
        // A folder of the name is refused and kept.
        fat.makeFolder("/", "Docs");
        bool threw = false;
        try { copy(fat, "Docs", small); } catch (const Error&) { threw = true; }
        CHECK(threw && fat.find("/Docs") && fat.find("/Docs")->directory);
        const auto endedEarly = [](char*, int64_t) -> int64_t { return 0; };
        CHECK(!frondNative::stageWinCeAutorun(fat, endedEarly, 10));
        CHECK(!fat.find("/2577"));
        size_t offset = 0;
        const auto readCab = [&](char* buffer, int64_t n) {
            const auto count = std::min(size_t(n), fresh.size() - offset);
            std::memcpy(buffer, fresh.data() + offset, count);
            offset += count;
            return int64_t(count);
        };
        CHECK(frondNative::stageWinCeAutorun(fat, readCab, int64_t(fresh.size())));
        CHECK(contents(fat, "/2577/FrondPending.cab") == fresh);
        const std::vector<uint8_t> helper(std::begin(frondNative::winCeAutorun), std::end(frondNative::winCeAutorun));
        CHECK(contents(fat, "/2577/autorun.exe") == helper);
        frondNative::clearWinCeAutorun(fat);
        CHECK(!fat.find("/2577/FrondPending.cab") && !fat.find("/2577/autorun.exe"));
        CHECK(contents(fat, "/setup.cab") == fresh);
        fat.add("/2577", "autorun.exe", [](char* buffer, int64_t) { *buffer = 'x'; return 1; }, 1);
        frondNative::clearWinCeAutorun(fat);
        CHECK(!frondNative::stageWinCeAutorun(fat, endedEarly, 10));
        CHECK(contents(fat, "/2577/autorun.exe") == std::vector<uint8_t>{'x'});
        CHECK(!fat.find("/2577/FrondPending.cab"));
        CHECK(fat.check().empty());
    }
    fs::remove(image);
    std::puts("card install: ok");
    return 0;
}
