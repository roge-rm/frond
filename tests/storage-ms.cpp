// Files on classic Memory Sticks (storage/ClassicStick.h through CardFile and FatImage): writes into
// held and unclaimed logical blocks read back through LogicalMsMedia's physical pages, sticks that
// aren't settled refused, the TG50's own format for each size, and cut-off changes settling.
// tests/storage-ms.py reads the formatted sticks back with mtools. Commands:
//   selftest       writer, refusals, format layout, crashes
//   make OUT MB    formats a blank OUT.frondms of MB, fills it, writes OUT (its logical image) and
//                  OUT.expect
//   ls STICK       lists the root of a stick a machine took out: "NAME SIZE" per file, "NAME/" per folder
//   put STICK NAME TEXT  adds a root file NAME holding TEXT, and reads it back
// (ls and put are for tests/frond-68k-ms.py.)
#include "storage/FatImage.h"

#include "MemoryStickImage.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using namespace frondStorage;
using frondMsLayout::Layout;

namespace {

#define CHECK(x)                                                                         \
    do {                                                                                 \
        if (!(x)) {                                                                      \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x);   \
            std::exit(1);                                                                \
        }                                                                                \
    } while (0)

struct Crash {};
constexpr uint32_t record = MemoryStickMedia::recordBytes;

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

std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

const frondMsGeometry::Geometry& geometry(unsigned mb) { return *frondMsGeometry::byMegabytes(mb); }
const Layout& layout(unsigned mb) { return *frondMs::layoutOf(geometry(mb)); }

uint8_t* page(std::vector<uint8_t>& physical, const Layout& l, uint32_t block, uint32_t p) {
    return physical.data() + (size_t(block) * l.pagesPerBlock + p) * record;
}

// A physical image as a v2 .frondms at generation 3.
void writeStick(const fs::path& file, const std::vector<uint8_t>& physical, const Layout& l) {
    std::vector<uint8_t> stored;
    CHECK(LogicalMsMedia::Build(l, physical.data(), stored));
    frondSdFile::ChunkCrcs crcs;
    crcs.add(stored.data(), stored.size());
    std::vector<uint8_t> out(frondSdFile::headerBytes), table;
    frondSdFile::makeHeaderV2(out.data(), l.PayloadBytes(), 3, crcs.table(), l.SideBytes(), true);
    out.insert(out.end(), stored.begin(), stored.end());
    frondSdFile::tableBytes(crcs.table(), table);
    out.insert(out.end(), table.begin(), table.end());
    writeFile(file, out);
    CHECK(verifiedGeneration(file) == std::optional<uint64_t>(3));
}

// A stick file read back as the core does: header and CRCs checked, then LogicalMsMedia.
struct Stick {
    Layout l;
    std::vector<uint8_t> stored;
    LogicalMsMedia media;
    explicit Stick(const fs::path& file) {
        CHECK(verifiedGeneration(file).has_value());
        auto bytes = readFile(file);
        frondSdFile::Info info;
        CHECK(frondSdFile::parseContainer(bytes.data(), info) && info.classic);
        l = *frondMsLayout::of(info);
        stored.assign(bytes.begin() + frondSdFile::headerBytes, bytes.begin() + frondSdFile::headerBytes + long(info.stored()));
        CHECK(media.Open(l, [this](uint64_t off, uint64_t n, void* dest) {
            if (off > stored.size() || n > stored.size() - off) return false;
            std::memcpy(dest, stored.data() + off, size_t(n));
            return true;
        }, [](uint64_t, uint64_t, const void*) { return false; }));
        CHECK(media.Consistent());
    }
    std::vector<uint8_t> physical(uint32_t block, uint32_t p) {
        std::vector<uint8_t> r(record);
        CHECK(media.Read(block, p, r.data()));
        return r;
    }
    std::vector<uint8_t> logical() {
        std::vector<uint8_t> image;
        CHECK(frondMs::logicalOf(media, image));
        return image;
    }
};

std::vector<uint8_t> newExtra(uint32_t L) { return {0xf8, 0xff, uint8_t(L >> 8), uint8_t(L), 0xff, 0xff, 0xff, 0xff, 0xff}; }

// Every page of `block` holds L as the driver's commit writes a new block.
void checkNewBlock(Stick& s, uint32_t block, uint32_t L) {
    CHECK(s.media.Holder(L) == int32_t(block));
    const auto want = newExtra(L);
    for (uint32_t p = 0; p < s.l.pagesPerBlock; ++p) {
        const auto r = s.physical(block, p);
        CHECK(std::equal(want.begin(), want.end(), r.begin() + 512));
    }
}

std::vector<uint8_t> viewOf(const fs::path& file) {
    CardFile c(file, false);
    std::vector<uint8_t> out(size_t(c.payloadBytes()));
    c.read(0, reinterpret_cast<char*>(out.data()), int64_t(out.size()));
    return out;
}

// Held blocks are written in place; an unclaimed L gets a free block in its segment, forward from
// its home block; a block that borrowed slot L moves out first.
void writer(const fs::path& dir) {
    {
        const auto& l = layout(16);
        // As a TG50 mount leaves it: every L at its home block with zero data.
        std::vector<uint8_t> zeros(size_t(l.PayloadBytes()));
        auto physical = frondMs::fromLogical(zeros.data(), zeros.size(), geometry(16));
        // L 5 and L 600 lose their blocks; L 10's home block claims L 11 instead, so L 10 has to
        // look past it.
        std::fill_n(page(physical, l, l.Home(5), 0), size_t(l.pagesPerBlock) * record, 0xff);
        std::fill_n(page(physical, l, l.Home(600), 0), size_t(l.pagesPerBlock) * record, 0xff);
        std::fill_n(page(physical, l, l.Home(11), 0), size_t(l.pagesPerBlock) * record, 0xff);
        for (uint32_t p = 0; p < l.pagesPerBlock; ++p) {
            page(physical, l, l.Home(10), p)[515] = 11;
            page(physical, l, l.Home(10), p)[0] = 0x5a;
        }
        const auto file = dir / "writer.frondms";
        writeStick(file, physical, l);
        auto model = viewOf(file);
        CHECK(model[size_t(11 * l.BlockBytes())] == 0x5a && model[size_t(10 * l.BlockBytes())] == 0);
        const auto held = randomBytes(1000, 1), into5 = randomBytes(512, 2), into600 = randomBytes(40000, 3),
                   into10 = randomBytes(512, 4);
        const int64_t heldAt = 4 * int64_t(l.BlockBytes()) + 3 * 512;
        {
            CardFile c(file, true);
            c.write(heldAt, reinterpret_cast<const char*>(held.data()), int64_t(held.size()));
            c.write(600 * int64_t(l.BlockBytes()) + 700, reinterpret_cast<const char*>(into600.data()), int64_t(into600.size()));
            Sectors sectors;
            sectors[5 * int64_t(l.BlockBytes()) + 2 * 512] = into5;
            sectors[10 * int64_t(l.BlockBytes())] = into10;
            c.commit(sectors);
        }
        std::copy(held.begin(), held.end(), model.begin() + heldAt);
        std::copy(into600.begin(), into600.end(), model.begin() + long(600 * l.BlockBytes() + 700));
        std::copy(into5.begin(), into5.end(), model.begin() + long(5 * l.BlockBytes() + 2 * 512));
        std::copy(into10.begin(), into10.end(), model.begin() + long(10 * l.BlockBytes()));
        CHECK(verifiedGeneration(file) == std::optional<uint64_t>(4));
        Stick s(file);
        CHECK(s.logical() == model);
        CHECK(viewOf(file) == model);
        // The held block kept its own extra bytes and took the data as it is.
        CHECK(s.media.Holder(4) == int32_t(l.Home(4)));
        const auto r = s.physical(l.Home(4), 3);
        CHECK(std::equal(held.begin(), held.begin() + 512, r.begin()));
        const auto heldExtra = newExtra(4);
        CHECK(std::equal(heldExtra.begin(), heldExtra.end(), r.begin() + 512));
        checkNewBlock(s, l.Home(5), 5);
        checkNewBlock(s, l.Home(600), 600);
        checkNewBlock(s, l.Home(11), 10);
        CHECK(s.media.Holder(11) == int32_t(l.Home(10)));
        // Unwritten pages of a new block are zeros, as an unclaimed L read.
        CHECK(s.physical(l.Home(5), 0)[0] == 0 && s.physical(l.Home(5), 31)[511] == 0);
    }
    {
        // A 4 MB stick with its spill area full of blocks that claim nothing: the 17th borrowed
        // logical slot 0, and has to move when L 0 gets a block.
        const auto& l = layout(4);
        auto physical = frondMs::create(geometry(4));
        for (uint32_t b = 100; b < 117; ++b)
            for (uint32_t p = 0; p < l.pagesPerBlock; ++p) {
                std::fill_n(page(physical, l, b, p), 512, uint8_t(b));
                page(physical, l, b, p)[512] = 0xf8;
            }
        const auto file = dir / "borrow.frondms";
        writeStick(file, physical, l);
        {
            Stick s(file);
            CHECK(s.media.MapEntry(116) == 0);
            // Slot 0 holds the borrower's bytes, but L 0 is nobody's and reads as zeros.
            CHECK(s.stored[0] == 116);
            const auto view = viewOf(file);
            CHECK(std::all_of(view.begin(), view.end(), [](uint8_t v) { return !v; }));
        }
        const auto data = randomBytes(512, 5);
        {
            CardFile c(file, true);
            CHECK(c.blankStick());
            c.write(512, reinterpret_cast<const char*>(data.data()), 512);
            c.commit({});
        }
        Stick s(file);
        checkNewBlock(s, l.Home(0), 0);
        CHECK(s.media.MapEntry(116) != 0);
        for (uint32_t b = 100; b < 117; ++b) CHECK(s.physical(b, 7)[100] == uint8_t(b));
        const auto view = viewOf(file);
        CHECK(std::equal(data.begin(), data.end(), view.begin() + 512));
        CHECK(std::all_of(view.begin(), view.begin() + 512, [](uint8_t v) { return !v; }));
    }
    std::puts("storage ms: writes into held and unclaimed blocks");
}

std::string refusal(const fs::path& file, bool writable) {
    try {
        CardFile c(file, writable);
    } catch (const Error& e) {
        return e.message;
    }
    return {};
}

// Two blocks claiming one L, and a lone claim sitting in the spill area.
void refusals(const fs::path& dir) {
    const auto& l = layout(8);
    std::vector<uint8_t> zeros(size_t(l.PayloadBytes()));
    auto physical = frondMs::fromLogical(zeros.data(), zeros.size(), geometry(8));
    const uint32_t spare = 500;
    for (uint32_t p = 0; p < l.pagesPerBlock; ++p) std::memcpy(page(physical, l, spare, p) + 512, newExtra(3).data(), 9);
    const auto twice = dir / "twice.frondms";
    writeStick(twice, physical, l);
    CHECK(refusal(twice, true) == unsettledStickMessage);
    CHECK(refusal(twice, false) == unsettledStickMessage);

    std::fill_n(page(physical, l, spare, 0), size_t(l.pagesPerBlock) * record, 0xff);
    const auto spilled = dir / "spilled.frondms";
    writeStick(spilled, physical, l);
    CHECK(refusal(spilled, false).empty());
    // Move L 3's holder into spill slot 0 by hand, as a cut-off guest commit leaves it.
    auto bytes = readFile(spilled);
    uint8_t* stored = bytes.data() + frondSdFile::headerBytes;
    const uint32_t block = l.Home(3);
    std::memcpy(stored + l.SlotAt(l.UserBlocks()), stored + l.SlotAt(3), size_t(l.BlockBytes()));
    frondSdFile::put32(stored + l.MapAt() + 4 * block, frondMsLayout::spillBit);
    frondSdFile::Info info;
    CHECK(frondSdFile::parseContainer(bytes.data(), info));
    frondSdFile::ChunkCrcs crcs;
    crcs.add(stored, size_t(info.stored()));
    frondSdFile::makeHeaderV2(bytes.data(), info.payloadBytes, info.generation, crcs.table(), info.sideBytes, true);
    std::vector<uint8_t> table;
    frondSdFile::tableBytes(crcs.table(), table);
    std::copy(table.begin(), table.end(), bytes.begin() + long(frondSdFile::headerBytes + info.stored()));
    writeFile(spilled, bytes);
    CHECK(verifiedGeneration(spilled).has_value());
    CHECK(refusal(spilled, true) == unsettledStickMessage);
    std::puts("storage ms: unsettled sticks refused");
}

uint32_t le16(const std::vector<uint8_t>& b, size_t at) { return b[at] | uint32_t(b[at + 1]) << 8; }
uint32_t le32(const std::vector<uint8_t>& b, size_t at) { return le16(b, at) | le16(b, at + 2) << 16; }

// The sectors the TG50's FORMAT writes for the stick's size (S0 table), and boot blocks untouched.
void checkFormat(const fs::path& file, unsigned mb, const std::vector<uint8_t>& bootBefore) {
    const auto* row = classicStick::formatRow(mb);
    CHECK(row);
    const auto view = viewOf(file);
    CHECK(std::equal(row->entry, row->entry + 8, view.begin() + 0x1be));
    CHECK(le32(view, 0x1c6) == row->hidden && le32(view, 0x1ca) == row->total);
    CHECK(std::all_of(view.begin() + 0x1ce, view.begin() + 0x1fe, [](uint8_t v) { return !v; }));
    CHECK(view[0x1fe] == 0x55 && view[0x1ff] == 0xaa);
    const size_t pbr = size_t(row->hidden) * 512;
    std::vector<uint8_t> boot(view.begin() + long(pbr), view.begin() + long(pbr + 512));
    CHECK(boot[0] == 0xe9 && !boot[1] && !boot[2] && std::string(boot.begin() + 3, boot.begin() + 11) == "        ");
    CHECK(le16(boot, 0x0b) == 512 && boot[0x0d] == row->cluster && le16(boot, 0x0e) == 1 && boot[0x10] == 2 &&
          le16(boot, 0x11) == 512 && boot[0x15] == 0xf8 && le16(boot, 0x16) == row->fatSectors && le16(boot, 0x18) == 16 &&
          le16(boot, 0x1a) == row->heads && le32(boot, 0x1c) == row->hidden && boot[0x26] == 0x29);
    CHECK((row->total < 0x10000 ? le16(boot, 0x13) : le32(boot, 0x20)) == row->total);
    CHECK(std::string(boot.begin() + 0x36, boot.begin() + 0x3e) == (mb == 128 ? "FAT16   " : "FAT12   "));
    const size_t rootAt = (row->hidden + 1 + 2 * row->fatSectors) * 512;
    CHECK(std::string(view.begin() + long(rootAt), view.begin() + long(rootAt) + 11) == "MEMSTICKIND");
    CHECK(view[rootAt + 11] == 0x03 && !le16(view, rootAt + 0x1a) && !le32(view, rootAt + 0x1c));
    // Frond's sticks are made 2026-11-01 00:00.
    CHECK(le16(view, rootAt + 0x18) == ((2026 - 1980) << 9 | 11 << 5 | 1) && !le16(view, rootAt + 0x16));
    // The data area starts on a block boundary.
    const size_t dataAt = rootAt + 32 * 512;
    CHECK(dataAt % layout(mb).BlockBytes() == 0);
    Stick s(file);
    std::vector<uint8_t> bootNow;
    for (uint32_t b = 0; b < 2; ++b)
        for (uint32_t p = 0; p < s.l.pagesPerBlock; ++p) {
            const auto r = s.physical(b, p);
            bootNow.insert(bootNow.end(), r.begin(), r.end());
        }
    CHECK(bootNow == bootBefore);
    CHECK(frondMs::validBootBlocks(bootNow.data(), geometry(mb)));
    for (uint32_t L = 0; L * s.l.BlockBytes() < dataAt; ++L) checkNewBlock(s, s.l.Home(L), L);
}

std::vector<uint8_t> bootOf(const std::vector<uint8_t>& physical, const Layout& l) {
    return {physical.begin(), physical.begin() + long(2 * l.pagesPerBlock * record)};
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

void checkSound(FatImage& fat) {
    const auto problems = fat.check();
    for (const auto& p : problems) std::fprintf(stderr, "problem: %s\n", p.c_str());
    CHECK(problems.empty());
}

fs::path blankStick(const fs::path& dir, const std::string& name, unsigned mb) {
    const auto file = dir / name;
    writeStick(file, frondMs::create(geometry(mb)), layout(mb));
    return file;
}

// A stick a TG50 mounted but never formatted: every L has a block of erased (0xff) data with the
// driver's new-block extra bytes. It counts as blank and formats; a formatted stick with a damaged
// MBR, or one block of anything else, doesn't.
void mountedBlank(const fs::path& dir) {
    const auto& l = layout(8);
    auto mounted = frondMs::create(geometry(8));
    for (uint32_t L = 0; L < l.UserBlocks(); ++L)
        for (uint32_t p = 0; p < l.pagesPerBlock; ++p) {
            std::fill_n(page(mounted, l, l.Home(L), p), 512, uint8_t(0xff));
            std::memcpy(page(mounted, l, l.Home(L), p) + 512, newExtra(L).data(), 9);
        }
    const auto file = dir / "mounted.frondms";
    writeStick(file, mounted, l);
    {
        Stick s(file);
        CHECK(s.media.Holder(100) == int32_t(l.Home(100)));
    }
    {
        CardFile c(file, true);
        CHECK(c.blankStick());
        FatImage fat(c);
        CHECK(fat.list("/").size() == 1 && fat.list("/")[0].name == "MEMSTICK.IND");
        checkSound(fat);
        put(fat, "/", "x.txt", {'x'});
    }
    // The same stick formatted, then its MBR sector erased: not blank, so not formatted over.
    std::vector<uint8_t> image;
    {
        Stick s(file);
        image = s.logical();
    }
    std::fill_n(image.begin(), 512, uint8_t(0xff));
    const auto damaged = dir / "damaged-mbr.frondms";
    writeStick(damaged, frondMs::fromLogical(image.data(), image.size(), geometry(8)), l);
    const auto before = readFile(damaged);
    {
        CardFile c(damaged, true);
        CHECK(!c.blankStick());
        bool refused = false;
        try {
            FatImage fat(c);
        } catch (const Error&) {
            refused = true;
        }
        CHECK(refused);
    }
    CHECK(readFile(damaged) == before);
    // An erased stick with one held block of other bytes isn't blank either.
    auto mixed = mounted;
    page(mixed, l, l.Home(40), 3)[17] = 0x00;
    const auto other = dir / "mixed.frondms";
    writeStick(other, mixed, l);
    {
        CardFile c(other, true);
        CHECK(!c.blankStick());
    }
    std::puts("storage ms: a mounted, unformatted stick is blank");
}

// Each size formats on its first open in Files, then takes files.
void formats(const fs::path& dir) {
    for (unsigned mb : {4u, 8u, 16u, 32u, 64u, 128u}) {
        const auto file = blankStick(dir, "format-" + std::to_string(mb) + ".frondms", mb);
        CHECK(refusal(file, false).empty());
        {
            CardFile c(file, false);
            bool refused = false;
            try {
                FatImage fat(c);
            } catch (const Error& e) {
                refused = e.message == blankStickMessage;
            }
            CHECK(refused);
        }
        {
            CardFile c(file, true);
            FatImage fat(c);
            CHECK(fat.fatBits() == (mb == 128 ? 16 : 12));
            CHECK(fat.list("/").size() == 1 && fat.list("/")[0].name == "MEMSTICK.IND");
            checkSound(fat);
        }
        CHECK(verifiedGeneration(file) == std::optional<uint64_t>(4));
        checkFormat(file, mb, bootOf(frondMs::create(geometry(mb)), layout(mb)));
        // Opening again doesn't format again.
        {
            CardFile c(file, true);
            FatImage fat(c);
            put(fat, "/", "x.txt", {'x'});
        }
        {
            CardFile c(file, true);
            FatImage fat(c);
            CHECK(contents(fat, "/x.txt") == std::vector<uint8_t>{'x'});
            // format() on a stick writes the TG50's layout too.
            FatImage::format(c);
        }
        checkFormat(file, mb, bootOf(frondMs::create(geometry(mb)), layout(mb)));
        std::printf("storage ms: %u MB formats as the TG50 does\n", mb);
    }
}

// Cut off at each step of the format, and of a large add into blocks nobody holds yet: settled,
// the stick loads, LogicalMsMedia finds it consistent, and the change is done or not.
void crashes(const fs::path& dir) {
    for (const char* step : {"claimed", "data", "journal", "partly", "applied"}) {
        const auto file = blankStick(dir, std::string("crash-format-") + step + ".frondms", 8);
        {
            CardFile c(file, true);
            c.crashHook = [&](const char* at) {
                if (!std::strcmp(at, step)) {
                    c.dead = true;
                    throw Crash();
                }
            };
            bool crashed = false;
            try {
                FatImage fat(c);
            } catch (const Crash&) {
                crashed = true;
            }
            CHECK(crashed);
        }
        CHECK(settleCard(file));
        Stick s(file);
        CardFile c(file, true);
        FatImage fat(c);
        checkSound(fat);
        CHECK(fat.find("/MEMSTICK.IND"));
    }
    const auto data = randomBytes(3 << 20, 9);
    for (const char* step : {"claimed", "writing", "data", "journal", "partly", "applied"}) {
        const auto file = blankStick(dir, std::string("crash-add-") + step + ".frondms", 8);
        {
            CardFile c(file, true);
            FatImage fat(c);
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
                put(fat, "/PALM", "Big file.bin", data);
            } catch (const Crash&) {
                crashed = true;
            }
            CHECK(crashed);
        }
        CHECK(unsettled(fs::path(file)));
        CHECK(settleCard(file));
        Stick s(file);
        CardFile c(file, false);
        FatImage fat(c);
        checkSound(fat);
        const bool done = std::strcmp(step, "claimed") && std::strcmp(step, "writing") && std::strcmp(step, "data");
        CHECK(bool(fat.find("/PALM/Big file.bin")) == done);
        if (done) CHECK(contents(fat, "/PALM/Big file.bin") == data);
    }
    std::puts("storage ms: cut-off formats and adds settle");
}

fs::path scratch() {
    const char* env = std::getenv("TMPDIR");
    auto dir = fs::path(env && *env ? env : "/tmp") / ("storage-ms-" + std::to_string(getpid()));
    fs::create_directories(dir);
    return dir;
}

// Each path with "dir" or its size and CRC-32, sorted by path, MEMSTICK.IND left out.
std::string listing(FatImage& fat) {
    std::map<std::string, std::string> out;
    std::function<void(const std::string&)> walk = [&](const std::string& path) {
        for (const auto& e : fat.list(path)) {
            const auto full = FatImage::joined(path, e.name);
            if (full == "/MEMSTICK.IND") continue;
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

void make(const fs::path& out, unsigned mb) {
    const auto file = fs::path(out.string() + ".frondms");
    fs::remove(file);
    writeStick(file, frondMs::create(geometry(mb)), layout(mb));
    {
        CardFile c(file, true);
        FatImage fat(c);
        fat.makeFolder("/", "PALM");
        fat.makeFolder("/PALM", "Launcher");
        put(fat, "/PALM/Launcher", "A rather long file name, with spaces.prc", randomBytes(70000, 1));
        put(fat, "/PALM/Launcher", "h\xc3\xa9llo.txt", {'c', 'a', 'f', 0xc3, 0xa9});
        put(fat, "/", "big.bin", randomBytes(size_t(fat.freeBytes() / 3), 2));
        put(fat, "/", "empty.txt", {});
        fat.makeFolder("/", "MANY");
        for (int i = 0; i < 200; ++i) {
            char name[16];
            std::snprintf(name, sizeof(name), "F%03d.TXT", i);
            put(fat, "/MANY", name, i % 10 ? std::vector<uint8_t>() : std::vector<uint8_t>{uint8_t(i)});
        }
        fat.move("/empty.txt", "/PALM");
        fat.remove("/MANY/F010.TXT");
        checkSound(fat);
        std::ofstream f(out.string() + ".expect", std::ios::trunc);
        f << "bits " << fat.fatBits() << "\n" << listing(fat);
        CHECK(f.good());
    }
    Stick s(file);
    writeFile(out, s.logical());
}

}  // namespace

int main(int argc, char** argv) {
    const std::string command = argc > 1 ? argv[1] : "selftest";
    try {
        if (command == "selftest") {
            const auto dir = scratch();
            writer(dir);
            refusals(dir);
            formats(dir);
            mountedBlank(dir);
            crashes(dir);
            fs::remove_all(dir);
        } else if (command == "make" && argc == 4) {
            make(argv[2], unsigned(std::stoul(argv[3])));
        } else if (command == "ls" && argc == 3) {
            CardFile c(argv[2], false);
            FatImage fat(c);
            for (const auto& e : fat.list("/"))
                std::printf("%s%s\n", e.name.c_str(), e.directory ? "/" : (" " + std::to_string(e.size)).c_str());
        } else if (command == "put" && argc == 5) {
            const std::string text = argv[4];
            {
                CardFile c(argv[2], true);
                FatImage fat(c);
                put(fat, "/", argv[3], std::vector<uint8_t>(text.begin(), text.end()));
            }
            CardFile c(argv[2], false);
            FatImage fat(c);
            CHECK(contents(fat, std::string("/") + argv[3]) == std::vector<uint8_t>(text.begin(), text.end()));
            checkSound(fat);
        } else {
            std::fprintf(stderr, "usage: %s selftest | make OUT MB | ls STICK | put STICK NAME TEXT\n", argv[0]);
            return 2;
        }
    } catch (const Error& e) {
        std::fprintf(stderr, "error: %s\n", e.message.c_str());
        return 1;
    }
    return 0;
}
