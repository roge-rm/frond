// The storage library's settle and verify (src/common/storage/): both apps' Files… leftovers,
// journal v2 session and standalone journals cut off at each step, pending lists, side regions,
// and a second settle changing nothing. Writes the settled leftover cards to argv[1] for
// tests/storage-settle.py to read with mtools.
#include "SessionIo.h"
#include "storage/CardFile.h"
#include "storage/Settle.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

static Bytes ReadAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    assert(in);
    return Bytes(std::istreambuf_iterator<char>(in), {});
}

static void WriteAll(const fs::path& path, const Bytes& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    assert(out);
    if (!bytes.empty()) assert(out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())));
}

// Every file in a folder by name.
using State = std::map<std::string, Bytes>;
static State Snapshot(const fs::path& dir) {
    State out;
    for (const auto& e : fs::directory_iterator(dir)) out[e.path().filename().string()] = ReadAll(e.path());
    return out;
}
static void Restore(const fs::path& dir, const State& state) {
    fs::remove_all(dir);
    fs::create_directories(dir);
    for (const auto& [name, bytes] : state) WriteAll(dir / name, bytes);
}

static fs::path Fresh(const fs::path& root, const std::string& name) {
    const auto dir = root / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

static Bytes Random(size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    Bytes out(n);
    for (auto& b : out) b = uint8_t(rng());
    return out;
}

static uint64_t GenerationOf(const fs::path& card) {
    frondSdFile::Info info;
    assert(frondStorage::verify(card, info).ok);
    return info.generation;
}

static Bytes Payload(const fs::path& card) {
    const auto all = ReadAll(card);
    frondSdFile::Info info;
    assert(frondSdFile::parseContainer(all.data(), info));
    return Bytes(all.begin() + 64, all.begin() + 64 + long(info.stored()));
}

// A settle that was done already leaves every byte and file as it is.
static void SecondSettleChangesNothing(const fs::path& dir, const fs::path& card, const uint64_t* want = nullptr,
                                       uint64_t payload = 0) {
    const auto before = Snapshot(dir);
    assert(!frondStorage::hasChanges(card));
    const auto again = want ? frondStorage::settle(card, *want, payload) : frondStorage::settle(card);
    assert(again.ok);
    assert(Snapshot(dir) == before);
}

// ---- The apps' leftovers ----

// "FRONDSP1", length u64, then (offset u64, 512 bytes) per block that isn't all zero.
static Bytes Unsparse(const Bytes& b) {
    assert(b.size() >= 16 && !std::memcmp(b.data(), "FRONDSP1", 8));
    Bytes out(size_t(frondSdFile::get64(b.data() + 8)), 0);
    for (size_t i = 16; i + 520 <= b.size(); i += 520) {
        const auto at = size_t(frondSdFile::get64(b.data() + i));
        const size_t n = std::min<size_t>(512, out.size() - at);
        std::memcpy(out.data() + at, b.data() + i + 8, n);
    }
    return out;
}

static uint64_t ExpectedGeneration(const fs::path& expect) {
    const auto text = ReadAll(expect);
    const std::string s(text.begin(), text.end());
    assert(s.rfind("generation ", 0) == 0);
    return std::stoull(s.substr(11));
}

static void LeftoversFromEitherApp(const fs::path& root, const fs::path& fixtures, const fs::path& out) {
    fs::create_directories(out);
    for (const char* side : {"android", "desktop"})
        for (const char* name : {"commit", "intent"}) {
            const auto from = fixtures / side / name;
            const auto dir = Fresh(root, std::string("leftover-") + side + "-" + name);
            const auto card = dir / "sd.frondsd";
            WriteAll(card, Unsparse(ReadAll(from.string() + ".sparse")));
            for (const char* kind : {".files-intent", ".files-commit"})
                if (fs::exists(from.string() + kind)) fs::copy_file(from.string() + kind, card.string() + kind);
            assert(frondStorage::hasChanges(card));
            const auto settled = frondStorage::settle(card);
            assert(settled.ok);
            assert(!frondStorage::legacy::present(card));
            assert(GenerationOf(card) == ExpectedGeneration(from.string() + ".expect"));
            SecondSettleChangesNothing(dir, card);

            // The same leftovers settled by a session load of the machine at that generation, and
            // as a card put in (both through the core's own load functions).
            const auto settledBytes = ReadAll(card);
            for (int how = 0; how < 2; ++how) {
                const auto load = Fresh(root, std::string("leftover-load-") + side + "-" + name);
                WriteAll(load / "sd.frondsd", Unsparse(ReadAll(from.string() + ".sparse")));
                for (const char* kind : {".files-intent", ".files-commit"})
                    if (fs::exists(from.string() + kind))
                        fs::copy_file(from.string() + kind, (load / "sd.frondsd").string() + kind);
                frondMachine::Media media;
                if (how == 0) {
                    media.kind = frondMachine::kindSd;
                    media.name = frondMachine::sdSibling;
                    frondSdFile::Info info;
                    assert(frondSdFile::parseContainer(settledBytes.data(), info));
                    media.logicalSize = media.physicalSize = info.payloadBytes;
                    media.contentGeneration = ExpectedGeneration(from.string() + ".expect");
                    assert(frondSession::settleSdJournal(load, media));
                } else {
                    frondSession::CardSwap swap;
                    swap.identityGiven = true;
                    swap.identity[0] = 1;
                    assert(frondSession::swappedInCard(load, frondMachine::sdSibling, swap, media));
                    assert(media.contentGeneration == ExpectedGeneration(from.string() + ".expect"));
                }
                assert(ReadAll(load / "sd.frondsd") == settledBytes);
                assert(Snapshot(load).size() == 1);
            }
            fs::copy_file(card, out / (std::string(side) + "-" + name + ".frondsd"), fs::copy_options::overwrite_existing);
        }
}

// ---- Standalone changes, as Files… will make them ----

constexpr uint64_t cardBytes = 4u << 20;

// The steps of one standalone change to generation `to`, stopping after `stop` of them:
// 1 pending made, 2 chunks listed, 3 data written, 4 journal complete, 5 pages and table applied,
// 6 header applied, 7 pending gone, 8 journal gone.
struct Change {
    Bytes data;      // into chunks 1 and 2 (free space)
    Bytes metadata;  // one sector in chunk 0
    uint64_t dataAt = (1u << 20) + 4096, metadataAt = 8192;
};

static void StandaloneChange(const fs::path& card, uint64_t from, uint64_t to, const Change& c, int stop,
                             bool v1Card = false) {
    const auto stored = Payload(card).size();
    frondStorage::PendingWriter pending;
    if (stop < 1) return;
    assert(pending.open(card, stored, to));
    if (stop < 2) return;
    assert(pending.add({uint32_t(c.dataAt >> 20), uint32_t((c.dataAt + c.data.size() - 1) >> 20)}));
    if (stop < 3) return;
    {
        std::fstream f(card, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(std::streamoff(64 + c.dataAt));
        f.write(reinterpret_cast<const char*>(c.data.data()), std::streamsize(c.data.size()));
    }
    if (stop < 4) return;
    auto next = Payload(card);
    std::memcpy(next.data() + c.metadataAt, c.metadata.data(), c.metadata.size());
    frondSdFile::ChunkCrcs crcs;
    crcs.add(next.data(), next.size());
    frondStorage::JournalWriter writer;
    assert(writer.open(card, stored, from, to, frondStorage::JournalKind::standalone));
    assert(writer.add(c.metadataAt, uint32_t(c.metadata.size()), c.metadata.data()));
    fs::path journal;
    assert(writer.finish(crcs.table(), journal) && journal == frondStorage::journalPath(card, to));
    if (stop < 5) return;
    if (v1Card) fs::resize_file(card, 64 + stored + 4 * crcs.table().size());
    assert(frondStorage::applyJournal(card, journal, nullptr, frondStorage::JournalStep::pages));
    if (stop < 6) return;
    assert(frondStorage::applyJournal(card, journal, nullptr, frondStorage::JournalStep::header));
    if (stop < 7) return;
    fs::remove(pending.path());
    if (stop < 8) return;
    fs::remove(journal);
}

static void StandaloneCrashes(const fs::path& root) {
    const auto old = Random(cardBytes, 7);
    Change c;
    c.data = Random(1u << 20, 8);  // chunks 1 and 2
    c.metadata = Random(512, 9);
    Bytes withData = old;
    std::memcpy(withData.data() + c.dataAt, c.data.data(), c.data.size());
    Bytes done = withData;
    std::memcpy(done.data() + c.metadataAt, c.metadata.data(), c.metadata.size());

    for (int v1 = 0; v1 < 2; ++v1)
        for (int stop = 0; stop <= 8; ++stop) {
            const auto dir = Fresh(root, "standalone-" + std::to_string(v1) + "-" + std::to_string(stop));
            const auto card = dir / "card.frondsd";
            Bytes file;
            if (v1) assert(frondSdFile::encode(old, file));
            else assert(frondSdFile::encodeV2(old, 3, file));
            WriteAll(card, file);
            const uint64_t from = v1 ? 0 : 3, to = from + 1;
            StandaloneChange(card, from, to, c, stop, v1);
            const auto crashed = Snapshot(dir);
            // A session load of a machine whose .frond is at `from` settles the change first (it moves
            // the card on, so that load is refused); one at `to` takes it.
            for (int how = 0; how < 3; ++how) {
                Restore(dir, crashed);
                frondStorage::Result r;
                const uint64_t want = how == 1 ? from : to;
                if (how == 0) r = frondStorage::settle(card);
                else r = frondStorage::settle(card, want, cardBytes);
                const bool changed = stop >= 1;
                if (how == 1 && changed) {
                    assert(!r.ok && r.code == "unsettled");
                    assert(!frondStorage::hasChanges(card));
                    continue;
                }
                if (how == 2 && !changed) {
                    assert(!r.ok);
                    continue;
                }
                assert(r.ok);
                assert(Snapshot(dir).size() == 1);
                const auto payload = Payload(card);
                if (stop == 0) assert(payload == old && GenerationOf(card) == from);
                else if (stop < 3) assert(payload == old && GenerationOf(card) == to);
                else if (stop < 4) assert(payload == withData && GenerationOf(card) == to);
                else assert(payload == done && GenerationOf(card) == to);
                if (how == 0) SecondSettleChangesNothing(dir, card);
                else SecondSettleChangesNothing(dir, card, &want, cardBytes);
            }
        }

    // A pending list cut off in its header: nothing was written yet.
    {
        const auto dir = Fresh(root, "pending-short");
        const auto card = dir / "card.frondsd";
        Bytes file;
        assert(frondSdFile::encodeV2(old, 3, file));
        WriteAll(card, file);
        WriteAll(frondStorage::pendingPath(card, 4), {'F', 'R', 'O', 'N'});
        assert(frondStorage::settle(card).ok && ReadAll(card) == file && Snapshot(dir).size() == 1);
    }

    // A standalone journal that doesn't fit the card is refused and nothing changes.
    {
        const auto dir = Fresh(root, "standalone-mismatch");
        const auto card = dir / "card.frondsd";
        Bytes file;
        assert(frondSdFile::encodeV2(old, 3, file));
        WriteAll(card, file);
        StandaloneChange(card, 3, 4, c, 4);
        Bytes later;
        assert(frondSdFile::encodeV2(old, 9, later));
        WriteAll(card, later);
        fs::remove(frondStorage::pendingPath(card, 4));
        const auto before = Snapshot(dir);
        const auto r = frondStorage::settle(card);
        assert(!r.ok && r.code == "mismatch" && Snapshot(dir) == before);
    }

    // A card open in Files… (locked) isn't touched.
    {
        const auto dir = Fresh(root, "standalone-locked");
        const auto card = dir / "card.frondsd";
        Bytes file;
        assert(frondSdFile::encodeV2(old, 3, file));
        WriteAll(card, file);
        StandaloneChange(card, 3, 4, c, 3);
        const int held = ::open(card.c_str(), O_RDWR | O_CLOEXEC);
        assert(held >= 0 && frondStorage::tryLock(held));
        const auto before = Snapshot(dir);
        const auto r = frondStorage::settle(card);
        assert(!r.ok && r.code == "busy" && Snapshot(dir) == before);
        ::close(held);
        assert(frondStorage::settle(card).ok && GenerationOf(card) == 4);
    }
}

// ---- Session journals, v1 as saves write them and v2 with the session flag ----

// A v1 journal rewritten as v2 with flags 0, its trailer CRC redone.
// A session journal as version 1, as saves before journal v2 wrote it.
static Bytes AsV1Session(Bytes journal) {
    frondSdFile::put32(journal.data() + 8, 1);
    const size_t body = journal.size() - 16;
    frondSdFile::put32(journal.data() + body + 8, mz_crc32(0, journal.data(), body));
    return journal;
}

static void SessionCrashes(const fs::path& root) {
    const auto old = Random(cardBytes, 11);
    auto next = old;
    const auto patch = Random(8192, 12);
    std::memcpy(next.data() + (2u << 20) - 4096, patch.data(), patch.size());
    Bytes oldFile, newFile;
    assert(frondSdFile::encodeV2(old, 5, oldFile));
    assert(frondSdFile::encodeV2(next, 6, newFile));
    frondSdFile::ChunkCrcs crcs;
    crcs.add(next.data(), next.size());

    for (int v2 = 0; v2 < 2; ++v2)
        for (int stop = 0; stop <= 4; ++stop) {
            // 0 nothing, 1 journal complete, 2 pages, 3 header, 4 journal gone.
            const auto dir = Fresh(root, "session-" + std::to_string(v2) + "-" + std::to_string(stop));
            const auto card = dir / frondMachine::sdSibling;
            WriteAll(card, oldFile);
            if (stop >= 1) {
                frondSession::SdJournalWriter writer;
                assert(writer.open(dir, cardBytes, 5, 6));
                assert(writer.add((2u << 20) - 4096, uint32_t(patch.size()), patch.data()));
                fs::path journal;
                assert(writer.finish(crcs.table(), journal));
                frondStorage::Journal parsed;
                assert(frondStorage::readJournal(journal, parsed) && parsed.version == 2 && !parsed.standalone);
                if (!v2) {
                    WriteAll(journal, AsV1Session(ReadAll(journal)));
                    assert(frondStorage::readJournal(journal, parsed) && parsed.version == 1 && !parsed.standalone);
                }
                if (stop >= 2) assert(frondSession::applySdJournal(dir, journal, nullptr, frondSession::JournalStep::pages));
                if (stop >= 3) assert(frondSession::applySdJournal(dir, journal, nullptr, frondSession::JournalStep::header));
                if (stop >= 4) assert(frondSession::applySdJournal(dir, journal));
            }
            const auto crashed = Snapshot(dir);
            frondMachine::Media media;
            media.kind = frondMachine::kindSd;
            media.name = frondMachine::sdSibling;
            media.logicalSize = media.physicalSize = cardBytes;

            // Outside a machine a session journal refuses and nothing changes.
            if (stop >= 1 && stop <= 3) {
                const auto r = frondStorage::settle(card);
                assert(!r.ok && r.code == "unsettled" && Snapshot(dir) == crashed);
            }
            // The .frond at 6 (published) takes the save. One still at 5 (the save stopped before
            // publishing it, so before any of the journal went in) drops the journal.
            for (uint64_t want : {uint64_t(5), uint64_t(6)}) {
                if (want == 5 && stop > 1) continue;
                Restore(dir, crashed);
                media.contentGeneration = want;
                const bool ok = frondSession::settleSdJournal(dir, media);
                assert(ok == (want == 5 || stop >= 1));
                if (!ok) {
                    assert(Snapshot(dir) == crashed);
                    continue;
                }
                assert(Snapshot(dir).size() == 1);
                assert(ReadAll(card) == (want == 6 ? newFile : oldFile));
                SecondSettleChangesNothing(dir, card, &want, cardBytes);
            }
        }
}

// ---- Side regions ----

static Bytes WithSide(const Bytes& payload, const Bytes& side, uint64_t generation) {
    const uint64_t stored = frondSdFile::storedBytes(payload.size(), side.size());
    Bytes body(size_t(stored), 0);
    std::memcpy(body.data(), payload.data(), payload.size());
    std::memcpy(body.data() + frondSdFile::sideOffset(payload.size()), side.data(), side.size());
    frondSdFile::ChunkCrcs crcs;
    crcs.add(body.data(), body.size());
    Bytes file(64);
    frondSdFile::makeHeaderV2(file.data(), payload.size(), generation, crcs.table(), side.size());
    file.insert(file.end(), body.begin(), body.end());
    Bytes table;
    frondSdFile::tableBytes(crcs.table(), table);
    file.insert(file.end(), table.begin(), table.end());
    return file;
}

static void SideRegions(const fs::path& root) {
    const auto payload = Random((3u << 20) + 4096, 21);  // not on a chunk boundary
    const auto side = Random(70000, 22);
    const auto file = WithSide(payload, side, 2);
    const auto dir = Fresh(root, "side");
    const auto card = dir / "card.frondsd";
    WriteAll(card, file);

    frondSdFile::Info info;
    assert(frondSdFile::parseContainer(file.data(), info) && info.sideBytes == side.size());
    assert(info.stored() == (4u << 20) + side.size() && info.chunks() == 5);
    assert(!frondSdFile::parseHeader(file.data(), info));  // not an SD or Pro card
    assert(frondStorage::verify(card, info).ok && info.generation == 2);

    // Every chunk is covered, the padding and side region included.
    for (uint64_t at : {uint64_t(64 + payload.size() + 10), uint64_t(64 + (4u << 20) + 69999)}) {
        auto bad = file;
        bad[size_t(at)] ^= 1;
        WriteAll(card, bad);
        assert(!frondStorage::verify(card, info).ok);
    }
    // Header bytes 56-63 stay zero.
    auto bad = file;
    bad[60] = 1;
    assert(!frondSdFile::parseContainer(bad.data(), info));

    // A standalone change into the side region, cut off before its journal: the pending list
    // fixes the side chunk's CRC.
    WriteAll(card, file);
    const uint64_t sideAt = frondSdFile::sideOffset(payload.size());
    {
        frondStorage::PendingWriter pending;
        assert(pending.open(card, info.stored(), 3));
        assert(pending.add({uint32_t(sideAt >> 20)}));
        std::fstream f(card, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(std::streamoff(64 + sideAt + 100));
        f.write("side", 4);
    }
    assert(frondStorage::settle(card).ok && GenerationOf(card) == 3);
    auto now = Payload(card);
    assert(!std::memcmp(now.data() + sideAt + 100, "side", 4));
    SecondSettleChangesNothing(dir, card);

    // And one with its journal: the replay keeps the side region size in the header.
    auto next = now;
    std::memcpy(next.data() + sideAt + 200, "more", 4);
    frondSdFile::ChunkCrcs crcs;
    crcs.add(next.data(), next.size());
    frondStorage::JournalWriter writer;
    assert(writer.open(card, next.size(), 3, 4, frondStorage::JournalKind::standalone));
    assert(writer.add(sideAt + 200, 4, reinterpret_cast<const uint8_t*>("more")));
    fs::path journal;
    assert(writer.finish(crcs.table(), journal));
    assert(frondStorage::settle(card).ok && GenerationOf(card) == 4 && Payload(card) == next);
    assert(frondStorage::verify(card, info).ok && info.sideBytes == side.size());
    SecondSettleChangesNothing(dir, card);
}

// ---- Cards with nothing beside them ----

static void NothingToDo(const fs::path& root) {
    const auto dir = Fresh(root, "clean");
    const auto card = dir / "card.frondsd";
    Bytes file;
    assert(frondSdFile::encodeV2(Random(cardBytes, 31), 8, file));
    WriteAll(card, file);
    SecondSettleChangesNothing(dir, card);
    const uint64_t want = 8;
    SecondSettleChangesNothing(dir, card, &want, cardBytes);
    assert(frondSdFile::encode(Random(cardBytes, 32), file));
    WriteAll(card, file);
    SecondSettleChangesNothing(dir, card);
    frondSdFile::Info info;
    assert(frondStorage::verify(card, info).ok && info.version == 1);
}

// A standalone journal's temp left by a crash goes at the next settle; one a CardFile is writing
// now (it holds the card's lock) stays. Session save temps are never touched.
static void EditTemps(const fs::path& root) {
    const auto dir = Fresh(root, "edit-temps");
    const auto card = dir / "card.frondsd";
    Bytes file;
    assert(frondSdFile::encodeV2(Random(cardBytes, 41), 4, file));
    WriteAll(card, file);
    const auto stale = dir / (frondStorage::editTempPrefix(card) + "abc123");
    const auto session = dir / ".frondsd.tmp-xyz789";
    const auto other = dir / (frondStorage::editTempPrefix(dir / "other.frondsd") + "def456");
    for (const auto& p : {stale, session, other}) WriteAll(p, Random(100, 42));
    assert(frondStorage::hasChanges(card));
    {
        frondStorage::CardFile open(card, true);
        assert(!fs::exists(stale));
        WriteAll(stale, Random(100, 43));
        assert(frondStorage::settleChanges(card).code == "busy");
        assert(fs::exists(stale));
        assert(frondStorage::readiness(card).code == "busy");
    }
    assert(frondStorage::settle(card).ok && !fs::exists(stale) && fs::exists(session) && fs::exists(other));
    assert(!frondStorage::hasChanges(card));
    assert(frondStorage::readiness(card).ok);
}

// A standalone journal that doesn't match its card is refused with its own message, untouched.
static void MismatchedJournal(const fs::path& root) {
    const auto dir = Fresh(root, "mismatch");
    const auto card = dir / "card.frondsd";
    const auto payload = Random(cardBytes, 51);
    Bytes file;
    assert(frondSdFile::encodeV2(payload, 9, file));
    WriteAll(card, file);
    frondSdFile::ChunkCrcs crcs;
    crcs.add(payload.data(), payload.size());
    frondStorage::JournalWriter writer;
    assert(writer.open(card, cardBytes, 3, 4, frondStorage::JournalKind::standalone));
    fs::path journal;
    assert(writer.finish(crcs.table(), journal));
    const auto before = Snapshot(dir);
    const auto r = frondStorage::readiness(card);
    assert(!r.ok && r.code == "mismatch" && r.message == frondStorage::mismatchMessage);
    assert(Snapshot(dir) == before);
    try {
        frondStorage::CardFile open(card, true);
        assert(false);
    } catch (const frondStorage::Error& e) {
        assert(e.message == frondStorage::mismatchMessage);
    }
}

int main(int argc, char** argv) {
    assert(argc == 3);
    const fs::path fixtures = argv[1], out = argv[2];
    const auto root = fs::path("build") / "storage-settle";
    fs::remove_all(root);
    fs::create_directories(root);
    LeftoversFromEitherApp(root, fixtures, out);
    StandaloneCrashes(root);
    SessionCrashes(root);
    SideRegions(root);
    NothingToDo(root);
    EditTemps(root);
    MismatchedJournal(root);
    fs::remove_all(root);
    std::puts("storage settle: ok");
    return 0;
}
