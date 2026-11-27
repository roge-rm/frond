#ifndef FROND_STORAGE_EXT3_WRITER_H
#define FROND_STORAGE_EXT3_WRITER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// A new ext3 file system as `mke2fs -t ext3 -I 128 -m 0 -b 1024 -O
// none,has_journal,filetype,sparse_super,large_file -d archive.tar` makes it: 1 KiB blocks,
// 8192 blocks a group, an inode per 4 KiB, a 4 or 8 MiB journal, lost+found, inodes with the
// archive's mode, owner and mtime (atime and ctime 0), all-zero file blocks left as holes.
// File data is written as it streams in; folders, inodes and the rest come at the end.
namespace frondExt3 {

// Writes whole blocks at a byte offset of the image.
using WriteFn = std::function<void(uint64_t offset, const uint8_t* data, size_t size)>;

struct Node {
    std::string path;  // relative to the partition, "a/b"; "" is the root
    char type{'0'};    // '0' file, '5' folder, '2' symlink, '1' hard link to `target`
    uint32_t mode{};   // permission bits, 07777
    uint32_t uid{}, gid{}, mtime{};
    uint32_t content{};  // beginFile's handle for a file
    std::string target;  // symlink text or the hard link's path
};

class Writer {
   public:
    // `offset` is the partition's first byte in the image, `blocks` its size in 1 KiB blocks.
    // `label` names it in messages ("hdd1"). `poll` runs every so often and may throw.
    Writer(WriteFn write, uint64_t offset, uint64_t blocks, uint32_t now, std::string label,
           std::function<void()> poll = {});

    uint32_t beginFile(uint64_t size);
    void fileData(const uint8_t* data, size_t size);
    void endFile();

    // Lays out `nodes` (parents before or after children, hard links after their files).
    void finish(const std::vector<Node>& nodes);

    uint32_t blocksCount() const { return blocks_; }
    uint32_t inodesCount() const { return inodesPerGroup_ * groups_; }

   private:
    struct File {
        uint64_t size{};
        std::vector<uint32_t> blocks;  // 0 for a hole
    };
    struct Inode;

    uint32_t alloc();
    void writeBlock(uint32_t block, const uint8_t* data);
    uint32_t mapBlocks(const std::vector<uint32_t>& logical, uint8_t* iblock);
    uint32_t indirect(const std::vector<uint32_t>& logical, int level, uint64_t start);
    bool hasSuper(uint32_t group) const;
    uint32_t groupStart(uint32_t group) const { return 1 + group * blocksPerGroup; }
    [[noreturn]] void full() const;

    static constexpr uint32_t blockSize = 1024, blocksPerGroup = 8192, inodeSize = 128;

    WriteFn write_;
    uint64_t offset_;
    uint32_t blocks_{}, groups_{}, inodesPerGroup_{}, tableBlocks_{}, gdtBlocks_{};
    uint32_t now_;
    std::string label_;
    std::function<void()> poll_;
    std::vector<bool> used_;  // by block number
    uint32_t cursor_{}, allocated_{};
    std::vector<File> files_;
    bool open_{};
    uint64_t openBytes_{};
    std::vector<uint8_t> partial_;
    uint32_t journalBlocks_{}, journalMeta_{};
    uint8_t journalMap_[60]{};
    uint8_t uuid_[16]{}, hashSeed_[16]{};
};

}  // namespace frondExt3

#endif
