// Writes made while a background save writes its files: they're noted (one bit per page or sector),
// read back from the running store and replayed onto the saved file's backing when the store moves
// onto it. A move that fails leaves the store running as it was, with its generation files.
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "CardBacking.h"
#include "FileCardBacking.h"
#include "memory_buffer.h"
#include "nand.h"
#include "sdcard.h"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

class FailingBacking : public RamCardBacking {
   public:
    using RamCardBacking::RamCardBacking;
    bool fail = false;

   protected:
    bool WriteBytes(size_t offset, size_t length, const void* src) override {
        return !fail && RamCardBacking::WriteBytes(offset, length, src);
    }
};

void noReschedule(void*, uint32_t) {}

// Tungsten E2 geometry: 528-byte pages, 32 to a block, 2048 blocks.
const NandSpecs specs = {528, 2048, 5, NAND_FLAG_SAMSUNG_ADDRESSED_VIA_AREAS, 2, {0xec, 0x75}};
constexpr uint32_t kPages = 2048 << 5;
constexpr uint64_t kSide = uint64_t(kPages) * 512;

bool cmd(NAND* n, uint8_t v) { return nandWrite(n, true, false, v); }
bool addr(NAND* n, uint8_t v) { return nandWrite(n, false, true, v); }
bool data(NAND* n, uint8_t v) { return nandWrite(n, false, false, v); }

void settle(NAND* n) {
    for (int i = 0; i < 1000 && !nandIsReady(n); i++) nandPeriodic(n);
}

bool program(NAND* n, uint32_t page, uint8_t value) {
    bool ok = cmd(n, 0x00) && cmd(n, 0x80) && addr(n, 0) && addr(n, page & 0xff) && addr(n, page >> 8);
    for (int i = 0; i < 16; i++) ok = ok && data(n, value);
    ok = ok && cmd(n, 0x10);
    settle(n);
    return ok;
}

bool erase(NAND* n, uint32_t page) {
    bool ok = cmd(n, 0x60) && addr(n, page & 0xff) && addr(n, page >> 8) && cmd(n, 0xd0);
    settle(n);
    return ok;
}

uint8_t readFirst(NAND* n, uint32_t page) {
    uint8_t v = 0xee;
    cmd(n, 0x00);
    addr(n, 0);
    addr(n, page & 0xff);
    addr(n, page >> 8);
    settle(n);
    nandRead(n, false, false, &v);
    return v;
}

// A copy of the whole raw store, as a save's snapshot holds it.
std::vector<uint8_t> rawCopy(NAND* n) {
    std::vector<uint8_t> bytes(size_t(kPages) * 528);
    check(nandBacking(n)->Read(0, bytes.size(), bytes.data()), "read the raw store");
    return bytes;
}

void nandReplay() {
    MemoryBuffer pageBuf{};
    check(memoryBufferAllocate(&pageBuf, MEMORY_BUFFER_GRANULARITY), "page buffer");
    NAND* n = nandInit(nullptr, &pageBuf, Reschedule{noReschedule, nullptr}, size_t(kPages) * 528, &specs,
                       nullptr, nullptr);
    check(n, "nandInit");
    if (!n) return;
    check(nandAdoptBacking(n, std::make_unique<RamCardBacking>(size_t(kPages) * 528), kSide), "first backing");
    settle(n);
    // Before the snapshot: block 1 (pages 32-63) holds pages 40 and 41, page 70 in block 2.
    check(erase(n, 32) && erase(n, 64), "erase blocks 1 and 2");
    check(program(n, 40, 0x11) && program(n, 41, 0x33) && program(n, 70, 0x44), "program before the snapshot");

    // The snapshot: the saved file's bytes. Tracking starts here.
    auto snapshot = rawCopy(n);
    nandTrackWrites(n, true);
    // After it: erase block 1 and program two pages in it, one of them (40) also in the snapshot.
    check(erase(n, 32), "erase block 1 after the snapshot");
    check(program(n, 33, 0x22) && program(n, 40, 0x01), "program block 1 after the snapshot");
    check(erase(n, 32) && program(n, 40, 0x05), "erase and program block 1 again");

    NandPages tracked;
    check(nandTakeTracked(n, tracked), "take tracked pages");
    check(tracked.size() == 32, "the note holds each page of block 1 once");

    // A replay the new backing refuses keeps the NAND on the backing it had.
    auto refusing = std::make_unique<FailingBacking>(snapshot);
    refusing->fail = true;
    check(!nandAdoptBacking(n, std::move(refusing), kSide, &tracked), "a refused replay fails the move");
    check(readFirst(n, 40) == 0x05 && readFirst(n, 33) == 0xff, "still on the old backing after a refused move");

    check(nandAdoptBacking(n, std::make_unique<RamCardBacking>(snapshot), kSide, &tracked), "adopt with replay");
    nandTrackWrites(n, false);
    check(readFirst(n, 40) == 0x05, "page 40 has its post-snapshot bytes");
    check(readFirst(n, 33) == 0xff, "page 33 was erased by the second erase");
    check(readFirst(n, 41) == 0xff, "page 41 was erased after the snapshot");
    check(readFirst(n, 70) == 0x44, "page 70 in an untouched block keeps the snapshot");
    std::vector<uint8_t> spare(16);
    check(nandBacking(n)->Read(kSide + 40 * 16, 16, spare.data()) && spare[0] == 0xff, "spare of page 40");
}

std::vector<uint8_t> fileBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

void writeFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
    const auto tmp = path.string() + ".tmp";
    std::ofstream(tmp, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    fs::rename(tmp, path);
}

std::set<std::string> namesWith(const fs::path& dir, const std::string& prefix) {
    std::set<std::string> names;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().filename().string().rfind(prefix, 0) == 0) names.insert(e.path().filename().string());
    return names;
}

int countPrefix(const fs::path& dir, const std::string& prefix) {
    int n = 0;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().filename().string().rfind(prefix, 0) == 0) n++;
    return n;
}

std::vector<uint8_t> cardBytes(uint64_t offset, uint64_t length) {
    std::vector<uint8_t> out(length);
    check(sdCardReadBytes(offset, length, out.data()), "read card bytes");
    return out;
}

constexpr uint64_t kCard = 1u << 20;

void memoryStickReplay(const fs::path& dir) {
    const auto path = dir / "card.frondms";
    std::vector<uint8_t> base(kCard);
    for (size_t i = 0; i < base.size(); i++) base[i] = uint8_t(i * 7);
    sdCardInitializeWithBytes(std::move(base), "ms");
    std::vector<uint8_t> before(512, 0xa1);
    check(sdCardWriteBytes(3 * 512, 512, before.data()), "write sector 3 before the snapshot");

    // The snapshot is what the save writes to the file.
    const auto snapshot = cardBytes(0, kCard);
    sdCardTrackWrites(true);
    std::vector<uint8_t> again(512, 0xb2), across(600, 0xc3);
    check(sdCardWriteBytes(3 * 512, 512, again.data()), "rewrite sector 3 after the snapshot");
    check(sdCardWriteBytes(5 * 512 + 100, across.size(), across.data()), "write across sectors 5 and 6");
    const auto live = cardBytes(0, kCard);
    writeFile(path, snapshot);

    FileCardIoHooks full;
    full.failWriteAfter = 0;
    sdCardSetAdoptHooks(&full);
    check(!sdCardAdoptSavedFromRam(path.c_str(), kCard, 0), "a Memory Stick move whose replay fails fails");
    sdCardSetAdoptHooks(nullptr);
    check(!sdCardFileBacked() && cardBytes(0, kCard) == live, "the Memory Stick stays in RAM with every write");
    check(countPrefix(dir, ".held-") == 0 && countPrefix(dir, ".gen-") == 0, "no files left by the failed move");
    check(sdCardAdoptSavedFromRam(path.c_str(), kCard, 0), "adopt the saved file from RAM");
    sdCardTrackWrites(false);
    check(sdCardFileBacked(), "the card runs on the file");
    check(cardBytes(0, kCard) == live, "the card has every write, before and after the snapshot");
    check(fileBytes(path) == snapshot, "the saved file still holds the snapshot");
    sdCardReset();
    for (const auto& e : fs::directory_iterator(dir)) fs::remove(e.path());
}

void failedMoveKeepsGenerations(const fs::path& dir) {
    const auto path = dir / "card.frondsd";
    std::vector<uint8_t> base(kCard, 0x5a);
    writeFile(path, base);
    check(sdCardInitializeFromFile(path.c_str(), kCard, 0, "sd"), "open the card on its file");
    std::vector<uint8_t> sector(512, 0x6b);
    check(sdCardWrite(9, sector.data()) && sdCardCommit(), "write and commit sector 9");
    const auto live = cardBytes(0, kCard);
    check(countPrefix(dir, ".gen-") > 0, "the running card has generation files");

    // A save published a good file but the replay onto it fails (disk full): the move fails and
    // the card keeps running on its generations, which are all back.
    sdCardTrackWrites(true);
    check(sdCardWrite(10, sector.data()), "write sector 10 after the snapshot");
    {
        const auto liveNow = cardBytes(0, kCard);
        writeFile(path, live);
        const auto before = namesWith(dir, ".gen-");
        FileCardIoHooks full;
        full.failWriteAfter = 0;
        sdCardSetAdoptHooks(&full);
        check(!sdCardAdoptSaved(path.c_str(), kCard, 0), "a move whose replay fails fails");
        sdCardSetAdoptHooks(nullptr);
        check(full.writes > 0, "the replay reached the new backing");
        check(namesWith(dir, ".gen-") == before, "after a failed replay its generations are all back");
        check(countPrefix(dir, ".held-") == 0, "nothing left held after a failed replay");
        check(cardBytes(0, kCard) == liveNow, "the card still reads every write after a failed replay");
    }
    // A save published a file the card can't open (too short): the move fails, nothing is lost.
    writeFile(path, std::vector<uint8_t>(4096));
    const auto gens = countPrefix(dir, ".gen-");
    check(!sdCardAdoptSaved(path.c_str(), kCard, 0), "a move onto a bad file fails");
    check(countPrefix(dir, ".gen-") == gens, "its generation files are back");
    check(countPrefix(dir, ".held-") == 0, "nothing left held");
    std::vector<uint8_t> live10 = cardBytes(0, kCard);
    check(memcmp(live10.data(), live.data(), 10 * 512) == 0 &&
              memcmp(live10.data() + 10 * 512, sector.data(), 512) == 0,
          "the card still reads every write");

    // The next save's file works: the old generations go and the card moves.
    writeFile(path, live10);
    check(sdCardWrite(11, sector.data()), "write sector 11 after the second snapshot");
    const auto live11 = cardBytes(0, kCard);
    check(sdCardAdoptSaved(path.c_str(), kCard, 0), "the move onto a good file works");
    sdCardTrackWrites(false);
    check(cardBytes(0, kCard) == live11, "the moved card has every write");
    check(countPrefix(dir, ".held-") == 0, "held generations are gone");
    sdCardReset();
    for (const auto& e : fs::directory_iterator(dir)) fs::remove(e.path());
}

// The old backing's uncommitted generation and the new backing's replay generation get the same
// name; dropping the old backing must leave the new one's file alone.
void collidingGenerations(const fs::path& dir) {
    const auto path = dir / "card.frondsd";
    writeFile(path, std::vector<uint8_t>(kCard, 0x33));
    check(sdCardInitializeFromFile(path.c_str(), kCard, 0, "sd"), "open the card");
    std::vector<uint8_t> sector(512, 0x8d);
    check(sdCardWrite(2, sector.data()), "write sector 2, uncommitted");
    const auto snapshot = cardBytes(0, kCard);
    sdCardTrackWrites(true);
    check(sdCardWrite(3, sector.data()), "write sector 3 after the snapshot");
    const auto live = cardBytes(0, kCard);
    const auto oldNames = namesWith(dir, ".gen-");
    writeFile(path, snapshot);
    check(sdCardAdoptSaved(path.c_str(), kCard, 0), "move onto the saved file");
    sdCardTrackWrites(false);
    bool collide = false;
    for (const auto& name : namesWith(dir, ".gen-")) collide = collide || oldNames.count(name);
    check(collide, "the new backing reuses a generation name the old one had");
    check(cardBytes(0, kCard) == live, "the moved card has every write");
    check(sdCardCommit(), "commit the moved card");
    sdCardReset();
    check(sdCardInitializeFromFile(path.c_str(), kCard, 0, "sd"), "reopen the moved card");
    check(cardBytes(0, kCard) == live, "the old backing left the new one's generation alone");
    sdCardReset();
    for (const auto& e : fs::directory_iterator(dir)) fs::remove(e.path());
}

// A crash between holding generations and dropping or returning them leaves .held- files.
void heldAfterCrash(const fs::path& dir) {
    const auto path = dir / "card.frondsd";
    std::vector<uint8_t> sector(512, 0x7c);

    // Still bound to the file there: they come back and the card keeps its writes.
    writeFile(path, std::vector<uint8_t>(kCard, 0x11));
    check(sdCardInitializeFromFile(path.c_str(), kCard, 0, "sd"), "open the card");
    check(sdCardWrite(4, sector.data()) && sdCardCommit(), "write and commit sector 4");
    const auto live = cardBytes(0, kCard);
    auto held = FileCardBacking::HoldGenerations(path);
    check(!held.moved.empty() && countPrefix(dir, ".gen-") == 0, "generations are held");
    sdCardReset();
    check(countPrefix(dir, ".held-") > 0, "the crash leaves them held");
    check(sdCardInitializeFromFile(path.c_str(), kCard, 0, "sd"), "reopen with held generations");
    check(cardBytes(0, kCard) == live, "held generations bound to the file come back");
    check(countPrefix(dir, ".held-") == 0, "none left held after the load");

    // A save replaced the file before the crash: they're for the old file and go.
    held = FileCardBacking::HoldGenerations(path);
    sdCardReset();
    std::vector<uint8_t> saved(kCard, 0x22);
    writeFile(path, saved);
    check(sdCardInitializeFromFile(path.c_str(), kCard, 0, "sd"), "open the saved file with held generations");
    check(cardBytes(0, kCard) == saved, "the saved file wins over the held generations");
    check(countPrefix(dir, ".held-") == 0, "held generations of the old file are gone");
    sdCardReset();
    for (const auto& e : fs::directory_iterator(dir)) fs::remove(e.path());
}

}  // namespace

int main() {
    nandReplay();
    char tmpl[] = "build/save-replay-XXXXXX";
    if (!mkdtemp(tmpl)) return 1;
    const fs::path dir = fs::absolute(tmpl);
    memoryStickReplay(dir);
    failedMoveKeepsGenerations(dir);
    collidingGenerations(dir);
    heldAfterCrash(dir);
    fs::remove_all(dir);
    if (failures) return 1;
    std::printf("save-replay: ok\n");
    return 0;
}
