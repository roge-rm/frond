#include "storage/Ext3Writer.h"

#include "storage/Errors.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <random>

using frondStorage::Error;

namespace frondExt3 {

namespace {

constexpr uint32_t rootIno = 2, journalIno = 8, lostFoundIno = 11, firstIno = 11;
constexpr uint32_t perBlock = 256;  // block numbers in an indirect block
constexpr uint32_t linkMax = 32000;
constexpr uint32_t lostFoundBlocks = 12;
constexpr uint16_t typeDir = 0x4000, typeFile = 0x8000, typeLink = 0xa000;

void put16(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}
void be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

bool powerOf(uint32_t n, uint32_t base) {
    while (n > 1 && n % base == 0) n /= base;
    return n == 1;
}

// mke2fs's default journal for a file system this many blocks big, in blocks.
uint32_t journalSize(uint32_t blocks) {
    if (blocks < 2048) return 0;
    if (blocks < 32768) return 1024;
    if (blocks < 256 * 1024) return 4096;
    if (blocks < 512 * 1024) return 8192;
    if (blocks < 4096 * 1024) return 16384;
    return 32768;
}

// Folder blocks: entries with file types, none crossing a block, the last one in each block
// filling it. A new entry goes in the first block with room, as e2fsprogs' ext2fs_link does.
class DirBlocks {
   public:
    std::vector<std::vector<uint8_t>> blocks;

    void add(uint32_t ino, const std::string& name, uint8_t type) {
        const uint32_t need = 8 + ((uint32_t(name.size()) + 3) & ~3u);
        size_t b = 0;
        while (b < blocks.size() && used_[b].pos + need > 1024) ++b;
        if (b == blocks.size()) {
            blocks.emplace_back(1024, 0);
            used_.push_back({});
        }
        auto& used = used_[b];
        uint8_t* e = blocks[b].data() + used.pos;
        put32(e, ino);
        put16(e + 4, need);
        e[6] = uint8_t(name.size());
        e[7] = type;
        memcpy(e + 8, name.data(), name.size());
        used.last = used.pos;
        used.pos += need;
    }
    // An empty block, as lost+found keeps spare ones.
    void spare() {
        blocks.emplace_back(1024, 0);
        put16(blocks.back().data() + 4, 1024);
        used_.push_back({1024, 0});
    }
    void close() {
        for (size_t b = 0; b < blocks.size(); ++b)
            put16(blocks[b].data() + used_[b].last + 4, 1024 - used_[b].last);
    }

   private:
    struct Used {
        uint32_t pos{0}, last{0};
    };
    std::vector<Used> used_;
};

}  // namespace

struct Writer::Inode {
    uint16_t mode{};
    uint32_t uid{}, gid{}, atime{}, ctime{}, mtime{};
    uint64_t size{};
    uint32_t links{}, sectors{};
    uint8_t map[60]{};
};

Writer::Writer(WriteFn write, uint64_t offset, uint64_t blocks, uint32_t now, std::string label,
               std::function<void()> poll)
    : write_(std::move(write)), offset_(offset), now_(now), label_(std::move(label)), poll_(std::move(poll)) {
    if (blocks < 4096 || blocks > 0xffffffffu) throw Error("The " + label_ + " partition has an unusable size.");
    const uint64_t inodes = (blocks * blockSize + 4095) / 4096;
    for (;;) {
        blocks_ = uint32_t(blocks);
        groups_ = (blocks_ - 1 + blocksPerGroup - 1) / blocksPerGroup;
        uint32_t ipg = uint32_t((inodes + groups_ - 1) / groups_);
        ipg = std::min<uint32_t>(ipg, blockSize * 8);
        tableBlocks_ = (ipg * inodeSize + blockSize - 1) / blockSize;
        inodesPerGroup_ = (tableBlocks_ * blockSize / inodeSize) & ~7u;
        gdtBlocks_ = (groups_ * 32 + blockSize - 1) / blockSize;
        const uint32_t rem = (blocks_ - 1) % blocksPerGroup;
        const uint32_t overhead = 3 + tableBlocks_ + (hasSuper(groups_ - 1) ? 1 + gdtBlocks_ : 0);
        if (rem && rem < overhead + 50) {
            blocks -= rem;
            continue;
        }
        break;
    }

    std::random_device random;
    for (auto& b : uuid_) b = uint8_t(random());
    for (auto& b : hashSeed_) b = uint8_t(random());
    uuid_[6] = uint8_t((uuid_[6] & 0x0f) | 0x40);
    uuid_[8] = uint8_t((uuid_[8] & 0x3f) | 0x80);
    hashSeed_[6] = uint8_t((hashSeed_[6] & 0x0f) | 0x40);
    hashSeed_[8] = uint8_t((hashSeed_[8] & 0x3f) | 0x80);

    used_.assign(blocks_, false);
    used_[0] = true;
    for (uint32_t g = 0; g < groups_; ++g) {
        uint32_t at = groupStart(g);
        const uint32_t meta = (hasSuper(g) ? 1 + gdtBlocks_ : 0) + 2 + tableBlocks_;
        for (uint32_t i = 0; i < meta; ++i) used_[at + i] = true;
    }

    // The journal: a version 2 superblock, the rest zeros.
    journalBlocks_ = journalSize(blocks_);
    std::vector<uint32_t> journal(journalBlocks_);
    for (auto& b : journal) b = alloc();
    journalMeta_ = mapBlocks(journal, journalMap_);
    uint8_t jsb[blockSize]{};
    be32(jsb, 0xc03b3998);
    be32(jsb + 4, 4);
    be32(jsb + 12, blockSize);
    be32(jsb + 16, journalBlocks_);
    be32(jsb + 20, 1);
    be32(jsb + 24, 1);
    memcpy(jsb + 48, uuid_, 16);
    be32(jsb + 64, 1);
    writeBlock(journal[0], jsb);
}

bool Writer::hasSuper(uint32_t group) const {
    return group <= 1 || powerOf(group, 3) || powerOf(group, 5) || powerOf(group, 7);
}

void Writer::full() const { throw Error("The archive doesn't fit in the " + label_ + " partition."); }

uint32_t Writer::alloc() {
    while (cursor_ < blocks_ && used_[cursor_]) ++cursor_;
    if (cursor_ >= blocks_) full();
    used_[cursor_] = true;
    ++allocated_;
    return cursor_++;
}

void Writer::writeBlock(uint32_t blockNo, const uint8_t* data) {
    write_(offset_ + uint64_t(blockNo) * blockSize, data, blockSize);
}

uint32_t Writer::indirect(const std::vector<uint32_t>& logical, int level, uint64_t start) {
    uint64_t span = 1;
    for (int i = 0; i < level; ++i) span *= perBlock;
    const uint64_t end = std::min<uint64_t>(start + span, logical.size());
    if (start >= end ||
        std::all_of(logical.begin() + ptrdiff_t(start), logical.begin() + ptrdiff_t(end), [](uint32_t b) { return !b; }))
        return 0;
    uint8_t data[blockSize]{};
    const uint64_t child = span / perBlock;
    for (uint32_t k = 0; k < perBlock && start + k * child < end; ++k) {
        uint32_t b;
        if (level == 1) {
            b = logical[size_t(start + k)];
        } else {
            b = indirect(logical, level - 1, start + k * child);
        }
        put32(data + 4 * k, b);
    }
    const uint32_t at = alloc();
    writeBlock(at, data);
    return at;
}

// Fills i_block for `logical` (0 = hole) and writes the indirect blocks; returns their count.
uint32_t Writer::mapBlocks(const std::vector<uint32_t>& logical, uint8_t* map) {
    memset(map, 0, 60);
    for (size_t i = 0; i < 12 && i < logical.size(); ++i) put32(map + 4 * i, logical[i]);
    const uint32_t before = allocated_;
    uint64_t start = 12;
    for (int level = 1; level <= 3; ++level) {
        put32(map + 4 * (11 + level), indirect(logical, level, start));
        uint64_t span = 1;
        for (int i = 0; i < level; ++i) span *= perBlock;
        start += span;
    }
    if (logical.size() > start) throw Error("A file is too large for ext3.");
    return allocated_ - before;
}

uint32_t Writer::beginFile(uint64_t size) {
    if (open_) throw Error("internal: a file is still open");
    if (size > uint64_t(blocks_) * blockSize) full();
    files_.push_back({size, {}});
    files_.back().blocks.reserve(size_t((size + blockSize - 1) / blockSize));
    open_ = true;
    openBytes_ = 0;
    partial_.clear();
    return uint32_t(files_.size() - 1);
}

void Writer::fileData(const uint8_t* data, size_t size) {
    auto& file = files_.back();
    if (!open_ || openBytes_ + size > file.size) throw Error("internal: file data past its size");
    openBytes_ += size;
    auto flush = [&](const uint8_t* b) {
        if (std::all_of(b, b + blockSize, [](uint8_t c) { return !c; })) {
            file.blocks.push_back(0);
            return;
        }
        const uint32_t at = alloc();
        writeBlock(at, b);
        file.blocks.push_back(at);
    };
    while (size) {
        if (partial_.empty() && size >= blockSize) {
            flush(data);
            data += blockSize;
            size -= blockSize;
            continue;
        }
        const size_t take = std::min<size_t>(size, blockSize - partial_.size());
        partial_.insert(partial_.end(), data, data + take);
        data += take;
        size -= take;
        if (partial_.size() == blockSize) {
            flush(partial_.data());
            partial_.clear();
        }
    }
}

void Writer::endFile() {
    auto& file = files_.back();
    if (!open_ || openBytes_ != file.size) throw Error("internal: file data short of its size");
    if (!partial_.empty()) {
        partial_.resize(blockSize, 0);
        if (std::all_of(partial_.begin(), partial_.end(), [](uint8_t c) { return !c; })) {
            file.blocks.push_back(0);
        } else {
            const uint32_t at = alloc();
            writeBlock(at, partial_.data());
            file.blocks.push_back(at);
        }
        partial_.clear();
    }
    open_ = false;
}

void Writer::finish(const std::vector<Node>& nodes) {
    if (open_) throw Error("internal: a file is still open");
    const uint32_t inodeCount = inodesCount();
    std::vector<Inode> inodes(firstIno + 1);
    auto tick = [&] {
        if (poll_) poll_();
    };

    auto& root = inodes[rootIno];
    root.mode = typeDir | 0755;
    root.atime = root.ctime = root.mtime = now_;
    auto& lostFound = inodes[lostFoundIno];
    lostFound.mode = typeDir | 0700;
    lostFound.atime = lostFound.ctime = lostFound.mtime = now_;

    auto apply = [](Inode& inode, const Node& node) {
        inode.mode = uint16_t((inode.mode & 0xf000) | (node.mode & 07777));
        inode.uid = node.uid;
        inode.gid = node.gid;
        inode.mtime = node.mtime;
        inode.atime = inode.ctime = 0;
    };

    // Inode numbers: root 2, lost+found 11, the rest from 12 in archive order.
    std::map<std::string, uint32_t> byPath{{"", rootIno}, {"lost+found", lostFoundIno}};
    std::vector<uint32_t> nodeIno(nodes.size(), 0);
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (node.path.empty() || node.path == "lost+found") {
            if (node.type != '5') throw Error("The archive's " + label_ + "/" + node.path + " must be a folder.");
            nodeIno[i] = node.path.empty() ? rootIno : lostFoundIno;
            apply(inodes[nodeIno[i]], node);
            continue;
        }
        if (node.type == '1') continue;
        if (inodes.size() > inodeCount) throw Error("The archive has too many files for the " + label_ + " partition.");
        const uint32_t ino = uint32_t(inodes.size());
        inodes.emplace_back();
        auto& inode = inodes.back();
        inode.mode = node.type == '5' ? typeDir : node.type == '2' ? typeLink : typeFile;
        apply(inode, node);
        nodeIno[i] = ino;
        byPath[node.path] = ino;
    }

    struct Entry {
        std::string name;
        uint32_t ino;
        uint8_t type;
    };
    std::map<uint32_t, std::vector<Entry>> dirs;
    std::map<uint32_t, uint32_t> parentOf{{rootIno, rootIno}, {lostFoundIno, rootIno}};
    dirs[rootIno].push_back({"lost+found", lostFoundIno, 2});
    dirs[lostFoundIno];
    auto parentIno = [&](const std::string& path, std::string& name) {
        const auto slash = path.rfind('/');
        const auto parent = slash == std::string::npos ? std::string() : path.substr(0, slash);
        name = slash == std::string::npos ? path : path.substr(slash + 1);
        const auto it = byPath.find(parent);
        if (it == byPath.end() || (inodes[it->second].mode & 0xf000) != typeDir)
            throw Error("The archive's " + label_ + "/" + path + " has no parent folder in the archive.");
        return it->second;
    };
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (node.type == '1' || node.path.empty() || node.path == "lost+found") continue;
        std::string name;
        const uint32_t parent = parentIno(node.path, name);
        const uint8_t type = node.type == '5' ? 2 : node.type == '2' ? 7 : 1;
        dirs[parent].push_back({name, nodeIno[i], type});
        if (node.type == '5') {
            dirs[nodeIno[i]];
            parentOf[nodeIno[i]] = parent;
        }
        inodes[nodeIno[i]].links = 1;
    }
    for (const auto& node : nodes) {
        if (node.type != '1') continue;
        const auto it = byPath.find(node.target);
        if (it == byPath.end() || (inodes[it->second].mode & 0xf000) != typeFile)
            throw Error("The archive's hard link " + label_ + "/" + node.path + " needs a regular file.");
        std::string name;
        const uint32_t parent = parentIno(node.path, name);
        dirs[parent].push_back({name, it->second, 1});
        auto& inode = inodes[it->second];
        if (++inode.links > linkMax) throw Error("The archive has too many hard links to one file.");
        apply(inode, node);
    }

    // Files, symlinks.
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (!nodeIno[i] || node.type == '5') continue;
        auto& inode = inodes[nodeIno[i]];
        if (node.type == '0') {
            const auto& file = files_.at(node.content);
            inode.size = file.size;
            const uint32_t meta = mapBlocks(file.blocks, inode.map);
            const auto data = std::count_if(file.blocks.begin(), file.blocks.end(), [](uint32_t b) { return b != 0; });
            inode.sectors = uint32_t((uint64_t(data) + meta) * 2);
        } else if (node.type == '2') {
            const auto& target = node.target;
            if (target.empty() || target.size() >= blockSize)
                throw Error("The archive's symlink " + label_ + "/" + node.path + " can't be stored on ext3.");
            inode.size = target.size();
            if (target.size() < 60) {
                memcpy(inode.map, target.data(), target.size());
            } else {
                uint8_t data[blockSize]{};
                memcpy(data, target.data(), target.size());
                const uint32_t at = alloc();
                writeBlock(at, data);
                put32(inode.map, at);
                inode.sectors = 2;
            }
        }
        tick();
    }

    // Folders: '.', '..', then entries in archive order.
    for (auto& [ino, entries] : dirs) {
        DirBlocks blocks;
        blocks.add(ino, ".", 2);
        blocks.add(parentOf.at(ino), "..", 2);
        uint32_t subdirs = 0;
        for (const auto& e : entries) {
            blocks.add(e.ino, e.name, e.type);
            if (e.type == 2) ++subdirs;
        }
        blocks.close();
        if (ino == lostFoundIno)
            while (blocks.blocks.size() < lostFoundBlocks) blocks.spare();
        std::vector<uint32_t> logical;
        for (const auto& b : blocks.blocks) {
            const uint32_t at = alloc();
            writeBlock(at, b.data());
            logical.push_back(at);
        }
        auto& inode = inodes[ino];
        inode.size = uint64_t(logical.size()) * blockSize;
        const uint32_t meta = mapBlocks(logical, inode.map);
        inode.sectors = uint32_t((logical.size() + meta) * 2);
        inode.links = 2 + subdirs;
        if (inode.links > linkMax) throw Error("The archive has too many folders in one folder.");
        tick();
    }

    auto& journal = inodes[journalIno];
    journal.mode = typeFile | 0600;
    journal.atime = journal.ctime = journal.mtime = now_;
    journal.links = 1;
    journal.size = uint64_t(journalBlocks_) * blockSize;
    journal.sectors = (journalBlocks_ + journalMeta_) * 2;
    memcpy(journal.map, journalMap_, 60);
    inodes[1].atime = inodes[1].ctime = inodes[1].mtime = now_;

    // Inode tables, bitmaps, group descriptors, superblocks.
    const uint32_t usedInodes = uint32_t(inodes.size() - 1);
    std::vector<uint8_t> gdt(size_t(gdtBlocks_) * blockSize, 0);
    uint32_t freeBlocks = 0;
    for (uint32_t g = 0; g < groups_; ++g) {
        const uint32_t start = groupStart(g);
        const uint32_t count = std::min(blocksPerGroup, blocks_ - start);
        const uint32_t bitmapAt = start + (hasSuper(g) ? 1 + gdtBlocks_ : 0);
        std::vector<uint8_t> table(size_t(tableBlocks_) * blockSize, 0);
        uint32_t dirsHere = 0, inodesHere = 0;
        uint8_t inodeBitmap[blockSize];
        memset(inodeBitmap, 0xff, sizeof inodeBitmap);
        for (uint32_t k = 0; k < inodesPerGroup_; ++k) {
            const uint32_t ino = g * inodesPerGroup_ + k + 1;
            if (ino > usedInodes) {
                inodeBitmap[k / 8] = uint8_t(inodeBitmap[k / 8] & ~(1u << (k % 8)));
                continue;
            }
            ++inodesHere;
            const auto& in = inodes[ino];
            if ((in.mode & 0xf000) == typeDir) ++dirsHere;
            uint8_t* p = table.data() + size_t(k) * inodeSize;
            put16(p, in.mode);
            put16(p + 2, in.uid);
            put32(p + 4, uint32_t(in.size));
            put32(p + 8, in.atime);
            put32(p + 12, in.ctime);
            put32(p + 16, in.mtime);
            put16(p + 24, in.gid);
            put16(p + 26, in.links);
            put32(p + 28, in.sectors);
            memcpy(p + 40, in.map, 60);
            put32(p + 108, uint32_t(in.size >> 32));
            put16(p + 120, in.uid >> 16);
            put16(p + 122, in.gid >> 16);
        }
        for (uint32_t b = 0; b < tableBlocks_; ++b) writeBlock(bitmapAt + 2 + b, table.data() + size_t(b) * blockSize);

        uint8_t blockBitmap[blockSize];
        memset(blockBitmap, 0xff, sizeof blockBitmap);
        uint32_t freeHere = 0;
        for (uint32_t k = 0; k < count; ++k)
            if (!used_[start + k]) {
                blockBitmap[k / 8] = uint8_t(blockBitmap[k / 8] & ~(1u << (k % 8)));
                ++freeHere;
            }
        freeBlocks += freeHere;
        writeBlock(bitmapAt, blockBitmap);
        writeBlock(bitmapAt + 1, inodeBitmap);

        uint8_t* d = gdt.data() + size_t(g) * 32;
        put32(d, bitmapAt);
        put32(d + 4, bitmapAt + 1);
        put32(d + 8, bitmapAt + 2);
        put16(d + 12, freeHere);
        put16(d + 14, inodesPerGroup_ - inodesHere);
        put16(d + 16, dirsHere);
        tick();
    }

    uint8_t sb[blockSize]{};
    put32(sb + 0, inodeCount);
    put32(sb + 4, blocks_);
    put32(sb + 12, freeBlocks);
    put32(sb + 16, inodeCount - usedInodes);
    put32(sb + 20, 1);
    put32(sb + 32, blocksPerGroup);
    put32(sb + 36, blocksPerGroup);
    put32(sb + 40, inodesPerGroup_);
    put32(sb + 48, now_);
    put16(sb + 54, 0xffff);
    put16(sb + 56, 0xef53);
    put16(sb + 58, 1);
    put16(sb + 60, 1);
    put32(sb + 64, now_);
    put32(sb + 76, 1);
    put32(sb + 84, firstIno);
    put16(sb + 88, inodeSize);
    put32(sb + 92, 0x4);         // has_journal
    put32(sb + 96, 0x2);         // filetype
    put32(sb + 100, 0x1 | 0x2);  // sparse_super, large_file
    memcpy(sb + 104, uuid_, 16);
    put32(sb + 224, journalIno);
    memcpy(sb + 236, hashSeed_, 16);
    sb[252] = 1;  // half_md4
    sb[253] = 1;  // journal blocks backed up below
    put32(sb + 256, 0x4 | 0x8);  // user_xattr, acl
    put32(sb + 264, now_);
    memcpy(sb + 268, journalMap_, 60);
    put32(sb + 268 + 60, uint32_t(journal.size >> 32));
    put32(sb + 268 + 64, uint32_t(journal.size));
    put32(sb + 352, 1);  // signed directory hash
    // Overhead as mke2fs counts it: the boot block, group metadata and the journal's data.
    uint32_t overhead = 1 + journalBlocks_;
    for (uint32_t g = 0; g < groups_; ++g) overhead += (hasSuper(g) ? 1 + gdtBlocks_ : 0) + 2 + tableBlocks_;
    put32(sb + 584, overhead);
    for (uint32_t g = 0; g < groups_; ++g) {
        if (!hasSuper(g)) continue;
        put16(sb + 90, g);
        const uint32_t start = groupStart(g);
        writeBlock(start, sb);
        for (uint32_t b = 0; b < gdtBlocks_; ++b) writeBlock(start + 1 + b, gdt.data() + size_t(b) * blockSize);
    }
}

}  // namespace frondExt3
