#include "RuntimeCards.h"
#ifdef _WIN32
extern "C" char* mkdtemp(char*);
#endif
#include "CardTool.h"
#include "EmSession.h"

#include <cassert>
#include <fstream>
#include <iostream>
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <sys/wait.h>
#endif

EmSession* gSession = nullptr;
EmTransportSerial* EmSession::GetTransportSerial(EmUARTDeviceType) { std::abort(); }

struct SdHal : EmHALHandler {
    bool SupportsImageInSlot(EmHAL::Slot slot, uint32 blocks) override {
        return slot == EmHAL::Slot::sdcard && blocks != 0;
    }
};

static void setTime(const std::filesystem::path& path, long nanoseconds) {
    constexpr uint64_t seconds = 1655526400;
#ifdef _WIN32
    const int fd = ::open(path.c_str(), O_RDWR);
    assert(fd >= 0);
    const uint64_t ticks = 116444736000000000ull + seconds * 10000000 + nanoseconds / 100;
    FILETIME time{DWORD(ticks), DWORD(ticks >> 32)};
    assert(SetFileTime(reinterpret_cast<HANDLE>(_get_osfhandle(fd)), nullptr, nullptr, &time));
    assert(close(fd) == 0);
#else
    const timespec times[2]{{0, UTIME_OMIT}, {seconds, nanoseconds}};
    assert(utimensat(AT_FDCWD, path.c_str(), times, 0) == 0);
#endif
}

static int childStatus(const char* program, const char* operation) {
#ifdef _WIN32
    return _spawnl(_P_WAIT, program, program, operation, nullptr);
#else
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        execl(program, program, operation, nullptr);
        _exit(127);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
    return WEXITSTATUS(status);
#endif
}

static std::string read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    assert(in);
    return {std::istreambuf_iterator<char>(in), {}};
}

static void write(const std::filesystem::path& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary);
    out << data;
    out.close();
    assert(out);
}

static std::filesystem::path racePath;

static void replaceBeforeLock() {
    if (racePath.empty()) return;
    std::filesystem::rename(racePath, "race-old");
    std::filesystem::rename("race-next", racePath);
    racePath.clear();
}

#ifdef _WIN32
extern "C" int __stdcall __real_LockFileEx(void*, unsigned long, unsigned long, unsigned long,
                                          unsigned long, frondWin::LockRange*);
extern "C" int __stdcall __wrap_LockFileEx(void* handle, unsigned long flags, unsigned long reserved,
                                          unsigned long low, unsigned long high,
                                          frondWin::LockRange* range) {
    replaceBeforeLock();
    return __real_LockFileEx(handle, flags, reserved, low, high, range);
}
#else
extern "C" int __real_flock(int, int);
extern "C" int __wrap_flock(int fd, int flags) {
    replaceBeforeLock();
    return __real_flock(fd, flags);
}
#endif

static void checkReplacedLock(const std::string& key) {
    const std::filesystem::path card = "race-card";
    const std::filesystem::path mark = ".settling-race-card";
    for (const auto* op : {"settle-linked", "recover-linked", "move-linked", "resume-move"}) {
        write(card, "original card");
        write("race-next", "replacement card");
        const auto stamp = frondCardTool::fileStamp("race-next") + '\n';
        write(mark, stamp);
        const std::string_view operation(op);
        racePath = card;
        frondCardTool::Result result;
        if (operation == "settle-linked") result = frondCardTool::settleLinked(card, key);
        else if (operation == "recover-linked") result = frondCardTool::recoverLinked(card);
        else if (operation == "move-linked") result = frondCardTool::moveLinked(card, "race-other");
        else result = frondCardTool::moveLinked("race-other", card);
        assert(racePath.empty());
        assert(!result.ok && result.code == (operation == "recover-linked" ? "busy" : "unsettled"));
        assert(read(card) == "replacement card" && read(mark) == stamp);
        assert(!std::filesystem::exists("race-other"));
        std::filesystem::remove("race-old");
    }
}

int main(int argc, char** argv) {
    const auto dir = std::filesystem::current_path();
    const auto path = dir / "sd.frondsd";
    const auto root = dir / ".gen-sd.frondsd-root";
    const auto heldRoot = dir / ".held-.gen-sd.frondsd-root";
    const auto mark = dir / ".settling-sd.frondsd";
    const auto manifest = dir / "manifest";
    const std::string key = "1234567890abcdef1234567890abcdef";
    constexpr uint64_t size = 32ull << 20;
    std::ostringstream args;
    args << std::quoted(path.string()) << ' ' << key << " writable sd";
    SdHal hal;
    RuntimeCards cards;
    cards.SetDevice(deviceTypeDana);
    auto attach = [&] { return cards.Command(0, "attach", args.str()); };

    if (argc > 1) {
        RuntimeCards::Pair pair;
        const auto result = std::string_view(argv[1]) == "stage" ? cards.Stage(read(manifest), pair) : attach();
        std::cout << "child " << argv[1] << ": " << (result.empty() ? "staged" : result) << std::endl;
        if (result == "inserted" || result.empty()) return 11;
        return result == "error: cannot lock card file" ? 12 : 13;
    }

    checkReplacedLock(key);
    std::array<uint8_t, frondMachine::identityBytes> identity{};
    assert(frondSession::writeSdFromReader(path, size, [](uint64_t, uint64_t n, void* p) {
        memset(p, 0, size_t(n));
        return true;
    }, identity));
    setTime(path, 123456700);
    assert(attach() == "inserted");
    assert(childStatus(argv[0], "attach") == 12);
    auto* image = gExternalStorage.GetImage(key);
    uint8_t patch[512], readback[512];
    memset(patch, 0x6b, sizeof patch);
    assert(image && image->WriteByteRange(patch, 1 << 20, sizeof patch));
    assert(cards.CanSave());
    write(manifest, cards.Manifest());
    {
        RuntimeCards::Pair pair;
        std::string protectedManifest = read(manifest);
        const auto policy = protectedManifest.find(" 33554432 0 ");
        assert(policy != std::string::npos);
        protectedManifest[policy + sizeof(" 33554432 ") - 1] = '1';
        assert(cards.Stage(protectedManifest, pair).empty());
        cards.Install(pair);
        assert(childStatus(argv[0], "attach") == 12);
    }

    // Simulate a crash while the first process still owns the lock.
    const auto held = FileCardBacking::HoldGenerations(path);
    assert(held.moved.size() >= 2 && !std::filesystem::exists(root));
    std::vector<std::string> heldBytes;
    for (const auto& entry : held.moved) heldBytes.push_back(read(entry.first));
    assert(childStatus(argv[0], "stage") == 12);
    for (size_t i = 0; i < held.moved.size(); ++i) {
        assert(!std::filesystem::exists(held.moved[i].second));
        assert(read(held.moved[i].first) == heldBytes[i]);
    }
    cards.Install({});
    assert(childStatus(argv[0], "stage") == 11);
    assert(!std::filesystem::exists(heldRoot) && std::filesystem::exists(root));
    assert(attach() == "inserted");
    image = gExternalStorage.GetImage(key);
    assert(image && image->ReadByteRange(readback, 1 << 20, sizeof readback));
    assert(memcmp(patch, readback, sizeof patch) == 0);

    using Binding = FileCardBacking::RootBinding;
    assert(FileCardBacking::RootBinds(root, path) == Binding::matches);
    const auto originalStamp = frondCardTool::fileStamp(path);
    const auto savedRoot = read(root);
    const int fd = ::open(path.c_str(), O_RDWR);
    assert(fd >= 0 && pwrite(fd, "x", 1, frondSdFile::headerBytes) == 1 && close(fd) == 0);
    setTime(path, 765432100);
    assert(!cards.CanSave());
    assert(frondCardTool::fileStamp(path) != originalStamp);
    assert(FileCardBacking::RootBinds(root, path) == Binding::mismatch);
    cards.Install({});
    {
        RuntimeCards::Pair pair;
        assert(cards.Stage(read(manifest), pair) == "linked card source was replaced or modified");
    }

    std::filesystem::rename(root, heldRoot);
    write(heldRoot, "unreadable root");
    assert(!FileCardBacking::SettleHeld(path) && read(heldRoot) == "unreadable root");
    write(heldRoot, savedRoot);
    assert(FileCardBacking::RootBinds(heldRoot, dir / "missing") == Binding::unknown);
    const auto missing = dir / "missing";
    std::filesystem::rename(path, missing);
    assert(!FileCardBacking::SettleHeld(path) && read(heldRoot) == savedRoot);
    std::filesystem::rename(missing, path);

#ifdef _WIN32
    {
        auto legacyManifest = read(manifest);
        std::istringstream row(legacyManifest.substr(legacyManifest.find('\n') + 1));
        unsigned slot, protect, insert;
        uint64_t serial, bytes, dev, ino, mtime, history;
        std::string kind, source, savedKey, protocol;
        assert(row >> slot >> kind >> std::quoted(source) >> savedKey >> serial >> bytes >>
               protect >> insert >> dev >> ino >> mtime >> protocol >> history);
        std::ostringstream text;
        text << "frond-linked-sd 3\n" << slot << ' ' << kind << ' ' << std::quoted(source)
             << ' ' << savedKey << ' ' << serial << ' ' << bytes << ' ' << protect << ' '
             << insert << " 0 0 1655526400000000000 " << protocol << ' ' << history << '\n';
        RuntimeCards::Pair pair;
        assert(cards.Stage(text.str(), pair) == "linked card file identity is ambiguous");
        assert(cards.Stage(text.str(), pair, dir.string(), true).empty());
        assert(pair.dropped.size() == 1 && pair.dropped.front().first == slot &&
               pair.dropped.front().second == key && !pair.card[0]);
    }
    // The old CRT fields can't prove that a base or settled replacement is the same file.
    auto legacy = savedRoot;
    uint8_t fields[24]{};
    frondSdFile::put64(fields + 16, 1655526400000000000ull);
    legacy.replace(28, sizeof fields, reinterpret_cast<const char*>(fields), sizeof fields);
    write(heldRoot, legacy);
    assert(!FileCardBacking::SettleHeld(path) && read(heldRoot) == legacy);
    std::filesystem::rename(heldRoot, root);
    FileCardOptions options;
    options.identity = key;
    assert(!FileCardBacking::Open(path, size, frondSdFile::headerBytes, options));
    assert(read(root) == legacy);
    write(mark, "0 0 1655526400000000000\n");
    assert(!frondCardTool::finishSettle(path));
    assert(std::filesystem::exists(mark) && read(root) == legacy);
    std::filesystem::remove(mark);
    std::filesystem::remove(root);
#else
    assert(FileCardBacking::SettleHeld(path) && !std::filesystem::exists(heldRoot));
#endif

    // Same timestamp and bytes, different file. The old root and settle stamp do not bind it.
    write(root, savedRoot);
    const auto replacement = dir / "replacement";
    std::filesystem::copy_file(path, replacement);
    setTime(path, 123456700);
    setTime(replacement, 123456700);
    const auto stamp = frondCardTool::fileStamp(path);
    assert(!stamp.empty() && stamp == originalStamp);
    write(mark, stamp + "\n");
    std::filesystem::rename(path, missing);
    std::filesystem::rename(replacement, path);
    assert(frondCardTool::fileStamp(path) != stamp);
    assert(FileCardBacking::RootBinds(root, path) == Binding::mismatch);
    assert(frondCardTool::finishSettle(path));
    assert(!std::filesystem::exists(mark) && read(root) == savedRoot);

    write(mark, frondCardTool::fileStamp(path) + "\n");
    if (!frondCardTool::finishSettle(path)) {
        std::cerr << "Could not finish a matching settle marker. Card stamp: "
                  << frondCardTool::fileStamp(path) << ", marker stamp: " << frondCardTool::fileStamp(mark) << '\n';
        for (const auto& generation : frondCardTool::linkedGenerations(path))
            std::cerr << generation.filename() << ": " << frondCardTool::fileStamp(generation) << '\n';
        return 1;
    }
    assert(!std::filesystem::exists(mark) && !std::filesystem::exists(root));
    std::cout << "Linked card lock, reopen, same-second edit and recovery checks passed\n";
}
