#ifndef FROND_STORAGE_FAT32_WRITER_H
#define FROND_STORAGE_FAT32_WRITER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// A new FAT32 file system with mformat's geometry for `mformat -F -T sectors -h 16 -s 63 -H
// hidden`: 32 reserved sectors, two FATs, root at cluster 2, FSInfo at 1, backup boot at 6, and
// mformat's cluster size (8 sectors, doubled while there would be over 2^21 clusters). Files
// get long names and Frond's 8.3 aliases (storage/FatNames.h), DOS times in local time. File
// data is written as it streams in; folders and the FATs come at the end.
namespace frondFat32 {

using WriteFn = std::function<void(uint64_t offset, const uint8_t* data, size_t size)>;

struct Node {
    std::string path;  // "a/b", UTF-8 as the archive has it
    bool dir{};
    int64_t mtime{};
    uint32_t content{};  // beginFile's handle for a file
};

struct Geometry {
    uint32_t sectors{}, clusterSectors{}, fatSectors{}, clusters{};
    static constexpr uint32_t reserved = 32;
    uint32_t dataStart() const { return reserved + 2 * fatSectors; }
};
Geometry geometry(uint32_t sectors);

class Writer {
   public:
    // `offset` is the partition's first byte in the image, `sectors` its size, `hidden` its
    // first sector on the disk. `now` stamps folders the archive doesn't list.
    Writer(WriteFn write, uint64_t offset, uint32_t sectors, uint32_t hidden, int64_t now,
           std::function<void()> poll = {});

    uint32_t beginFile(uint64_t size);
    void fileData(const uint8_t* data, size_t size);
    void endFile();

    // Folders the archive skips are made; children of a file are refused.
    void finish(const std::vector<Node>& nodes);

    const Geometry& layout() const { return geo_; }

   private:
    struct File {
        uint64_t size{};
        uint32_t first{};
    };
    uint32_t allocRun(uint32_t count);
    uint64_t clusterOffset(uint32_t cluster) const;
    [[noreturn]] void full() const;

    WriteFn write_;
    uint64_t offset_;
    uint32_t hidden_;
    int64_t now_;
    std::function<void()> poll_;
    Geometry geo_;
    uint32_t clusterBytes_{};
    uint32_t cursor_{3};
    std::vector<uint32_t> fat_;  // by cluster, up to the last one used
    std::vector<File> files_;
    bool open_{};
    uint64_t openBytes_{};
};

}  // namespace frondFat32

#endif
