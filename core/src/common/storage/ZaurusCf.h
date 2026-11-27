#ifndef FROND_STORAGE_ZAURUS_CF_H
#define FROND_STORAGE_ZAURUS_CF_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The SL-C3000 internal CF (Microdrive) image from a Sharp HDD1 archive, as
// tools/create-zaurus-cf.py makes it with mke2fs and mtools: an MBR, hdd1 ext3 (sectors 63 to
// 205632) from the archive, hdd2 ext3 (to 1028160) and hdd3 FAT32 (to the end) from its
// .sys/hdimage2.tgz factory seed, empty LC Font files filled from a CS0 dump. No host tools.
namespace frondZaurusCf {

constexpr uint64_t sectorBytes = 512;
constexpr uint64_t stockSectors = 7'999'488;
constexpr uint64_t hdd1Start = 63, hdd2Start = 205'632, hdd3Start = 1'028'160;

// 4 is the stock 4,095,737,856 bytes, larger sizes are decimal GB. 0 when out of 4 to 32.
uint64_t imageSectors(int sizeGb);

// Writes `size` bytes at `offset`. Never called for all-zero ranges: the output must read as
// zeros wherever nothing was written. Throws frondStorage::Error to stop.
using WriteFn = std::function<void(uint64_t offset, const uint8_t* data, size_t size)>;

struct Request {
    int hdd1Fd{-1};        // the HDD1 archive (.tar, .tar.gz, .tgz), read once from where it is
    int64_t hdd1Size{-1};  // its size for progress, or -1
    int fontRomFd{-1};     // optional 8 MiB SL-C3000 CS0 dump for the empty LC Font files
    bool requireFonts{};   // fail unless every LC Font file ends up filled
    int sizeGb{4};
    // Input bytes read so far against hdd1Size (or 0); called often, from the builder's thread.
    std::function<void(uint64_t done, uint64_t total)> progress;
    // Polled as it runs; true stops it with frondStorage::Cancelled.
    std::function<bool()> cancelled;
};

// Builds into `write`. Throws frondStorage::Error with a message for the user (Cancelled when
// stopped); what was written by then is incomplete and should be deleted.
void build(const Request& request, const WriteFn& write);

// Builds into an empty file descriptor, sets its size and leaves the zero ranges as holes.
void buildToFd(const Request& request, int outFd);

// `frond --zaurus-cf --hdd1 FILE [--size-gb N] [--font-rom FILE] [--require-fonts] --output FILE`:
// the script's options; the output must not exist. Prints the error and returns 1 on failure.
int cliMain(const std::vector<std::string>& args);

}  // namespace frondZaurusCf

#endif
