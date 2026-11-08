// The storage library's FatImage on FAT12 cards (storage/FatImage.h): the layouts format() picks,
// FAT32 refused, cut-off changes on a FAT12 card file settling, and the cards tests/storage-fat.py
// reads back with mtools. Commands:
//   selftest                     layouts, refusals, crashes
//   make OUT MIB raw|container   formats a card, fills it, writes OUT.expect
//   fill IMAGE                   fills a raw image another tool made, writes IMAGE.expect
//   list IMAGE                   prints what is on a raw image, as the .expect files have it
//   refuse IMAGE                 prints why FatImage refuses the image
#include "storage/FatImage.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace frondStorage;

namespace {

#define CHECK(x)                                                                         \
    do {                                                                                 \
        if (!(x)) {                                                                      \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x);   \
            std::exit(1);                                                                \
        }                                                                                \
    } while (0)

struct Crash {};

std::vector<uint8_t> randomBytes(size_t n, uint32_t seed) {
    std::mt19937 g(seed);
    std::vector<uint8_t> b(n);
    for (auto& c : b) c = uint8_t(g());
    return b;
}

void writeFile(const fs::path& p, const std::vector<uint8_t>& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    CHECK(f.good());
}

void put(FatImage& fat, const std::string& folder, const std::string& name, const std::vector<uint8_t>& data) {
    size_t at = 0;
    fat.add(folder, name, [&](char* b, int64_t n) {
        const size_t m = std::min<size_t>(size_t(n), data.size() - at);
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

// Each path with "dir" or its size and CRC-32, sorted by path.
std::string listing(FatImage& fat) {
    std::map<std::string, std::string> out;
    std::function<void(const std::string&)> walk = [&](const std::string& path) {
        for (const auto& e : fat.list(path)) {
            const auto full = FatImage::joined(path, e.name);
            if (e.directory) {
                out[full] = "dir";
                walk(full);
            } else {
                const auto bytes = contents(fat, full);
                char crc[16];
                std::snprintf(crc, sizeof(crc), "%08x", unsigned(frondSdFile::crc32(bytes.data(), bytes.size())));
                out[full] = std::to_string(bytes.size()) + "\t" + crc;
            }
        }
    };
    walk("/");
    std::string text;
    for (const auto& [path, what] : out) text += path + "\t" + what + "\n";
    return text;
}

void checkSound(FatImage& fat) {
    const auto problems = fat.check();
    for (const auto& p : problems) std::fprintf(stderr, "problem: %s\n", p.c_str());
    CHECK(problems.empty());
}

// Folders, long and non-ASCII names, a file across many FAT sectors, files at cluster edges, a
// folder grown past one cluster, then copy, move, rename, delete and replace.
void fill(FatImage& fat) {
    const auto cluster = size_t(fat.clusterBytes());
    fat.makeFolder("/", "PALM");
    fat.makeFolder("/PALM", "Launcher");
    if (!fat.find("/DCIM")) fat.makeFolder("/", "DCIM");
    if (!fat.find("/DCIM/100FROND")) fat.makeFolder("/DCIM", "100FROND");
    put(fat, "/PALM/Launcher", "A rather long file name for a card, with spaces.prc", randomBytes(70000, 1));
    put(fat, "/PALM/Launcher", "h\xc3\xa9llo \xc3\xbc" "ber \xe6\x97\xa5\xe6\x9c\xac.txt", {'c', 'a', 'f', 0xc3, 0xa9});
    // A third of the free space: its chain crosses FAT12 entries that straddle sectors.
    const auto big = size_t(fat.freeBytes() / 3) / cluster * cluster - 100;
    put(fat, "/DCIM/100FROND", "IMG_0001.JPG", randomBytes(big, 2));
    put(fat, "/", "empty.txt", {});
    put(fat, "/", "exact.bin", randomBytes(cluster, 3));
    put(fat, "/", "plus.bin", randomBytes(cluster + 1, 4));
    fat.makeFolder("/", "MANY");
    // 300 names fill more than one cluster of a folder at 8 or 16 KiB; most are empty.
    for (int i = 0; i < 300; ++i) {
        char name[16];
        std::snprintf(name, sizeof(name), "F%03d.TXT", i);
        put(fat, "/MANY", name, i % 10 ? std::vector<uint8_t>() : std::vector<uint8_t>{uint8_t(i)});
    }
    fat.copy("/PALM/Launcher", "/", "Backup");
    fat.move("/exact.bin", "/PALM");
    fat.move("/plus.bin", "/", "Plus one.bin");
    fat.remove("/Backup/A rather long file name for a card, with spaces.prc");
    fat.replace("/empty.txt", [&] { put(fat, "/", "empty.txt", {'r', 'e', 'p', 'l', 'a', 'c', 'e', 'd'}); });
    for (int i = 0; i < 300; i += 3) {
        char name[24];
        std::snprintf(name, sizeof(name), "/MANY/F%03d.TXT", i);
        fat.remove(name);
    }
    checkSound(fat);
}

void writeExpect(FatImage& fat, const fs::path& out) {
    std::ofstream f(out.string() + ".expect", std::ios::trunc);
    f << "bits " << fat.fatBits() << "\n" << listing(fat);
    CHECK(f.good());
}

fs::path scratch() {
    const char* env = std::getenv("TMPDIR");
    auto dir = fs::path(env && *env ? env : "/tmp") / ("storage-fat-" + std::to_string(getpid()));
    fs::create_directories(dir);
    return dir;
}

// A v2 card file of `mib` MiB at generation 5 with a fresh FAT12 or FAT16 file system.
fs::path containerCard(const fs::path& dir, const std::string& name, unsigned mib) {
    const auto file = dir / name;
    std::vector<uint8_t> payload(static_cast<size_t>(mib) << 20), bytes;
    CHECK(frondSdFile::encodeV2(payload, 5, bytes));
    writeFile(file, bytes);
    {
        CardFile c(file, true);
        FatImage::format(c);
    }
    CHECK(verifiedGeneration(file) == std::optional<uint64_t>(6));
    return file;
}

void layouts() {
    struct Want {
        unsigned mib, bits, start, clusterSectors, fatSectors, clusters;
    };
    // Partition starts 27, 25, 57, 51 and 39 are what Memory Stick and SD cards of these sizes have.
    for (const auto& w : {Want{4, 12, 27, 16, 2, 508}, Want{8, 12, 25, 16, 3, 1020}, Want{16, 12, 57, 32, 3, 1021},
                          Want{32, 12, 51, 32, 6, 2045}, Want{64, 12, 39, 32, 12, 4084}, Want{128, 16, 95, 32, 32, 8186}}) {
        const auto l = fatLayout(int64_t(w.mib) << 20);
        std::printf("%u MiB: FAT%u, start %u, %u sectors, %u-sector clusters, FAT %u sectors, %u clusters\n", w.mib, l.bits,
                    l.start, l.sectors, l.clusterSectors, l.fatSectors, l.clusters);
        CHECK(l.bits == w.bits && l.start == w.start && l.clusterSectors == w.clusterSectors && l.fatSectors == w.fatSectors &&
              l.clusters == w.clusters);
        // The data area starts on a boundary unit.
        CHECK((l.start + 1 + 2 * l.fatSectors + 32) % l.boundary == 0);
    }
}

std::string refusal(const fs::path& image) {
    try {
        RawBlocks raw(image, false);
        FatImage fat(raw);
    } catch (const Error& e) {
        return e.message;
    }
    return {};
}

void fat32Refused(const fs::path& dir) {
    // A FAT32 boot sector as a superfloppy, and a FAT32 partition type.
    std::vector<uint8_t> image(static_cast<size_t>(40) << 20);
    uint8_t* b = image.data();
    b[0] = 0xeb;
    b[1] = 0x58;
    b[2] = 0x90;
    fat::put16(b + 11, 512);
    b[13] = 1;
    fat::put16(b + 14, 32);
    b[16] = 2;
    fat::put32(b + 32, 81920);
    fat::put32(b + 36, 640);
    std::memcpy(b + 82, "FAT32   ", 8);
    b[510] = 0x55;
    b[511] = 0xaa;
    writeFile(dir / "fat32.img", image);
    CHECK(refusal(dir / "fat32.img") == FatImage::fat32Message);
    std::vector<uint8_t> mbr(static_cast<size_t>(40) << 20);
    mbr[446 + 4] = 0x0c;
    fat::put32(mbr.data() + 446 + 8, 2048);
    fat::put32(mbr.data() + 446 + 12, 79872);
    mbr[510] = 0x55;
    mbr[511] = 0xaa;
    writeFile(dir / "fat32-part.img", mbr);
    CHECK(refusal(dir / "fat32-part.img") == FatImage::fat32Message);
}

// Cut off at each step of an add on a FAT12 card file, then settled: the card loads at the
// visit's generation, holds the change or not, and is sound.
void crashes(const fs::path& dir) {
    const auto data = randomBytes(300001, 7);
    for (const char* step : {"writing", "data", "journal", "partly", "applied"}) {
        const auto file = containerCard(dir, std::string("crash-") + step + ".frondsd", 8);
        {
            CardFile c(file, true);
            FatImage fat(c);
            CHECK(fat.fatBits() == 12);
            fat.makeFolder("/", "PALM");
        }
        {
            CardFile c(file, true);
            c.crashHook = [&](const char* at) {
                if (!std::strcmp(at, step)) {
                    c.dead = true;
                    throw Crash();
                }
            };
            FatImage fat(c);
            bool crashed = false;
            try {
                put(fat, "/PALM", "A long file name.prc", data);
            } catch (const Crash&) {
                crashed = true;
            }
            CHECK(crashed);
        }
        CHECK(unsettled(file));
        // No leftovers in the apps' older format.
        CHECK(!legacy::present(file));
        CHECK(settleCard(file));
        CHECK(!unsettled(file));
        CHECK(verifiedGeneration(file) == std::optional<uint64_t>(8));
        CardFile c(file, false);
        FatImage fat(c);
        checkSound(fat);
        const bool done = std::strcmp(step, "writing") && std::strcmp(step, "data");
        const auto e = fat.find("/PALM/A long file name.prc");
        CHECK(bool(e) == done);
        if (done) CHECK(contents(fat, "/PALM/A long file name.prc") == data);
    }
}

void selftest() {
    const auto dir = scratch();
    layouts();
    fat32Refused(dir);
    crashes(dir);
    // A FAT16 card still works as before, and FAT12 reads what format wrote.
    for (unsigned mib : {4u, 128u}) {
        const auto file = containerCard(dir, "fresh-" + std::to_string(mib) + ".frondsd", mib);
        CardFile c(file, true);
        FatImage fat(c);
        CHECK(fat.fatBits() == (mib <= 64 ? 12 : 16));
        put(fat, "/", "x.txt", {'x'});
        checkSound(fat);
    }
    fs::remove_all(dir);
    std::puts("storage fat: layouts, FAT32 refused, FAT12 crashes settle");
}

}  // namespace

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "selftest";
    try {
        if (command == "selftest") {
            selftest();
        } else if (command == "make" && argc == 5) {
            const fs::path out = argv[2];
            const unsigned mib = unsigned(std::stoul(argv[3]));
            const bool container = std::string(argv[4]) == "container";
            if (container) {
                fs::remove(out);
                const auto file = containerCard(out.parent_path(), out.filename().string(), mib);
                CardFile c(file, true);
                FatImage fat(c);
                fill(fat);
                writeExpect(fat, out);
            } else {
                writeFile(out, std::vector<uint8_t>(size_t(mib) << 20));
                RawBlocks raw(out, true);
                FatImage::format(raw);
                FatImage fat(raw);
                fill(fat);
                writeExpect(fat, out);
            }
        } else if (command == "fill" && argc == 3) {
            RawBlocks raw(argv[2], true);
            FatImage fat(raw);
            fill(fat);
            writeExpect(fat, argv[2]);
        } else if (command == "list" && argc == 3) {
            RawBlocks raw(argv[2], false);
            FatImage fat(raw);
            checkSound(fat);
            std::printf("bits %d\n%s", fat.fatBits(), listing(fat).c_str());
        } else if (command == "refuse" && argc == 3) {
            std::printf("%s\n", refusal(argv[2]).c_str());
        } else {
            std::fprintf(stderr, "usage: %s selftest | make OUT MIB raw|container | fill IMAGE | list IMAGE | refuse IMAGE\n",
                         argv[0]);
            return 2;
        }
    } catch (const Error& e) {
        std::fprintf(stderr, "error: %s\n", e.message.c_str());
        return 1;
    }
    return 0;
}
