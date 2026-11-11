// The card tool (patch 0250): make, import and export card files without a machine. The made
// files are loaded as a card put into an empty machine (the same checks a swap load makes), and
// SD and Pro files are opened in the card store the way put-in does.
#include "CardSwap.h"
#include "CardTool.h"
#include "MemoryStickImage.h"
#include "NandFile.h"
#include "SessionIo.h"
#include "pxa_DMA.h"
#include "sdcard.h"

#include <dlfcn.h>
#include <sys/statvfs.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// The card store and the stick controller link against these; no SoC here.
bool memRegionAdd(ArmMem*, uint32_t, uint32_t, ArmMemAccessF, void*) { return true; }
void pxaGpioSetState(PxaGpio*, uint_fast8_t, bool) {}
void pxaDmaExternalReq(PxaDma*, uint_fast8_t, bool) {}
void pxaDmaSetPeripheral(PxaDma*, uint_fast8_t, const PxaDmaPeripheral*) {}
extern "C" void uarmAbort() { std::abort(); }

namespace fs = std::filesystem;
using frondCardTool::Kind;

// The free space the card tool's check sees, when set: this program replaces statvfs, so a full
// file system needs no real one.
static bool fakeSpace = false;
static uint64_t fakeFree = 0;
extern "C" int statvfs(const char* path, struct statvfs* out) noexcept {
    using Real = int (*)(const char*, struct statvfs*);
    static const auto real = reinterpret_cast<Real>(dlsym(RTLD_NEXT, "statvfs"));
    const int result = real(path, out);
    if (result == 0 && fakeSpace) out->f_frsize = 4096, out->f_bavail = fakeFree / 4096;
    return result;
}

static const fs::path& Root() {
    static const fs::path root = [] {
        const auto dir = fs::path("build") / "card-tool-fixtures";
        fs::remove_all(dir);
        fs::create_directories(dir);
        return dir;
    }();
    return root;
}

static fs::path Dir(const std::string& name) {
    const auto dir = Root() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

static std::vector<uint8_t> ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void WriteAll(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    assert(out);
}

static void SetSize(const fs::path& path, uint64_t bytes) {
    std::ofstream(path, std::ios::binary).close();
    fs::resize_file(path, bytes);
}

// Nothing but `names` is left in the folder (no temp files from a failed run).
static void OnlyHas(const fs::path& dir, std::initializer_list<const char*> names) {
    size_t count = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        bool known = false;
        for (const char* name : names) known |= entry.path().filename() == name;
        if (!known) std::printf("unexpected %s\n", entry.path().c_str());
        assert(known);
        ++count;
    }
    assert(count == names.size());
}

static bool Ok(const frondCardTool::Result& r) {
    if (!r.ok) std::printf("failed: %s %s\n", r.code.c_str(), r.message.c_str());
    return r.ok;
}

static bool Refused(const frondCardTool::Result& r, const char* code) {
    if (r.ok || r.code != code) std::printf("expected %s, got %s %s\n", code, r.ok ? "ok" : r.code.c_str(), r.message.c_str());
    return !r.ok && r.code == code;
}

// The payload of a settled .frondsd v2 (generation 1), with its table checked.
static std::vector<uint8_t> Payload(const fs::path& path, uint64_t* generation = nullptr) {
    const auto file = ReadAll(path);
    frondSdFile::Info info;
    assert(file.size() >= frondSdFile::headerBytes && frondSdFile::parseHeader(file.data(), info));
    assert(info.version == frondSdFile::versionV2 && file.size() == info.fileBytes());
    std::vector<uint8_t> payload(file.begin() + frondSdFile::headerBytes, file.begin() + frondSdFile::headerBytes + info.payloadBytes);
    frondSdFile::ChunkCrcs crcs;
    crcs.add(payload.data(), payload.size());
    std::vector<uint8_t> table;
    frondSdFile::tableBytes(crcs.table(), table);
    assert(std::equal(table.begin(), table.end(), file.begin() + frondSdFile::headerBytes + payload.size()));
    assert(frondSdFile::headerCrc(file.data(), crcs.table()) == info.crc);
    if (generation) *generation = info.generation;
    return payload;
}

// The card file loaded as a card put into an empty machine.
static void LoadsAsPutIn(const fs::path& card, const char* siblingName, const char* device) {
    const auto machine = Dir(std::string("machine-") + siblingName);
    fs::copy_file(card, machine / siblingName);
    frondSession::Contents contents;
    contents.deviceName = device;
    contents.displayMode = 1;
    contents.ramSize = 32u << 20;
    contents.nor = {1, 2, 3, 4};
    contents.memory = {7, 8, 9};
    contents.savestate = {10, 11, 12, 13};
    assert(frondSession::saveV7(machine / (machine.filename().string() + ".frond"), contents));
    frondSession::CardSwap swap;
    swap.identityGiven = true;
    for (auto& b : swap.identity) b = 'a';
    frondSession::Contents got;
    fs::path loaded;
    assert(frondSession::loadPath(machine, got, loaded, frondSession::msPhysicalBytes, {}, &swap));
    assert(got.media.size() == 1 && got.media[0].name == siblingName);
}

static void OpensInStore(const fs::path& card, uint32_t kind, int32_t device, uint64_t expectBytes, bool expectOk) {
    const auto machine = Dir("store");
    fs::copy_file(card, machine / frondMachine::fileCardSibling(kind));
    frondNative::CardIdentity id{};
    for (auto& b : id) b = 'b';
    const auto result = frondNative::openFileCard(machine, kind, device, id);
    assert(result.ok == expectOk);
    if (expectOk) {
        assert(uint64_t(sdCardSectorCount()) * SD_SECTOR_SIZE == expectBytes);
        uint8_t sector[SD_SECTOR_SIZE];
        assert(sdCardRead(0, sector));
        sdCardReset();
        frondSdCandidate::releaseUnused();
    }
}

static void MakeFiles() {
    const auto dir = Dir("make");
    // SD: a small and a large size; the payload is the layout and zeros.
    for (const unsigned mib : {32u, 1886u}) {
        const auto path = dir / ("sd" + std::to_string(mib));
        std::vector<uint64_t> seen;
        uint64_t last = 0;
        assert(Ok(frondCardTool::make(Kind::sd, mib, path, [&](uint64_t done, uint64_t total) {
            assert(done > last || done == total);
            assert(total == uint64_t(mib) << 20);
            last = done;
            return true;
        })));
        assert(last == uint64_t(mib) << 20);
        uint64_t generation = 0;
        const auto payload = Payload(path, &generation);
        assert(generation == 1 && payload.size() == uint64_t(mib) << 20);
        std::vector<uint8_t> expect(payload.size(), 0);
        for (const auto& sector : frondSd::layout(mib))
            std::memcpy(expect.data() + size_t(sector.index) * 512, sector.bytes.data(), 512);
        assert(payload == expect);
        if (mib == 32) assert(payload == frondSd::create(32));
        LoadsAsPutIn(path, "sd.frondsd", "palm-tx");
        OpensInStore(path, frondMachine::kindSd, deviceTypeTX, uint64_t(mib) << 20, true);
        OpensInStore(path, frondMachine::kindSd, deviceTypeE2, uint64_t(mib) << 20, mib <= 1024);
    }
    // Memory Stick Pro, also a .frondsd v2.
    for (const unsigned mib : {256u, 1024u, 1886u}) {
        const auto path = dir / ("pro" + std::to_string(mib));
        assert(Ok(frondCardTool::make(Kind::msPro, mib, path)));
        const auto payload = Payload(path);
        assert(payload.size() == uint64_t(mib) << 20);
        LoadsAsPutIn(path, "ms.frondmsp", "sony-clie-tg50");
        OpensInStore(path, frondMachine::kindMsPro, deviceTypeTG50, uint64_t(mib) << 20, true);
        // A Pro stick fits no SD-only machine.
        OpensInStore(path, frondMachine::kindMsPro, deviceTypeTX, 0, false);
    }
    // Classic sticks: a v2 .frondms of a new stick, every L held by its home block with zero data, so
    // the TG50 driver's first mount doesn't check every free block page by page (50 s at 128 MB).
    for (const unsigned mb : {4u, 128u}) {
        const auto path = dir / ("ms" + std::to_string(mb));
        assert(Ok(frondCardTool::make(Kind::ms, mb, path)));
        const auto file = ReadAll(path);
        std::vector<uint8_t> physical;
        const auto& g = *frondMsGeometry::byMegabytes(mb);
        assert(frondMs::get32(file.data() + 8) == 2 && frondMs::readPhysical(path, physical) &&
               physical == frondMs::blank(g) && frondMs::validBoot(physical));
        const auto& l = *frondMs::layoutOf(g);
        for (uint32_t L = 0; L < l.UserBlocks(); ++L)
            for (uint32_t p = 0; p < g.pagesPerBlock; ++p) {
                const auto* r = physical.data() + (size_t(l.Home(L)) * g.pagesPerBlock + p) * MemoryStickMedia::recordBytes;
                assert(r[512] == 0xf8 && r[513] == 0xff && frondMs::getBe16(r + 514) == L);
                assert(std::all_of(r, r + 512, [](uint8_t b) { return b == 0; }));
            }
        LoadsAsPutIn(path, "ms.frondms", "sony-clie-tg50");
    }
    // The tool replaces an existing output and leaves only the card.
    assert(Ok(frondCardTool::make(Kind::ms, 4, dir / "ms4")));
    std::printf("card tool make ok\n");
}

static void ImportExport() {
    const auto dir = Dir("roundtrip");
    // Raw SD: some sectors marked.
    auto raw = frondSd::create(64);
    for (size_t at : {size_t(0), size_t(512) * 7, raw.size() - 512}) std::memset(raw.data() + at, 0x5a ^ (at >> 9), 512);
    WriteAll(dir / "in.img", raw);
    assert(Ok(frondCardTool::importImage(dir / "in.img", Kind::sd, dir / "sd.frondsd")));
    assert(Payload(dir / "sd.frondsd") == raw);
    LoadsAsPutIn(dir / "sd.frondsd", "sd.frondsd", "palm-tx");
    assert(Ok(frondCardTool::exportCard(dir / "sd.frondsd", dir / "out.img")));
    assert(ReadAll(dir / "out.img") == raw);
    // Re-import of the export gives the same card file.
    assert(Ok(frondCardTool::importImage(dir / "out.img", Kind::sd, dir / "sd2.frondsd")));
    assert(ReadAll(dir / "sd2.frondsd") == ReadAll(dir / "sd.frondsd"));

    // Raw Pro, 256 MiB, a size that is also a valid SD size.
    std::vector<uint8_t> pro(size_t(256) << 20, 0);
    for (size_t at = 0; at < pro.size(); at += (size_t(37) << 20) + 512) std::memset(pro.data() + at, 0xc3, 512);
    WriteAll(dir / "pro.img", pro);
    assert(Ok(frondCardTool::importImage(dir / "pro.img", Kind::msPro, dir / "ms.frondmsp")));
    assert(Payload(dir / "ms.frondmsp") == pro);
    assert(Ok(frondCardTool::exportCard(dir / "ms.frondmsp", dir / "pro-out.img")));
    assert(ReadAll(dir / "pro-out.img") == pro);
    pro.clear();
    fs::remove(dir / "pro.img");
    fs::remove(dir / "pro-out.img");

    // Classic stick: a v1 stick with a change counter imports as a v2 one.
    auto image = frondMs::create(*frondMsGeometry::byMegabytes(16));
    auto file = frondMs::encode(image);
    frondMs::putChanges(file.data(), 9);
    WriteAll(dir / "in.frondms", file);
    assert(Ok(frondCardTool::importImage(dir / "in.frondms", Kind::ms, dir / "ms.frondms")));
    const auto imported = ReadAll(dir / "ms.frondms");
    {
        std::vector<uint8_t> back;
        assert(frondMs::readPhysical(dir / "ms.frondms", back) && back == image);
    }
    LoadsAsPutIn(dir / "ms.frondms", "ms.frondms", "sony-clie-tg50");
    assert(Ok(frondCardTool::exportCard(dir / "ms.frondms", dir / "out.frondms")));
    assert(ReadAll(dir / "out.frondms") == imported && frondMs::get32(imported.data() + 8) == 2);
    // A made card exports and re-imports to the same bytes.
    assert(Ok(frondCardTool::make(Kind::sd, 32, dir / "made.frondsd")));
    assert(Ok(frondCardTool::exportCard(dir / "made.frondsd", dir / "made.img")));
    assert(Ok(frondCardTool::importImage(dir / "made.img", Kind::sd, dir / "made2.frondsd")));
    assert(ReadAll(dir / "made2.frondsd") == ReadAll(dir / "made.frondsd"));
    assert(Ok(frondCardTool::run({"export", (dir / "made.frondsd").string(), (dir / "made-run.img").string()})));
    assert(ReadAll(dir / "made-run.img") == ReadAll(dir / "made.img"));
    // A v1 card file (older Frond, or a candidate) exports too.
    {
        std::vector<uint8_t> v1;
        assert(frondSdFile::encode(raw, v1));
        WriteAll(dir / "old.frondsd", v1);
        assert(Ok(frondCardTool::exportCard(dir / "old.frondsd", dir / "old.img")));
        assert(ReadAll(dir / "old.img") == raw);
    }
    std::printf("card tool import and export ok\n");
}

static void Refusals() {
    const auto dir = Dir("refused");
    using frondCardTool::make;
    using frondCardTool::importImage;
    // Sizes no device takes.
    assert(Refused(make(Kind::sd, 100, dir / "a"), "size"));
    assert(Refused(make(Kind::sd, 2048, dir / "a"), "size"));
    assert(Refused(make(Kind::sd, 0, dir / "a"), "size"));
    assert(Refused(make(Kind::msPro, 128, dir / "a"), "size"));
    assert(Refused(make(Kind::msPro, 2048, dir / "a"), "size"));
    assert(Refused(make(Kind::ms, 3, dir / "a"), "size"));
    assert(Refused(make(Kind::ms, 256, dir / "a"), "size"));
    assert(Refused(frondCardTool::run({"make", "sd", "abc", (dir / "a").string()}), "size"));
    assert(Refused(frondCardTool::run({"make", "tape", "32", (dir / "a").string()}), "usage"));
    assert(Refused(frondCardTool::run({"make", "sd"}), "usage"));
    assert(Refused(frondCardTool::run({"fold", "sd", "1", "2"}), "usage"));
    // Output folder missing.
    assert(Refused(make(Kind::sd, 32, dir / "none" / "a"), "write"));
    OnlyHas(dir, {});

    // Raw images.
    SetSize(dir / "odd.img", 32u << 20 | 1);
    assert(Refused(importImage(dir / "odd.img", Kind::sd, dir / "a"), "size"));
    SetSize(dir / "zero.img", 0);
    assert(Refused(importImage(dir / "zero.img", Kind::sd, dir / "a"), "size"));
    SetSize(dir / "big.img", (uint64_t(2048) << 20) + 512);
    assert(Refused(importImage(dir / "big.img", Kind::sd, dir / "a"), "size"));
    fs::remove(dir / "big.img");
    SetSize(dir / "p300.img", uint64_t(300) << 20);
    assert(Refused(importImage(dir / "p300.img", Kind::msPro, dir / "a"), "size"));
    SetSize(dir / "p128.img", uint64_t(128) << 20);
    assert(Refused(importImage(dir / "p128.img", Kind::msPro, dir / "a"), "size"));
    assert(Ok(importImage(dir / "p128.img", Kind::sd, dir / "p128.frondsd")));
    fs::remove(dir / "p128.frondsd");
    assert(Refused(importImage(dir / "none.img", Kind::sd, dir / "a"), "read"));
    assert(Refused(importImage(dir / "p128.img", Kind::sd, dir / "p128.img"), "same"));
    // A Frond card file is not a raw image.
    assert(Ok(make(Kind::sd, 32, dir / "card.frondsd")));
    assert(Refused(importImage(dir / "card.frondsd", Kind::sd, dir / "a"), "size"));
    auto card = ReadAll(dir / "card.frondsd");
    card.resize(size_t(32) << 20);  // right size for a raw SD image, Frond header
    WriteAll(dir / "framed.img", card);
    assert(Refused(importImage(dir / "framed.img", Kind::sd, dir / "a"), "image"));
    fs::remove(dir / "framed.img");

    // Classic stick images.
    auto file = frondMs::encode(frondMs::create(*frondMsGeometry::byMegabytes(8)));
    WriteAll(dir / "ok.frondms", file);
    assert(Ok(importImage(dir / "ok.frondms", Kind::ms, dir / "ok-out.frondms")));
    fs::remove(dir / "ok-out.frondms");
    auto bad = file;
    bad[0] = 'X';
    WriteAll(dir / "magic.frondms", bad);
    assert(Refused(importImage(dir / "magic.frondms", Kind::ms, dir / "a"), "size"));
    bad = file;
    bad.pop_back();
    WriteAll(dir / "short.frondms", bad);
    assert(Refused(importImage(dir / "short.frondms", Kind::ms, dir / "a"), "size"));
    bad = file;
    bad[frondMs::headerBytes + 100] ^= 1;
    WriteAll(dir / "crc.frondms", bad);
    assert(Refused(importImage(dir / "crc.frondms", Kind::ms, dir / "a"), "image"));
    assert(Refused(importImage(dir / "ok.frondms", Kind::sd, dir / "a"), "size"));
    assert(Refused(importImage(dir / "card.frondsd", Kind::ms, dir / "a"), "image"));

    // Export of damaged, unsettled or foreign files.
    assert(Ok(frondCardTool::exportCard(dir / "card.frondsd", dir / "x.img")));
    fs::remove(dir / "x.img");
    bad = ReadAll(dir / "card.frondsd");
    bad[frondSdFile::headerBytes + 5000] ^= 1;
    WriteAll(dir / "damaged.frondsd", bad);
    assert(Refused(frondCardTool::exportCard(dir / "damaged.frondsd", dir / "x.img"), "image"));
    bad = ReadAll(dir / "card.frondsd");
    bad.pop_back();
    WriteAll(dir / "short.frondsd", bad);
    assert(Refused(frondCardTool::exportCard(dir / "short.frondsd", dir / "x.img"), "image"));
    bad = ReadAll(dir / "card.frondsd");
    bad[8] = 9;
    WriteAll(dir / "version.frondsd", bad);
    assert(Refused(frondCardTool::exportCard(dir / "version.frondsd", dir / "x.img"), "image"));
    WriteAll(dir / "text.frondsd", std::vector<uint8_t>(100, 'x'));
    assert(Refused(frondCardTool::exportCard(dir / "text.frondsd", dir / "x.img"), "image"));
    WriteAll(dir / "tiny.frondsd", std::vector<uint8_t>(10, 'x'));
    assert(Refused(frondCardTool::exportCard(dir / "tiny.frondsd", dir / "x.img"), "image"));
    assert(Refused(frondCardTool::exportCard(dir / "gone.frondsd", dir / "x.img"), "read"));
    assert(Refused(frondCardTool::exportCard(dir / "card.frondsd", dir / "card.frondsd"), "same"));
    bad = ReadAll(dir / "ok.frondms");
    bad[frondMs::headerBytes + 50] ^= 1;
    WriteAll(dir / "damaged.frondms", bad);
    assert(Refused(frondCardTool::exportCard(dir / "damaged.frondms", dir / "x.frondms"), "image"));
    WriteAll(dir / "card.frondsd.journal-2", {1, 2, 3});
    assert(Refused(frondCardTool::exportCard(dir / "card.frondsd", dir / "x.img"), "unsettled"));
    fs::remove(dir / "card.frondsd.journal-2");
    for (const char* name : {"odd.img", "zero.img", "p300.img", "p128.img", "card.frondsd", "ok.frondms", "magic.frondms",
                             "short.frondms", "crc.frondms", "damaged.frondsd", "short.frondsd", "version.frondsd",
                             "text.frondsd", "tiny.frondsd", "damaged.frondms"})
        fs::remove(dir / name);
    OnlyHas(dir, {});
    std::printf("card tool refusals ok\n");
}

static void Cancels() {
    const auto dir = Dir("cancel");
    int calls = 0;
    auto stopAtThird = [&](uint64_t, uint64_t) { return ++calls < 3; };
    // Big enough to need several chunks.
    calls = 0;
    assert(Refused(frondCardTool::make(Kind::sd, 64, dir / "new.frondsd", stopAtThird), "cancelled"));
    assert(calls == 3);
    calls = 0;
    assert(Refused(frondCardTool::make(Kind::msPro, 256, dir / "new.frondmsp", stopAtThird), "cancelled"));
    calls = 0;
    assert(Refused(frondCardTool::make(Kind::ms, 128, dir / "new.frondms", [&](uint64_t, uint64_t) { return ++calls < 2; }), "cancelled"));
    OnlyHas(dir, {});

    assert(Ok(frondCardTool::make(Kind::sd, 64, dir / "card.frondsd")));
    assert(Ok(frondCardTool::make(Kind::ms, 128, dir / "stick.frondms")));
    fs::copy_file(dir / "card.frondsd", dir / "keep.frondsd");
    WriteAll(dir / "raw.img", Payload(dir / "card.frondsd"));
    // Cancel while writing a new import, an export and a classic export; an old output stays.
    calls = 0;
    assert(Refused(frondCardTool::importImage(dir / "raw.img", Kind::sd, dir / "imp.frondsd", stopAtThird), "cancelled"));
    calls = 0;
    assert(Refused(frondCardTool::exportCard(dir / "card.frondsd", dir / "out.img", stopAtThird), "cancelled"));
    calls = 0;
    assert(Refused(frondCardTool::exportCard(dir / "stick.frondms", dir / "out.frondms", [&](uint64_t, uint64_t) { return ++calls < 2; }), "cancelled"));
    // Cancel in the second half of an export (the copy after the check).
    calls = 0;
    const uint64_t total = 64u << 20;
    assert(Refused(frondCardTool::exportCard(dir / "card.frondsd", dir / "out.img",
                                             [&](uint64_t done, uint64_t t) { assert(t == 2 * total); return done <= total + (4u << 20); }),
                   "cancelled"));
    // An existing output survives a cancelled replacement.
    calls = 0;
    assert(Refused(frondCardTool::importImage(dir / "raw.img", Kind::sd, dir / "keep.frondsd", stopAtThird), "cancelled"));
    assert(ReadAll(dir / "keep.frondsd") == ReadAll(dir / "card.frondsd"));
    OnlyHas(dir, {"card.frondsd", "stick.frondms", "keep.frondsd", "raw.img"});
    std::printf("card tool cancel ok\n");
}

// Each operation refuses with `space` when the folder can't take the output plus the 16 MiB spare,
// before writing anything, and goes ahead with just enough.
static void Space() {
    const auto dir = Dir("space");
    const uint64_t spare = uint64_t(16) << 20;
    auto raw = frondSd::create(32);
    WriteAll(dir / "in.img", raw);
    assert(Ok(frondCardTool::make(Kind::sd, 32, dir / "card.frondsd")));
    assert(Ok(frondCardTool::make(Kind::ms, 4, dir / "stick.frondms")));
    const auto sdFile = fs::file_size(dir / "card.frondsd");
    fakeSpace = true;
    fakeFree = spare + raw.size() - 4096;
    assert(Refused(frondCardTool::make(Kind::sd, 32, dir / "new.frondsd"), "space"));
    assert(Refused(frondCardTool::make(Kind::msPro, 256, dir / "new.frondmsp"), "space"));
    assert(Refused(frondCardTool::importImage(dir / "in.img", Kind::sd, dir / "imp.frondsd"), "space"));
    assert(Refused(frondCardTool::exportCard(dir / "card.frondsd", dir / "out.img"), "space"));
    fakeFree = spare + (uint64_t(4) << 20) - 4096;
    assert(Refused(frondCardTool::make(Kind::ms, 4, dir / "new.frondms"), "space"));
    assert(Refused(frondCardTool::exportCard(dir / "stick.frondms", dir / "out.frondms"), "space"));
    OnlyHas(dir, {"in.img", "card.frondsd", "stick.frondms"});
    // Enough for the largest of these outputs.
    fakeFree = spare + sdFile + (uint64_t(1) << 20);
    assert(Ok(frondCardTool::make(Kind::sd, 32, dir / "new.frondsd")));
    assert(Ok(frondCardTool::importImage(dir / "in.img", Kind::sd, dir / "imp.frondsd")));
    assert(Ok(frondCardTool::exportCard(dir / "card.frondsd", dir / "out.img")));
    assert(Ok(frondCardTool::make(Kind::ms, 4, dir / "new.frondms")));
    fakeSpace = false;
    std::printf("card tool space ok\n");
}

// A machine's nand.frondnand settles and verifies like a card file.
static void Nand() {
    const auto dir = Dir("nand");
    constexpr uint64_t raw = 528 * 4096;
    const auto file = dir / "nand.frondnand";
    std::array<uint8_t, frondMachine::identityBytes> identity{};
    assert(frondSession::writeSdFromReader(
        file, frondNand::storedBytes(raw),
        [&](uint64_t off, uint64_t n, void* out) { return std::memset(out, int(off >> 12), size_t(n)), true; }, identity, 1,
        frondSession::nandShape(raw)));
    auto r = frondCardTool::run({"settle", file.string()});
    assert(r.ok && r.message == "settled 1");
    r = frondCardTool::run({"verify", file.string()});
    assert(r.ok && r.message == "verified 1");
    // A save's journal beside it waits for its machine.
    WriteAll(dir / "nand.frondnand.journal-2", {1});
    assert(Refused(frondCardTool::run({"settle", file.string()}), "unsettled"));
    assert(Refused(frondCardTool::run({"verify", file.string()}), "unsettled"));
    fs::remove(dir / "nand.frondnand.journal-2");
    auto bytes = ReadAll(file);
    bytes[64 + 5000] ^= 1;
    WriteAll(file, bytes);
    assert(Refused(frondCardTool::run({"verify", file.string()}), "image"));
    fs::remove_all(dir);
    std::printf("card tool nand ok\n");
}

int main() {
    Space();
    Nand();
    MakeFiles();
    ImportExport();
    Refusals();
    Cancels();
    fs::remove_all(Root());
    std::printf("card tool ok\n");
}
