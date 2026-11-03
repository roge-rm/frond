// Layout checks for the SD images frondSd::create makes, 32 MiB to 2 GB (1886 MiB).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include "SdImage.h"

static uint32_t g16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t g32(const uint8_t* p) { return g16(p) | (uint32_t(g16(p + 2)) << 16); }

static uint64_t fnv(const std::vector<uint8_t>& bytes) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : bytes) { h ^= b; h *= 1099511628211ull; }
    return h;
}

static bool mtools() { return system("command -v mdir >/dev/null 2>&1 && command -v minfo >/dev/null 2>&1") == 0; }

// Writes only the sectors with data, so a 2 GB image stays sparse on disk.
static void save(const std::vector<uint8_t>& image, const char* path) {
    FILE* f = fopen(path, "wb");
    assert(f);
    static const uint8_t zero[512] = {};
    for (size_t at = 0; at < image.size(); at += 512)
        if (memcmp(image.data() + at, zero, 512)) { assert(fseeko(f, at, SEEK_SET) == 0); assert(fwrite(image.data() + at, 512, 1, f) == 1); }
    assert(ftruncate(fileno(f), image.size()) == 0);
    fclose(f);
}

static void check(unsigned mib) {
    const auto image = frondSd::create(mib);
    const uint32_t total = image.size() / 512;
    assert(image.size() == size_t(mib) << 20 && frondSd::validSize(image.size()));
    assert(image[510] == 0x55 && image[511] == 0xaa);
    const uint8_t* part = image.data() + 446;
    assert(part[4] == 6 && g32(part + 8) == 63 && g32(part + 12) == total - 63);
    const uint8_t* boot = image.data() + 63 * 512;
    assert(boot[510] == 0x55 && boot[511] == 0xaa && memcmp(boot + 54, "FAT16   ", 8) == 0);
    const unsigned cluster = boot[13], reserved = g16(boot + 14), fatSectors = g16(boot + 22);
    assert(g16(boot + 11) == 512 && boot[16] == 2 && g16(boot + 17) == 512);
    assert(g32(boot + 28) == 63);
    assert((g16(boot + 19) ? g16(boot + 19) : g32(boot + 32)) == total - 63);
    const unsigned expectCluster = mib <= 64 ? 4 : mib <= 256 ? 8 : mib == 512 ? 16 : mib == 1024 ? 32 : 64;
    assert(cluster == expectCluster);
    const uint32_t root = 63 + reserved + 2 * fatSectors, dataStart = root + 32;
    const uint32_t clusters = (total - dataStart) / cluster;
    assert(clusters >= 4085 && clusters <= 65524);
    assert(fatSectors * 256u >= clusters + 2 && (fatSectors - 1) * 256u < clusters + 2);
    assert(dataStart + clusters * cluster <= total);
    if (mib <= 128) {
        assert(reserved == 1);
    } else {
        assert(dataStart % cluster == 0 && dataStart + clusters * cluster == total);
    }
    // A real 2 GB card: 3,862,528 sectors.
    if (mib == 1886) assert(total == 3862528 && reserved == 9 && dataStart == 576 && clusters == 60343 && fatSectors == 236);
    for (unsigned fat = 0; fat < 2; fat++) {
        const uint8_t* p = image.data() + (63 + reserved + fat * fatSectors) * 512;
        assert(g16(p) == 0xfff8 && g16(p + 2) == 0xffff);
    }
    assert(memcmp(image.data() + root * 512, "FROND      ", 11) == 0 && image[root * 512 + 11] == 8);
    if (mtools()) {
        const std::string path = "build/test-sd-image-" + std::to_string(mib) + ".img";
        save(image, path.c_str());
        const std::string spec = "-i " + path + "@@32256";
        assert(system(("minfo " + spec + " :: >/dev/null").c_str()) == 0);
        assert(system(("mdir " + spec + " :: >/dev/null").c_str()) == 0);
        unlink(path.c_str());
    } else {
        puts("mtools not found, skipped the mdir check");
    }
}

int main() {
    // The original 32 to 128 MiB images must not change.
    const struct { unsigned mib; uint64_t hash; } known[] = {
        {32, 0x068861bfccbbf53bull}, {64, 0x632f670675b37c33ull}, {128, 0xde040cbddb8a1ac7ull}};
    for (const auto& k : known) assert(fnv(frondSd::create(k.mib)) == k.hash);
    for (unsigned mib : {32u, 64u, 128u, 256u, 512u, 1024u, 1886u}) check(mib);
    for (unsigned mib : {0u, 16u, 96u, 2048u, 4096u}) {
        bool refused = false;
        try { frondSd::create(mib); } catch (const std::runtime_error&) { refused = true; }
        assert(refused);
    }
    assert(frondSd::validSize(size_t(2048) << 20) && !frondSd::validSize((size_t(2048) << 20) + 512));
    puts("PASS: SD images 32 MiB to 2 GB (1886 MiB)");
}
