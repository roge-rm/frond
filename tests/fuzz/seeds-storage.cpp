// Seeds for the card and FAT targets, kept apart from seeds.cpp: the storage headers use miniz and
// GzipContext there uses zlib, and the two can't share a file.
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "storage/Container.h"
#include "storage/FatImage.h"
#include "SeedFdp.h"

using Bytes = std::vector<uint8_t>;

namespace {
Bytes Cat(Bytes a, const Bytes& b) { return a.insert(a.end(), b.begin(), b.end()), a; }
void Le32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
void Be16(Bytes& b, uint16_t v) { b.push_back(uint8_t(v >> 8)), b.push_back(uint8_t(v)); }
void Be32(Bytes& b, uint32_t v) { Be16(b, uint16_t(v >> 16)), Be16(b, uint16_t(v)); }

// A card made by FatImage itself: 1 MiB, a folder and two files.
class VectorBlocks : public frondStorage::Blocks {
   public:
    explicit VectorBlocks(size_t size) : bytes(size) {}
    int64_t length() const override { return int64_t(bytes.size()); }
    void read(int64_t at, char* b, int64_t n) override { std::memcpy(b, bytes.data() + at, size_t(n)); }
    void write(int64_t at, const char* b, int64_t n) override { std::memcpy(bytes.data() + at, b, size_t(n)); }
    void sync() override {}
    void commit(const frondStorage::Sectors& sectors) override {
        for (const auto& [at, sector] : sectors) write(at, reinterpret_cast<const char*>(sector.data()), int64_t(sector.size()));
    }
    Bytes bytes;
};
Bytes FatCard() {
    VectorBlocks card(1 << 20);
    frondStorage::FatImage::format(card);
    frondStorage::FatImage image(card);
    image.makeFolder("/", "PALM");
    auto fill = [](char* b, int64_t n) { return std::memset(b, 'f', size_t(n)), n; };
    image.add("/PALM", "Fuzz Long Name.pdb", fill, 5000);
    image.add("/", "A.TXT", fill, 10);
    return card.bytes;
}
}  // namespace

void StorageSeeds(const std::function<void(const std::string&, const std::vector<uint8_t>&)>& seed) {
    {
        const Bytes card = FatCard();
        // fat-image takes the card's start; the rest reads as zeros.
        size_t used = card.size();
        while (used > 512 && card[used - 1] == 0) --used;
        used = (used + 511) / 512 * 512;
        Fdp fat;
        fat.Bool(true).Int(card.size() / 512, 64, 8192).Int(used, 0, 1 << 20).Data(Bytes(card.begin(), card.begin() + used));
        for (unsigned op : {5u, 0u, 2u}) fat.Int(op, 0, 6);
        seed("fat-image", fat.Build());

        Bytes small(1024, 0x5a), file;
        frondSdFile::encode(small, file);
        seed("card-container", Fdp().Int(0, 0, 5).Bool(false).Int(file.size(), 0, 3 << 20).Data(file).Build());
        frondSdFile::encodeV2(small, 7, file);
        seed("card-container", Fdp().Int(0, 0, 5).Bool(false).Int(file.size(), 0, 3 << 20).Data(file).Build());
        // A journal with one record and its chunk table, a commit file and a pending list.
        Bytes journal{'F', 'R', 'O', 'N', 'D', 'J', 'N', 'L'};
        Le32(journal, 2), Le32(journal, 0);
        for (uint64_t v : {uint64_t(1024), uint64_t(7), uint64_t(8), uint64_t(1), uint64_t(1)}) Le32(journal, uint32_t(v)), Le32(journal, 0);
        journal.resize(64, 0);
        Le32(journal, 512), Le32(journal, 0), Le32(journal, 4), journal.insert(journal.end(), {1, 2, 3, 4});
        Le32(journal, 0);
        journal.insert(journal.end(), {'F', 'R', 'O', 'N', 'D', 'E', 'N', 'D', 0, 0, 0, 0, 0, 0, 0, 0});
        seed("card-container", Fdp().Int(1, 0, 5).Bool(true).Data(journal).Build());
        Bytes commit{'F', 'R', 'O', 'N', 'D', 'F', 'C', '1'};
        Be32(commit, 1), Be32(commit, 0), Be32(commit, 1024), Be32(commit, 0), Be32(commit, 16), Be32(commit, 2);
        commit.insert(commit.end(), {9, 9});
        commit.insert(commit.end(), {'F', 'C', 'E', 'N', 'D', 0, 0, 0, 0, 0, 0, 0});
        seed("card-container", Fdp().Int(2, 0, 5).Bool(true).Data(commit).Build());
        Bytes pending{'F', 'R', 'O', 'N', 'D', 'J', 'P', '1'};
        Le32(pending, 1024), Le32(pending, 0), Le32(pending, 7), Le32(pending, 0), Le32(pending, 0);
        seed("card-container", Fdp().Int(3, 0, 5).Bool(false).Data(pending).Build());
        seed("card-container", Fdp().Int(4, 0, 5).Bool(false).Int(0, 0, 2).Data(Bytes(4096, 0xff)).Build());
        frondSdFile::encodeV2(card, 1, file);
        seed("card-container", Fdp().Int(5, 0, 5).Bool(false).Int(file.size(), 0, 3 << 20).Data(file).Bool(false).Bool(true)
                                   .Int(5, 0, 6).Int(0, 0, 6).Build());
    }

}
