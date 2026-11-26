// FAT12/16 cards as the card manager and the file transfer open them: the boot sector, partition
// table, FAT and folders, then file reads and edits. The card is sparse: the input's bytes from 0,
// zeros after, to a length the input picks, so the layout's fields can point anywhere on a large card.
#include <fuzzer/FuzzedDataProvider.h>

#include <map>

#include "FatWalk.h"

namespace {
class SparseBlocks : public frondStorage::Blocks {
   public:
    SparseBlocks(std::vector<uint8_t> start, int64_t size) : start(std::move(start)), size(size) {}
    int64_t length() const override { return size; }
    void read(int64_t at, char* b, int64_t n) override {
        if (at < 0 || n < 0 || at + n > size) throw frondStorage::Error("The card is shorter than its file system says.");
        for (int64_t i = 0; i < n; ++i) {
            const int64_t p = at + i;
            auto it = written.find(p / 512);
            b[i] = char(it != written.end() ? it->second[size_t(p % 512)] : size_t(p) < start.size() ? start[size_t(p)] : 0);
        }
    }
    void write(int64_t at, const char* b, int64_t n) override {
        if (at < 0 || n < 0 || at + n > size) throw frondStorage::Error("Couldn't write the card image.");
        for (int64_t i = 0; i < n; ++i) {
            const int64_t p = at + i;
            auto& sector = written[p / 512];
            if (sector.empty()) {
                sector.resize(512);
                for (int j = 0; j < 512; ++j) {
                    const int64_t q = p / 512 * 512 + j;
                    sector[size_t(j)] = size_t(q) < start.size() ? start[size_t(q)] : 0;
                }
            }
            sector[size_t(p % 512)] = uint8_t(b[i]);
        }
    }
    void sync() override {}
    void commit(const frondStorage::Sectors& sectors) override {
        for (const auto& [at, sector] : sectors) write(at, reinterpret_cast<const char*>(sector.data()), int64_t(sector.size()));
    }

   private:
    std::vector<uint8_t> start;
    std::map<int64_t, std::vector<uint8_t>> written;
    int64_t size;
};
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    FuzzedDataProvider data(bytes, size);
    // 32 KiB to 2 GiB in sectors, most of them small.
    const int64_t sectors = data.ConsumeBool() ? data.ConsumeIntegralInRange<int64_t>(64, 8192)
                                               : data.ConsumeIntegralInRange<int64_t>(64, 4194304);
    auto start = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(0, 1 << 20));
    SparseBlocks blocks(std::move(start), sectors * 512);
    FuzzFat(blocks, data);
    return 0;
}
