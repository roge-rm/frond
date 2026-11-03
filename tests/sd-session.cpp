#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <ctime>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include "session/session_file5.h"
#include "CardSave.h"
#include "SdCards.h"
#include "SessionIo.h"
#include "SdImage.h"

namespace fs = std::filesystem;

static long PeakKiB() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss;
}

// Sectors the large-card checks write: first, last and either side of the 1 MiB chunk edges.
static std::vector<uint32_t> Probes(uint64_t bytes) {
    const uint32_t last = uint32_t(bytes / 512 - 1);
    return {0, 1, 2047, 2048, 4095, 4096, last / 2, last - 1, last};
}

static std::vector<uint8_t> Pattern(uint32_t sector) {
    std::vector<uint8_t> bytes(512);
    for (unsigned i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(sector * 31 + i * 7 + 1);
    return bytes;
}

static fs::path Candidate(const fs::path& folder) {
    fs::path found;
    for (const auto& entry : fs::directory_iterator(folder))
        if (frondSdCandidate::candidateName(entry.path().filename().string())) found = entry.path();
    return found;
}

// Same card steps as frondNative::saveMachine, without a SoC.
static bool SaveCard(const fs::path& folder) {
    frondSession::Contents contents;
    contents.deviceName = "palm-tx";
    contents.displayMode = 1;
    contents.ramSize = 32u << 20;
    contents.nor = {1, 2, 3, 4};
    contents.memory = {5, 6, 7};
    contents.savestate = {8, 9};
    std::vector<fs::path> temps;
    frondMachine::Media media;
    frondNative::SdSave sd;
    if (!frondNative::beginSdSave(folder, media, true, sd)) return false;
    temps.emplace_back();
    contents.media.push_back(media);
    if (!frondSession::saveFolderFromTemps(folder, contents, temps)) {
        frondNative::abandonSdSave(sd);
        return false;
    }
    return frondNative::completeSdSave(folder, sd);
}

static double Seconds() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

// 1 MiB of changes: 256 pages of 4 KiB spread over the card.
static std::vector<uint32_t> DirtySectors(uint64_t bytes) {
    std::vector<uint32_t> sectors;
    const uint64_t stride = bytes / 256 / 4096 * 8;
    for (uint32_t page = 0; page < 256; ++page)
        for (uint32_t s = 0; s < 8; ++s) sectors.push_back(uint32_t(page * stride + 8 + s));
    return sectors;
}

static bool AttachFolderCard(const fs::path& folder, uint64_t bytes) {
    frondSession::Contents contents;
    fs::path loaded;
    if (!frondSession::loadPath(folder, contents, loaded) || contents.media.size() != 1) return false;
    char id[SD_CARD_ID_MAX_LEN + 1]{};
    std::memcpy(id, contents.media[0].identity.data(), SD_CARD_ID_MAX_LEN);
    return sdCardInitializeFromFile((folder / frondMachine::sdSibling).c_str(), bytes, frondSdFile::headerBytes, id);
}

// Child: fresh load of a saved large card, 1 MiB of writes, then an incremental save.
static int IncrementalSave(const fs::path& folder, unsigned mib) {
    const uint64_t bytes = uint64_t(mib) << 20;
    const auto sidecar = folder / frondMachine::sdSibling;
    struct stat before {};
    assert(stat(sidecar.c_str(), &before) == 0);
    const double loadStart = Seconds();
    assert(AttachFolderCard(folder, bytes));
    const double loadTime = Seconds() - loadStart;
    const long loadPeak = PeakKiB();
    for (auto sector : DirtySectors(bytes)) {
        const auto pattern = Pattern(sector);
        assert(sdCardWrite(sector, pattern.data()));
    }
    const double saveStart = Seconds();
    assert(SaveCard(folder));
    const double saveTime = Seconds() - saveStart;
    // Applied in place: same file, next generation, no journal left.
    struct stat after {};
    assert(stat(sidecar.c_str(), &after) == 0 && after.st_ino == before.st_ino);
    assert(frondSession::sdJournals(folder).empty());
    std::printf("incremental save %u MiB with 1 MiB dirty: %.3f s, fresh load %.3f s (peak %ld KiB), peak RSS %ld KiB\n",
                mib, saveTime, loadTime, loadPeak, PeakKiB());
    return 0;
}

// Child: fresh load reads back the incremental writes and checks the whole sidecar.
static int CheckIncremental(const fs::path& folder, unsigned mib) {
    const uint64_t bytes = uint64_t(mib) << 20;
    const double start = Seconds();
    assert(AttachFolderCard(folder, bytes));
    const double loadTime = Seconds() - start;
    std::vector<uint8_t> sector(512);
    for (auto s : DirtySectors(bytes)) {
        assert(sdCardRead(s, sector.data()));
        assert(sector == Pattern(s));
    }
    frondMachine::Media media;
    media.kind = frondMachine::kindSd;
    media.name = frondMachine::sdSibling;
    media.logicalSize = media.physicalSize = bytes;
    frondSdFile::Info info;
    assert(frondMachine::readSdHeader(folder / frondMachine::sdSibling, info) == frondMachine::SiblingStatus::ok);
    media.contentGeneration = info.generation;
    assert(info.version == 2 && info.generation == 2);
    assert(frondMachine::checkSdSibling(folder / frondMachine::sdSibling, media, true) == frondMachine::SiblingStatus::ok);
    std::printf("fresh load %u MiB after incremental save: %.3f s, peak RSS %ld KiB\n", mib, loadTime, PeakKiB());
    return 0;
}

// Child: create a card in `folder`, write the probe sectors and save in place.
static int CreateAndSave(const fs::path& folder, unsigned mib) {
    const uint64_t bytes = uint64_t(mib) << 20;
    uint64_t lastDone = 0;
    const auto made = frondNative::createSdCard(folder, deviceTypeTX, mib, [&](uint64_t done, uint64_t total) {
        assert(total == bytes && done >= lastDone && done <= total);
        lastDone = done;
        return true;
    });
    assert(made.ok);
    assert(lastDone == bytes);
    const auto candidate = Candidate(folder);
    assert(!candidate.empty() && sdCardBackedBy(candidate.c_str()));
    assert(sdCardSectorCount() == bytes / 512);
    // Random identity, 32 hex digits.
    const std::string id = sdCardGetId();
    assert(id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos);
    // The candidate is a complete .frondsd: header, size and CRC check out.
    frondMachine::Media expect;
    expect.kind = frondMachine::kindSd;
    expect.name = frondMachine::sdSibling;
    expect.logicalSize = expect.physicalSize = bytes;
    assert(frondMachine::checkSdSibling(candidate, expect) == frondMachine::SiblingStatus::ok);
    // Fresh FAT16 layout as SdImage.h makes it.
    std::vector<uint8_t> sector(512);
    for (const auto& want : frondSd::layout(mib)) {
        assert(sdCardRead(want.index, sector.data()));
        assert(!memcmp(sector.data(), want.bytes.data(), 512));
    }
    for (auto probe : Probes(bytes)) {
        const auto pattern = Pattern(probe);
        assert(sdCardWrite(probe, pattern.data()));
    }
    assert(!fs::exists(folder / frondMachine::sdSibling));
    assert(SaveCard(folder));
    // The card moved onto the published sidecar and the candidate went.
    assert(sdCardBackedBy((folder / frondMachine::sdSibling).c_str()));
    assert(Candidate(folder).empty());
    for (const auto& entry : fs::directory_iterator(folder))
        assert(entry.path().filename().string().find(".frondsd.tmp-") == std::string::npos);
    std::printf("create+save %u MiB: peak RSS %ld KiB\n", mib, PeakKiB());
    return 0;
}

// Child: a fresh process loads the folder and reads back what the first one wrote.
static int Reload(const fs::path& folder, unsigned mib) {
    const uint64_t bytes = uint64_t(mib) << 20;
    frondSession::Contents contents;
    fs::path loaded;
    assert(frondSession::loadPath(folder, contents, loaded));
    assert(contents.media.size() == 1 && contents.media[0].physicalSize == bytes);
    char id[SD_CARD_ID_MAX_LEN + 1]{};
    std::memcpy(id, contents.media[0].identity.data(), SD_CARD_ID_MAX_LEN);
    assert(std::strlen(id) == 32);
    assert(sdCardInitializeFromFile((folder / frondMachine::sdSibling).c_str(), bytes,
                                    frondSdFile::headerBytes, id));
    std::vector<uint8_t> sector(512);
    for (auto probe : Probes(bytes)) {
        assert(sdCardRead(probe, sector.data()));
        assert(sector == Pattern(probe));
    }
    assert(sdCardRead(5000, sector.data()) && sector == std::vector<uint8_t>(512, 0));
    // Streamed export of the raw sectors.
    const auto raw = folder / "export.img";
    assert(frondNative::exportSd(raw).ok);
    assert(fs::file_size(raw) == bytes);
    FILE* in = std::fopen(raw.c_str(), "rb");
    assert(in);
    for (auto probe : Probes(bytes)) {
        assert(!fseeko(in, off_t(probe) * 512, SEEK_SET) && std::fread(sector.data(), 1, 512, in) == 512);
        assert(sector == Pattern(probe));
    }
    std::fclose(in);
    fs::remove(raw);
    std::printf("reload+export %u MiB: peak RSS %ld KiB\n", mib, PeakKiB());
    return 0;
}

// Child: stream a raw image in, check bounds and the folder rule.
static int Import(const fs::path& folder) {
    const unsigned mib = 256;
    const uint64_t bytes = uint64_t(mib) << 20;
    const auto raw = folder / "source.img";
    {
        FILE* out = std::fopen(raw.c_str(), "wb");
        assert(out);
        for (auto probe : Probes(bytes)) {
            const auto pattern = Pattern(probe);
            assert(!fseeko(out, off_t(probe) * 512, SEEK_SET) && std::fwrite(pattern.data(), 1, 512, out) == 512);
        }
        std::fclose(out);
    }
    assert(fs::file_size(raw) == bytes);
    // No machine folder: refused, nothing changes.
    auto refused = frondNative::loadSdImage({}, deviceTypeTX, raw);
    assert(!refused.ok && refused.code == "folder" && !sdCardInitialized());
    refused = frondNative::createSdCard({}, deviceTypeTX, 256);
    assert(!refused.ok && refused.code == "folder" && !sdCardInitialized());
    assert(frondNative::loadSdImage(folder, deviceTypeTX, raw).ok);
    const auto candidate = Candidate(folder);
    assert(!candidate.empty() && sdCardBackedBy(candidate.c_str()));
    std::vector<uint8_t> sector(512);
    for (auto probe : Probes(bytes)) {
        assert(sdCardRead(probe, sector.data()));
        assert(sector == Pattern(probe));
    }
    const long peak = PeakKiB();
    // Replacing the card drops its candidate.
    assert(frondNative::createSdCard(folder, deviceTypeTX, 32).ok);
    assert(Candidate(folder).empty() && !sdCardFileBacked());
    // Bounds: not whole sectors, past the profile.
    fs::resize_file(raw, bytes + 1);
    assert(frondNative::loadSdImage(folder, deviceTypeTX, raw).code == "size");
    fs::resize_file(raw, uint64_t(1024 + 512) << 20);
    assert(frondNative::loadSdImage(folder, deviceTypeE2, raw).code == "size");
    assert(Candidate(folder).empty());
    fs::remove(raw);
    std::printf("import %u MiB: peak RSS %ld KiB\n", mib, peak);
    return 0;
}

static void RunChild(const char* self, const std::vector<std::string>& args, long maxKiB) {
    std::string command = std::string(self);
    for (const auto& a : args) command += " '" + a + "'";
    command += " > build/sd-session-child.txt";
    assert(std::system(command.c_str()) == 0);
    FILE* in = std::fopen("build/sd-session-child.txt", "r");
    char line[256]{};
    assert(in && std::fgets(line, sizeof(line), in));
    std::fclose(in);
    std::fputs(line, stdout);
    const char* at = std::strstr(line, "peak RSS ");
    assert(at);
    const long kib = std::atol(at + 9);
    assert(kib > 0 && kib < maxKiB);
}

int main(int argc, char** argv) {
    if (argc == 4 && !std::strcmp(argv[1], "create")) return CreateAndSave(argv[2], unsigned(std::atoi(argv[3])));
    if (argc == 4 && !std::strcmp(argv[1], "reload")) return Reload(argv[2], unsigned(std::atoi(argv[3])));
    if (argc == 3 && !std::strcmp(argv[1], "import")) return Import(argv[2]);
    if (argc == 4 && !std::strcmp(argv[1], "incremental")) return IncrementalSave(argv[2], unsigned(std::atoi(argv[3])));
    if (argc == 4 && !std::strcmp(argv[1], "check")) return CheckIncremental(argv[2], unsigned(std::atoi(argv[3])));
    uint8_t nor[] = {1,2,3}, ram[] = {4,5,6,7}, state[] = {8,9};
    std::vector<uint8_t> card(512);
    for (unsigned i=0;i<card.size();i++) card[i]=i*17;
    SessionFile5 source;
    source.SetDeviceName("palm-tx").SetRamSize(32u<<20).SetNor(sizeof(nor),nor)
        .SetMemory(sizeof(ram),ram).SetSavestate(sizeof(state),state).SetSd(card.size(),card.data());
    assert(source.SerializeAs(6));
    std::vector<uint8_t> saved(static_cast<const uint8_t*>(source.GetSerializedSession()),
                               static_cast<const uint8_t*>(source.GetSerializedSession())+source.GetSerializedSessionSize());
    assert(saved[4]==6);
    SessionFile5 decoded;
    assert(decoded.Deserialize(saved.size(),saved.data()));
    assert(decoded.GetSdSize()==card.size() && !memcmp(decoded.GetSd(),card.data(),card.size()));
    assert(decoded.GetMemorySize()==sizeof(ram) && !memcmp(decoded.GetMemory(),ram,sizeof(ram)));
    auto broken=saved;broken.back()^=1;
    assert(!decoded.Deserialize(broken.size(),broken.data()));
    assert(!decoded.Deserialize(saved.size()-1,saved.data()));
    // An oversized SD TOC must be rejected before allocating its claimed payload.
    broken=saved;for(int i=68;i<72;i++) broken[i]=255;
    assert(!decoded.Deserialize(broken.size(),broken.data()));
    source.SetSd(0,nullptr);assert(source.SerializeAs(6));
    assert(static_cast<const uint8_t*>(source.GetSerializedSession())[4]==6);
    assert(decoded.Deserialize(source.GetSerializedSessionSize(),source.GetSerializedSession()));
    assert(decoded.GetSdSize()==0 && decoded.GetSd()==nullptr);
    source.SetSd(513,card.data());assert(!source.SerializeAs(6));
    for(unsigned mib : {32,64,128}) {
        auto image=frondSd::create(mib);
        assert(image.size()==size_t(mib)*1024*1024 && image[510]==0x55 && image[511]==0xaa);
        auto* boot=image.data()+63*512;
        assert(boot[510]==0x55 && boot[511]==0xaa && boot[16]==2);
        auto get16=[](const uint8_t* p){return unsigned(p[0])|(unsigned(p[1])<<8);};
        const auto fat=get16(boot+22); const auto cluster=boot[13];
        const auto clusters=(image.size()/512-63-1-32-2*fat)/cluster;
        assert(clusters>=4085 && clusters<65525 && (clusters+2)*2<=fat*512);
        assert(!memcmp(image.data()+(63+1)*512,image.data()+(63+1+fat)*512,fat*512));
    }
    // CRC of zero runs matches the real thing.
    {
        std::vector<uint8_t> zeros(70000, 0), head(300, 0x5a);
        uint32_t crc = uint32_t(mz_crc32(0, head.data(), head.size()));
        for (size_t n : {size_t(0), size_t(1), size_t(511), size_t(4096), size_t(70000)})
            assert(frondSdCandidate::crcZeros(crc, n) == uint32_t(mz_crc32(crc, zeros.data(), n)));
        assert(frondSdCandidate::crcZeros(0, 1000) == uint32_t(mz_crc32(0, zeros.data(), 1000)));
    }
    // Create sizes follow the profile: 2 GB is 1886 MiB, not on the E2; 2048 and 4096 MiB never.
    assert(frondNative::sdCreateAllowed(deviceTypeTX, 1886) && !frondNative::sdCreateAllowed(deviceTypeTX, 2048));
    assert(!frondNative::sdCreateAllowed(deviceTypeTX, 4096));
    assert(!frondNative::sdCreateAllowed(deviceTypeE2, 1886) && frondNative::sdCreateAllowed(deviceTypeE2, 1024));
    assert(frondNative::sdCreateSizes(deviceTypeTX) == "Choose 32, 64, 128, 256, 512, 1024 or 1886 MiB.");
    assert(!frondNative::sdCreateAllowed(deviceTypeTX, 100));
    assert(frondNative::sdCreateSizes(deviceTypeE2) == "Choose 32, 64, 128, 256, 512 or 1024 MiB.");

    // Large cards run from a candidate, save, and reload in a fresh process without a
    // capacity-sized allocation. The folder is deleted afterwards.
    const fs::path root = fs::absolute("build/sd-session-large");
    fs::remove_all(root);
    for (unsigned mib : {256u, 1886u}) {
        const auto folder = root / std::to_string(mib);
        fs::create_directories(folder);
        RunChild(argv[0], {"create", folder.string(), std::to_string(mib)}, 64 * 1024);
        RunChild(argv[0], {"reload", folder.string(), std::to_string(mib)}, 64 * 1024);
        fs::remove_all(folder);
    }
    {
        // 1 GiB: a full first save, then an incremental save of 1 MiB in a fresh process.
        const auto folder = root / "incremental";
        fs::create_directories(folder);
        RunChild(argv[0], {"create", folder.string(), "1024"}, 64 * 1024);
        RunChild(argv[0], {"incremental", folder.string(), "1024"}, 64 * 1024);
        RunChild(argv[0], {"check", folder.string(), "1024"}, 64 * 1024);
        fs::remove_all(folder);
    }
    fs::create_directories(root / "import");
    RunChild(argv[0], {"import", (root / "import").string()}, 64 * 1024);
    fs::remove_all(root);
    std::remove("build/sd-session-child.txt");
    puts("PASS: self-contained SD session integrity, legacy loading, corrupt/truncated rejection and FAT16 geometry");
    puts("PASS: 256 MiB and 2 GB (1886 MiB) cards from a candidate sidecar, saved and reloaded in a fresh process");
}
