#include "storage/Fat32Writer.h"

#include "storage/Errors.h"
#include "storage/FatNames.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <map>
#include <random>
#include <set>

using frondStorage::Error;
namespace fat = frondStorage::fat;

namespace frondFat32 {

namespace {
constexpr uint32_t sectorBytes = 512;
constexpr uint32_t endOfChain = 0x0fffffff;
constexpr size_t maxDirEntries = 65536;
}  // namespace

Geometry geometry(uint32_t sectors) {
    Geometry g;
    g.sectors = sectors;
    g.clusterSectors = 8;
    while (sectors / g.clusterSectors > (1u << 21) && g.clusterSectors < 128) g.clusterSectors *= 2;
    // The smallest FAT that holds every cluster the rest of the space makes.
    auto fits = [&](uint32_t fatSectors) {
        if (uint64_t(Geometry::reserved) + 2ull * fatSectors >= sectors) return false;
        const uint32_t clusters = (sectors - Geometry::reserved - 2 * fatSectors) / g.clusterSectors;
        return (uint64_t(clusters) + 2) * 4 <= uint64_t(fatSectors) * sectorBytes;
    };
    uint32_t f = 1;
    while (!fits(f)) {
        const uint32_t clusters = (sectors - Geometry::reserved - std::min(sectors - Geometry::reserved, 2 * f)) / g.clusterSectors;
        const uint32_t need = uint32_t(((uint64_t(clusters) + 2) * 4 + sectorBytes - 1) / sectorBytes);
        f = std::max(f + 1, need);
    }
    while (f > 1 && fits(f - 1)) --f;
    g.fatSectors = f;
    g.clusters = (sectors - g.dataStart()) / g.clusterSectors;
    return g;
}

Writer::Writer(WriteFn write, uint64_t offset, uint32_t sectors, uint32_t hidden, int64_t now,
               std::function<void()> poll)
    : write_(std::move(write)), offset_(offset), hidden_(hidden), now_(now), poll_(std::move(poll)) {
    if (sectors < 66600 * 8) throw Error("The hdd3 partition is too small for FAT32.");
    geo_ = geometry(sectors);
    clusterBytes_ = geo_.clusterSectors * sectorBytes;
    fat_.assign(3, 0);
    fat_[0] = 0x0ffffff8;
    fat_[1] = endOfChain;
    fat_[2] = endOfChain;  // the root's first cluster
}

void Writer::full() const { throw Error("The archive doesn't fit in the hdd3 partition."); }

uint64_t Writer::clusterOffset(uint32_t cluster) const {
    return offset_ + (uint64_t(geo_.dataStart()) + uint64_t(cluster - 2) * geo_.clusterSectors) * sectorBytes;
}

// `count` clusters in a row, chained.
uint32_t Writer::allocRun(uint32_t count) {
    if (!count) return 0;
    if (uint64_t(cursor_) + count - 1 > uint64_t(geo_.clusters) + 1) full();
    const uint32_t first = cursor_;
    fat_.resize(size_t(first) + count, 0);
    for (uint32_t i = 0; i < count; ++i) fat_[first + i] = i + 1 < count ? first + i + 1 : endOfChain;
    cursor_ += count;
    return first;
}

uint32_t Writer::beginFile(uint64_t size) {
    if (open_) throw Error("internal: a file is still open");
    if (size > 0xffffffffull) throw Error("A file is too large for FAT32.");
    const uint64_t clusters = (size + clusterBytes_ - 1) / clusterBytes_;
    if (clusters > geo_.clusters) full();
    files_.push_back({size, allocRun(uint32_t(clusters))});
    open_ = true;
    openBytes_ = 0;
    return uint32_t(files_.size() - 1);
}

void Writer::fileData(const uint8_t* data, size_t size) {
    const auto& file = files_.back();
    if (!open_ || openBytes_ + size > file.size) throw Error("internal: file data past its size");
    if (size) write_(clusterOffset(file.first) + openBytes_, data, size);
    openBytes_ += size;
}

void Writer::endFile() {
    if (!open_ || openBytes_ != files_.back().size) throw Error("internal: file data short of its size");
    open_ = false;
}

void Writer::finish(const std::vector<Node>& nodes) {
    if (open_) throw Error("internal: a file is still open");
    struct Dir {
        std::string name;
        int64_t mtime{};
        uint32_t parent{}, cluster{}, clusters{};
        std::vector<size_t> children;  // indexes into `items`
    };
    struct Item {
        std::string name, shortName;
        bool dir{};
        size_t dirIndex{}, node{};
    };
    std::vector<Dir> dirs(1);
    std::vector<Item> items;
    std::map<std::string, size_t> dirByPath{{"", 0}};
    std::set<std::string> filePaths;

    auto split = [](const std::string& path, std::string& parent, std::string& name) {
        const auto slash = path.rfind('/');
        parent = slash == std::string::npos ? std::string() : path.substr(0, slash);
        name = slash == std::string::npos ? path : path.substr(slash + 1);
    };
    std::function<size_t(const std::string&, int64_t, bool)> dirFor = [&](const std::string& path, int64_t mtime,
                                                                           bool listed) -> size_t {
        auto it = dirByPath.find(path);
        if (it != dirByPath.end()) {
            if (listed) dirs[it->second].mtime = mtime;
            return it->second;
        }
        if (filePaths.count(path)) throw Error("The archive has hdd3/" + path + " as both a file and a folder.");
        std::string parent, name;
        split(path, parent, name);
        const size_t up = dirFor(parent, now_, false);
        const size_t index = dirs.size();
        dirs.push_back({name, listed ? mtime : now_, uint32_t(up), 0, 0, {}});
        dirByPath[path] = index;
        items.push_back({name, {}, true, index, 0});
        dirs[up].children.push_back(items.size() - 1);
        return index;
    };
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (node.path.empty()) continue;
        if (node.dir) {
            dirFor(node.path, node.mtime, true);
            continue;
        }
        std::string parent, name;
        split(node.path, parent, name);
        if (dirByPath.count(node.path)) throw Error("The archive has hdd3/" + node.path + " as both a file and a folder.");
        const size_t up = dirFor(parent, now_, false);
        filePaths.insert(node.path);
        items.push_back({name, {}, false, 0, i});
        dirs[up].children.push_back(items.size() - 1);
    }

    // Short names, entry counts and clusters for every folder.
    for (size_t d = 0; d < dirs.size(); ++d) {
        auto& dir = dirs[d];
        std::set<std::string> taken;
        size_t entries = d ? 2 : 0;
        for (const size_t c : dir.children) {
            auto& item = items[c];
            fat::checkName(item.name);
            item.shortName = fat::shortName(item.name, taken);
            if (item.shortName.empty()) throw Error("Too many similar names in hdd3/" + dir.name + ".");
            taken.insert(fat::shortDisplay(item.shortName));
            entries += fat::dirEntry(item.name, item.shortName, 0, 0, 0, {}).size() / 32;
        }
        if (entries > maxDirEntries) throw Error("A folder in the archive's hdd3 has too many files for FAT32.");
        dir.clusters = std::max<uint32_t>(1, uint32_t((entries * 32 + clusterBytes_ - 1) / clusterBytes_));
        if (d == 0) {
            dir.cluster = 2;
            if (dir.clusters > 1) {
                const uint32_t more = allocRun(dir.clusters - 1);
                fat_[2] = more;
            }
        } else {
            dir.cluster = allocRun(dir.clusters);
        }
        if (poll_) poll_();
    }

    auto stamp = [](int64_t when) {
        std::time_t t = std::time_t(when);
        std::tm local{};
        if (!localtime_r(&t, &local)) return fat::Stamp{};
        return fat::stampOf(local);
    };
    auto writeChain = [&](uint32_t cluster, const std::vector<uint8_t>& bytes) {
        for (size_t at = 0; at < bytes.size(); at += clusterBytes_) {
            if (cluster < 2 || cluster >= endOfChain) throw Error("internal: folder chain too short");
            write_(clusterOffset(cluster), bytes.data() + at, clusterBytes_);
            cluster = fat_[cluster];
        }
    };
    for (size_t d = 0; d < dirs.size(); ++d) {
        const auto& dir = dirs[d];
        std::vector<uint8_t> bytes;
        auto append = [&](const std::vector<uint8_t>& e) { bytes.insert(bytes.end(), e.begin(), e.end()); };
        auto setHigh = [&](size_t at, uint32_t cluster) { fat::put16(bytes.data() + at + 20, cluster >> 16); };
        if (d) {
            const auto when = stamp(dir.mtime);
            const uint32_t up = dir.parent ? dirs[dir.parent].cluster : 0;
            append(fat::dirEntry(".", ".          ", 0x10, dir.cluster & 0xffff, 0, when));
            setHigh(bytes.size() - 32, dir.cluster);
            append(fat::dirEntry("..", "..         ", 0x10, up & 0xffff, 0, when));
            setHigh(bytes.size() - 32, up);
        }
        for (const size_t c : dir.children) {
            const auto& item = items[c];
            uint32_t cluster, size = 0;
            fat::Stamp when;
            uint8_t attr;
            if (item.dir) {
                cluster = dirs[item.dirIndex].cluster;
                when = stamp(dirs[item.dirIndex].mtime);
                attr = 0x10;
            } else {
                const auto& node = nodes[item.node];
                const auto& file = files_.at(node.content);
                cluster = file.first;
                size = uint32_t(file.size);
                when = stamp(node.mtime);
                attr = 0x20;
            }
            append(fat::dirEntry(item.name, item.shortName, attr, cluster & 0xffff, size, when));
            setHigh(bytes.size() - 32, cluster);
        }
        bytes.resize(size_t(dir.clusters) * clusterBytes_, 0);
        writeChain(dir.cluster, bytes);
        if (poll_) poll_();
    }

    // Both FATs, then the boot sectors and FSInfo.
    const uint32_t used = uint32_t(fat_.size() - 2);
    std::vector<uint8_t> sector(sectorBytes);
    for (uint32_t s = 0; s < geo_.fatSectors; ++s) {
        const size_t first = size_t(s) * (sectorBytes / 4);
        if (first >= fat_.size()) break;
        std::fill(sector.begin(), sector.end(), 0);
        for (size_t k = 0; k < sectorBytes / 4 && first + k < fat_.size(); ++k)
            fat::put32(sector.data() + 4 * k, fat_[first + k]);
        for (uint32_t copy = 0; copy < 2; ++copy)
            write_(offset_ + (uint64_t(Geometry::reserved) + uint64_t(copy) * geo_.fatSectors + s) * sectorBytes,
                   sector.data(), sectorBytes);
        if (poll_ && s % 1024 == 0) poll_();
    }

    std::random_device random;
    uint8_t boot[sectorBytes]{};
    const uint8_t jump[3] = {0xeb, 0x58, 0x90};
    memcpy(boot, jump, 3);
    memcpy(boot + 3, "MSWIN4.1", 8);
    fat::put16(boot + 11, sectorBytes);
    boot[13] = uint8_t(geo_.clusterSectors);
    fat::put16(boot + 14, Geometry::reserved);
    boot[16] = 2;
    boot[21] = 0xf8;
    fat::put16(boot + 24, 63);
    fat::put16(boot + 26, 16);
    fat::put32(boot + 28, hidden_);
    fat::put32(boot + 32, geo_.sectors);
    fat::put32(boot + 36, geo_.fatSectors);
    fat::put32(boot + 44, 2);
    fat::put16(boot + 48, 1);
    fat::put16(boot + 50, 6);
    boot[64] = 0x80;
    boot[66] = 0x29;
    fat::put32(boot + 67, uint32_t(random()));
    memcpy(boot + 71, "NO NAME    ", 11);
    memcpy(boot + 82, "FAT32   ", 8);
    boot[510] = 0x55;
    boot[511] = 0xaa;
    uint8_t info[sectorBytes]{};
    fat::put32(info, 0x41615252);
    fat::put32(info + 484, 0x61417272);
    fat::put32(info + 488, geo_.clusters - used);
    fat::put32(info + 492, cursor_ <= geo_.clusters + 1 ? cursor_ : 0xffffffffu);
    fat::put32(info + 508, 0xaa550000);
    for (uint32_t at : {0u, 6u}) {
        write_(offset_ + uint64_t(at) * sectorBytes, boot, sectorBytes);
        write_(offset_ + uint64_t(at + 1) * sectorBytes, info, sectorBytes);
    }
}

}  // namespace frondFat32
