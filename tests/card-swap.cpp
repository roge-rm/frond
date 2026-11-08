// Card swap in the core (patches 0248 and 0249): loads with --card-swap, take-out and put-in, and
// the change counters, without a SoC.
#include "CardSwap.h"
#include "MemoryStickImage.h"
#include "SessionIo.h"
#include "pxa_DMA.h"
#include "sdcard.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// The controller's SoC links: no GPIO or DMA; the last controller's MMIO, for writes to a stick.
static ArmMemAccessF mmio;
static void* mmioContext;
bool memRegionAdd(ArmMem*, uint32_t, uint32_t, ArmMemAccessF fn, void* context) {
    mmio = fn;
    mmioContext = context;
    return true;
}
void pxaGpioSetState(PxaGpio*, uint_fast8_t, bool) {}
void pxaDmaExternalReq(PxaDma*, uint_fast8_t, bool) {}
void pxaDmaSetPeripheral(PxaDma*, uint_fast8_t, const PxaDmaPeripheral*) {}
extern "C" void uarmAbort() { std::abort(); }

namespace fs = std::filesystem;
using frondMachine::Media;
using frondNative::CardIdentity;

static fs::path Root() {
    static const fs::path root = [] {
        const auto dir = fs::path("build") / "card-swap-fixtures";
        fs::remove_all(dir);
        fs::create_directories(dir);
        return dir;
    }();
    return root;
}

static fs::path Machine(const std::string& name) {
    const auto dir = Root() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

static CardIdentity Id(uint8_t fill) {
    CardIdentity id{};
    // Identities are hex text, as cards have.
    for (auto& b : id) b = uint8_t("0123456789abcdef"[fill & 15]);
    return id;
}

static frondSession::Contents Contents(const char* device) {
    frondSession::Contents c;
    c.deviceName = device;
    c.displayMode = 1;
    c.ramSize = 32u << 20;
    c.nor = {1, 2, 3, 4};
    c.memory = {7, 8, 9};
    c.savestate = {10, 11, 12, 13};
    return c;
}

static fs::path FrondOf(const fs::path& folder) { return folder / (folder.filename().string() + ".frond"); }

static void SaveFrond(const fs::path& folder, const frondSession::Contents& contents) {
    assert(frondSession::saveV7(FrondOf(folder), contents));
}

// A v2 card file of `bytes`, each sector filled with `fill`, at `generation`.
static void WriteSd(const fs::path& path, uint64_t bytes, uint8_t fill, uint64_t generation, CardIdentity id) {
    // A non-empty identity, so the writer doesn't fill in a digest.
    assert(frondSession::writeSdFromReader(
        path, bytes,
        [&](uint64_t, uint64_t n, void* dest) {
            std::memset(dest, fill, size_t(n));
            return true;
        },
        id, generation));
}

static Media FileMedia(uint32_t kind, uint64_t bytes, uint64_t generation, CardIdentity id) {
    Media m;
    m.kind = kind;
    m.name = frondMachine::fileCardSibling(kind);
    m.inserted = true;
    m.identity = id;
    m.logicalSize = m.physicalSize = bytes;
    m.contentGeneration = generation;
    assert(frondMachine::validMedia(m));
    return m;
}

static bool Load(const fs::path& folder, frondSession::Contents& out, const frondSession::CardSwap* swap) {
    fs::path loaded;
    out = {};
    return frondSession::loadPath(folder, out, loaded, frondSession::msPhysicalBytes, {}, swap);
}

static frondSession::CardSwap Swap(bool withIdentity, CardIdentity id = {}, bool protect = false) {
    frondSession::CardSwap swap;
    swap.identityGiven = withIdentity;
    swap.identity = id;
    swap.writeProtected = protect;
    return swap;
}

static bool Swapped(const frondSession::Contents& c, uint32_t kind) {
    return kind == frondMachine::kindSd ? c.sdSwapped : c.msSwapped;
}

// Each load case for a card kept as a .frondsd: SD and Memory Stick Pro.
static void FileCardLoads(uint32_t kind, uint64_t bytes) {
    const char* device = kind == frondMachine::kindSd ? "palm-tx" : "sony-clie-tg50";
    const char* name = frondMachine::fileCardSibling(kind);
    const auto a = Machine(std::string("load-") + name);
    const auto drawer = Machine(std::string("drawer-") + name);
    auto contents = Contents(device);
    WriteSd(a / name, bytes, 0x11, 1, Id(1));
    contents.media.push_back(FileMedia(kind, bytes, 1, Id(1)));
    SaveFrond(a, contents);
    frondSession::Contents got;
    assert(Load(a, got, nullptr) && got.media.size() == 1);

    // Unchanged: no swap, as saved.
    for (const bool withId : {false, true}) {
        const auto swap = Swap(withId, Id(1));
        assert(Load(a, got, &swap) && got.media.size() == 1 && !Swapped(got, kind));
        assert(got.media[0].identity == Id(1) && got.media[0].contentGeneration == 1);
    }

    // Taken out while off: refused as saved, empty under --card-swap.
    fs::rename(a / name, drawer / name);
    assert(!Load(a, got, nullptr));
    auto swap = Swap(false);
    assert(Load(a, got, &swap) && got.media.empty() && Swapped(got, kind));

    // Another card put in: refused as saved and without an identity, a new card with one.
    WriteSd(a / name, bytes, 0x22, 5, Id(2));
    assert(!Load(a, got, nullptr));
    assert(!Load(a, got, &swap));
    swap = Swap(true, Id(2), true);
    assert(Load(a, got, &swap) && got.media.size() == 1 && Swapped(got, kind));
    assert(got.media[0].identity == Id(2) && got.media[0].contentGeneration == 5 && got.media[0].kind == kind);
    assert(got.media[0].inserted && got.media[0].writeProtected);

    // The same card back, changed elsewhere: a new card with the same identity.
    fs::remove(a / name);
    WriteSd(a / name, bytes, 0x33, 2, Id(1));
    swap = Swap(true, Id(1));
    assert(Load(a, got, &swap) && got.media.size() == 1 && Swapped(got, kind) && got.media[0].contentGeneration == 2);
    // Damage is never taken for a swap: without the flag a changed card still refuses.
    assert(!Load(a, got, nullptr));

    // A new card with save files of another beside it is refused.
    const auto journal = frondSession::sdJournalPath(a, 9, kind);
    { std::FILE* f = std::fopen(journal.c_str(), "wb"); assert(f); std::fclose(f); }
    assert(!Load(a, got, &swap));
    fs::remove(journal);

    // A damaged card file is refused, not taken for a new card.
    {
        std::FILE* f = std::fopen((a / name).c_str(), "r+b");
        assert(f && std::fseek(f, 70, SEEK_SET) == 0 && std::fputc(0x99, f) != EOF);
        std::fclose(f);
        auto bad = Swap(true, Id(3));
        // v2 payloads are checked chunk by chunk as they are read; the header and table still load.
        assert(Load(a, got, &bad));
        std::FILE* g = std::fopen((a / name).c_str(), "r+b");
        assert(g && std::fseek(g, 30, SEEK_SET) == 0 && std::fputc(0x99, g) != EOF);
        std::fclose(g);
        assert(!Load(a, got, &bad));
    }

    // A machine saved with no card takes one in.
    const auto empty = Machine(std::string("empty-") + name);
    SaveFrond(empty, Contents(device));
    fs::rename(drawer / name, empty / name);
    assert(!Load(empty, got, nullptr));
    swap = Swap(true, Id(1));
    assert(Load(empty, got, &swap) && got.media.size() == 1 && Swapped(got, kind));
    std::printf("card swap loads for %s ok\n", name);
}

static void ClassicLoads() {
    const auto* g = frondMsGeometry::byMegabytes(16);
    assert(g);
    const auto image = frondMs::create(*g);
    const auto a = Machine("load-classic");
    auto contents = Contents("sony-clie-tg50");
    assert(frondSession::writeMsFile(a / frondMachine::msSibling, image.data(), image.size()));
    Media m;
    m.kind = frondMachine::kindMs;
    m.name = frondMachine::msSibling;
    m.inserted = true;
    m.identity = Id(4);
    m.physicalSize = g->PhysicalBytes();
    m.logicalSize = g->LogicalBytes();
    contents.media.push_back(m);
    SaveFrond(a, contents);
    frondSession::Contents got;
    assert(Load(a, got, nullptr) && got.media.size() == 1);
    auto swap = Swap(true, Id(4));
    assert(Load(a, got, &swap) && !got.msSwapped && got.media[0].identity == Id(4));

    // Another stick: a new card; a Pro beside a classic stick is refused.
    swap = Swap(true, Id(5));
    assert(Load(a, got, &swap) && got.msSwapped && got.media[0].identity == Id(5) && got.media[0].kind == frondMachine::kindMs);
    WriteSd(a / frondMachine::msProSibling, 256u << 20, 0, 1, Id(6));
    assert(!Load(a, got, &swap));

    // Classic out, Pro in.
    fs::remove(a / frondMachine::msSibling);
    swap = Swap(true, Id(6));
    assert(!Load(a, got, nullptr));
    assert(Load(a, got, &swap) && got.msSwapped && got.media.size() == 1 && got.media[0].kind == frondMachine::kindMsPro);

    // Stick removed.
    fs::remove(a / frondMachine::msProSibling);
    assert(Load(a, got, &swap) && got.msSwapped && got.media.empty());
    std::printf("card swap loads for ms.frondms ok\n");
}

static std::vector<uint8_t> Sector(uint32_t index) {
    std::vector<uint8_t> s(SD_SECTOR_SIZE);
    assert(sdCardRead(index, s.data()));
    return s;
}

static bool Settled(const fs::path& folder, uint32_t kind) {
    for (const auto& entry : fs::directory_iterator(folder)) {
        const auto name = entry.path().filename().string();
        if (name.find(".tmp-") != std::string::npos || name.find(".gen-") != std::string::npos) return false;
    }
    return frondSession::sdJournals(folder, kind).empty();
}

// After take-out the card file is settled and loads as a new card in another machine, where put-in,
// a save and a plain load follow.
static void MoveAndPutIn(const fs::path& from, uint32_t kind, const CardIdentity& identity, uint8_t marker) {
    const char* name = frondMachine::fileCardSibling(kind);
    const char* device = kind == frondMachine::kindSd ? "palm-tx" : "sony-clie-tg50";
    const int32_t type = kind == frondMachine::kindSd ? deviceTypeTX : deviceTypeTG50;
    assert(Settled(from, kind));
    frondMachine::Media check;
    auto swap = Swap(true, identity);
    assert(frondSession::swappedInCard(from, name, swap, check));
    assert(frondMachine::checkSdSibling(from / name, check, true) == frondMachine::SiblingStatus::ok);

    // Loads in another machine that was off.
    const auto other = Machine(std::string("other-") + name + std::to_string(marker));
    SaveFrond(other, Contents(device));
    fs::rename(from / name, other / name);
    frondSession::Contents got;
    assert(Load(other, got, &swap) && got.media.size() == 1 && got.media[0].identity == identity);

    // Or goes into a running one: put-in, then a save and a plain load.
    const auto running = Machine(std::string("running-") + name + std::to_string(marker));
    SaveFrond(running, Contents(device));
    fs::rename(other / name, running / name);
    // Not a size this device takes.
    assert(!frondNative::openFileCard(running, kind, kind == frondMachine::kindSd ? deviceTypeTG50 : deviceTypeTX,
                                      identity).ok);
    const auto put = kind == frondMachine::kindSd ? frondNative::putInSd(running, type, identity, true)
                                                  : frondNative::openFileCard(running, kind, type, identity);
    assert(put.ok && sdCardInitialized());
    assert(std::string(sdCardGetId()) == std::string(identity.begin(), identity.end()));
    assert(Sector(3)[0] == marker);
    std::vector<uint8_t> write(SD_SECTOR_SIZE, uint8_t(marker + 1));
    assert(sdCardWrite(4, write.data()));
    auto contents = Contents(device);
    frondMachine::Media media;
    frondNative::SdSave save;
    assert(frondNative::beginSdSave(running, media, true, save, kind,
                                    kind == frondMachine::kindSd && frondNative::slotCards().sdProtected));
    contents.media.push_back(media);
    assert(frondNative::publishWithSd(running, contents, {fs::path()}, save));
    assert(Load(running, got, nullptr) && got.media.size() == 1 && got.media[0].identity == identity);
    if (kind == frondMachine::kindSd) assert(got.media[0].writeProtected);
    sdCardReset();
    frondSdCandidate::releaseUnused();
    frondNative::slotCards() = {};
}

static void TakeOutFileCards() {
    std::vector<uint8_t> marker(SD_SECTOR_SIZE);

    // A card in RAM, never saved.
    {
        const auto a = Machine("take-ram");
        const size_t sectors = (32u << 20) / SD_SECTOR_SIZE;
        auto* buffer = static_cast<uint8_t*>(std::calloc(sectors, SD_SECTOR_SIZE));
        sdCardInitializeWithData(sectors, buffer, "0123456789abcdef0123456789abcdef");
        marker.assign(SD_SECTOR_SIZE, 0x41);
        assert(sdCardWrite(3, marker.data()));
        CardIdentity id{};
        assert(frondNative::takeOutFileCard(a, frondMachine::kindSd, false, id).ok && !sdCardInitialized());
        assert(std::string(id.begin(), id.end()) == "0123456789abcdef0123456789abcdef");
        MoveAndPutIn(a, frondMachine::kindSd, id, 0x41);
    }

    // A saved card with unsaved writes in its overlay.
    {
        const auto a = Machine("take-overlay");
        auto id = Id(7);
        WriteSd(a / frondMachine::sdSibling, 32u << 20, 0, 1, id);
        auto contents = Contents("palm-tx");
        contents.media.push_back(FileMedia(frondMachine::kindSd, 32u << 20, 1, id));
        SaveFrond(a, contents);
        char cid[SD_CARD_ID_MAX_LEN + 1]{};
        std::memcpy(cid, id.data(), SD_CARD_ID_MAX_LEN);
        assert(sdCardInitializeFromFile((a / frondMachine::sdSibling).c_str(), 32u << 20, frondSdFile::headerBytes, cid));
        marker.assign(SD_SECTOR_SIZE, 0x42);
        assert(sdCardWrite(3, marker.data()));
        CardIdentity got{};
        assert(frondNative::takeOutFileCard(a, frondMachine::kindSd, false, got).ok && got == id);
        // The next save of this machine has no card and leaves the file for the frontend.
        frondSession::Contents none = Contents("palm-tx");
        bool published = false;
        assert(frondSession::saveFolderFromTemps(a, none, {}, &published, &frondNative::slotCards().takenOut));
        assert(published && fs::exists(a / frondMachine::sdSibling));
        MoveAndPutIn(a, frondMachine::kindSd, id, 0x42);
    }

    // A card on a candidate, made in this run.
    {
        const auto a = Machine("take-candidate");
        frondSdCandidate::Candidate candidate;
        assert(frondSdCandidate::create(a, 64u << 20, {}, candidate).ok);
        assert(frondSdCandidate::attach(candidate, 64u << 20, "fedcba9876543210fedcba9876543210"));
        marker.assign(SD_SECTOR_SIZE, 0x43);
        assert(sdCardWrite(3, marker.data()));
        CardIdentity id{};
        assert(frondNative::takeOutFileCard(a, frondMachine::kindSd, false, id).ok);
        assert(frondSdCandidate::live().path.empty());
        MoveAndPutIn(a, frondMachine::kindSd, id, 0x43);
    }

    // A Memory Stick Pro on its saved file, mid-overlay, and on a candidate.
    {
        const auto a = Machine("take-pro");
        auto id = Id(8);
        WriteSd(a / frondMachine::msProSibling, 256u << 20, 0, 3, id);
        char cid[SD_CARD_ID_MAX_LEN + 1]{};
        std::memcpy(cid, id.data(), SD_CARD_ID_MAX_LEN);
        assert(sdCardInitializeFromFile((a / frondMachine::msProSibling).c_str(), 256u << 20, frondSdFile::headerBytes, cid));
        marker.assign(SD_SECTOR_SIZE, 0x44);
        assert(sdCardWrite(3, marker.data()));
        CardIdentity got{};
        assert(frondNative::takeOutFileCard(a, frondMachine::kindMsPro, true, got).ok && got == id);
        MoveAndPutIn(a, frondMachine::kindMsPro, id, 0x44);

        const auto b = Machine("take-pro-candidate");
        frondSdCandidate::Candidate candidate;
        assert(frondSdCandidate::create(b, 256u << 20, {}, candidate).ok);
        assert(frondSdCandidate::attach(candidate, 256u << 20, "00112233445566778899aabbccddeeff"));
        marker.assign(SD_SECTOR_SIZE, 0x45);
        assert(sdCardWrite(3, marker.data()));
        assert(frondNative::takeOutFileCard(b, frondMachine::kindMsPro, false, got).ok);
        MoveAndPutIn(b, frondMachine::kindMsPro, got, 0x45);
    }
    // No folder: refused, the card stays.
    {
        const size_t sectors = (32u << 20) / SD_SECTOR_SIZE;
        sdCardInitializeWithData(sectors, std::calloc(sectors, SD_SECTOR_SIZE), "0123456789abcdef0123456789abcdef");
        CardIdentity id{};
        assert(!frondNative::takeOutFileCard({}, frondMachine::kindSd, false, id).ok && sdCardInitialized());
        sdCardReset();
    }
    std::printf("card take-out and put-in for SD and Memory Stick Pro ok\n");
}

static void TakeOutClassic() {
    const auto* g = frondMsGeometry::byMegabytes(16);
    auto* ms = msCtrlrInit(nullptr, 0x14000000);
    assert(msCtrlrAttach(ms, frondMs::create(*g)));
    CardIdentity id{};
    bool protect = false;
    const auto a = Machine("take-classic");
    // Only once ejected.
    assert(!frondNative::takeOutMs(a, ms, id, protect).ok);
    assert(msCtrlrInsert(ms, false) && msCtrlrProtect(ms, true));
    frondNative::slotCards().msIdentity = Id(9);
    assert(frondNative::takeOutMs(a, ms, id, protect).ok && id == Id(9) && protect && !msCtrlrPresent(ms));
    assert(frondNative::slotCards().msIdentity == CardIdentity{});

    // Into another machine that was off, and into a running one.
    const auto off = Machine("other-classic");
    SaveFrond(off, Contents("sony-clie-tg50"));
    fs::rename(a / frondMachine::msSibling, off / frondMachine::msSibling);
    frondSession::Contents got;
    auto swap = Swap(true, id, protect);
    assert(Load(off, got, &swap) && got.media.size() == 1 && got.media[0].writeProtected && got.media[0].identity == id);
    auto* other = msCtrlrInit(nullptr, 0x14000000);
    assert(!frondNative::putInMs(off, other, deviceTypeTX, id, protect).ok);
    assert(frondNative::putInMs(off, other, deviceTypeTG50, id, protect).ok);
    assert(msCtrlrPresent(other) && msCtrlrInserted(other) && msCtrlrReadOnly(other) && !msCtrlrPro(other));
    assert(frondNative::slotCards().msIdentity == id);
    // A save carries the identity.
    frondMachine::Media media;
    frondNative::SdSave save;
    assert(frondNative::beginMsSave(off, other, media, save) && media.identity == id);
    if (!save.full.empty()) fs::remove(save.full);
    if (!save.journal.empty()) fs::remove(save.journal);
    msCtrlrDestroy(ms);
    msCtrlrDestroy(other);
    frondNative::slotCards() = {};
    std::printf("card take-out and put-in for classic Memory Stick ok\n");
}

// A classic stick write as the TG50 driver makes one: block 2, page 0, through the controller.
static void WriteStickPage(MemoryStickController* ms, uint32_t word) {
    auto wr = [](unsigned offset, uint32_t n, unsigned size = 2) {
        assert(mmio(mmioContext, 0x14000000 + offset, size, true, &n));
    };
    auto tpc = [&](uint8_t id, const std::vector<uint8_t>& bytes) {
        wr(0xb0, 0x100);
        wr(0x80, (uint16_t(id) << 12) | bytes.size());
        for (size_t i = 0; i < ((bytes.size() + 7) & ~size_t(7)); i += 4) {
            uint32_t w = 0;
            for (unsigned b = 0; b < 4 && i + b < bytes.size(); ++b) w |= uint32_t(bytes[i + b]) << (8 * b);
            wr(0x90, w, 4);
            if ((i & 7) == 4) msCtrlrPeriodic(ms);
        }
    };
    tpc(8, {0, 30, 16, 15});
    std::vector<uint8_t> params(15, 0xff);
    params[0] = 0x80;
    params[1] = params[2] = 0;
    params[3] = 2;
    params[4] = 0x20;
    params[5] = 0;
    tpc(0xb, params);
    tpc(0xe, {0x55});
    wr(0xb0, 0x100);
    wr(0x80, 0xd200);
    for (unsigned i = 0; i < 128; ++i) {
        wr(0x90, word, 4);
        if (i & 1) msCtrlrPeriodic(ms);
    }
}

// A .frondms's change counter (v1) or generation (v2).
static uint64_t StickGeneration(const fs::path& path, uint64_t physical) {
    if (frondMachine::msFileVersion(path) == 2) {
        frondSdFile::Info info;
        assert(frondMachine::readSdHeader(path, info) == frondMachine::SiblingStatus::ok && info.classic);
        return info.generation;
    }
    uint64_t changes = ~0ull;
    assert(frondMachine::msHeaderLooksRight(path, physical, &changes));
    return changes;
}

// The SD content generation is the change counter: every save moves it on, so a card saved in
// another machine comes back as changed.
static void SdCounter() {
    const auto a = Machine("counter-sd-a");
    const auto b = Root() / "counter-sd-b";
    fs::remove_all(b);
    const auto id = Id(10);
    WriteSd(a / frondMachine::sdSibling, 32u << 20, 0, 1, id);
    auto contents = Contents("palm-tx");
    contents.media.push_back(FileMedia(frondMachine::kindSd, 32u << 20, 1, id));
    SaveFrond(a, contents);
    fs::copy(a, b, fs::copy_options::recursive);
    fs::rename(b / FrondOf(a).filename(), FrondOf(b));

    // Machine b runs on the card, writes and saves twice; the second save has no writes.
    char cid[SD_CARD_ID_MAX_LEN + 1]{};
    std::memcpy(cid, id.data(), SD_CARD_ID_MAX_LEN);
    assert(sdCardInitializeFromFile((b / frondMachine::sdSibling).c_str(), 32u << 20, frondSdFile::headerBytes, cid));
    std::vector<uint8_t> marker(SD_SECTOR_SIZE, 0x51);
    assert(sdCardWrite(3, marker.data()));
    for (const uint64_t want : {2, 3}) {
        auto saved = Contents("palm-tx");
        frondMachine::Media media;
        frondNative::SdSave save;
        assert(frondNative::beginSdSave(b, media, true, save));
        saved.media.push_back(media);
        assert(frondNative::publishWithSd(b, saved, {fs::path()}, save));
        frondSdFile::Info info;
        assert(frondMachine::readSdHeader(b / frondMachine::sdSibling, info) == frondMachine::SiblingStatus::ok);
        assert(info.generation == want && media.contentGeneration == want);
    }
    sdCardReset();
    frondSdCandidate::releaseUnused();

    // Back in a: refused as saved, changed under --card-swap, unchanged once a has saved it.
    fs::remove(a / frondMachine::sdSibling);
    fs::rename(b / frondMachine::sdSibling, a / frondMachine::sdSibling);
    frondSession::Contents got;
    assert(!Load(a, got, nullptr));
    auto swap = Swap(true, id);
    assert(Load(a, got, &swap) && got.sdSwapped && got.media[0].contentGeneration == 3 && got.media[0].identity == id);
    SaveFrond(a, got);
    assert(Load(a, got, &swap) && !got.sdSwapped);
    std::printf("SD change counter ok\n");
}

// A classic stick's generation: a v1 file's change counter carries on as its v2 generation, and
// then every save moves it on, like SD.
static void ClassicCounter() {
    const auto* g = frondMsGeometry::byMegabytes(16);
    const auto id = Id(11);
    const auto a = Machine("counter-ms-a");
    const auto b = Root() / "counter-ms-b";
    fs::remove_all(b);
    const auto stick = a / frondMachine::msSibling;
    // A v1 stick at counter 4, as an older Frond saved it.
    {
        auto fresh = frondMs::create(*g);
        assert(frondSession::writeMsFile(stick, fresh.data(), fresh.size()));
        std::fstream f(stick, std::ios::in | std::ios::out | std::ios::binary);
        const uint8_t four[8] = {4};
        f.seekp(32);
        f.write(reinterpret_cast<const char*>(four), 8);
    }
    frondMachine::Media v1;
    v1.kind = frondMachine::kindMs;
    v1.name = frondMachine::msSibling;
    v1.inserted = true;
    v1.identity = id;
    v1.logicalSize = g->LogicalBytes();
    v1.physicalSize = g->PhysicalBytes();
    v1.contentGeneration = 4;
    auto contents = Contents("sony-clie-tg50");
    contents.media.push_back(v1);
    SaveFrond(a, contents);
    frondSession::Contents got;
    assert(Load(a, got, nullptr) && got.media[0].contentGeneration == 4);
    assert(StickGeneration(stick, g->PhysicalBytes()) == 4);

    // It loads into RAM, and the first save writes v2 at generation 5, a full file.
    auto* ms = msCtrlrInit(nullptr, 0x14000000);
    assert(frondNative::attachClassicFile(ms, stick, id, true, false));
    frondNative::msOpened(id, 4);
    assert(!sdCardFileBacked());
    auto save = [&](const fs::path& folder, uint64_t want, bool full) {
        auto saved = Contents("sony-clie-tg50");
        frondMachine::Media media;
        frondNative::SdSave sd;
        assert(frondNative::beginMsSave(folder, ms, media, sd));
        assert(full == !sd.full.empty() && full == sd.journal.empty());
        saved.media.push_back(media);
        assert(frondNative::publishWithSd(folder, saved, {fs::path()}, sd));
        assert(media.contentGeneration == want && media.identity == id);
        assert(StickGeneration(folder / frondMachine::msSibling, g->PhysicalBytes()) == want);
        assert(frondMachine::msFileVersion(folder / frondMachine::msSibling) == 2);
        return media;
    };
    WriteStickPage(ms, 0x5a5a5a5a);
    save(a, 5, true);
    // Now on the file: later saves are journals, one generation each.
    assert(sdCardFileBacked());
    WriteStickPage(ms, 0x1a1a1a1a);
    save(a, 6, false);
    auto last = save(a, 7, false);
    std::vector<uint8_t> image;
    assert(msCtrlrExport(ms, image));
    msCtrlrDestroy(ms);
    sdCardReset();
    frondSdCandidate::releaseUnused();

    // It reads back as the stick that was saved, and its descriptor carries the generation.
    std::vector<uint8_t> physical;
    assert(frondMs::readPhysical(stick, physical) && physical == image);
    std::vector<uint8_t> bytes;
    frondMachine::Media decoded;
    assert(frondMachine::encodeDescriptor(last, bytes) && frondMachine::decodeDescriptor(bytes.data(), bytes.size(), decoded));
    assert(decoded.contentGeneration == 7);
    assert(Load(a, got, nullptr) && got.media[0].contentGeneration == 7);

    // Into another folder, saved there, and back: refused as saved, changed under --card-swap.
    fs::copy(a, b, fs::copy_options::recursive);
    fs::rename(b / FrondOf(a).filename(), FrondOf(b));
    assert(Load(b, got, nullptr));
    ms = msCtrlrInit(nullptr, 0x14000000);
    assert(frondNative::attachClassicFile(ms, b / frondMachine::msSibling, id, true, false));
    frondNative::msOpened(id, 7);
    WriteStickPage(ms, 0x2b2b2b2b);
    save(b, 8, false);
    msCtrlrDestroy(ms);
    sdCardReset();
    frondSdCandidate::releaseUnused();
    fs::remove(stick);
    for (const auto& e : fs::directory_iterator(a))
        if (e.path().filename().string().rfind("ms.frondms.", 0) == 0) fs::remove(e.path());
    for (const auto& e : fs::directory_iterator(b))
        if (e.path().filename().string().rfind("ms.frondms", 0) == 0) fs::rename(e.path(), a / e.path().filename());
    assert(!Load(a, got, nullptr));
    auto swap = Swap(true, id);
    assert(Load(a, got, &swap) && got.msSwapped && got.media[0].contentGeneration == 8 && got.media[0].identity == id);
    frondNative::slotCards() = {};
    std::printf("classic Memory Stick generation ok\n");
}

static void Arguments() {
    CardIdentity id{};
    bool protect = true;
    const std::string hex(64, 'a');
    assert(frondNative::parsePutIn(hex, id, protect) && !protect && id[0] == 0xaa);
    assert(frondNative::parsePutIn(hex + " protected", id, protect) && protect);
    assert(!frondNative::parsePutIn(hex + " locked", id, protect));
    assert(!frondNative::parsePutIn(hex.substr(1), id, protect));
    assert(frondNative::identityHex(Id(1)).size() == 64);
    assert(frondNative::takenOutReply(id, true) == hex + " protected");
    assert(frondMachine::packableName("slot.json"));
    std::printf("card swap arguments ok\n");
}

int main() {
    Arguments();
    FileCardLoads(frondMachine::kindSd, 32u << 20);
    FileCardLoads(frondMachine::kindMsPro, 256u << 20);
    ClassicLoads();
    TakeOutFileCards();
    TakeOutClassic();
    SdCounter();
    ClassicCounter();
    fs::remove_all(Root());
    std::printf("card swap ok\n");
}
