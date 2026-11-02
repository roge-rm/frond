#include "CardBacking.h"
#include "FileCardBacking.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
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

void CardBackingGenerations() {
    RamGenerations();
    FileSectorsAndRecords();
    NestedAndRefused();
    SnapshotDoesNotClone();
    FailuresKeepRoot();
    CompactKeepsPin();
    IndexReadErrorFails();
    GenerationsFollowCard();
}
