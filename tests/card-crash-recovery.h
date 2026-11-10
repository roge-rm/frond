#ifndef FROND_TEST_CARD_CRASH_RECOVERY_H
#define FROND_TEST_CARD_CRASH_RECOVERY_H
#include <sys/wait.h>
#include <unistd.h>

static void RootlessCrashRecovery(const fs::path& dir) {
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto card = dir / "card", peer = dir / "card-2";
    WriteAll(card, std::vector<uint8_t>(65536, 0x31));
    WriteAll(peer, std::vector<uint8_t>(65536, 0x42));
    FileCardOptions opt;
    opt.recoverRootlessBase = true;
    auto other = FileCardBacking::Open(peer, 0, 0);
    std::vector<uint8_t> dirty(512, 0xe7), got(512);
    assert(other && other->Write(0, 512, dirty.data()) && other->Commit());
    std::vector<uint8_t> peerPending(512, 0xc8);
    assert(other->Write(0, 512, peerPending.data()) && other->Snapshot());
    auto files = [&]() {
        std::map<std::string, std::vector<uint8_t>> result;
        for (const auto& e : fs::directory_iterator(dir)) {
            std::ifstream in(e.path(), std::ios::binary);
            result[e.path().filename().string()] = std::vector<uint8_t>(
                std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        return result;
    };
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto before = files();
        const auto pid = fork();
        assert(pid >= 0);
        if (!pid) {
            auto b = FileCardBacking::Open(card, 0, 0, opt);
            if (!b || !b->Write(0, 512, dirty.data()) || !b->Snapshot()) _exit(2);
            _exit(0);
        }
        int status = 0;
        assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
        const auto crashed = files();
        for (const auto& e : before) assert(crashed.at(e.first) == e.second);
        assert(!FileCardBacking::Open(card, 0, 0)); // generic history policy stays strict
        FileCardOptions strict = opt;
        strict.rejectInvalidRoot = true;
        assert(!FileCardBacking::Open(card, 0, 0, strict));
        strict.requireRoot = true;
        strict.minimumRoot = 1;
        assert(!FileCardBacking::Open(card, 0, 0, strict));
        assert(files() == crashed);
        auto b = FileCardBacking::Open(card, 0, 0, opt);
        assert(b && b->Read(0, 512, got.data()) && got == std::vector<uint8_t>(512, 0x31));
        assert(files() == crashed);
    }
    // Pending data-only/index-only files and all temp forms are retained on open.
    for (const auto* name : {".gen-card-50", ".gen-card-70.idx", ".gen-card-tmp-1-1",
                             ".gen-card-tmp-idx-1-1", ".gen-card-tmp-root-1-1"})
        WriteAll(dir / name, {0x99});
    const auto before = files();
    auto b = FileCardBacking::Open(card, 0, 0, opt);
    assert(b && b->Write(0, 512, dirty.data()) && b->WorkingGenerationId() > 70 && b->Snapshot());
    const auto after = files();
    for (const auto& e : before) assert(after.at(e.first) == e.second);
    assert(other->Read(0, 512, got.data()) && got == peerPending);
    assert(b->Commit());
    b.reset();
    b = FileCardBacking::Open(card, 0, 0); // ordinary valid bound root
    assert(b && b->Read(0, 512, got.data()) && got == dirty);
    b.reset();
    const auto rootPath = dir / ".gen-card-root";
    const auto rootBytes = files().at(rootPath.filename().string());
    WriteAll(rootPath, {0x01});
    const auto malformed = files();
    assert(!FileCardBacking::Open(card, 0, 0, opt));
    FileCardOptions malformedStrict = opt;
    malformedStrict.rejectInvalidRoot = true;
    assert(!FileCardBacking::Open(card, 0, 0, malformedStrict));
    assert(files() == malformed);
    fs::remove(rootPath);
    fs::create_symlink("missing-root-target", rootPath);
    const auto dangling = files();
    assert(!FileCardBacking::Open(card, 0, 0, opt) && files() == dangling);
    fs::remove(rootPath);
    WriteAll(rootPath, rootBytes);
    fs::remove(rootPath);
    FileCardOptions strict;
    strict.rejectInvalidRoot = true;
    strict.requireRoot = true;
    strict.minimumRoot = 1;
    const auto missing = files();
    assert(!FileCardBacking::Open(card, 0, 0, strict) && files() == missing);
    b = FileCardBacking::Open(card, 0, 0, opt);
    WriteAll(dir / ".gen-card-72", {0x77}); // scan selected 72, then another file appeared
    const auto collided = files();
    assert(b && !b->Write(0, 512, dirty.data()) && files() == collided);
    b.reset();
    b = FileCardBacking::Open(card, 0, 0, opt);
    assert(b && b->Write(0, 512, dirty.data()));
    const auto pendingIndex = dir / (".gen-card-" + std::to_string(b->WorkingGenerationId()) + ".idx");
    WriteAll(pendingIndex, {0x88});
    assert(!b->Snapshot()); // first index publication cannot replace a preserved file
    b.reset();
    assert(files().at(pendingIndex.filename().string()) == std::vector<uint8_t>({0x88}));
    // Exhausted ID space refuses writes without touching any retained entry.
    WriteAll(dir / ".gen-card-18446744073709551615.idx", {0x12});
    b = FileCardBacking::Open(card, 0, 0, opt);
    const auto exhausted = files();
    assert(b && !b->Write(0, 512, dirty.data()) && files() == exhausted);
    b.reset();
    other.reset();
    fs::remove_all(dir);
    std::puts("rootless fork/_exit recovery, retention, IDs, strict history and peer PASS");
}
#endif
