#ifndef FROND_STORAGE_FAT_IMAGE_H
#define FROND_STORAGE_FAT_IMAGE_H

#include "storage/CardFile.h"
#include "storage/Errors.h"
#include "storage/FatNames.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Reads and edits a FAT12 or FAT16 card in place: one primary FAT partition (as the core and
// cameras make cards) or a superfloppy, up to 2 GiB. Lists folders, copies files in and out, makes
// folders, and copies, moves, renames and deletes inside the card; format() makes a fresh one.
// FAT32 is refused. New data only goes into free clusters; the FAT and folder sectors a change
// touches are kept in memory and committed together by the Blocks (CardFile: crash safe with the
// chunk CRCs and header). A change that fails rolls back to the FAT on the card. Not thread safe;
// one editor per card. Names and paths are UTF-8, "/" or "/PALM/Launcher".
namespace frondStorage {

struct Entry {
    std::string name, shortName;
    bool directory{false};
    int64_t size{0};
    uint32_t firstCluster{0};
    fat::Stamp modified;  // date 0 when the card has none
};

// A fresh file system's layout, as the SD Association's file system rules give it for a card
// of that size (the same table Memory Stick follows): FAT12 to 64 MiB with 8 KiB clusters to
// 8 MiB and 16 KiB above, FAT16 above 64 MiB; 1 reserved sector, 2 FATs, 512 root entries; the
// partition starts at least one boundary unit in and the data area starts on a boundary unit.
struct FatLayout {
    uint32_t bits{}, start{}, sectors{}, clusterSectors{}, boundary{}, fatSectors{}, clusters{};
    uint16_t heads{}, trackSectors{};
    uint8_t partitionType{};
    static constexpr uint32_t reserved = 1, rootEntries = 512, rootSectors = 32;
};

inline FatLayout fatLayout(int64_t bytes) {
    if (bytes < (int64_t(1) << 20) || bytes > (int64_t(2048) << 20) || bytes % 512)
        throw Error("Use a card of 1 MB to 2 GB.");
    const uint32_t total = uint32_t(bytes / 512);
    const int64_t mib = bytes >> 20;
    FatLayout l;
    l.bits = mib <= 64 ? 12 : 16;
    l.clusterSectors = mib <= 8 ? 16 : mib <= 1024 ? 32 : 64;
    l.boundary = mib <= 8 ? 16 : mib <= 64 ? 32 : mib <= 256 ? 64 : 128;
    const uint32_t maxClusters = l.bits == 12 ? 4084 : 65524;
    struct Fit {
        uint32_t start, clusters, needed;
    };
    auto fit = [&](uint32_t fatSectors) {
        const uint32_t system = FatLayout::reserved + 2 * fatSectors + FatLayout::rootSectors;
        const uint32_t start = l.boundary + (l.boundary - system % l.boundary) % l.boundary;
        const uint32_t clusters =
            std::min<uint32_t>(total > start + system ? (total - start - system) / l.clusterSectors : 0, maxClusters);
        const uint32_t needed = uint32_t(((uint64_t(clusters) + 2) * l.bits + 4095) / 4096);
        return Fit{start, clusters, needed};
    };
    // From the FAT the whole card would need down to the smallest that holds its clusters.
    uint32_t fatSectors = uint32_t((uint64_t(total / l.clusterSectors) * l.bits + 4095) / 4096);
    for (Fit f = fit(fatSectors); f.needed < fatSectors && fit(f.needed).needed <= f.needed; f = fit(fatSectors))
        fatSectors = f.needed;
    const Fit f = fit(fatSectors);
    if (f.clusters < 16 || f.needed > fatSectors) throw Error("The card is too small for a FAT file system.");
    l.fatSectors = fatSectors;
    l.start = f.start;
    l.clusters = f.clusters;
    const uint32_t system = FatLayout::reserved + 2 * fatSectors + FatLayout::rootSectors;
    // A FAT12 card past 4084 clusters ends its partition after the last one.
    l.sectors = std::min<uint32_t>(total - l.start, system + l.clusters * l.clusterSectors + l.clusterSectors - 1);
    l.partitionType = l.bits == 12 ? 0x01 : l.sectors < 65536 ? 0x04 : 0x06;
    l.heads = mib <= 2 ? 2 : mib <= 16 ? 2 : mib <= 32 ? 4 : mib <= 128 ? 8 : mib <= 504 ? 16 : mib <= 1008 ? 32 : 64;
    l.trackSectors = mib <= 2 ? 16 : mib <= 256 ? 32 : 63;
    return l;
}

class FatImage {
   public:
    // Reads the card's layout. Throws Error.
    explicit FatImage(Blocks& b) : blocks_(b) {
        blocks_.prepare();
        const int64_t size = blocks_.length();
        if (size < 512 * 64 || size > (int64_t(2048) << 20) || size % sectorBytes) throw Error("Use a card of up to 2 GB.");
        auto boot = readAt(0, sectorBytes);
        if (!bootSector(boot)) {
            // A single primary partition, as the core and cameras make. Never guess offsets.
            const uint8_t* entry = boot.data() + 446;
            const int type = entry[4];
            const int64_t begin = fat::get32(entry + 8), sectors = fat::get32(entry + 12);
            if (fat::get16(boot.data() + 510) != 0xaa55) throw Error("The card has no FAT partition.");
            if (type == 0x0b || type == 0x0c) throw Error(fat32Message);
            if ((type != 1 && type != 4 && type != 6 && type != 14) || !begin || !sectors || (begin + sectors) * sectorBytes > size)
                throw Error("The card has no FAT partition.");
            offset_ = begin * sectorBytes;
            boot = readAt(offset_, sectorBytes);
        }
        const uint8_t* bpb = boot.data();
        if (fat::get16(bpb + 11) != sectorBytes || fat::get16(bpb + 510) != 0xaa55)
            throw Error("The card doesn't hold a FAT file system.");
        clusterSectors_ = bpb[13];
        const int reserved = int(fat::get16(bpb + 14));
        fatCount_ = bpb[16];
        rootEntries_ = int(fat::get16(bpb + 17));
        const int64_t total = fat::get16(bpb + 19) ? int64_t(fat::get16(bpb + 19)) : int64_t(fat::get32(bpb + 32));
        fatSectors_ = int(fat::get16(bpb + 22));
        if (!fatSectors_ && !rootEntries_) throw Error(fat32Message);
        if (!clusterSectors_ || (clusterSectors_ & (clusterSectors_ - 1)) || !reserved || !fatCount_ || !fatSectors_ ||
            !rootEntries_)
            throw Error("The card's FAT header is damaged.");
        fatStart_ = offset_ + int64_t(reserved) * sectorBytes;
        rootStart_ = fatStart_ + int64_t(fatCount_) * fatSectors_ * sectorBytes;
        const int rootSectors = (rootEntries_ * 32 + sectorBytes - 1) / sectorBytes;
        dataStart_ = rootStart_ + int64_t(rootSectors) * sectorBytes;
        const int64_t dataSectors = total - (reserved + int64_t(fatCount_) * fatSectors_ + rootSectors);
        const int64_t count = dataSectors > 0 ? dataSectors / clusterSectors_ : 0;
        bits_ = count < 4085 ? 12 : 16;
        if (count < 1 || count >= 65525 || offset_ + total * sectorBytes > size ||
            fatBytesFor(count) > int64_t(fatSectors_) * sectorBytes)
            throw Error("The card's FAT header is damaged.");
        clusterCount_ = uint32_t(count);
        fatRaw_ = readAt(fatStart_, int64_t(fatSectors_) * sectorBytes);
        fat_.resize(size_t(count + 2));
        for (size_t i = 0; i < fat_.size(); ++i) fat_[i] = entryIn(fatRaw_, i);
        fatOnDisk_ = fat_;
    }

    static constexpr const char* fat32Message = "This card uses FAT32.";

    int fatBits() const { return bits_; }
    int64_t clusterBytes() const { return int64_t(clusterSectors_) * sectorBytes; }
    int64_t freeBytes() const {
        int64_t n = 0;
        for (uint32_t c = 2; c < clusterCount_ + 2; ++c)
            if (!fat_[c]) ++n;
        return n * clusterBytes();
    }
    int64_t totalBytes() const { return int64_t(clusterCount_) * clusterBytes(); }
    // Bytes written to the card, and whether any change was committed.
    int64_t bytesWritten{0};
    bool committed{false};

    // A fresh FAT12 or FAT16 file system over the whole card (fatLayout), in one commit: the
    // partition table, boot sector, both FATs and the root folder with the volume label. A classic
    // stick gets the TG50's own layout instead (storage/ClassicStick.h), with no label.
    static void format(Blocks& blocks, const std::string& label = "FROND") {
        if (blocks.formatStick()) return;
        const auto l = fatLayout(blocks.length());
        Sectors out;
        auto sector = [&](int64_t index) -> std::vector<uint8_t>& {
            auto& s = out[index * sectorBytes];
            s.resize(sectorBytes, 0);
            return s;
        };
        auto chs = [&](uint32_t lba, uint8_t* p) {
            const uint32_t perCylinder = uint32_t(l.heads) * l.trackSectors;
            const uint32_t c = std::min<uint32_t>(lba / perCylinder, 1023);
            const uint32_t h = lba / perCylinder > 1023 ? l.heads - 1u : (lba / l.trackSectors) % l.heads;
            const uint32_t s = lba / perCylinder > 1023 ? l.trackSectors : lba % l.trackSectors + 1;
            p[0] = uint8_t(h);
            p[1] = uint8_t((s & 0x3f) | ((c >> 2) & 0xc0));
            p[2] = uint8_t(c);
        };
        auto& mbr = sector(0);
        uint8_t* part = mbr.data() + 446;
        part[0] = 0x80;
        chs(l.start, part + 1);
        part[4] = l.partitionType;
        chs(l.start + l.sectors - 1, part + 5);
        fat::put32(part + 8, l.start);
        fat::put32(part + 12, l.sectors);
        mbr[510] = 0x55;
        mbr[511] = 0xaa;
        std::string name = fat::asciiUpper(label).substr(0, 11);
        name.resize(11, ' ');
        auto& boot = sector(l.start);
        const uint8_t jump[3] = {0xeb, 0x3c, 0x90};
        std::memcpy(boot.data(), jump, 3);
        std::memcpy(boot.data() + 3, "FROND001", 8);
        fat::put16(boot.data() + 11, sectorBytes);
        boot[13] = uint8_t(l.clusterSectors);
        fat::put16(boot.data() + 14, FatLayout::reserved);
        boot[16] = 2;
        fat::put16(boot.data() + 17, FatLayout::rootEntries);
        if (l.sectors < 65536) fat::put16(boot.data() + 19, l.sectors);
        else fat::put32(boot.data() + 32, l.sectors);
        boot[21] = 0xf8;
        fat::put16(boot.data() + 22, l.fatSectors);
        fat::put16(boot.data() + 24, l.trackSectors);
        fat::put16(boot.data() + 26, l.heads);
        fat::put32(boot.data() + 28, l.start);
        boot[36] = 0x80;
        boot[38] = 0x29;
        fat::put32(boot.data() + 39, 0x46524f4e);
        std::memcpy(boot.data() + 43, name.data(), 11);
        std::memcpy(boot.data() + 54, l.bits == 12 ? "FAT12   " : "FAT16   ", 8);
        boot[510] = 0x55;
        boot[511] = 0xaa;
        const int64_t fatAt = int64_t(l.start) + FatLayout::reserved;
        for (int64_t copy = 0; copy < 2; ++copy)
            for (int64_t i = 0; i < l.fatSectors; ++i) {
                auto& s = sector(fatAt + copy * l.fatSectors + i);
                if (i) continue;
                s[0] = 0xf8;
                s[1] = 0xff;
                s[2] = 0xff;
                if (l.bits == 16) s[3] = 0xff;
            }
        const int64_t rootAt = fatAt + 2 * int64_t(l.fatSectors);
        for (int64_t i = 0; i < FatLayout::rootSectors; ++i) sector(rootAt + i);
        auto& root = sector(rootAt);
        std::memcpy(root.data(), name.data(), 11);
        root[11] = 0x08;
        fat::putStamp(root.data(), fat::stampNow());
        blocks.commit(out);
    }

    // A folder: folders first, then files, by name.
    std::vector<Entry> list(const std::string& path) {
        std::vector<Entry> out;
        for (const auto& s : slotsIn(folder(path))) out.push_back(s.entry);
        std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
            if (a.directory != b.directory) return a.directory;
            return fat::folded(fat::utf16(a.name)) < fat::folded(fat::utf16(b.name));
        });
        return out;
    }

    std::optional<Entry> find(const std::string& path) {
        auto p = parts(path);
        if (p.empty()) return std::nullopt;
        const auto last = p.back();
        p.pop_back();
        for (const auto& s : slotsIn(folder(joinAll(p))))
            if (fat::sameName(s.entry.name, last)) return s.entry;
        return std::nullopt;
    }

    // The file's bytes, a cluster at a time.
    void read(const Entry& entry, const std::function<void(const char*, int64_t)>& out) {
        if (entry.directory) throw Error(entry.name + " is a folder.");
        int64_t left = entry.size;
        std::vector<char> buffer(static_cast<size_t>(clusterBytes()));
        for (const auto c : chain(entry.firstCluster)) {
            if (left <= 0) break;
            const int64_t n = std::min(left, clusterBytes());
            blocks_.read(clusterAt(c), buffer.data(), n);
            out(buffer.data(), n);
            left -= n;
        }
        if (left > 0) throw Error(entry.name + " is shorter on the card than its size says.");
    }

    // A new file of `size` bytes from `input` (reads up to n bytes, returns how many, 0 at the end),
    // dated `when` or now.
    void add(const std::string& path, const std::string& name, const std::function<int64_t(char*, int64_t)>& input,
             int64_t size, std::optional<fat::Stamp> when = {}) {
        change([&] {
            fat::checkName(name);
            if (size < 0 || size >= (int64_t(1) << 32)) throw Error(name + " is too big for a card.");
            const auto f = folder(path);
            const auto existing = slotsIn(f);
            refuseTaken(existing, name);
            const auto clusters = allocate(size_t((size + clusterBytes() - 1) / clusterBytes()));
            std::vector<uint8_t> buffer(static_cast<size_t>(clusterBytes()));
            int64_t left = size;
            for (const auto c : clusters) {
                const int64_t n = std::min(left, clusterBytes());
                int64_t got = 0;
                while (got < n) {
                    const auto r = input(reinterpret_cast<char*>(buffer.data()) + got, n - got);
                    if (r <= 0) throw Error("The file ended early.");
                    got += r;
                }
                std::memset(buffer.data() + n, 0, size_t(clusterBytes() - n));
                writeData(clusterAt(c), buffer);
                left -= n;
            }
            addEntry(f, existing, name, 0x20, clusters.empty() ? 0 : clusters.front(), size, {}, when);
        });
    }

    void makeFolder(const std::string& path, const std::string& name, std::optional<fat::Stamp> when = {}) {
        change([&] {
            fat::checkName(name);
            const auto parent = folder(path);
            const auto existing = slotsIn(parent);
            refuseTaken(existing, name);
            const auto cluster = allocate(1).front();
            const auto stamp = when ? *when : fat::stampNow();
            writeData(clusterAt(cluster), folderBlock(cluster, parent.cluster, stamp));
            addEntry(parent, existing, name, 0x10, cluster, 0, {}, stamp);
        });
    }

    // Runs `work` (adds, folders and the rest) as one change: one commit at the end, so a new card
    // filled from a host folder is written once. A failure or cancel rolls all of it back.
    void batch(const std::function<void()>& work) { change(work); }

    // A file, or a folder with everything in it.
    void remove(const std::string& path) {
        change([&] {
            const auto [parent, slot] = locate(path);
            freeTree(slot.entry, 0);
            eraseSlots(parent, slot);
        });
    }

    // Into toFolder, under newName when given (a rename when the folder is the same).
    void move(const std::string& from, const std::string& toFolder, const std::string& newName = {}) {
        change([&] {
            const auto [parent, slot] = locate(from);
            const auto name = newName.empty() ? slot.entry.name : newName;
            fat::checkName(name);
            const auto dest = folder(toFolder);
            if (slot.entry.directory && within(from, toFolder)) throw Error(slot.entry.name + " can't go into itself.");
            const auto inner = slot.entry.directory ? sub(slot.entry) : Folder{};
            const auto raw = rawOf(parent, slot);
            eraseSlots(parent, slot);
            const auto existing = slotsIn(dest);
            refuseTaken(existing, name);
            addEntry(dest, existing, name, raw[11], slot.entry.firstCluster, slot.entry.size, raw);
            if (slot.entry.directory && dest.cluster != parent.cluster) {
                auto bytes = folderBytes(inner);
                fat::put16(bytes.data() + 32 + 26, dest.cluster);
                writeSlots(inner, bytes, 1, 1);
            }
        });
    }

    // A file or folder into toFolder under newName (or its own). `bytes` gets the amount so far.
    void copy(const std::string& from, const std::string& toFolder, const std::string& newName = {},
              const std::function<bool()>& cancelled = {}, const std::function<void(int64_t)>& bytes = {}) {
        change([&] {
            const auto [parent, slot] = locate(from);
            const auto name = newName.empty() ? slot.entry.name : newName;
            fat::checkName(name);
            const auto dest = folder(toFolder);
            if (slot.entry.directory && within(from, toFolder)) throw Error(slot.entry.name + " can't go into itself.");
            refuseTaken(slotsIn(dest), name);
            int64_t done = 0;
            copyOne(rawOf(parent, slot), slot.entry, dest, name, 0, cancelled, [&](int64_t n) {
                done += n;
                if (bytes) bytes(done);
            });
        });
    }

    // Runs `work` (an add, copy or move that makes an entry of the victim's name) and removes
    // `victim` in the same commit. The victim's clusters stay used until then, so a cancel, a
    // failure or a crash leaves the old one; the new data needs room beside it.
    void replace(const std::string& victim, const std::function<void()>& work) {
        change([&] {
            const auto [parent, slot] = locate(victim);
            // Out of its folder first so the new entry can take the name; freed after the new data.
            eraseSlots(parent, slot);
            work();
            freeTree(slot.entry, 0);
        });
    }

    // The bytes in a file, or in every file under a folder.
    int64_t sizeOf(const std::string& path) {
        const auto located = locate(path);
        std::function<int64_t(const Entry&, int)> total = [&](const Entry& e, int depth) -> int64_t {
            if (depth > maxDepth) throw Error("Folders are nested too deeply.");
            if (!e.directory) return e.size;
            int64_t sum = 0;
            for (const auto& s : slotsIn(sub(e))) sum += total(s.entry, depth + 1);
            return sum;
        };
        return total(located.second.entry, 0);
    }

    // Whether every FAT copy matches the first, and a check as fsck would do it: chains sound and
    // used once, sizes that fit and no lost clusters. The problems found.
    bool fatCopiesMatch() {
        const auto first = readAt(fatStart_, int64_t(fatSectors_) * sectorBytes);
        for (int i = 1; i < fatCount_; ++i)
            if (readAt(fatStart_ + int64_t(i) * fatSectors_ * sectorBytes, int64_t(fatSectors_) * sectorBytes) != first)
                return false;
        return true;
    }
    std::vector<std::string> check() {
        std::vector<std::string> problems;
        if (!fatCopiesMatch()) problems.push_back("FAT copies differ");
        std::vector<std::string> owner(clusterCount_ + 2);
        std::function<void(const Folder&, const std::string&, int)> walk = [&](const Folder& f, const std::string& path,
                                                                              int depth) {
            if (depth > maxDepth) {
                problems.push_back(path + " nested too deeply");
                return;
            }
            for (const auto& slot : slotsIn(f)) {
                const auto& e = slot.entry;
                const auto full = joined(path, e.name);
                std::vector<uint32_t> list;
                try {
                    list = chain(e.firstCluster);
                } catch (const Error& x) {
                    problems.push_back(full + ": " + x.message);
                    continue;
                }
                for (const auto c : list) {
                    if (!owner[c].empty())
                        problems.push_back(full + " shares cluster " + std::to_string(c) + " with " + owner[c]);
                    owner[c] = full;
                }
                if (e.directory) {
                    if (list.empty()) problems.push_back(full + " has no cluster");
                    else walk(Folder{e.firstCluster}, full, depth + 1);
                } else if (int64_t(list.size()) != (e.size + clusterBytes() - 1) / clusterBytes()) {
                    problems.push_back(full + " size " + std::to_string(e.size) + " doesn't fit " +
                                       std::to_string(list.size()) + " clusters");
                }
            }
        };
        walk(Folder{}, "/", 0);
        for (uint32_t c = 2; c < clusterCount_ + 2; ++c)
            if (fat_[c] && owner[c].empty()) problems.push_back("cluster " + std::to_string(c) + " is lost");
        return problems;
    }

    static std::vector<std::string> parts(const std::string& path) {
        std::vector<std::string> out;
        size_t at = 0;
        while (at <= path.size()) {
            const size_t slash = path.find('/', at);
            const size_t end = slash == std::string::npos ? path.size() : slash;
            if (end > at) out.push_back(path.substr(at, end - at));
            if (slash == std::string::npos) break;
            at = slash + 1;
        }
        return out;
    }
    static std::string joined(const std::string& folder, const std::string& name) {
        return folder == "/" || folder.empty() ? "/" + name : folder + "/" + name;
    }
    // Whether `inner` is `outer` or below it.
    static bool within(const std::string& outer, const std::string& inner) {
        const auto o = parts(outer), i = parts(inner);
        if (i.size() < o.size()) return false;
        for (size_t k = 0; k < o.size(); ++k)
            if (!fat::sameName(o[k], i[k])) return false;
        return true;
    }
    // Throws when FAT can't hold the name.
    static void checkName(const std::string& name) { fat::checkName(name); }

   private:
    using Bytes = std::vector<uint8_t>;
    static constexpr int sectorBytes = 512;
    static constexpr int maxDepth = 64;
    struct Slot {
        Entry entry;
        int first{0}, count{0};
    };
    struct Folder {
        uint32_t cluster{0};
        bool root() const { return cluster == 0; }
    };

    static bool bootSector(const Bytes& b) {
        const auto type = [&](size_t at, const char* t) { return !std::memcmp(b.data() + at, t, 8); };
        return type(54, "FAT12   ") || type(54, "FAT16   ") || type(82, "FAT32   ");
    }
    static std::string joinAll(const std::vector<std::string>& p) {
        std::string out;
        for (const auto& s : p) out += "/" + s;
        return out.empty() ? "/" : out;
    }

    int64_t fatBytesFor(int64_t count) const { return bits_ == 12 ? ((count + 2) * 3 + 1) / 2 : (count + 2) * 2; }
    uint32_t endMark() const { return bits_ == 12 ? 0xfff : 0xffff; }
    uint32_t lastData() const { return bits_ == 12 ? 0xff8 : 0xfff8; }
    uint32_t entryIn(const Bytes& raw, size_t i) const {
        if (bits_ == 16) return fat::get16(raw.data() + 2 * i);
        const uint32_t v = fat::get16(raw.data() + i * 3 / 2);
        return i & 1 ? v >> 4 : v & 0xfff;
    }
    void putEntry(Bytes& raw, size_t i, uint32_t v) const {
        if (bits_ == 16) return fat::put16(raw.data() + 2 * i, v);
        uint8_t* p = raw.data() + i * 3 / 2;
        if (i & 1) {
            p[0] = uint8_t((p[0] & 0x0f) | ((v << 4) & 0xf0));
            p[1] = uint8_t(v >> 4);
        } else {
            p[0] = uint8_t(v);
            p[1] = uint8_t((p[1] & 0xf0) | ((v >> 8) & 0x0f));
        }
    }

    // Card bytes, with staged sectors laid over them so a change sees its own edits.
    Bytes readAt(int64_t at, int64_t length) {
        Bytes b(static_cast<size_t>(length));
        blocks_.read(at, reinterpret_cast<char*>(b.data()), length);
        const int64_t first = at / sectorBytes * sectorBytes;
        for (auto it = staged_.lower_bound(first); it != staged_.end() && it->first < at + length; ++it) {
            const int64_t from = std::max(at, it->first);
            const int64_t until = std::min(at + length, it->first + sectorBytes);
            if (until > from) std::memcpy(b.data() + (from - at), it->second.data() + (from - it->first), size_t(until - from));
        }
        return b;
    }
    // Straight to the card: new data in free clusters.
    void writeData(int64_t at, const Bytes& b) {
        blocks_.write(at, reinterpret_cast<const char*>(b.data()), int64_t(b.size()));
        bytesWritten += int64_t(b.size());
    }
    // Keeps a metadata edit in memory, whole sectors, until commit.
    void stage(int64_t at, const uint8_t* b, int64_t length) {
        int64_t done = 0;
        while (done < length) {
            const int64_t position = at + done;
            const int64_t sectorAt = position / sectorBytes * sectorBytes;
            auto it = staged_.find(sectorAt);
            if (it == staged_.end()) {
                Bytes sector(sectorBytes);
                blocks_.read(sectorAt, reinterpret_cast<char*>(sector.data()), sectorBytes);
                it = staged_.emplace(sectorAt, std::move(sector)).first;
            }
            const int64_t inSector = position - sectorAt;
            const int64_t n = std::min<int64_t>(sectorBytes - inSector, length - done);
            std::memcpy(it->second.data() + inSector, b + done, size_t(n));
            done += n;
        }
    }
    int64_t clusterAt(uint32_t cluster) const { return dataStart_ + int64_t(cluster - 2) * clusterBytes(); }

    std::vector<uint32_t> chain(uint32_t start) const {
        std::vector<uint32_t> list;
        uint32_t c = start;
        while (c >= 2 && c < clusterCount_ + 2) {
            if (list.size() > clusterCount_) throw Error("The card's FAT has a loop.");
            list.push_back(c);
            c = fat_[c];
        }
        if (c >= 1 && c < lastData()) throw Error("The card's FAT is damaged.");
        return list;
    }

    Bytes folderBytes(const Folder& f) {
        if (f.root()) return readAt(rootStart_, int64_t(rootEntries_) * 32);
        const auto list = chain(f.cluster);
        Bytes out;
        out.reserve(size_t(int64_t(list.size()) * clusterBytes()));
        for (const auto c : list) {
            const auto part = readAt(clusterAt(c), clusterBytes());
            out.insert(out.end(), part.begin(), part.end());
        }
        return out;
    }
    void writeSlots(const Folder& f, const Bytes& bytes, int first, int count) {
        const auto list = f.root() ? std::vector<uint32_t>() : chain(f.cluster);
        for (int slot = first; slot < first + count; ++slot) {
            const int64_t at = int64_t(slot) * 32;
            const int64_t position =
                f.root() ? rootStart_ + at : clusterAt(list[size_t(at / clusterBytes())]) + at % clusterBytes();
            stage(position, bytes.data() + at, 32);
        }
    }

    static std::string longPart(const uint8_t* e) {
        std::u16string s;
        for (int o : fat::longOffsets) {
            const auto c = char16_t(fat::get16(e + o));
            if (c == 0 || c == 0xffff) break;
            s += c;
        }
        return fat::utf8(s);
    }
    // The short name as a reader shows it, with the case bits other writers set.
    static std::string shortText(const uint8_t* e) {
        const int caseBits = e[12];
        std::u16string base, ext;
        for (int i = 0; i < 8; ++i) base += char16_t(i == 0 && e[0] == 0x05 ? 0xe5 : e[i]);
        for (int i = 8; i < 11; ++i) ext += char16_t(e[i]);
        while (!base.empty() && base.back() == u' ') base.pop_back();
        while (!ext.empty() && ext.back() == u' ') ext.pop_back();
        auto lower = [](std::u16string& s) {
            for (auto& c : s)
                if (c >= u'A' && c <= u'Z') c = char16_t(c + 32);
        };
        if (caseBits & 0x08) lower(base);
        if (caseBits & 0x10) lower(ext);
        return fat::utf8(ext.empty() ? base : base + u"." + ext);
    }

    std::vector<Slot> slotsIn(const Folder& f) {
        const auto bytes = folderBytes(f);
        std::vector<Slot> result;
        std::string longParts[20];
        bool have[20] = {};
        int longFirst = -1, longSum = -1, longCount = 0;
        for (int i = 0; i < int(bytes.size() / 32); ++i) {
            const uint8_t* e = bytes.data() + i * 32;
            const int first = e[0];
            if (first == 0) break;
            const int attr = e[11];
            if (first == 0xe5) {
                longFirst = -1;
                continue;
            }
            if ((attr & 0x3f) == 0x0f) {
                const int order = first & 0x3f;
                if (first & 0x40) {
                    std::fill(std::begin(have), std::end(have), false);
                    longFirst = i;
                    longCount = order;
                    longSum = e[13];
                }
                if (longFirst >= 0 && order >= 1 && order <= 20 && e[13] == longSum) {
                    longParts[order - 1] = longPart(e);
                    have[order - 1] = true;
                } else {
                    longFirst = -1;
                }
                continue;
            }
            const auto shown = shortText(e);
            const int sum = fat::shortChecksum(std::string(reinterpret_cast<const char*>(e), 11));
            bool complete = longFirst >= 0 && sum == longSum && longCount >= 1 && longCount <= 20;
            for (int k = 0; complete && k < longCount; ++k)
                if (!have[k]) complete = false;
            std::string longName;
            if (complete)
                for (int k = 0; k < longCount; ++k) longName += longParts[k];
            const int start = complete ? longFirst : i;
            longFirst = -1;
            if (attr & 0x08) continue;
            if (shown == "." || shown == "..") continue;
            Entry entry{complete ? longName : shown, shown, bool(attr & 0x10), int64_t(fat::get32(e + 28)),
                        fat::get16(e + 26), {uint16_t(fat::get16(e + 24)), uint16_t(fat::get16(e + 22))}};
            result.push_back({entry, start, i - start + 1});
        }
        return result;
    }

    // A subfolder starts at a used cluster; anything else would read as the top folder or nothing.
    Folder sub(const Entry& e) const {
        const auto c = e.firstCluster;
        if (c < 2 || c >= clusterCount_ + 2 || fat_[c] == 0) throw Error("The folder " + e.name + " is damaged.");
        return Folder{c};
    }
    Folder folder(const std::string& path) {
        Folder f;
        for (const auto& part : parts(path)) {
            const auto list = slotsIn(f);
            const auto hit =
                std::find_if(list.begin(), list.end(), [&](const Slot& s) { return fat::sameName(s.entry.name, part); });
            if (hit == list.end()) throw Error("There's no folder " + part + " on the card.");
            if (!hit->entry.directory) throw Error(part + " is a file.");
            f = sub(hit->entry);
        }
        return f;
    }
    std::pair<Folder, Slot> locate(const std::string& path) {
        auto p = parts(path);
        if (p.empty()) throw Error("That is the top folder.");
        const auto last = p.back();
        p.pop_back();
        const auto parent = folder(joinAll(p));
        for (const auto& s : slotsIn(parent))
            if (fat::sameName(s.entry.name, last)) return {parent, s};
        throw Error("There's no " + last + " on the card.");
    }
    Bytes rawOf(const Folder& parent, const Slot& slot) {
        const auto bytes = folderBytes(parent);
        const size_t at = size_t(slot.first + slot.count - 1) * 32;
        return Bytes(bytes.begin() + long(at), bytes.begin() + long(at + 32));
    }
    void eraseSlots(const Folder& parent, const Slot& slot) {
        auto bytes = folderBytes(parent);
        for (int i = slot.first; i < slot.first + slot.count; ++i) bytes[size_t(i) * 32] = 0xe5;
        writeSlots(parent, bytes, slot.first, slot.count);
    }
    void freeTree(const Entry& entry, int depth) {
        if (depth > maxDepth) throw Error("Folders are nested too deeply.");
        if (entry.directory)
            for (const auto& s : slotsIn(sub(entry))) freeTree(s.entry, depth + 1);
        for (const auto c : chain(entry.firstCluster)) fat_[c] = 0;
    }
    void copyOne(const Bytes& raw, const Entry& entry, const Folder& dest, const std::string& name, int depth,
                 const std::function<bool()>& cancelled, const std::function<void(int64_t)>& bytes) {
        if (depth > maxDepth) throw Error("Folders are nested too deeply.");
        if (cancelled && cancelled()) throw Cancelled();
        const auto existing = slotsIn(dest);
        if (entry.directory) {
            const auto cluster = allocate(1).front();
            writeData(clusterAt(cluster),
                      folderBlock(cluster, dest.cluster, {uint16_t(fat::get16(raw.data() + 24)), uint16_t(fat::get16(raw.data() + 22))}));
            addEntry(dest, existing, name, 0x10, cluster, 0, raw);
            const Folder inner = sub(entry);
            const auto innerBytes = folderBytes(inner);
            const Folder target{cluster};
            for (const auto& child : slotsIn(inner)) {
                const size_t at = size_t(child.first + child.count - 1) * 32;
                const Bytes childRaw(innerBytes.begin() + long(at), innerBytes.begin() + long(at + 32));
                copyOne(childRaw, child.entry, target, child.entry.name, depth + 1, cancelled, bytes);
            }
        } else {
            const auto source = chain(entry.firstCluster);
            const auto clusters = allocate(source.size());
            Bytes buffer(static_cast<size_t>(clusterBytes()));
            int64_t left = entry.size;
            for (size_t i = 0; i < clusters.size(); ++i) {
                if (cancelled && cancelled()) throw Cancelled();
                const int64_t n = std::max<int64_t>(0, std::min(left, clusterBytes()));
                blocks_.read(clusterAt(source[i]), reinterpret_cast<char*>(buffer.data()), n);
                std::memset(buffer.data() + n, 0, size_t(clusterBytes() - n));
                writeData(clusterAt(clusters[i]), buffer);
                left -= n;
                bytes(n);
            }
            addEntry(dest, existing, name, raw[11], clusters.empty() ? 0 : clusters.front(), entry.size, raw);
        }
    }
    // A new folder's first cluster: the "." and ".." entries, then empty.
    Bytes folderBlock(uint32_t cluster, uint32_t parent, fat::Stamp when) {
        Bytes block(static_cast<size_t>(clusterBytes()), 0);
        auto dot = [&](size_t at, const char* label, uint32_t target) {
            std::memset(block.data() + at, ' ', 11);
            for (size_t i = 0; label[i]; ++i) block[at + i] = uint8_t(label[i]);
            block[at + 11] = 0x10;
            fat::putStamp(block.data() + at, when);
            fat::put16(block.data() + at + 26, target);
        };
        dot(0, ".", cluster);
        dot(32, "..", parent);
        return block;
    }
    void refuseTaken(const std::vector<Slot>& existing, const std::string& name) {
        for (const auto& s : existing)
            if (fat::sameName(s.entry.name, name) || fat::sameName(s.entry.shortName, name))
                throw Error(name + " is already on the card. Choose another name.");
    }
    std::vector<uint32_t> allocate(size_t count) {
        std::vector<uint32_t> free;
        for (uint32_t c = 2; free.size() < count && c < clusterCount_ + 2; ++c)
            if (!fat_[c]) free.push_back(c);
        if (free.size() < count) throw Error("There isn't enough space on the card.");
        for (size_t i = 0; i < free.size(); ++i) fat_[free[i]] = i + 1 < free.size() ? free[i + 1] : endMark();
        return free;
    }
    void addEntry(const Folder& f, const std::vector<Slot>& existing, const std::string& name, uint8_t attr,
                  uint32_t cluster, int64_t size, const Bytes& templ = {}, std::optional<fat::Stamp> when = {}) {
        // Long names count too, so a new ~N alias never equals another file's name.
        std::set<std::string> taken;
        for (const auto& s : existing) {
            taken.insert(fat::asciiUpper(s.entry.shortName));
            taken.insert(fat::asciiUpper(s.entry.name));
        }
        const auto eleven = fat::shortName(name, taken);
        if (eleven.empty()) throw Error("Too many similar names in this folder.");
        auto entries = fat::dirEntry(name, eleven, attr, cluster, uint32_t(size), when ? *when : fat::stampNow());
        const int total = int(entries.size() / 32);
        // A moved or copied entry keeps its dates.
        if (templ.size() == 32) {
            uint8_t* at = entries.data() + entries.size() - 32;
            for (int i = 13; i <= 19; ++i) at[i] = templ[size_t(i)];
            for (int i = 22; i <= 25; ++i) at[i] = templ[size_t(i)];
        }
        auto [bytes, first] = freeSlots(f, total);
        std::memcpy(bytes.data() + size_t(first) * 32, entries.data(), entries.size());
        writeSlots(f, bytes, first, total);
    }
    // Finds count free slots in a row, adding a cluster to a full subfolder. Returns the folder's
    // bytes (grown if needed) and the first slot.
    std::pair<Bytes, int> freeSlots(const Folder& f, int count) {
        auto bytes = folderBytes(f);
        auto search = [&]() {
            int run = 0;
            for (int i = 0; i < int(bytes.size() / 32); ++i) {
                const int first = bytes[size_t(i) * 32];
                if (first == 0 || first == 0xe5) {
                    if (++run == count) return i - count + 1;
                } else {
                    run = 0;
                }
            }
            return -1;
        };
        int found = search();
        while (found < 0) {
            if (f.root()) throw Error("The card's top folder is full. Use a subfolder.");
            const auto list = chain(f.cluster);
            if (list.empty()) throw Error("The card's FAT is damaged.");
            // A FAT folder holds at most 65536 entries (2 MB).
            if (int64_t(list.size() + 1) * clusterBytes() > 65536 * 32)
                throw Error("That folder is full (a card folder holds at most 65536 names). Use another folder.");
            const auto added = allocate(1).front();
            fat_[list.back()] = added;
            writeData(clusterAt(added), Bytes(size_t(clusterBytes()), 0));
            bytes = folderBytes(f);
            found = search();
        }
        return {bytes, found};
    }

    // Runs one change and commits it, or rolls the in-memory state back when it fails. Data
    // already written went into clusters the card's FAT still calls free.
    void change(const std::function<void()>& block) {
        if (changing_) return block();
        changing_++;
        try {
            block();
            commit();
            changing_--;
        } catch (...) {
            changing_--;
            fat_ = fatOnDisk_;
            staged_.clear();
            try {
                blocks_.settleData();
            } catch (...) {
            }
            throw;
        }
    }
    // Stages the FAT sectors that differ from the card, in every FAT copy.
    void stageFat() {
        Bytes raw = fatRaw_;
        for (size_t i = 0; i < fat_.size(); ++i)
            if (fat_[i] != fatOnDisk_[i]) putEntry(raw, i, fat_[i]);
        for (int64_t s = 0; s < fatSectors_; ++s) {
            const auto from = raw.begin() + long(s * sectorBytes);
            if (std::equal(from, from + sectorBytes, fatRaw_.begin() + long(s * sectorBytes))) continue;
            for (int copy = 0; copy < fatCount_; ++copy)
                stage(fatStart_ + int64_t(copy) * fatSectors_ * sectorBytes + s * sectorBytes, &*from, sectorBytes);
        }
        pendingFatRaw_ = std::move(raw);
    }
    void commit() {
        stageFat();
        if (staged_.empty()) return;
        blocks_.commit(staged_);
        fatOnDisk_ = fat_;
        fatRaw_ = std::move(pendingFatRaw_);
        staged_.clear();
        committed = true;
    }

    Blocks& blocks_;
    int64_t offset_{0}, fatStart_{0}, rootStart_{0}, dataStart_{0};
    int clusterSectors_{0}, fatCount_{0}, fatSectors_{0}, rootEntries_{0}, bits_{16};
    uint32_t clusterCount_{0};
    std::vector<uint32_t> fat_, fatOnDisk_;
    // The first FAT's sectors as on the card, and as the change being committed leaves them.
    Bytes fatRaw_, pendingFatRaw_;
    // Metadata sectors changed by the current change, by card offset, until commit.
    Sectors staged_;
    // A change inside another (replace) commits with the outer one.
    int changing_{0};
};

}  // namespace frondStorage

#endif
