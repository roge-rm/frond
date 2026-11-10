#ifndef FROND_CARD_HISTORY_LIMITS_TEST_H
#define FROND_CARD_HISTORY_LIMITS_TEST_H
#include "FileCardBacking.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <sys/file.h>

static std::vector<uint8_t> HistoryBytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
static void HistoryPut(const std::filesystem::path& p, const std::vector<uint8_t>& b) {
    std::ofstream f(p, std::ios::binary); assert(f.write(reinterpret_cast<const char*>(b.data()), b.size()));
}
static void HistoryLimits(const std::filesystem::path& dir, bool strict) {
    namespace fs = std::filesystem;
    fs::create_directories(dir);
    for (bool depth : {false, true}) {
        const auto p = dir / (depth ? "depth" : "records");
        const size_t pages = depth ? 32 : 2, size = pages * 512;
        HistoryPut(p, std::vector<uint8_t>(size));
        FileCardOptions o; o.pageSize = 512; o.rejectInvalidRoot = strict;
        FileCardIoHooks h; h.failCompact = true;
        auto card = FileCardBacking::Open(p, size, 0, o, &h); assert(card);
        auto basePin = card->Snapshot(); assert(basePin);
        auto check = [&](const std::vector<uint8_t>& want) {
            auto fresh = FileCardBacking::Open(p, size, 0, o); assert(fresh);
            std::vector<uint8_t> got(size); assert(fresh->Read(0, size, got.data()) && got == want);
        };
        std::vector<uint8_t> expected(size), next(size);
        std::shared_ptr<const CardSnapshot> pin;
        const int boundary = depth ? 256 : 16;
        for (int i = 1; i <= boundary + 12; ++i) {
            next = expected;
            std::fill(next.begin(), next.begin() + (depth ? 512 : size), uint8_t(i % 253 + 1));
            assert(card->Write(0, depth ? 512 : size, next.data()));
            if (i <= boundary) {
                assert(card->Commit()); expected = next;
                if (i == 2) pin = card->Snapshot();
            } else {
                const auto root = HistoryBytes(dir / (".gen-" + p.filename().string() + "-root"));
                assert(!card->Commit());
                assert(HistoryBytes(dir / (".gen-" + p.filename().string() + "-root")) == root);
                assert(card->Rollback());
            }
            check(expected);
        }
        // Reopen read failures and missing roots cannot discard acknowledged history.
        const auto rootPath = dir / (".gen-" + p.filename().string() + "-root");
        const auto savedRoot = HistoryBytes(rootPath);
        FileCardIoHooks readFailure; readFailure.failReadAfter = 0;
        assert(!FileCardBacking::Open(p, size, 0, o, &readFailure));
        assert(HistoryBytes(rootPath) == savedRoot); check(expected);
        fs::remove(rootPath); assert(!FileCardBacking::Open(p, size, 0, o));
        HistoryPut(rootPath, savedRoot); check(expected);
        // Mandatory compaction failures must retain the root and allow a retry.
        assert(card->Write(0, depth ? 512 : size, next.data()));
        assert(!card->Commit()); h.failCompact = false;
        for (int failure = 0; failure < 5; ++failure) {
            int* limit[] = {&h.failReadAfter, &h.failWriteAfter, &h.failFlushAfter, &h.failCloseAfter, &h.failRenameAfter};
            int count[] = {h.reads, h.writes, h.flushes, h.closes, h.renames};
            *limit[failure] = count[failure];
            assert(!card->Commit()); *limit[failure] = -1; check(expected);
        }
        assert(card->Commit()); expected = next; check(expected);
        std::vector<uint8_t> old(size); assert(pin->Read(0, size, old.data()) && old[0] == 3);
        assert(card->Restore(*pin) && card->Commit()); check(old);
        assert(card->Rollback() && card->Compact()); check(old);
        assert(card->Restore(*basePin) && card->Compact());
        expected.assign(size, 0); check(expected);
        pin.reset(); basePin.reset(); card.reset(); check(expected);
        std::cout << (strict ? "strict " : "default ") << (depth ? "depth" : "record")
                  << " boundary, repeated compaction faults, pin and retry PASS\n";
    }
    // Open both neighbours before creating the shorter card's strict root.
    for (const auto* neighbour : {"card-2", "card-tmp-1", "card-tmp-idx-1", "card-tmp-root-1"}) {
        const auto sub = dir / neighbour; fs::create_directories(sub);
        const auto a = sub / "card", b = sub / neighbour;
        HistoryPut(a, std::vector<uint8_t>(1024)); HistoryPut(b, std::vector<uint8_t>(1024));
        FileCardOptions o; o.pageSize = 512; o.rejectInvalidRoot = strict;
        auto peer = FileCardBacking::Open(b, 1024, 0, o); assert(peer);
        uint8_t v = 47; assert(peer->Write(0, 1, &v) && peer->Commit());
        v = 63; assert(peer->Write(512, 1, &v)); auto pending = peer->Snapshot(); assert(pending);
        auto card = FileCardBacking::Open(a, 1024, 0, o); assert(card);
        v = 91; assert(card->Write(0, 1, &v) && card->Commit());
        auto bytes = [&] { std::map<fs::path, std::vector<uint8_t>> m;
            for (const auto& e : fs::directory_iterator(sub))
                if (e.path() == b || FileCardBacking::GeneratedName(neighbour, e.path().filename().string()))
                    m[e.path()] = HistoryBytes(e.path());
            return m;
        };
        const int lock = ::open(b.c_str(), O_RDWR | O_CLOEXEC); assert(lock >= 0);
        assert(flock(lock, LOCK_EX | LOCK_NB) == 0);
        const auto before = bytes();
        const auto peerRoot = sub / (".gen-" + std::string(neighbour) + "-root");
        const auto peerRootBytes = HistoryBytes(peerRoot);
        fs::remove(peerRoot);
        FileCardOptions reject = o; reject.rejectInvalidRoot = true;
        assert(!FileCardBacking::Open(b, 1024, 0, reject));
        HistoryPut(peerRoot, peerRootBytes);
        for (const auto* suffix : {"tmp-987654-12345", "tmp-idx-987654-12345", "tmp-root-987654-12345"})
            HistoryPut(sub / (".gen-card-" + std::string(suffix)), {9});
        card.reset(); FileCardBacking::DiscardGenerations(a); assert(bytes() == before);
        for (const auto* suffix : {"tmp-987654-12345", "tmp-idx-987654-12345", "tmp-root-987654-12345"})
            assert(!fs::exists(sub / (".gen-card-" + std::string(suffix))));
        // Unknown entries survive cleanup and forbid a pristine strict attach.
        const auto malformed = sub / ".gen-card-tmp-bad";
        HistoryPut(malformed, {7}); FileCardBacking::DiscardGenerations(a);
        assert(HistoryBytes(malformed) == std::vector<uint8_t>{7});
        FileCardOptions strictO = o; strictO.rejectInvalidRoot = true;
        assert(!FileCardBacking::Open(a, 1024, 0, strictO)); fs::remove(malformed);
        card = FileCardBacking::Open(a, 1024, 0, strictO); assert(card && bytes() == before);
        const int probe = ::open(b.c_str(), O_RDWR | O_CLOEXEC); assert(probe >= 0);
        assert(flock(probe, LOCK_EX | LOCK_NB) == -1); ::close(probe);
        ::close(lock);
        assert(peer->Commit()); pending.reset(); peer.reset();
        peer = FileCardBacking::Open(b, 1024, 0, o); assert(peer);
        assert(peer->Read(512, 1, &v) && v == 63);
        std::cout << neighbour << " namespace isolation PASS\n";
    }
}
#endif
