#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>
#include "EmHAL.h"
#include "ExternalStorage.h"
#include "RuntimeCards.h"

// Stick writes the guest made reach the file when the machine quits without a save.
static std::string ManifestFor(const std::string& path, const std::string& key, uint64_t stored) {
    struct stat st{};
    assert(!stat(path.c_str(), &st));
    std::ostringstream out;
    out << "frond-linked-sd 3\n1 ms " << std::quoted(path) << " " << key << " "
        << std::stoul(key.substr(0, 8), nullptr, 16) << " " << stored << " 0 0 " << st.st_dev << " " << st.st_ino << " "
        << uint64_t(st.st_mtim.tv_sec) * 1000000000ull + st.st_mtim.tv_nsec << " sd 0\n";
    return out.str();
}

struct StickSlotHal : EmHALHandler {
    bool SupportsImageInSlot(EmHAL::Slot slot, uint32 blocks) override { return slot == EmHAL::Slot::memorystick && blocks > 0; }
};

int main() {
    StickSlotHal hal;
    EmHAL::AddHandler(&hal);
    const auto dir = std::filesystem::temp_directory_path() / ("runtime-cards-ms-" + std::to_string(getpid()));
    std::filesystem::create_directories(dir);
    const std::string path = (dir / "stick.frondms").string();
    const std::string key = "0123456789abcdef0123456789abcdef";
    std::vector<uint8_t> blank = frondMs::blank(*frondMsGeometry::byMegabytes(4)), image0;
    assert(frondMs::storedOf(blank, image0));
    const uint64_t stored = image0.size();
    std::array<uint8_t, frondMachine::identityBytes> identity{};
    identity[0] = 1;
    assert(frondSession::writeSdFromReader(path, stored,
        [&](uint64_t off, uint64_t n, void* out) { memcpy(out, image0.data() + off, size_t(n)); return true; },
        identity, 1, frondSession::msShape(stored)));
    {
        // The first generation root, as a saved session would have left.
        FileCardOptions options; options.rejectInvalidRoot = true; options.identity = key;
        auto backing = FileCardBacking::Open(path, stored, frondSdFile::headerBytes, options);
        assert(backing && backing->Commit());
    }
    const auto manifest = ManifestFor(path, key, stored);
    const size_t offset = size_t(stored / 2);
    uint8_t first[64], second[64];
    memset(first, 0x5a, sizeof first);
    memset(second, 0xa5, sizeof second);

    // Two writes close together: the second one waits for the idle commit or the exit.
    {
        RuntimeCards cards;
        cards.SetDevice(deviceTypeT415);
        RuntimeCards::Pair pair;
        auto error = cards.Stage(manifest, pair);
        if (!error.empty()) { std::cerr << error << std::endl; return 1; }
        cards.Install(pair);
        auto* image = gExternalStorage.GetImage(key);
        assert(image);
        assert(image->WriteByteRange(first, offset, sizeof first));
        assert(image->WriteByteRange(second, offset + 4096, sizeof second));
        cards.Install(RuntimeCards::Pair{});
    }
    {
        RuntimeCards cards;
        cards.SetDevice(deviceTypeT415);
        RuntimeCards::Pair pair;
        std::string again = ManifestFor(path, key, stored);
        auto error = cards.Stage(again, pair);
        if (!error.empty()) { std::cerr << error << std::endl; return 1; }
        cards.Install(pair);
        auto* image = gExternalStorage.GetImage(key);
        uint8_t got[64];
        assert(image->ReadByteRange(got, offset, sizeof got) && !memcmp(got, first, sizeof got));
        assert(image->ReadByteRange(got, offset + 4096, sizeof got) && !memcmp(got, second, sizeof got));
        // The idle commit settles a write once the guest leaves the stick alone.
        memset(got, 0x33, sizeof got);
        assert(image->WriteByteRange(got, offset + 8192, sizeof got));
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        cards.Service();
        cards.Install(RuntimeCards::Pair{});
    }
    std::filesystem::remove_all(dir);
    puts("runtime-cards-ms ok");
    return 0;
}
