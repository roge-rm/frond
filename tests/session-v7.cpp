#include "SessionIo.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "zip/miniz.h"

namespace fs = std::filesystem;

static void WriteFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    assert(out);
    if (!bytes.empty()) assert(out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

static std::vector<uint8_t> ReadAll(const fs::path& path) {
    std::vector<uint8_t> bytes;
    assert(frondMachine::readBytes(path, bytes));
    return bytes;
}

static fs::path FixtureDir() {
    const auto dir = fs::path("build") / "session-v7-fixtures";
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

static frondSession::Contents TinyContents() {
    frondSession::Contents c;
    c.deviceName = "palm-tx";
    c.displayMode = 1;
    c.ramSize = 32u << 20;
    c.nor = {1, 2, 3, 4};
    c.nand = {5, 6};
    c.memory = {7, 8, 9};
    c.savestate = {10, 11, 12, 13};
    return c;
}

static frondMachine::Media TinySd(uint64_t bytes) {
    frondMachine::Media media;
    media.kind = frondMachine::kindSd;
    media.name = frondMachine::sdSibling;
    media.inserted = true;
    media.identity.fill(0x11);
    media.logicalSize = bytes;
    media.physicalSize = bytes;
    return media;
}

static std::vector<uint8_t> TinyPayload() { return std::vector<uint8_t>(512, 0xa5); }

static void RoundTrip() {
    const auto dir = FixtureDir();
    const auto machine = dir / "palm-tx";
    fs::create_directories(machine);
    auto contents = TinyContents();
    const auto payload = TinyPayload();
    std::vector<uint8_t> sdFile;
    assert(frondSdFile::encode(payload, sdFile));
    WriteFile(machine / frondMachine::sdSibling, sdFile);
    contents.media.push_back(TinySd(payload.size()));
    assert(frondSession::saveFolder(machine, contents));

    const auto leftover = machine / ".frond.tmp-leftover";
    WriteFile(leftover, {1, 2, 3});
    WriteFile(machine / ".gen-sd.frondsd-1", {4});

    frondSession::Contents loaded;
    fs::path folder;
    assert(frondSession::loadPath(machine, loaded, folder));
    assert(folder == machine);
    assert(loaded.deviceName == "palm-tx" && loaded.displayMode == 1 && loaded.ramSize == (32u << 20));
    assert(loaded.nor == contents.nor && loaded.memory == contents.memory);
    assert(loaded.savestate == contents.savestate && loaded.nand == contents.nand);
    assert(loaded.media.size() == 1 && loaded.media[0].kind == frondMachine::kindSd);

    fs::path session;
    assert(frondMachine::sessionFile(machine, session));
    const auto saved = ReadAll(session);
    assert(saved.size() >= 8 && saved[4] == 7);
    SessionFile5 probe;
    assert(probe.Deserialize(saved.size(), saved.data()));
    assert(probe.GetVersion() == 7 && probe.GetSdSize() == 0 && probe.GetMediaCount() == 1);

    fs::remove(leftover);
    fs::remove(machine / ".gen-sd.frondsd-1");
    frondSession::Contents again;
    assert(frondSession::loadPath(machine, again, folder));
    assert(again.nor == contents.nor && again.media.size() == 1);
    assert(ReadAll(machine / frondMachine::sdSibling) == sdFile);
    fs::remove_all(dir);
}

static void RefusedSiblingsLeaveBytes() {
    const auto dir = FixtureDir();
    const auto machine = dir / "palm-tx";
    fs::create_directories(machine);
    auto contents = TinyContents();
    const auto payload = TinyPayload();
    std::vector<uint8_t> sdFile;
    assert(frondSdFile::encode(payload, sdFile));
    WriteFile(machine / frondMachine::sdSibling, sdFile);
    contents.media.push_back(TinySd(payload.size()));
    assert(frondSession::saveFolder(machine, contents));
    fs::path session;
    assert(frondMachine::sessionFile(machine, session));
    const auto previousFrond = ReadAll(session);
    const auto previousSd = ReadAll(machine / frondMachine::sdSibling);

    fs::remove(machine / frondMachine::sdSibling);
    frondSession::Contents loaded;
    fs::path folder;
    assert(!frondSession::loadPath(machine, loaded, folder));
    assert(ReadAll(session) == previousFrond);

    WriteFile(machine / frondMachine::sdSibling, previousSd);
    auto damaged = previousSd;
    damaged[0] = 'X';
    WriteFile(machine / frondMachine::sdSibling, damaged);
    assert(!frondSession::loadPath(machine, loaded, folder));
    assert(ReadAll(session) == previousFrond);
    assert(ReadAll(machine / frondMachine::sdSibling) == damaged);

    WriteFile(machine / frondMachine::sdSibling, previousSd);
    contents.media[0].name = frondMachine::msSibling;
    std::vector<uint8_t> bad;
    assert(!frondMachine::encodeDescriptor(contents.media[0], bad));
    auto mismatch = contents;
    mismatch.media[0].kind = frondMachine::kindMs;
    mismatch.media[0].name = frondMachine::msSibling;
    mismatch.media[0].logicalSize = frondMachine::msLogicalBytes;
    mismatch.media[0].physicalSize = frondMachine::msPhysicalBytes;
    assert(!frondSession::saveFolder(machine, mismatch));
    assert(ReadAll(session) == previousFrond);
    assert(ReadAll(machine / frondMachine::sdSibling) == previousSd);
    fs::remove_all(dir);
}

static void TruncatedHeader() {
    const auto dir = FixtureDir();
    const auto machine = dir / "palm-tx";
    fs::create_directories(machine);
    auto contents = TinyContents();
    assert(frondSession::saveFolder(machine, contents));
    fs::path session;
    assert(frondMachine::sessionFile(machine, session));
    auto bytes = ReadAll(session);
    bytes.resize(20);
    WriteFile(session, bytes);
    frondSession::Contents loaded;
    fs::path folder;
    assert(!frondSession::loadPath(machine, loaded, folder));

    contents.media.push_back(TinySd(512));
    std::vector<uint8_t> media;
    assert(frondSession::encodeMedia(contents.media, media));
    SessionFile5 sessionFile;
    sessionFile.SetDeviceName("palm-tx").SetNor(contents.nor.size(), contents.nor.data())
        .SetMemory(contents.memory.size(), contents.memory.data())
        .SetSavestate(contents.savestate.size(), contents.savestate.data())
        .SetRamSize(contents.ramSize)
        .SetMedia(media.size() - 1, media.data());
    assert(!sessionFile.Serialize());
    sessionFile.SetMedia(media.size(), media.data());
    std::vector<uint8_t> sdFile;
    assert(frondSdFile::encode(TinyPayload(), sdFile));
    WriteFile(machine / frondMachine::sdSibling, sdFile);
    assert(sessionFile.Serialize());
    auto serialized = std::vector<uint8_t>(
        static_cast<const uint8_t*>(sessionFile.GetSerializedSession()),
        static_cast<const uint8_t*>(sessionFile.GetSerializedSession()) +
            sessionFile.GetSerializedSessionSize());
    serialized.resize(SessionFile5::kHeaderBytes + 52 + 10);
    SessionFile5 truncated;
    assert(!truncated.Deserialize(serialized.size(), serialized.data()));
    fs::remove_all(dir);
}

static void LegacyV6() {
    const auto dir = FixtureDir();
    auto payload = TinyPayload();
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 17);
    uint8_t nor[] = {1, 2, 3}, ram[] = {4, 5, 6, 7}, state[] = {8, 9};
    SessionFile5 source;
    source.SetDeviceName("palm-tx").SetRamSize(32u << 20).SetDisplayMode(1)
        .SetNor(sizeof(nor), nor).SetMemory(sizeof(ram), ram).SetSavestate(sizeof(state), state)
        .SetSd(payload.size(), payload.data());
    assert(source.SerializeAs(6));
    const auto* data = static_cast<const uint8_t*>(source.GetSerializedSession());
    const auto size = source.GetSerializedSessionSize();
    assert(data[4] == 6);

    const auto lone = dir / "old.frond";
    WriteFile(lone, std::vector<uint8_t>(data, data + size));
    std::vector<uint8_t> bytes(data, data + size);
    assert(frondSession::extractLoneV6(lone, bytes));
    const auto folder = dir / "old";
    assert(fs::is_directory(folder));
    std::vector<uint8_t> extracted, decoded;
    assert(frondMachine::readBytes(folder / frondMachine::sdSibling, extracted));
    assert(frondSdFile::decode(extracted.data(), extracted.size(), decoded) && decoded == payload);

    frondSession::Contents loaded;
    fs::path got;
    assert(frondSession::loadPath(lone, loaded, got));
    assert(got == folder);
    assert(loaded.nor == std::vector<uint8_t>(nor, nor + sizeof(nor)));
    assert(loaded.memory == std::vector<uint8_t>(ram, ram + sizeof(ram)));
    assert(loaded.media.size() == 1 && loaded.media[0].kind == frondMachine::kindSd);
    fs::path session;
    assert(frondMachine::sessionFile(folder, session));
    auto v7 = ReadAll(session);
    assert(v7[4] == 7);
    SessionFile5 v7file;
    assert(v7file.Deserialize(v7.size(), v7.data()) && v7file.GetSdSize() == 0);

    SessionFile5 emptySd;
    emptySd.SetDeviceName("palm-tx").SetNor(sizeof(nor), nor).SetMemory(sizeof(ram), ram)
        .SetSavestate(sizeof(state), state).SetRamSize(32u << 20);
    assert(emptySd.SerializeAs(6));
    const auto none = dir / "none.frond";
    WriteFile(none, std::vector<uint8_t>(static_cast<const uint8_t*>(emptySd.GetSerializedSession()),
                                        static_cast<const uint8_t*>(emptySd.GetSerializedSession()) +
                                            emptySd.GetSerializedSessionSize()));
    std::vector<uint8_t> noneBytes(
        static_cast<const uint8_t*>(emptySd.GetSerializedSession()),
        static_cast<const uint8_t*>(emptySd.GetSerializedSession()) + emptySd.GetSerializedSessionSize());
    assert(frondSession::extractLoneV6(none, noneBytes));
    frondSession::Contents loadedNone;
    fs::path noneFolder;
    assert(frondSession::loadPath(none, loadedNone, noneFolder));
    assert(loadedNone.media.empty());
    assert(!fs::exists(noneFolder / frondMachine::sdSibling));

    const auto android = dir / "android-id";
    fs::create_directories(android);
    WriteFile(android / frondMachine::androidSession, std::vector<uint8_t>(data, data + size));
    frondSession::Contents fromAndroid;
    fs::path androidFolder;
    assert(frondSession::loadPath(android / frondMachine::androidSession, fromAndroid, androidFolder));
    assert(androidFolder == android);
    assert(fromAndroid.media.size() == 1 && fromAndroid.media[0].kind == frondMachine::kindSd);
    assert(frondMachine::readBytes(android / frondMachine::sdSibling, extracted));
    assert(frondSdFile::decode(extracted.data(), extracted.size(), decoded) && decoded == payload);
    auto androidSession = ReadAll(android / frondMachine::androidSession);
    assert(androidSession[4] == 7);
    fs::remove_all(dir);
}

static std::vector<uint8_t> TinySavestateWithMs(const std::vector<uint8_t>& physical) {
    std::vector<uint8_t> compressed(mz_compressBound(physical.size()));
    mz_ulong dest = compressed.size();
    assert(mz_compress(compressed.data(), &dest, physical.data(), physical.size()) == MZ_OK);
    compressed.resize(dest);
    const uint32_t crc = frondSdFile::crc32(physical.data(), physical.size());
    const uint32_t packedWords = (uint32_t(compressed.size()) + 3) / 4;
    const uint32_t chunkWords = uint32_t(frondSession::msV0SizeWord + 2 + packedWords);
    std::vector<uint32_t> words;
    words.push_back(1);
    words.push_back(frondSession::msCardChunk);
    words.push_back(chunkWords);
    words.resize(1 + 2 + chunkWords);
    words[3] = 0;  // version
    words[3 + frondSession::msV0PresentWord] = 1;  // present
    words[3 + frondSession::msV0SizeWord] = uint32_t(compressed.size());
    words[3 + frondSession::msV0SizeWord + 1] = crc;
    std::memcpy(words.data() + 3 + frondSession::msV0SizeWord + 2, compressed.data(), compressed.size());
    std::vector<uint8_t> bytes(words.size() * 4);
    std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}

static void MsExtractStub() {
    const auto dir = FixtureDir();
    const std::vector<uint8_t> physical(512, 0x5a);
    const auto savestate = TinySavestateWithMs(physical);
    const auto dest = dir / frondMachine::msSibling;
    bool found = false;
    assert(frondSession::extractMsFromSavestate(savestate.data(), savestate.size(), dest, physical.size(),
                                               &found));
    assert(found);
    auto file = ReadAll(dest);
    assert(file.size() == frondSdFile::headerBytes + physical.size());
    assert(std::memcmp(file.data(), "FRONDMS", 7) == 0);
    assert(std::vector<uint8_t>(file.begin() + 64, file.end()) == physical);

    auto contents = TinyContents();
    contents.savestate = savestate;
    SessionFile5 v6;
    v6.SetDeviceName("sony-tg50").SetRamSize(16u << 20).SetNor(contents.nor.size(), contents.nor.data())
        .SetMemory(contents.memory.size(), contents.memory.data())
        .SetSavestate(contents.savestate.size(), contents.savestate.data());
    assert(v6.SerializeAs(6));
    const auto lone = dir / "ms.frond";
    std::vector<uint8_t> bytes(static_cast<const uint8_t*>(v6.GetSerializedSession()),
                               static_cast<const uint8_t*>(v6.GetSerializedSession()) +
                                   v6.GetSerializedSessionSize());
    WriteFile(lone, bytes);
    const auto folder = dir / "ms-out";
    fs::create_directories(folder);
    assert(frondSession::extractV6ToFolder(bytes.data(), bytes.size(), folder, physical.size()));
    auto extracted = ReadAll(folder / frondMachine::msSibling);
    assert(std::vector<uint8_t>(extracted.begin() + 64, extracted.end()) == physical);

    auto wrongSlot = TinySavestateWithMs(physical);
    auto* wrongWords = reinterpret_cast<uint32_t*>(wrongSlot.data());
    wrongWords[3 + frondSession::msV0PresentWord] = 0;
    wrongWords[3 + frondSession::msV0PresentWord + 1] = 1;
    bool wrongFound = true;
    const auto wrongDest = dir / "wrong.frondms";
    assert(frondSession::extractMsFromSavestate(wrongSlot.data(), wrongSlot.size(), wrongDest, physical.size(),
                                               &wrongFound));
    assert(!wrongFound);
    fs::remove_all(dir);
}

static bool identityHex(const std::array<uint8_t, frondMachine::identityBytes>& id) {
    for (auto b : id) {
        const char c = char(b);
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

static void IdentityAndFailedPublish() {
    const auto dir = FixtureDir();
    const auto machine = dir / "palm-tx";
    fs::create_directories(machine);
    auto payload = TinyPayload();
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 17);
    auto read = [&](uint64_t off, uint64_t n, void* dest) {
        std::memcpy(dest, payload.data() + size_t(off), size_t(n));
        return true;
    };
    std::array<uint8_t, frondMachine::identityBytes> identity{};
    assert(frondSession::writeSdFromReader(machine / "out.frondsd", payload.size(), read, identity));
    assert(identityHex(identity));
    MD5Context digest{};
    md5Init(&digest);
    md5Update(&digest, payload.data(), payload.size());
    md5Finalize(&digest);
    std::array<uint8_t, frondMachine::identityBytes> want{};
    frondSession::md5HexIdentity(digest.digest, want);
    assert(identity == want);

    std::array<uint8_t, frondMachine::identityBytes> kept{};
    kept.fill('A');
    assert(frondSession::writeSdFromReader(machine / "kept.frondsd", payload.size(), read, kept));
    assert(kept[0] == 'A' && kept[31] == 'A');

    uint8_t nor[] = {1, 2, 3}, ram[] = {4, 5, 6, 7}, state[] = {8, 9};
    SessionFile5 source;
    source.SetDeviceName("palm-tx").SetRamSize(32u << 20).SetDisplayMode(1)
        .SetNor(sizeof(nor), nor).SetMemory(sizeof(ram), ram).SetSavestate(sizeof(state), state)
        .SetSd(payload.size(), payload.data());
    assert(source.SerializeAs(6));
    const auto* data = static_cast<const uint8_t*>(source.GetSerializedSession());
    const auto size = source.GetSerializedSessionSize();
    const auto lone = dir / "id.frond";
    WriteFile(lone, std::vector<uint8_t>(data, data + size));
    std::vector<uint8_t> bytes(data, data + size);
    assert(frondSession::extractLoneV6(lone, bytes));
    frondSession::Contents loaded;
    fs::path got;
    assert(frondSession::loadPath(lone, loaded, got));
    assert(loaded.media.size() == 1 && identityHex(loaded.media[0].identity));
    assert(loaded.media[0].identity == want);

    auto contents = TinyContents();
    std::vector<uint8_t> sdFile;
    assert(frondSdFile::encode(TinyPayload(), sdFile));
    WriteFile(machine / frondMachine::sdSibling, sdFile);
    contents.media.push_back(TinySd(TinyPayload().size()));
    assert(frondSession::saveFolder(machine, contents));
    fs::path session;
    assert(frondMachine::sessionFile(machine, session));
    const auto previousSd = ReadAll(machine / frondMachine::sdSibling);
    fs::remove(session);
    fs::create_directories(session);
    std::filesystem::path temp;
    assert(frondSession::createTemp(machine, ".frondsd.tmp-", temp));
    WriteFile(temp, sdFile);
    assert(!frondSession::saveFolderFromTemps(machine, contents, {temp}));
    assert(ReadAll(machine / frondMachine::sdSibling) == previousSd);
    assert(!fs::exists(temp));
    fs::remove_all(dir);
}

static void NewWritesAreV7() {
    uint8_t nor[] = {1};
    SessionFile5 session;
    session.SetDeviceName("palm-tx").SetNor(sizeof(nor), nor).SetRamSize(16u << 20);
    assert(session.Serialize());
    const auto* bytes = static_cast<const uint8_t*>(session.GetSerializedSession());
    assert(bytes[4] == 7);
    session.SetSd(512, nor);
    assert(!session.Serialize());
}

int main() {
    RoundTrip();
    RefusedSiblingsLeaveBytes();
    TruncatedHeader();
    LegacyV6();
    MsExtractStub();
    IdentityAndFailedPublish();
    NewWritesAreV7();
    std::puts("Session v7 and streamed v6 extraction checks passed");
}
