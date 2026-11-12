#ifndef FROND_STORAGE_CARD_FILE_H
#define FROND_STORAGE_CARD_FILE_H

#include "storage/ClassicStick.h"
#include "storage/Container.h"
#include "storage/Errors.h"
#include "storage/Io.h"
#include "storage/Journal.h"
#include "storage/Pending.h"
#include "storage/Settle.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// A card file (sd.frondsd, ms.frondmsp, ms.frondms) edited in place for one Files… visit, and the
// Blocks a FatImage reads and writes through. Offsets are payload bytes from 0, whole sectors for
// metadata. A classic stick (ms.frondms) is its logical sectors as storage/ClassicStick.h has them:
// only a settled one opens, an L nobody holds reads as zeros and gets a block before its first
// write, and a blank one is formatted the TG50's way when a FatImage first opens it.
//
// Crash safety, per change. 1: before data goes into a chunk, its number is appended to
// <card>.journal-N.pending and fsynced; the data only goes into clusters the card's FAT calls free.
// 2: the data is fsynced, then the FAT and folder sectors and the new chunk table go into a
// standalone journal <card>.journal-N (a temp, fsynced, renamed). 3: they're written in place with
// the header and fsynced, then both files go. A cut-off change is settled by Settle.h: a journal is
// replayed (the change is done); a pending list alone gets its chunks' CRCs and the header
// rewritten (the change isn't done, only free clusters changed). Either way the card loads at N.
//
// One CardFile is one visit: N is the generation at open plus 1, for every change it writes and
// for a settle after a crash. The card file holds an open file description lock (Io.h tryLock,
// which also conflicts with Android's FileChannel lock) while open. Not thread safe.
namespace frondStorage {

constexpr const char* inUseMessage = "The card is open somewhere else. Close it there first.";

using Sectors = std::map<int64_t, std::vector<uint8_t>>;

// Where a FatImage's bytes live.
class Blocks {
   public:
    virtual ~Blocks() = default;
    virtual int64_t length() const = 0;
    virtual void read(int64_t at, char* b, int64_t n) = 0;
    // New data, only into clusters the FAT on the card calls free.
    virtual void write(int64_t at, const char* b, int64_t n) = 0;
    virtual void sync() = 0;
    // Commits one change's metadata, whole sectors by offset.
    virtual void commit(const Sectors& sectors) = 0;
    // A change failed after writing data: the card is made sound again now.
    virtual void settleData() {}
    // Called when a FatImage opens the card, before it reads anything.
    virtual void prepare() {}
    // A classic stick formats itself as the TG50 does (FatImage::format); false for any other card.
    virtual bool formatStick() { return false; }
};

// Whether a cut-off change is waiting beside the card file.
inline bool unsettled(const std::filesystem::path& card) { return hasChanges(card); }

// Whether a card file is open in a CardFile now (this process or another).
inline bool inUse(const std::filesystem::path& card) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(card, ec)) return false;
    const int fd = ::open(card.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    Fd closer(fd);
    return !tryLock(fd);
}

// Finishes a cut-off change, if there is one. False when the card is open in a CardFile now or
// can't be settled; true when there was nothing to do or it's done.
inline bool settleCard(const std::filesystem::path& card) { return settleChanges(card).ok; }

// A card file nobody has open in Files…, with any change a crash cut off finished.
inline bool ready(const std::filesystem::path& card) { return !inUse(card) && settleCard(card); }

// As ready(), with why not: code "busy" and inFilesMessage when it's open in Files.
inline Result readiness(const std::filesystem::path& card) {
    if (inUse(card)) return failed("busy", inFilesMessage);
    return settleChanges(card);
}

// What a card load checks: the header and every CRC. The generation (0 for v1), or nothing.
inline std::optional<uint64_t> verifiedGeneration(const std::filesystem::path& card) {
    frondSdFile::Info info;
    if (!verify(card, info).ok) return std::nullopt;
    return info.version == frondSdFile::versionV2 ? info.generation : 0;
}

class CardFile : public Blocks {
   public:
    // Opens a card file for a visit, settling a cut-off change first. Throws Error(inUseMessage)
    // when another CardFile (this process or another) has it.
    CardFile(const std::filesystem::path& card, bool canWrite)
        : path(card), writable(canWrite), fd_(::open(card.c_str(), O_RDWR | O_CLOEXEC)) {
        if (fd_.fd < 0) throw Error("Couldn't open the card file.");
        if (!tryLock(fd_.fd)) throw Error(inUseMessage);
        if (const auto settled = settleChangesLocked(card, fd_.fd); !settled.ok) throw Error(settled.message);
        load();
    }
    static std::unique_ptr<CardFile> open(const std::filesystem::path& card, bool canWrite) {
        return std::make_unique<CardFile>(card, canWrite);
    }
    ~CardFile() override {
        try {
            if (!dead) settleData();
        } catch (...) {
        }
    }
    CardFile(const CardFile&) = delete;
    CardFile& operator=(const CardFile&) = delete;

    const std::filesystem::path path;
    const bool writable;
    int64_t payloadBytes() const { return payload_; }
    uint64_t generation() const { return gen_; }

    // Called once, before the first byte this visit writes, e.g. to mark the machine's slot
    // swapped. It may throw to stop the change.
    std::function<void()> beforeFirstWrite;
    // Bytes written into the card file, the write calls (file offset, length), and journal bytes.
    int64_t cardBytesWritten{0}, journalBytesWritten{0};
    std::vector<std::pair<int64_t, int64_t>> writes;
    // Tests: called at "claimed" (a classic stick's new blocks are in), "writing" (after the first
    // data of a change), "data", "journal", "partly", "applied". After a simulated crash (`dead`)
    // nothing more reaches the disk.
    std::function<void(const char*)> crashHook;
    bool dead{false};

    int64_t length() const override { return payload_; }
    void read(int64_t at, char* b, int64_t n) override {
        if (at < 0 || n < 0 || at + n > payload_) throw Error("The card is shorter than its file system says.");
        if (!classic_) {
            if (!readAllAt(fd_.fd, b, size_t(n), uint64_t(frondSdFile::headerBytes + at)))
                throw Error("Couldn't read the card.");
            return;
        }
        // Block by block: an L nobody holds reads as zeros.
        const int64_t blockBytes = int64_t(stick_.layout.BlockBytes());
        while (n > 0) {
            const int64_t logical = at / blockBytes;
            const int64_t m = std::min(n, (logical + 1) * blockBytes - at);
            if (stick_.holder[size_t(logical)] < 0) std::memset(b, 0, size_t(m));
            else if (!readAllAt(fd_.fd, b, size_t(m), uint64_t(frondSdFile::headerBytes + at)))
                throw Error("Couldn't read the card.");
            at += m;
            b += m;
            n -= m;
        }
    }
    // Whether this is a classic stick with nothing on it yet: no boot sector or partition table in
    // sector 0, and every block that holds an L all zeros or all 0xff. `frond --card make ms` and a
    // format cut off before its commit leave zeros; a TG50 mount of an unformatted stick gives every
    // L an erased (0xff) block. A formatted stick has its FATs and root folder in held blocks that
    // are neither, so a damaged one isn't taken as blank.
    bool blankStick() {
        if (!classic_ || written_) return false;
        std::vector<char> block(size_t(stick_.layout.BlockBytes()));
        read(0, block.data(), 512);
        if (uint8_t(block[510]) == 0x55 && uint8_t(block[511]) == 0xaa) return !(written_ = true);
        for (uint32_t L = 0; L < stick_.layout.UserBlocks(); ++L) {
            if (stick_.holder[L] < 0) continue;
            read(int64_t(L) * int64_t(block.size()), block.data(), int64_t(block.size()));
            const char first = block[0];
            if ((first != 0 && first != char(0xff)) ||
                std::any_of(block.begin(), block.end(), [first](char c) { return c != first; }))
                return !(written_ = true);
        }
        return true;
    }
    // A blank classic stick gets the file system the TG50's driver writes for its size.
    void prepare() override {
        if (!blankStick()) return;
        if (!writable) throw Error(blankStickMessage);
        formatStick();
    }
    bool formatStick() override {
        if (!classic_) return false;
        if (!writable) throw Error("The card is write protected.");
        if (!stick_.bootOk) throw Error(stickBootMessage);
        commit(classicStick::formatSectors(stick_.layout, stick_.made));
        return true;
    }
    void write(int64_t at, const char* b, int64_t n) override {
        if (!writable) throw Error("The card is write protected.");
        if (at < 0 || n < 0 || at + n > payload_) throw Error("The card is shorter than its file system says.");
        if (dead || n == 0) return;
        start();
        const bool first = dirty_.empty();
        claimRange(at, n);
        if (dead) return;
        noteRange(at, n);
        put(int64_t(frondSdFile::headerBytes) + at, reinterpret_cast<const uint8_t*>(b), n);
        if (first) hook("writing");
    }
    void sync() override {
        if (!dead && fsync(fd_.fd) != 0) throw Error("Couldn't write the card.");
    }

    // Commits one change: `sectors` are whole card sectors by offset (FAT and folders). With no
    // sectors, fixes the CRCs of chunks that got data without a commit (a failed change).
    void commit(const Sectors& sectors) override {
        if (!writable) throw Error("The card is write protected.");
        if (dead) return;
        if (sectors.empty() && dirty_.empty()) return;
        start();
        for (const auto& s : sectors) claimRange(s.first, int64_t(s.second.size()));
        if (dead) return;
        if (fsync(fd_.fd) != 0) throw Error("Couldn't write the card.");
        hook("data");
        if (dead) return;
        std::set<uint32_t> touched(dirty_.begin(), dirty_.end());
        for (const auto& s : sectors) {
            touched.insert(uint32_t(chunkOf(s.first)));
            touched.insert(uint32_t(chunkOf(s.first + int64_t(s.second.size()) - 1)));
        }
        const auto count = size_t(frondSdFile::chunkCount(stored_));
        std::vector<uint32_t> next = table_.empty() ? std::vector<uint32_t>(count, 0) : table_;
        auto recompute = [&](size_t i) {
            if (!chunkCrc(i, sectors, next[i])) throw Error("Couldn't read the card.");
        };
        if (table_.empty())
            for (size_t i = 0; i < count; ++i) recompute(i);
        else
            for (auto i : touched) recompute(i);

        JournalWriter journal;
        std::filesystem::path journalFile;
        bool written = journal.open(path, stored_, openGen_, target_, JournalKind::standalone);
        int64_t journalBytes = int64_t(journalHeaderBytes + journalTrailerBytes + 4 * count);
        for (auto it = sectors.begin(); written && it != sectors.end(); ++it) {
            written = journal.add(uint64_t(it->first), uint32_t(it->second.size()), it->second.data());
            journalBytes += int64_t(journalRecordBytes + it->second.size());
        }
        if (!written || !journal.finish(next, journalFile)) throw Error("Couldn't write the card's journal.");
        journalBytesWritten += journalBytes;
        hook("journal");
        if (dead) return;

        // In place: the sectors, the changed table entries (all of them for a v1 card), the header.
        std::vector<std::pair<int64_t, std::vector<uint8_t>>> records;
        for (const auto& [at, sector] : sectors) records.emplace_back(int64_t(frondSdFile::headerBytes) + at, sector);
        const int64_t tableAt = int64_t(frondSdFile::headerBytes + stored_);
        auto entries = [&](size_t from, size_t until) {
            std::vector<uint8_t> b(4 * (until - from));
            for (size_t i = from; i < until; ++i) frondSdFile::put32(b.data() + 4 * (i - from), next[i]);
            return b;
        };
        if (table_.empty()) {
            records.emplace_back(tableAt, entries(0, count));
        } else {
            for (size_t i = 0; i < count;) {
                if (next[i] == table_[i]) {
                    ++i;
                    continue;
                }
                size_t j = i;
                while (j < count && next[j] != table_[j]) ++j;
                records.emplace_back(tableAt + 4 * int64_t(i), entries(i, j));
                i = j;
            }
        }
        std::vector<uint8_t> header(frondSdFile::headerBytes);
        frondSdFile::makeHeaderV2(header.data(), uint64_t(payload_), target_, next, side_, classic_);
        records.emplace_back(0, header);
        const int64_t finalLength = tableAt + 4 * int64_t(count);
        if (sizeOf(fd_.fd) < finalLength && ftruncate(fd_.fd, off_t(finalLength)) != 0)
            throw Error("Couldn't write the card.");
        for (size_t k = 0; k < records.size(); ++k) {
            if (k == records.size() / 2) {
                hook("partly");
                if (dead) return;
            }
            put(records[k].first, records[k].second.data(), int64_t(records[k].second.size()));
        }
        if (fsync(fd_.fd) != 0) throw Error("Couldn't write the card.");
        hook("applied");
        if (dead) return;
        removePath(journalFile);
        pending_.reset();
        removePath(pendingPath(path, target_));
        syncDir(folderOf(path));
        table_ = next;
        version_ = frondSdFile::versionV2;
        gen_ = target_;
        dirty_.clear();
    }

    // A change that failed after writing data: the chunk CRCs are fixed now so the card loads.
    void settleData() override {
        if (!dirty_.empty() && !dead) commit({});
    }

   private:
    static int64_t chunkOf(int64_t at) { return at / int64_t(frondSdFile::chunkBytes); }

    void load() {
        uint8_t head[frondSdFile::headerBytes];
        frondSdFile::Info info;
        // Internal NAND is never edited from outside (its NVFS sits behind the guest's FTL).
        if (!readAllAt(fd_.fd, head, sizeof(head), 0) || !frondSdFile::parseContainer(head, info) || info.nand ||
            sizeOf(fd_.fd) != int64_t(info.fileBytes()))
            throw Error(damagedMessage);
        if (info.version == frondSdFile::versionV2) {
            std::vector<uint8_t> raw(static_cast<size_t>(4 * info.chunks()));
            std::vector<uint32_t> table;
            if (!readAllAt(fd_.fd, raw.data(), raw.size(), frondSdFile::headerBytes + info.stored()) ||
                !frondSdFile::readTable(head, raw.data(), raw.size(), info, table))
                throw Error(damagedMessage);
            table_ = std::move(table);
        } else {
            table_.clear();
        }
        if (info.classic) {
            const auto* layout = frondMsLayout::of(info);
            if (!layout || !classicStick::load(*layout, [&](uint64_t off, uint64_t n, void* dest) {
                    return readAllAt(fd_.fd, dest, size_t(n), frondSdFile::headerBytes + off);
                }, stick_))
                throw Error(damagedMessage);
            if (!stick_.settled) throw Error(unsettledStickMessage);
        }
        version_ = info.version;
        payload_ = int64_t(info.payloadBytes);
        side_ = info.sideBytes;
        classic_ = info.classic;
        stored_ = info.stored();
        gen_ = openGen_ = info.version == frondSdFile::versionV2 ? info.generation : 0;
        target_ = gen_ + 1;
    }
    void hook(const char* step) {
        if (crashHook) crashHook(step);
    }
    void start() {
        if (started_) return;
        if (beforeFirstWrite) beforeFirstWrite();
        started_ = true;
    }
    // Adds chunk numbers to the pending list, making it first with the card's size and the visit's
    // generation. On disk before the data is written.
    void note(const std::vector<uint32_t>& list) {
        if (!pending_) {
            auto writer = std::make_unique<PendingWriter>();
            if (!writer->open(path, stored_, target_)) throw Error("Couldn't write the card's journal.");
            pending_ = std::move(writer);
            journalBytesWritten += int64_t(pendingHeaderBytes);
        }
        if (!pending_->add(list)) throw Error("Couldn't write the card's journal.");
        journalBytesWritten += int64_t(4 * list.size());
    }
    // Adds the chunks of stored bytes [at, at + n) that have no data yet to the pending list.
    // Whether any were new.
    bool noteRange(int64_t at, int64_t n) {
        std::vector<uint32_t> fresh;
        for (int64_t i = chunkOf(at); i <= chunkOf(at + n - 1); ++i)
            if (!dirty_.count(uint32_t(i))) fresh.push_back(uint32_t(i));
        if (fresh.empty()) return false;
        note(fresh);
        dirty_.insert(fresh.begin(), fresh.end());
        return true;
    }
    // Stored bytes outside the file system's own sectors, noted first as data.
    void putStored(uint64_t at, const uint8_t* b, uint64_t n) {
        noteRange(int64_t(at), int64_t(n));
        put(int64_t(frondSdFile::headerBytes + at), b, int64_t(n));
    }
    // Gives every L in payload bytes [at, at + n) that nobody holds a block of its own, with zero
    // data. In steps with an fsync between, so a crash leaves each L unheld or held with zeros,
    // which read the same: a borrower's copy, its new map entry, then L's zeros and map entries,
    // then the extra bytes that make the claim.
    void claimRange(int64_t at, int64_t n) {
        if (!classic_ || n <= 0) return;
        const auto& l = stick_.layout;
        const int64_t blockBytes = int64_t(l.BlockBytes());
        std::vector<classicStick::Claim> claims;
        for (int64_t L = at / blockBytes; L <= (at + n - 1) / blockBytes; ++L)
            if (stick_.holder[size_t(L)] < 0) claims.push_back({uint32_t(L)});
        if (claims.empty()) return;
        try {
            for (auto& c : claims) {
                c = classicStick::plan(stick_, c.logical);
                classicStick::apply(stick_, c);
            }
            writeClaims(claims);
        } catch (...) {
            // What is on the disk is a state a crash could leave, so it's read again.
            classicStick::State again;
            if (classicStick::load(stick_.layout, [&](uint64_t off, uint64_t len, void* dest) {
                    return readAllAt(fd_.fd, dest, size_t(len), frondSdFile::headerBytes + off);
                }, again))
                stick_ = std::move(again);
            throw;
        }
        hook("claimed");
    }
    void writeClaims(const std::vector<classicStick::Claim>& claims) {
        const auto& l = stick_.layout;
        auto sync = [&] {
            if (fsync(fd_.fd) != 0) throw Error("Couldn't write the card.");
        };
        auto mapEntry = [&](uint32_t block, uint32_t entry) {
            uint8_t raw[4];
            frondSdFile::put32(raw, entry);
            putStored(l.MapAt() + 4ull * block, raw, 4);
        };
        std::vector<uint8_t> data(size_t(l.BlockBytes()));
        bool borrowers = false;
        for (const auto& c : claims) {
            if (c.borrower < 0) continue;
            borrowers = true;
            if (!readAllAt(fd_.fd, data.data(), data.size(), frondSdFile::headerBytes + l.SlotAt(c.logical)))
                throw Error("Couldn't read the card.");
            putStored(l.SlotAt(c.borrowerSlot), data.data(), data.size());
        }
        if (borrowers) {
            sync();
            for (const auto& c : claims)
                if (c.borrower >= 0) mapEntry(uint32_t(c.borrower), stick_.entryOf(c.borrowerSlot));
            sync();
        }
        std::fill(data.begin(), data.end(), 0);
        for (const auto& c : claims) {
            putStored(l.SlotAt(c.logical), data.data(), data.size());
            mapEntry(c.block, c.logical);
        }
        sync();
        for (const auto& c : claims) {
            const auto extras = classicStick::newBlockExtras(l, c.logical);
            putStored(l.ExtraAt(c.block, 0), extras.data(), extras.size());
        }
        sync();
    }
    void put(int64_t at, const uint8_t* b, int64_t n) {
        if (!writeAllAt(fd_.fd, b, size_t(n), uint64_t(at))) throw Error("Couldn't write the card.");
        cardBytesWritten += n;
        writes.emplace_back(at, n);
    }
    // CRC-32 of chunk i of the stored bytes as it will be, with `over` laid on top.
    bool chunkCrc(size_t i, const Sectors& over, uint32_t& out) {
        const int64_t start = int64_t(i) * int64_t(frondSdFile::chunkBytes);
        const int64_t n = std::min<int64_t>(frondSdFile::chunkBytes, int64_t(stored_) - start);
        std::vector<uint8_t> buf(static_cast<size_t>(n));
        if (!readAllAt(fd_.fd, buf.data(), buf.size(), uint64_t(frondSdFile::headerBytes + start))) return false;
        auto it = over.lower_bound(start - int64_t(frondSdFile::sectorBytes) + 1);
        for (; it != over.end() && it->first < start + n; ++it) {
            const int64_t from = std::max(it->first, start);
            const int64_t until = std::min(it->first + int64_t(it->second.size()), start + n);
            if (until > from) std::memcpy(buf.data() + (from - start), it->second.data() + (from - it->first), size_t(until - from));
        }
        out = frondSdFile::crc32(buf.data(), buf.size());
        return true;
    }

    Fd fd_;
    std::unique_ptr<PendingWriter> pending_;
    uint32_t version_{0};
    int64_t payload_{0};
    uint64_t side_{0}, stored_{0};
    bool classic_{};
    classicStick::State stick_;
    bool written_{false};  // the stick is known not to be blank
    uint64_t gen_{0}, openGen_{0}, target_{0};
    // Chunk CRCs as on the card; empty for a v1 card until its first change.
    std::vector<uint32_t> table_;
    // Chunks that got data since the last commit, all in the pending list.
    std::set<uint32_t> dirty_;
    bool started_{false};
};

// A raw card image: writes go straight in, and a commit writes its sectors and syncs. The caller
// keeps an earlier copy until a change is checked.
class RawBlocks : public Blocks {
   public:
    RawBlocks(const std::filesystem::path& image, bool canWrite)
        : writable_(canWrite), fd_(::open(image.c_str(), (canWrite ? O_RDWR : O_RDONLY) | O_CLOEXEC)) {
        if (fd_.fd < 0) throw Error("Couldn't open the card image.");
        size_ = sizeOf(fd_.fd);
    }
    int64_t length() const override { return size_; }
    void read(int64_t at, char* b, int64_t n) override {
        if (at < 0 || n < 0 || at + n > size_ || !readAllAt(fd_.fd, b, size_t(n), uint64_t(at)))
            throw Error("The card is shorter than its file system says.");
    }
    void write(int64_t at, const char* b, int64_t n) override {
        if (!writable_) throw Error("The card is write protected.");
        if (at < 0 || n < 0 || at + n > size_ || !writeAllAt(fd_.fd, b, size_t(n), uint64_t(at)))
            throw Error("Couldn't write the card image.");
    }
    void sync() override { fsync(fd_.fd); }
    void commit(const Sectors& sectors) override {
        for (const auto& [at, sector] : sectors)
            write(at, reinterpret_cast<const char*>(sector.data()), int64_t(sector.size()));
        sync();
    }

   private:
    bool writable_;
    Fd fd_;
    int64_t size_{0};
};

}  // namespace frondStorage

#endif
