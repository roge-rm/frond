#include "CardBacking.h"
#include "FileCardBacking.h"
#include "SessionIo.h"
#include "card-history-limits.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void WriteAll(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    assert(out);
    if (!bytes.empty()) assert(out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

static fs::path GenDir() {
    const auto dir = fs::path("build") / "card-backing-generations";
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

static std::vector<uint8_t> Filled(size_t n, uint8_t v) { return std::vector<uint8_t>(n, v); }

static void ExpectRange(CardBacking& card, uint64_t off, const std::vector<uint8_t>& want) {
    std::vector<uint8_t> got(want.size(), 0x3c);
    assert(card.Read(off, want.size(), got.data()) && got == want);
}

static void RamGenerations() {
    RamCardBacking card(Filled(2048, 0xa5), 512);
    const uint8_t a[] = {1, 2, 3, 4};
    assert(card.Write(0, 4, a));
    auto s1 = card.Snapshot();
    assert(s1 && s1->GenerationId() == 0);
    const uint8_t b[] = {9, 9, 9, 9};
    assert(card.Write(0, 4, b));
    auto s2 = card.Snapshot();
    ExpectRange(card, 0, {9, 9, 9, 9});
    assert(card.Restore(*s1));
    ExpectRange(card, 0, {1, 2, 3, 4});
    RamCardBacking other(Filled(2048, 0x11), 512);
    assert(!card.Restore(*other.Snapshot()));
    ExpectRange(card, 0, {1, 2, 3, 4});
    assert(card.Restore(*s2));
    ExpectRange(card, 0, {9, 9, 9, 9});
    assert(card.Commit());
    assert(card.Write(100, 4, a));
    assert(card.Rollback());
    ExpectRange(card, 0, {9, 9, 9, 9});
    uint8_t probe[4] = {0};
    assert(card.Read(100, 4, probe) && probe[0] == 0xa5);
}

static std::unique_ptr<FileCardBacking> OpenRaw(const fs::path& path, uint64_t size, uint32_t page,
                                               size_t cache, FileCardIoHooks* hooks = nullptr) {
    FileCardOptions opt;
    opt.pageSize = page;
    opt.cacheBytes = cache;
    opt.indexCacheEntries = 2;
    opt.maxGenerations = 4;
    return FileCardBacking::Open(path, size, 0, opt, hooks);
}

static void FileSectorsAndRecords() {
    const auto dir = GenDir();
    const auto sdPath = dir / "tiny.sd";
    WriteAll(sdPath, Filled(4096, 0xa5));
    FileCardIoHooks hooks;
    auto card = OpenRaw(sdPath, 4096, 512, 1024, &hooks);
    assert(card);
    std::vector<uint8_t> first(512, 0x11), last(512, 0x22), cross{1, 2, 3, 4};
    assert(card->Write(0, 512, first.data()));
    assert(card->Write(4096 - 512, 512, last.data()));
    assert(card->Write(510, 4, cross.data()));
    assert(card->Write(100, 0, nullptr));
    assert(card->Read(100, 0, nullptr));
    uint8_t scratch = 0xee;
    assert(!card->Write(4096, 1, &scratch) && !card->Read(4097, 0, &scratch));
    assert(!card->Write(4095, 2, first.data()));
    ExpectRange(*card, 0, std::vector<uint8_t>(510, 0x11));
    ExpectRange(*card, 510, {1, 2, 3, 4});
    ExpectRange(*card, 4096 - 512, last);
    assert(card->Commit());

    const auto msPath = dir / "tiny.ms";
    WriteAll(msPath, Filled(521 * 4, 0xff));
    auto ms = OpenRaw(msPath, 521 * 4, 521, 521 * 2);
    assert(ms);
    std::vector<uint8_t> rec(521, 0xc3);
    rec.back() = 0x42;
    assert(ms->Write(521 * 3, 521, rec.data()));
    std::vector<uint8_t> got(521);
    assert(ms->Read(521 * 3, 521, got.data()) && got == rec);
    assert(ms->Read(0, 521, got.data()) && got[0] == 0xff);
    assert(ms->Commit());
}

static void NestedAndRefused() {
    const auto dir = GenDir();
    const auto path = dir / "card.bin";
    WriteAll(path, Filled(2048, 0xa5));
    auto card = OpenRaw(path, 2048, 512, 512);
    const uint8_t a[] = {0x10, 0x10, 0x10, 0x10};
    const uint8_t b[] = {0x20, 0x20, 0x20, 0x20};
    assert(card->Write(0, 4, a));
    auto s1 = card->Snapshot();
    assert(card->Write(0, 4, b));
    auto s2 = card->Snapshot();
    ExpectRange(*card, 0, {0x20, 0x20, 0x20, 0x20});
    assert(card->Restore(*s1));
    ExpectRange(*card, 0, {0x10, 0x10, 0x10, 0x10});
    WriteAll(dir / "other.bin", Filled(2048, 0x00));
    auto other = OpenRaw(dir / "other.bin", 2048, 512, 512);
    assert(!card->Restore(*other->Snapshot()));
    ExpectRange(*card, 0, {0x10, 0x10, 0x10, 0x10});
    assert(card->Restore(*s2));
    ExpectRange(*card, 0, {0x20, 0x20, 0x20, 0x20});
    assert(card->Commit());
}

static void SnapshotDoesNotClone() {
    const auto dir = GenDir();
    const auto path = dir / "card.bin";
    WriteAll(path, Filled(8192, 0xa5));
    FileCardIoHooks hooks;
    auto card = OpenRaw(path, 8192, 512, 1024, &hooks);
    std::vector<uint8_t> poke(512, 0x5a);
    assert(card->Write(0, 512, poke.data()));
    hooks.bytesRead = 0;
    hooks.bytesWritten = 0;
    auto pin = card->Snapshot();
    assert(pin);
    assert(hooks.bytesRead < 2048);
    assert(hooks.bytesRead < 8192);
    uint64_t genBytes = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        const auto name = entry.path().filename().string();
        if (name.find(".gen-") == std::string::npos || name == ".gen-root") continue;
        if (entry.is_regular_file()) genBytes += entry.file_size();
    }
    assert(genBytes < 8192);
    ExpectRange(*card, 0, poke);
}

static void FailuresKeepRoot() {
    const auto dir = GenDir();
    const auto path = dir / "card.bin";
    const auto original = Filled(2048, 0xa5);
    WriteAll(path, original);

    auto reopen = [&]() {
        auto card = OpenRaw(path, 2048, 512, 512);
        assert(card);
        std::vector<uint8_t> got(2048);
        assert(card->Read(0, 2048, got.data()));
        return got;
    };

    {
        FileCardIoHooks hooks;
        auto card = OpenRaw(path, 2048, 512, 512, &hooks);
        std::vector<uint8_t> poke(512, 0x11);
        assert(card->Write(0, 512, poke.data()));
        assert(card->Commit());
        hooks = {};
        hooks.failWriteAfter = 0;
        std::vector<uint8_t> next(512, 0x22);
        const bool wrote = card->Write(512, 512, next.data());
        if (wrote) assert(!card->Commit());
        assert(card->Rollback());
        ExpectRange(*card, 0, poke);
        ExpectRange(*card, 512, Filled(512, 0xa5));
    }
    {
        auto got = reopen();
        assert(std::vector<uint8_t>(got.begin(), got.begin() + 512) == Filled(512, 0x11));
        assert(std::vector<uint8_t>(got.begin() + 512, got.end()) == Filled(1536, 0xa5));
    }
    {
        FileCardIoHooks hooks;
        auto card = OpenRaw(path, 2048, 512, 512, &hooks);
        std::vector<uint8_t> next(512, 0x33);
        assert(card->Write(1024, 512, next.data()));
        hooks = {};
        hooks.failFlushAfter = 0;
        assert(!card->Commit());
        assert(card->Rollback());
        ExpectRange(*card, 1024, Filled(512, 0xa5));
    }
    assert(reopen()[1024] == 0xa5);
    {
        FileCardIoHooks hooks;
        auto card = OpenRaw(path, 2048, 512, 512, &hooks);
        std::vector<uint8_t> next(512, 0x44);
        assert(card->Write(1536, 512, next.data()));
        hooks = {};
        hooks.failCloseAfter = 0;
        assert(!card->Commit());
        assert(card->Rollback());
        ExpectRange(*card, 1536, Filled(512, 0xa5));
    }
    assert(reopen()[1536] == 0xa5);
    {
        FileCardIoHooks hooks;
        auto card = OpenRaw(path, 2048, 512, 512, &hooks);
        std::vector<uint8_t> next(512, 0x55);
        assert(card->Write(1536, 512, next.data()));
        hooks = {};
        hooks.failRenameAfter = 0;
        assert(!card->Commit());
        assert(card->Rollback());
        ExpectRange(*card, 1536, Filled(512, 0xa5));
    }
    assert(reopen()[1536] == 0xa5);
    assert(reopen()[0] == 0x11);
}

static void CompactKeepsPin() {
    const auto dir = GenDir();
    const auto path = dir / "card.bin";
    WriteAll(path, Filled(4096, 0xa5));
    auto card = OpenRaw(path, 4096, 512, 1024);
    std::vector<uint8_t> first(512, 0x31), second(512, 0x32);
    assert(card->Write(0, 512, first.data()));
    assert(card->Commit());
    auto pin = card->Snapshot();
    assert(card->Write(512, 512, second.data()));
    assert(card->Commit());
    assert(card->Compact());
    ExpectRange(*card, 0, first);
    ExpectRange(*card, 512, second);
    std::vector<uint8_t> got(512);
    assert(pin->Read(0, 512, got.data()) && got == first);
    assert(pin->Read(512, 512, got.data()) && got == Filled(512, 0xa5));
}

static void IndexReadErrorFails() {
    const auto dir = GenDir();
    const auto path = dir / "card.bin";
    WriteAll(path, Filled(2048, 0xa5));
    {
        auto card = OpenRaw(path, 2048, 512, 512);
        std::vector<uint8_t> poke(512, 0x11);
        assert(card->Write(0, 512, poke.data()));
        assert(card->Commit());
    }
    FileCardIoHooks hooks;
    auto card = OpenRaw(path, 2048, 512, 512, &hooks);
    assert(card);
    hooks.failReadAfter = hooks.reads;
    uint8_t buf[512];
    std::memset(buf, 0x3c, sizeof(buf));
    assert(!card->Read(0, 512, buf));
    assert(buf[0] == 0x3c);
}

// Generations only apply to the file and card they were written for.
static void GenerationsFollowCard() {
    const auto dir = GenDir();
    const auto path = dir / "card.bin";
    WriteAll(path, Filled(2048, 0xa5));
    auto open = [&](const std::string& identity) {
        FileCardOptions opt;
        opt.pageSize = 512;
        opt.cacheBytes = 512;
        opt.identity = identity;
        return FileCardBacking::Open(path, 2048, 0, opt);
    };
    auto hasGenerations = [&] {
        for (const auto& entry : fs::directory_iterator(dir))
            if (entry.path().filename().string().rfind(".gen-", 0) == 0) return true;
        return false;
    };
    const auto poke = Filled(512, 0x11);
    {
        auto card = open("one");
        assert(card->Write(0, 512, poke.data()) && card->Commit());
    }
    {
        auto card = open("one");
        ExpectRange(*card, 0, poke);
    }
    {
        // Another card in the same file: its generations are dropped, not applied.
        auto card = open("two");
        ExpectRange(*card, 0, Filled(512, 0xa5));
        assert(!hasGenerations());
    }
    {
        auto card = open("one");
        assert(card->Write(0, 512, poke.data()) && card->Commit());
    }
    // The file replaced by a new one of the same size and card.
    const auto replacement = dir / "replacement.bin";
    WriteAll(replacement, Filled(2048, 0x5a));
    fs::rename(replacement, path);
    {
        auto card = open("one");
        ExpectRange(*card, 0, Filled(512, 0x5a));
        assert(!hasGenerations());
    }
    // A different size discards instead of refusing the card.
    {
        auto card = open("one");
        assert(card->Write(0, 512, poke.data()) && card->Commit());
    }
    const auto bigger = dir / "bigger.bin";
    WriteAll(bigger, Filled(4096, 0x77));
    fs::rename(bigger, path);
    {
        FileCardOptions opt;
        opt.pageSize = 512;
        opt.identity = "one";
        auto card = FileCardBacking::Open(path, 4096, 0, opt);
        assert(card);
        ExpectRange(*card, 0, Filled(512, 0x77));
    }
}

static std::vector<uint8_t> ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

static std::vector<uint8_t> V2(const std::vector<uint8_t>& payload, uint64_t generation) {
    std::vector<uint8_t> file;
    assert(frondSdFile::encodeV2(payload, generation, file));
    return file;
}

// Opens a .frondsd v2 the way sdcard.cpp does: chunk table from the file.
static std::unique_ptr<FileCardBacking> OpenV2(const fs::path& path, uint64_t size) {
    frondSdFile::Info info;
    FileCardOptions opt;
    assert(frondMachine::readSdHeader(path, info, &opt.chunkCrcs) == frondMachine::SiblingStatus::ok);
    assert(info.version == frondSdFile::versionV2 && info.payloadBytes == size);
    return FileCardBacking::Open(path, size, frondSdFile::headerBytes, opt);
}

// Journal of a FileCardBacking's changes, applied in place, in steps and twice.
static void JournalApplyReplay() {
    const auto dir = GenDir();
    const auto sidecar = dir / frondMachine::sdSibling;
    std::vector<uint8_t> payload((3u << 20) + 4096);  // four chunks, the last one short
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 5 + i / 777);
    const auto oldFile = V2(payload, 4);
    WriteAll(sidecar, oldFile);
    auto expect = payload;
    std::vector<uint8_t> journal;
    {
        auto card = OpenV2(sidecar, payload.size());
        assert(card);
        const auto a = Filled(512, 0x11), b = Filled(4096, 0x22), c = Filled(512, 0x33);
        const uint64_t at[] = {512, (2u << 20) - 512, payload.size() - 512};
        assert(card->Write(at[0], a.size(), a.data()) && card->Write(at[1], b.size(), b.data()) &&
               card->Write(at[2], c.size(), c.data()));
        std::memcpy(expect.data() + at[0], a.data(), a.size());
        std::memcpy(expect.data() + at[1], b.data(), b.size());
        std::memcpy(expect.data() + at[2], c.data(), c.size());
        auto pin = card->Snapshot();
        assert(pin);
        frondSession::SdJournalWriter writer;
        assert(writer.open(dir, payload.size(), 4, 5));
        uint64_t pages = 0;
        assert(card->ForEachOverlayIn(*pin, [&](uint64_t off, uint64_t n, const uint8_t* data) {
            ++pages;
            assert(!std::memcmp(data, expect.data() + off, n));
            return writer.add(off, uint32_t(n), data);
        }));
        assert(pages == 4);  // the 4 KiB write spans two pages
        frondSdFile::Info info;
        std::vector<uint8_t> newFile = V2(expect, 5);
        std::vector<uint32_t> table;
        assert(frondSdFile::parseHeader(newFile.data(), info));
        assert(frondSdFile::readTable(newFile.data(), newFile.data() + 64 + expect.size(), 16, info, table));
        fs::path done;
        assert(writer.finish(table, done) && done == frondSession::sdJournalPath(dir, 5));
        journal = ReadBytes(done);
    }
    const auto neighbour = fs::path(sidecar.string() + "-2");
    WriteAll(neighbour, oldFile);
    auto peer = OpenV2(neighbour, payload.size()); assert(peer);
    const auto peerPage = Filled(4096, 0x81);
    assert(peer->Write(0, peerPage.size(), peerPage.data()) && peer->Commit());
    assert(peer->Write(8192, peerPage.size(), peerPage.data()));
    auto peerPending = peer->Snapshot(); assert(peerPending);
    auto peerFiles = [&] {
        std::map<fs::path, std::vector<uint8_t>> files;
        for (const auto& e : fs::directory_iterator(dir))
            if (e.path() == neighbour || FileCardBacking::GeneratedName(neighbour.filename().string(), e.path().filename().string()))
                files[e.path()] = ReadBytes(e.path());
        return files;
    };
    const auto peerBefore = peerFiles();
    const auto journalPath = frondSession::sdJournalPath(dir, 5);
    frondSession::SdJournal parsed;
    assert(frondSession::readSdJournal(journalPath, parsed));
    assert(parsed.from == 4 && parsed.to == 5 && parsed.records == 4 && parsed.table.size() == 4);

    // Damaged or short journals are refused and the sidecar is untouched.
    auto bad = journal;
    bad[70] ^= 1;
    WriteAll(journalPath, bad);
    assert(!frondSession::readSdJournal(journalPath, parsed));
    assert(!frondSession::applySdJournal(dir, journalPath) && ReadBytes(sidecar) == oldFile);
    WriteAll(journalPath, std::vector<uint8_t>(journal.begin(), journal.end() - 4));
    assert(!frondSession::applySdJournal(dir, journalPath) && ReadBytes(sidecar) == oldFile);
    WriteAll(journalPath, journal);

    // Pages and table, then header, then the journal goes.
    assert(frondSession::applySdJournal(dir, journalPath, nullptr, frondSession::JournalStep::pages));
    auto mid = ReadBytes(sidecar);
    assert(std::equal(oldFile.begin(), oldFile.begin() + 64, mid.begin()));
    assert(std::equal(mid.begin() + 64, mid.end(), V2(expect, 5).begin() + 64));
    std::vector<uint32_t> table;
    assert(frondSession::applySdJournal(dir, journalPath, &table, frondSession::JournalStep::header));
    assert(ReadBytes(sidecar) == V2(expect, 5) && fs::exists(journalPath) && table.size() == 4);
    assert(frondSession::applySdJournal(dir, journalPath));
    assert(ReadBytes(sidecar) == V2(expect, 5) && !fs::exists(journalPath));
    WriteAll(journalPath, journal);
    assert(frondSession::applySdJournal(dir, journalPath));
    assert(ReadBytes(sidecar) == V2(expect, 5) && !fs::exists(journalPath));

    // Load rules: descriptor N+1 replays, descriptor N discards, anything else refuses.
    frondMachine::Media media;
    media.kind = frondMachine::kindSd;
    media.name = frondMachine::sdSibling;
    media.logicalSize = media.physicalSize = payload.size();
    WriteAll(sidecar, oldFile);
    WriteAll(journalPath, journal);
    media.contentGeneration = 4;
    assert(frondSession::settleSdJournal(dir, media));
    assert(ReadBytes(sidecar) == oldFile && !fs::exists(journalPath));
    WriteAll(journalPath, journal);
    media.contentGeneration = 5;
    assert(frondSession::settleSdJournal(dir, media));
    assert(ReadBytes(sidecar) == V2(expect, 5) && !fs::exists(journalPath));
    WriteAll(sidecar, oldFile);
    media.contentGeneration = 6;
    assert(!frondSession::settleSdJournal(dir, media) && ReadBytes(sidecar) == oldFile);
    media.contentGeneration = 3;
    assert(!frondSession::settleSdJournal(dir, media) && ReadBytes(sidecar) == oldFile);
    media.contentGeneration = 5;
    assert(!frondSession::settleSdJournal(dir, media) && ReadBytes(sidecar) == oldFile);

    assert(peerFiles() == peerBefore);
    assert(peer->Commit()); peerPending.reset(); peer.reset();
    peer = OpenV2(neighbour, payload.size()); assert(peer);
    ExpectRange(*peer, 8192, peerPage);

    // After the in-place write the reopened card reads the new bytes with the new table.
    auto card = OpenV2(sidecar, payload.size());
    std::vector<uint8_t> all(payload.size());
    assert(card && card->Read(0, all.size(), all.data()) && all == payload);
    fs::remove_all(dir);
}

// Chunks are checked on first read from the durable file only; overlay pages never are.
static void LazyChunkCrc() {
    const auto dir = GenDir();
    const auto sidecar = dir / frondMachine::sdSibling;
    std::vector<uint8_t> payload(2u << 20, 0x5c);
    auto file = V2(payload, 1);
    WriteAll(sidecar, file);
    auto card = OpenV2(sidecar, payload.size());
    assert(card);
    file[64 + (1u << 20) + 9] ^= 1;  // damage chunk 1 behind the card's back
    WriteAll(sidecar, file);
    std::vector<uint8_t> got(512);
    assert(card->Read(0, 512, got.data()) && got == Filled(512, 0x5c));
    assert(!card->Read((1u << 20) + 4096, 512, got.data()));
    const auto page = Filled(4096, 0x99);
    assert(card->Write((1u << 20) + 8192, page.size(), page.data()));  // whole page, no read
    assert(card->Read((1u << 20) + 8192, 512, got.data()) && got == Filled(512, 0x99));
    // A new table that matches the bytes on disk clears the failure.
    frondSdFile::ChunkCrcs crcs;
    crcs.add(file.data() + 64, payload.size());
    assert(card->SetChunkCrcs(crcs.table()));
    assert(card->Read((1u << 20) + 4096, 512, got.data()) && got == Filled(512, 0x5c));
    assert(!card->SetChunkCrcs({1, 2, 3}));
    fs::remove_all(dir);
}

#include "card-crash-recovery.h"

void CardBackingGenerations() {
    RootlessCrashRecovery(GenDir());
    HistoryLimits(GenDir(), false);
    RamGenerations();
    FileSectorsAndRecords();
    NestedAndRefused();
    SnapshotDoesNotClone();
    FailuresKeepRoot();
    CompactKeepsPin();
    IndexReadErrorFails();
    GenerationsFollowCard();
    JournalApplyReplay();
    LazyChunkCrc();
}
