#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "EmHAL.h"
#include "ExternalStorage.h"
#include "RuntimeCards.h"

// frond-68k linked cards moving into the cards library: a card with saved changes (generations)
// moves with `frond --card move-linked` and its old save still loads from the machine folder; a
// saved card that isn't there loads empty under card swap; `settle-linked` writes the changes into
// the file. argv[1] is the frond binary.

static std::string ManifestFor(const std::string& path, unsigned id, const std::string& key, uint64_t size, uint64_t history) {
    struct stat st{};
    assert(!stat(path.c_str(), &st));
    std::ostringstream out;
    out << "frond-linked-sd 3\n" << id << (id == 1 ? " ms " : " sd ") << std::quoted(path) << " " << key << " "
        << std::stoul(key.substr(0, 8), nullptr, 16) << " " << size << " 0 1 " << st.st_dev << " " << st.st_ino << " "
        << uint64_t(st.st_mtim.tv_sec) * 1000000000ull + st.st_mtim.tv_nsec << " sd " << history << "\n";
    return out.str();
}

struct SdSlotHal : EmHALHandler {
    bool SupportsImageInSlot(EmHAL::Slot slot, uint32 blocks) override {
        return (slot == EmHAL::Slot::sdcard || slot == EmHAL::Slot::sdcard2) && blocks > 0;
    }
};

static std::string tool;
static std::string run(const std::string& words) {
    const auto command = tool + " --card " + words + " 2>&1";
    FILE* pipe = popen(command.c_str(), "r");
    assert(pipe);
    std::string out;
    char line[512];
    while (fgets(line, sizeof line, pipe)) out = line;
    pclose(pipe);
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}
static size_t generations(const std::filesystem::path& dir, const std::string& tag) {
    size_t n = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir))
        if (!e.path().filename().string().compare(0, 6 + tag.size(), ".gen-" + tag + "-")) ++n;
    return n;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: test-runtime-cards-swap FROND" << std::endl; return 2; }
    tool = argv[1];
    SdSlotHal hal;
    const auto root = std::filesystem::temp_directory_path() / ("runtime-cards-swap-" + std::to_string(getpid()));
    const auto old = root / "dana-cards", machine = root / "sessions" / "dana", other = root / "cards" / "x";
    for (const auto& d : {old, machine, other}) std::filesystem::create_directories(d);
    const std::string key = "1234567890abcdef1234567890abcdef";
    const auto card = old / (key + ".frondsd");
    assert(run("make sd 32 " + card.string()).rfind("ok ", 0) == 0);
    const uint64_t size = 32ull << 20;
    uint8_t written[512], got[512];
    memset(written, 0x6b, sizeof written);
    std::string manifest, inMachine;
    // The Dana writes to the card: the write lands in a generation, the file stays as it was.
    {
        RuntimeCards cards;
        cards.SetDevice(deviceTypeDana);
        assert(cards.Command(0, "attach", "\"" + card.string() + "\" " + key + " writable sd") == "inserted");
        auto* image = gExternalStorage.GetImage(key);
        assert(image && image->WriteByteRange(written, 1 << 20, sizeof written));
        manifest = cards.Manifest();
        cards.Install(RuntimeCards::Pair{});
    }
    assert(generations(old, card.filename().string()) > 0);
    // A Files change or a held generation left beside it stops the move.
    {
        const auto held = old / (".held-.gen-" + card.filename().string() + "-root");
        std::ofstream(held) << "x";
        assert(run("move-linked " + card.string() + " " + (machine / "sd.frondsd").string()).rfind("error unsettled", 0) == 0);
        std::filesystem::remove(held);
    }
    // Never over another file.
    std::ofstream(machine / "sd.frondsd") << "someone else";
    assert(run("move-linked " + card.string() + " " + (machine / "sd.frondsd").string()).rfind("error exists", 0) == 0);
    std::filesystem::remove(machine / "sd.frondsd");
    // Locked by a running machine.
    {
        const int fd = ::open(card.c_str(), O_RDWR);
        assert(fd >= 0 && !flock(fd, LOCK_EX));
        assert(run("move-linked " + card.string() + " " + (machine / "sd.frondsd").string()).rfind("error busy", 0) == 0);
        ::close(fd);
    }
    const auto moved = machine / "sd.frondsd";
    const size_t before = generations(old, card.filename().string());
    assert(run("move-linked " + card.string() + " " + moved.string()) == "ok moved");
    assert(!std::filesystem::exists(card) && generations(old, card.filename().string()) == 0);
    assert(generations(machine, "sd.frondsd") == before);
    // Running it again after it finished does nothing harmful.
    assert(run("move-linked " + card.string() + " " + moved.string()) == "ok moved");
    // The old save still loads: the card is found in its machine folder, with the write.
    {
        RuntimeCards cards;
        cards.SetDevice(deviceTypeDana);
        RuntimeCards::Pair pair;
        const auto refused = cards.Stage(manifest, pair);
        assert(!refused.empty());
        RuntimeCards::Pair found;
        const auto error = cards.Stage(manifest, found, machine.string());
        if (!error.empty()) { std::cerr << error << std::endl; return 1; }
        assert(found.card[0] && found.inserted[0] && found.dropped.empty());
        cards.Install(found);
        auto* image = gExternalStorage.GetImage(key);
        assert(image && image->ReadByteRange(got, 1 << 20, sizeof got) && !memcmp(got, written, sizeof got));
        inMachine = cards.Manifest();
        assert(inMachine.find(moved.string()) != std::string::npos);
        cards.Install(RuntimeCards::Pair{});
    }
    // Under card swap a card outside the machine folder isn't used, as in a copied machine.
    {
        RuntimeCards cards;
        cards.SetDevice(deviceTypeDana);
        RuntimeCards::Pair pair;
        assert(cards.Stage(inMachine, pair, other.string(), true).empty());
        assert(!pair.card[0] && pair.dropped.size() == 1);
        RuntimeCards::Pair own;
        assert(cards.Stage(inMachine, own, machine.string(), true).empty() && own.card[0] && own.dropped.empty());
        cards.Install(RuntimeCards::Pair{});
    }
    // Taken out to the drawer while stopped: under card swap the slot loads empty.
    {
        assert(run("move-linked " + moved.string() + " " + (other / "sd.frondsd").string()) == "ok moved");
        RuntimeCards cards;
        cards.SetDevice(deviceTypeDana);
        RuntimeCards::Pair pair;
        assert(!cards.Stage(manifest, pair, machine.string()).empty());
        RuntimeCards::Pair empty;
        assert(cards.Stage(manifest, empty, machine.string(), true).empty());
        assert(!empty.card[0] && empty.dropped.size() == 1 && empty.dropped[0].first == 0 && empty.dropped[0].second == key);
    }
    // Settled for another machine: the write is in the file and the generations are gone.
    const auto drawer = other / "sd.frondsd";
    assert(run("settle-linked " + drawer.string() + " " + key).rfind("ok settled ", 0) == 0);
    assert(generations(other, "sd.frondsd") == 0);
    {
        frondSdFile::Info info;
        std::vector<uint32_t> table;
        assert(frondMachine::readSdHeader(drawer, info, &table) == frondMachine::SiblingStatus::ok && info.payloadBytes == size);
        FileCardOptions options;
        options.chunkCrcs = table;
        auto backing = FileCardBacking::Open(drawer, size, frondSdFile::headerBytes, options);
        assert(backing && backing->Read(1 << 20, sizeof got, got) && !memcmp(got, written, sizeof got));
    }
    // A settled card has nothing more to settle, and a bad key is refused.
    assert(run("settle-linked " + drawer.string() + " " + key) == "ok settled 0");
    assert(run("settle-linked " + drawer.string() + " xyz").rfind("error usage", 0) == 0);
    // The settled file is a new file: the old save leaves the slot empty under card swap.
    assert(run("move-linked " + drawer.string() + " " + moved.string()) == "ok moved");
    {
        RuntimeCards cards;
        cards.SetDevice(deviceTypeDana);
        RuntimeCards::Pair empty;
        assert(cards.Stage(manifest, empty, machine.string(), true).empty() && empty.dropped.size() == 1);
        // The frontend then attaches it, and a save links it from the machine folder.
        assert(cards.Command(0, "attach", "\"" + moved.string() + "\" " + key + " writable sd") == "inserted");
        assert(cards.Manifest().find(std::string("\"") + moved.string() + "\"") != std::string::npos);
        cards.Install(RuntimeCards::Pair{});
    }
    std::filesystem::remove_all(root);
    puts("runtime-cards-swap ok");
    return 0;
}
