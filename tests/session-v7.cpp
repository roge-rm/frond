#include "CardSave.h"
#include "MemoryStickImage.h"
#include "NandFile.h"
#include "SessionIo.h"
#include "sdcard.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sys/stat.h>
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
    assert(saved.size() >= 8 && saved[4] == 8);
    SessionFile5 probe;
    assert(probe.Deserialize(saved.size(), saved.data()));
    assert(probe.GetVersion() == 8 && probe.GetSdSize() == 0 && probe.GetMediaCount() == 1);

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

    // Opening the lone file never reuses or overwrites the folder beside it.
    fs::path existingSession;
    assert(frondMachine::sessionFile(folder, existingSession));
    const auto existingBytes = ReadAll(existingSession);
    WriteFile(folder / "notes.txt", {'k', 'e', 'e', 'p'});
    const auto loneBytes = ReadAll(lone);
    frondSession::Contents loaded;
    fs::path got;
    assert(frondSession::loadPath(lone, loaded, got));
    assert(got == dir / "old 2");
    assert(ReadAll(existingSession) == existingBytes && ReadAll(folder / "notes.txt").size() == 4);
    assert(ReadAll(lone) == loneBytes);
    assert(loaded.nor == std::vector<uint8_t>(nor, nor + sizeof(nor)));
    assert(loaded.memory == std::vector<uint8_t>(ram, ram + sizeof(ram)));
    assert(loaded.media.size() == 1 && loaded.media[0].kind == frondMachine::kindSd);
    fs::path session;
    assert(frondMachine::sessionFile(got, session));
    auto v7 = ReadAll(session);
    assert(v7[4] == 9);
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
    assert(androidSession[4] == 9);
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

    // A stale Pro stick in the folder, with its journal and full copy, goes with the v6 load.
    const auto stale = dir / "ms-stale";
    fs::create_directories(stale);
    for (const char* name : {"ms.frondmsp", "ms.frondmsp.journal-3", "ms.frondmsp.journal-3.full"})
        WriteFile(stale / name, std::vector<uint8_t>(64, 1));
    assert(frondSession::extractV6ToFolder(bytes.data(), bytes.size(), stale, physical.size()));
    assert(!fs::exists(stale / "ms.frondmsp") && !fs::exists(stale / "ms.frondmsp.journal-3") &&
           !fs::exists(stale / "ms.frondmsp.journal-3.full"));
    assert(fs::is_regular_file(stale / frondMachine::msSibling));

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

static void LoneV6Bin() {
    const auto dir = FixtureDir();
    uint8_t nor[] = {1, 2, 3}, ram[] = {4}, state[] = {5};
    SessionFile5 source;
    source.SetDeviceName("sony-clie-tg50").SetRamSize(16u << 20).SetDisplayMode(5)
        .SetNor(sizeof(nor), nor).SetMemory(sizeof(ram), ram).SetSavestate(sizeof(state), state);
    assert(source.SerializeAs(6));
    const auto* data = static_cast<const uint8_t*>(source.GetSerializedSession());
    const auto size = source.GetSerializedSessionSize();
    const auto lone = dir / "synthetic.bin";
    WriteFile(lone, std::vector<uint8_t>(data, data + size));
    frondSession::Contents loaded;
    fs::path got;
    assert(frondSession::loadPath(lone, loaded, got));
    assert(got == dir / "synthetic");
    assert(loaded.deviceName == "sony-clie-tg50");
    assert(loaded.nor == std::vector<uint8_t>(nor, nor + sizeof(nor)));
    assert(loaded.memory == std::vector<uint8_t>(ram, ram + sizeof(ram)));
    assert(loaded.savestate == std::vector<uint8_t>(state, state + sizeof(state)));
    fs::path session;
    assert(frondMachine::sessionFile(got, session));
    auto v7 = ReadAll(session);
    assert(v7[4] == 9);

    auto payload = TinyPayload();
    SessionFile5 withSd;
    withSd.SetDeviceName("palm-tx").SetRamSize(32u << 20).SetDisplayMode(1)
        .SetNor(sizeof(nor), nor).SetMemory(sizeof(ram), ram).SetSavestate(sizeof(state), state)
        .SetSd(payload.size(), payload.data());
    assert(withSd.SerializeAs(6));
    WriteFile(lone, std::vector<uint8_t>(static_cast<const uint8_t*>(withSd.GetSerializedSession()),
                                        static_cast<const uint8_t*>(withSd.GetSerializedSession()) +
                                            withSd.GetSerializedSessionSize()));
    frondSession::Contents loadedSd;
    assert(frondSession::loadPath(lone, loadedSd, got));
    assert(got == dir / "synthetic 2");
    assert(loadedSd.media.size() == 1 && fs::is_regular_file(got / frondMachine::sdSibling));
    assert(!fs::exists(dir / "synthetic" / frondMachine::sdSibling));
    WriteFile(lone, std::vector<uint8_t>(data, data + size));
    frondSession::Contents loadedAgain;
    assert(frondSession::loadPath(lone, loadedAgain, got));
    assert(loadedAgain.media.empty());
    assert(!fs::exists(got / frondMachine::sdSibling));

    auto truncated = std::vector<uint8_t>(data, data + size);
    truncated.resize(truncated.size() - 2);
    const auto bad = dir / "truncated.bin";
    WriteFile(bad, truncated);
    frondSession::Contents failed;
    fs::path failedFolder;
    assert(!frondSession::loadPath(bad, failed, failedFolder));

    const auto unnamed = dir / "unnamed";
    WriteFile(unnamed, std::vector<uint8_t>(data, data + size));
    frondSession::Contents loadedUnnamed;
    fs::path unnamedFolder;
    assert(frondSession::loadPath(unnamed, loadedUnnamed, unnamedFolder));
    assert(unnamedFolder == dir / "unnamed-session");
    assert(loadedUnnamed.nor == std::vector<uint8_t>(nor, nor + sizeof(nor)));
    fs::remove_all(dir);
}

static std::vector<uint8_t> SdSector(uint8_t v) { return std::vector<uint8_t>(SD_SECTOR_SIZE, v); }

static std::vector<uint8_t> SidecarPayload(const fs::path& path) {
    std::vector<uint8_t> file, payload;
    assert(frondMachine::readBytes(path, file));
    assert(frondSdFile::decode(file.data(), file.size(), payload));
    return payload;
}

static bool HasGenerations(const fs::path& dir) {
    for (const auto& entry : fs::directory_iterator(dir))
        if (entry.path().filename().string().rfind(".gen-", 0) == 0) return true;
    return false;
}

// Runs the card part of frondNative::saveMachine against `folder`.
static bool SaveCard(const fs::path& folder) {
    auto contents = TinyContents();
    std::vector<fs::path> temps;
    frondNative::SdSave sd;
    if (sdCardInitialized()) {
        frondMachine::Media media;
        if (!frondNative::beginSdSave(folder, media, true, sd)) return false;
        temps.emplace_back();
        contents.media.push_back(media);
    }
    return frondNative::publishWithSd(folder, contents, temps, sd);
}

static void SavesKeepCardsConsistent() {
    const auto dir = FixtureDir();
    const auto machine = dir / "machine";
    const auto other = dir / "other";
    fs::create_directories(machine);
    fs::create_directories(other);
    const size_t sectors = 8;
    std::vector<uint8_t> base(sectors * SD_SECTOR_SIZE, 0xa5);
    std::vector<uint8_t> sdFile;
    assert(frondSdFile::encode(base, sdFile));
    WriteFile(machine / frondMachine::sdSibling, sdFile);
    // Generations of a card this sidecar replaced: a v1 root and a page of other bytes.
    {
        auto old = FileCardBacking::Open(machine / frondMachine::sdSibling, base.size(),
                                         frondSdFile::headerBytes);
        assert(old);
        const auto stale = SdSector(0xee);
        assert(old->Write(0, stale.size(), stale.data()) && old->Commit());
    }
    WriteFile(machine / frondMachine::sdSibling, sdFile);  // the replacement card, same size
    {
        std::vector<uint8_t> v1(32, 0);
        std::memcpy(v1.data(), "FRONDRT\0", 8);
        v1[8] = 1;
        v1[12] = 1;
        WriteFile(machine / ".gen-sd.frondsd-root", v1);
    }
    assert(HasGenerations(machine));
    auto contents = TinyContents();
    contents.media.push_back(TinySd(base.size()));
    assert(frondSession::saveFolder(machine, contents));

    // The replaced card's pages are never laid over the new one, and are discarded.
    assert(sdCardInitializeFromFile((machine / frondMachine::sdSibling).c_str(), base.size(),
                                    frondSdFile::headerBytes, "card-one"));
    std::vector<uint8_t> got(SD_SECTOR_SIZE);
    assert(sdCardRead(0, got.data()) && got == SdSector(0xa5));
    assert(!HasGenerations(machine));

    const auto written = SdSector(0x42);
    assert(sdCardWrite(0, written.data()));

    // Saving elsewhere leaves this folder's card and root alone.
    assert(SaveCard(other));
    assert(SidecarPayload(other / frondMachine::sdSibling)[0] == 0x42);
    assert(SidecarPayload(machine / frondMachine::sdSibling) == base);
    assert(!fs::exists(machine / ".gen-sd.frondsd-root"));

    // A failed save leaves the old .frond and card files as a pair.
    fs::path session;
    assert(frondMachine::sessionFile(machine, session));
    const auto oldFrond = ReadAll(session);
    fs::rename(session, dir / "kept.frond");
    fs::create_directories(session);
    assert(!SaveCard(machine));
    fs::remove(session);
    fs::rename(dir / "kept.frond", session);
    assert(ReadAll(session) == oldFrond);
    assert(SidecarPayload(machine / frondMachine::sdSibling) == base);
    assert(!fs::exists(machine / ".gen-sd.frondsd-root"));
    assert(sdCardRead(0, got.data()) && got == written);

    // A save in place moves the live card onto the new sidecar and leaves no generations.
    assert(SaveCard(machine));
    assert(SidecarPayload(machine / frondMachine::sdSibling)[0] == 0x42);
    assert(sdCardBackedBy((machine / frondMachine::sdSibling).c_str()));
    assert(!HasGenerations(machine));
    assert(sdCardRead(0, got.data()) && got == written);
    const auto later = SdSector(0x43);
    assert(sdCardWrite(1, later.data()));
    assert(SaveCard(machine));
    {
        frondSession::Contents loaded;
        fs::path folder;
        assert(frondSession::loadPath(machine, loaded, folder));
        const auto payload = SidecarPayload(machine / frondMachine::sdSibling);
        assert(payload[0] == 0x42 && payload[SD_SECTOR_SIZE] == 0x43);
    }

    // A replaced card writes a fresh sidecar; the old card's generations go.
    sdCardInitialize(sectors, "card-two");
    assert(SaveCard(machine));
    assert(SidecarPayload(machine / frondMachine::sdSibling) == std::vector<uint8_t>(base.size(), 0));
    assert(!HasGenerations(machine));

    // After an unmount the sidecar goes too, so the folder still loads.
    sdCardReset();
    assert(SaveCard(machine));
    assert(!fs::exists(machine / frondMachine::sdSibling));
    {
        frondSession::Contents loaded;
        fs::path folder;
        assert(frondSession::loadPath(machine, loaded, folder));
        assert(loaded.media.empty());
    }
    fs::remove_all(dir);
}

static fs::path LiveCandidate(const fs::path& dir) {
    fs::path found;
    for (const auto& entry : fs::directory_iterator(dir))
        if (frondSdCandidate::candidateName(entry.path().filename().string())) found = entry.path();
    return found;
}

// A large card runs from a candidate sidecar until a save in its folder adopts it.
static void CandidateCards() {
    const auto dir = FixtureDir();
    const auto machine = dir / "machine";
    fs::create_directories(machine);
    const size_t sectors = 16;
    std::vector<uint8_t> base(sectors * SD_SECTOR_SIZE, 0xa5), sdFile;
    assert(frondSdFile::encode(base, sdFile));
    WriteFile(machine / frondMachine::sdSibling, sdFile);
    auto contents = TinyContents();
    contents.media.push_back(TinySd(base.size()));
    assert(frondSession::saveFolder(machine, contents));

    // A replacement card in a candidate: header and CRC complete, sd.frondsd untouched.
    const auto layoutSector = SdSector(0x77);
    std::vector<frondSdCandidate::LayoutSector> layout{{3, layoutSector.data()}};
    frondSdCandidate::Candidate made;
    assert(frondSdCandidate::create(machine, base.size(), layout, made).ok);
    const auto candidatePath = made.path;
    assert(frondSdCandidate::attach(made, base.size(), frondSdCandidate::randomIdentity()));
    assert(LiveCandidate(machine) == candidatePath && sdCardBackedBy(candidatePath.c_str()));
    {
        auto payload = SidecarPayload(candidatePath);
        assert(payload.size() == base.size() && payload[3 * SD_SECTOR_SIZE] == 0x77 && payload[0] == 0);
    }
    const auto written = SdSector(0x42);
    assert(sdCardWrite(0, written.data()));
    assert(SidecarPayload(machine / frondMachine::sdSibling) == base);

    // List, pack and load ignore it.
    const auto name = candidatePath.filename().string();
    assert(frondMachine::ignoredName(name) && !frondMachine::packableName(name));
    for (const auto& file : frondMachine::packableFiles(machine)) assert(file.filename() != name);
    assert(frondMachine::checkSiblings(machine, contents.media) == frondMachine::SiblingStatus::ok);
    {
        frondSession::Contents loaded;
        fs::path folder;
        assert(frondSession::loadPath(machine, loaded, folder));
        assert(loaded.media.size() == 1);
    }

    // Startup cleanup keeps the live (locked) candidate and removes an abandoned one.
    const auto abandoned = machine / ".frondsd.tmp-live-zzzzzz";
    WriteFile(abandoned, sdFile);
    WriteFile(machine / ".gen-.frondsd.tmp-live-zzzzzz-1", {1});
    WriteFile(machine / ".gen-.frondsd.tmp-live-yyyyyy-1", {1});
    frondSdCandidate::cleanup(machine);
    assert(!fs::exists(abandoned) && fs::exists(candidatePath));
    assert(!fs::exists(machine / ".gen-.frondsd.tmp-live-zzzzzz-1"));
    assert(!fs::exists(machine / ".gen-.frondsd.tmp-live-yyyyyy-1"));

    // A failed save leaves the candidate live and the old pair in place.
    fs::path session;
    assert(frondMachine::sessionFile(machine, session));
    const auto oldFrond = ReadAll(session);
    fs::rename(session, dir / "kept.frond");
    fs::create_directories(session);
    assert(!SaveCard(machine));
    fs::remove(session);
    fs::rename(dir / "kept.frond", session);
    assert(ReadAll(session) == oldFrond);
    assert(SidecarPayload(machine / frondMachine::sdSibling) == base);
    assert(fs::exists(candidatePath) && sdCardBackedBy(candidatePath.c_str()));
    std::vector<uint8_t> got(SD_SECTOR_SIZE);
    assert(sdCardRead(0, got.data()) && got == written);

    // Saving to another folder keeps the candidate too.
    const auto other = dir / "other";
    fs::create_directories(other);
    assert(SaveCard(other));
    assert(SidecarPayload(other / frondMachine::sdSibling)[0] == 0x42);
    assert(fs::exists(candidatePath) && sdCardBackedBy(candidatePath.c_str()));

    // A save in place publishes, the card moves onto sd.frondsd and the candidate goes.
    assert(SaveCard(machine));
    {
        const auto payload = SidecarPayload(machine / frondMachine::sdSibling);
        assert(payload[0] == 0x42 && payload[3 * SD_SECTOR_SIZE] == 0x77);
    }
    assert(sdCardBackedBy((machine / frondMachine::sdSibling).c_str()));
    assert(!fs::exists(candidatePath) && LiveCandidate(machine).empty());
    for (const auto& entry : fs::directory_iterator(machine))
        assert(entry.path().filename().string().find(name) == std::string::npos);
    assert(sdCardRead(0, got.data()) && got == written);
    sdCardReset();
    fs::remove_all(dir);
}

static std::vector<std::string> Listing(const fs::path& dir) {
    std::vector<std::string> names;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) names.push_back(entry.path().string());
    std::sort(names.begin(), names.end());
    return names;
}

// A session from outside the sessions folder runs from a work folder and leaves its own alone.
static void OutsideRunsFromWorkFolder() {
    const auto dir = FixtureDir();
    const auto outside = dir / "outside";
    const auto trip = outside / "Trip";
    fs::create_directories(trip);
    std::vector<uint8_t> sdFile;
    assert(frondSdFile::encode(TinyPayload(), sdFile));
    WriteFile(trip / frondMachine::sdSibling, sdFile);
    auto contents = TinyContents();
    contents.media.push_back(TinySd(TinyPayload().size()));
    assert(frondSession::saveFolder(trip, contents));

    uint8_t nor[] = {1, 2, 3}, ram[] = {4, 5, 6, 7}, state[] = {8, 9};
    const auto card = TinyPayload();
    SessionFile5 v6;
    v6.SetDeviceName("palm-tx").SetRamSize(32u << 20).SetDisplayMode(1)
        .SetNor(sizeof(nor), nor).SetMemory(sizeof(ram), ram).SetSavestate(sizeof(state), state)
        .SetSd(card.size(), card.data());
    assert(v6.SerializeAs(6));
    const auto* data = static_cast<const uint8_t*>(v6.GetSerializedSession());
    WriteFile(outside / "old.frond", std::vector<uint8_t>(data, data + v6.GetSerializedSessionSize()));
    const auto before = Listing(outside);
    std::vector<std::vector<uint8_t>> bytes;
    for (const auto& name : before)
        if (fs::is_regular_file(name)) bytes.push_back(ReadAll(name));

    const auto work = dir / "sessions" / "Palm TX";
    fs::create_directories(work);
    frondSession::Contents loaded;
    fs::path folder;
    assert(frondSession::loadPath(trip, loaded, folder, frondSession::msPhysicalBytes, work));
    assert(folder == work && loaded.media.size() == 1);
    assert(SidecarPayload(work / frondMachine::sdSibling) == TinyPayload());

    const auto work2 = dir / "sessions" / "Palm TX 2";
    fs::create_directories(work2);
    assert(frondSession::loadPath(outside / "old.frond", loaded, folder, frondSession::msPhysicalBytes, work2));
    assert(folder == work2 && loaded.media.size() == 1);
    assert(SidecarPayload(work2 / frondMachine::sdSibling) == TinyPayload());

    // The source folder and the lone file are byte for byte as they were, with nothing added.
    assert(Listing(outside) == before);
    size_t i = 0;
    for (const auto& name : before)
        if (fs::is_regular_file(name)) assert(ReadAll(name) == bytes[i++]);

    // Its own folder as the work folder loads in place.
    assert(frondSession::loadPath(work2, loaded, folder, frondSession::msPhysicalBytes, work2));
    assert(folder == work2);
    fs::remove_all(dir);
}

static void NewWritesAreV7() {
    uint8_t nor[] = {1};
    SessionFile5 session;
    session.SetDeviceName("palm-tx").SetNor(sizeof(nor), nor).SetRamSize(16u << 20);
    assert(session.Serialize());
    const auto* bytes = static_cast<const uint8_t*>(session.GetSerializedSession());
    assert(bytes[4] == 9);
    session.SetSd(512, nor);
    assert(!session.Serialize());
}

static std::map<std::string, std::vector<uint8_t>> FolderBytes(const fs::path& dir) {
    std::map<std::string, std::vector<uint8_t>> out;
    for (const auto& entry : fs::directory_iterator(dir)) out[entry.path().filename().string()] = ReadAll(entry.path());
    return out;
}

static std::vector<uint8_t> V2File(const std::vector<uint8_t>& payload, uint64_t generation) {
    std::vector<uint8_t> file;
    assert(frondSdFile::encodeV2(payload, generation, file));
    return file;
}

// 2 MiB and 8 sectors: three chunks, the last one short.
static constexpr size_t kJournalSectors = 4096 + 8;

static std::vector<uint8_t> JournalPayload() {
    std::vector<uint8_t> payload(kJournalSectors * SD_SECTOR_SIZE);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 13 + i / 4096);
    return payload;
}

// Sectors written before the save: first, either side of the first chunk edge, last.
static const std::vector<uint32_t> kJournalWrites = {0, 2047, 2048, uint32_t(kJournalSectors - 1)};

static frondSession::Contents JournalContents(uint64_t generation) {
    auto contents = TinyContents();
    auto media = TinySd(kJournalSectors * SD_SECTOR_SIZE);
    media.contentGeneration = generation;
    contents.media.push_back(media);
    return contents;
}

// A folder saved at generation 1 with the card attached and four sectors written.
static void JournalSetup(const fs::path& machine, std::vector<uint8_t>& oldPayload, std::vector<uint8_t>& newPayload) {
    fs::remove_all(machine);
    fs::create_directories(machine);
    oldPayload = JournalPayload();
    WriteFile(machine / frondMachine::sdSibling, V2File(oldPayload, 1));
    assert(frondSession::saveFolder(machine, JournalContents(1)));
    char id[SD_CARD_ID_MAX_LEN + 1]{};
    std::memset(id, 0x11, SD_CARD_ID_MAX_LEN);
    assert(sdCardInitializeFromFile((machine / frondMachine::sdSibling).c_str(), oldPayload.size(),
                                    frondSdFile::headerBytes, id));
    newPayload = oldPayload;
    for (auto sector : kJournalWrites) {
        const auto bytes = SdSector(uint8_t(0x60 + sector % 7));
        assert(sdCardWrite(sector, bytes.data()));
        std::memcpy(newPayload.data() + size_t(sector) * SD_SECTOR_SIZE, bytes.data(), bytes.size());
    }
}

enum class Stop { none, journal, frond, pages, header, all };

// Runs the save order up to `stop`, then drops the card as a crash would.
static void SaveUntil(const fs::path& machine, Stop stop) {
    frondNative::SdSave sd;
    if (stop != Stop::none) {
        frondMachine::Media media;
        assert(frondNative::beginSdSave(machine, media, true, sd));
        assert(sd.full.empty() && !sd.journal.empty() && media.contentGeneration == 2);
        assert(fs::exists(frondSession::sdJournalPath(machine, 2)));
        if (stop != Stop::journal) {
            auto contents = TinyContents();
            contents.media.push_back(media);
            assert(frondSession::saveFolderFromTemps(machine, contents, {fs::path()}));
        }
        if (stop == Stop::pages)
            assert(frondSession::applySdJournal(machine, sd.journal, nullptr, frondSession::JournalStep::pages));
        if (stop == Stop::header)
            assert(frondSession::applySdJournal(machine, sd.journal, nullptr, frondSession::JournalStep::header));
        if (stop == Stop::all) assert(frondNative::finishSdSave(machine, sd));
    }
    sdCardReset();
}

static void CheckLoadsAs(const fs::path& machine, const std::vector<uint8_t>& payload, uint64_t generation) {
    frondSession::Contents loaded;
    fs::path folder;
    assert(frondSession::loadPath(machine, loaded, folder));
    assert(loaded.media.size() == 1 && loaded.media[0].contentGeneration == generation);
    assert(ReadAll(machine / frondMachine::sdSibling) == V2File(payload, generation));
    assert(frondSession::sdJournals(machine).empty());
    char id[SD_CARD_ID_MAX_LEN + 1]{};
    std::memcpy(id, loaded.media[0].identity.data(), SD_CARD_ID_MAX_LEN);
    assert(sdCardInitializeFromFile((machine / frondMachine::sdSibling).c_str(), payload.size(),
                                    frondSdFile::headerBytes, id));
    std::vector<uint8_t> got(SD_SECTOR_SIZE);
    for (auto sector : kJournalWrites) {
        assert(sdCardRead(sector, got.data()));
        assert(!std::memcmp(got.data(), payload.data() + size_t(sector) * SD_SECTOR_SIZE, SD_SECTOR_SIZE));
    }
    sdCardReset();
}

static void RefusedAsIs(const fs::path& machine) {
    const auto before = FolderBytes(machine);
    frondSession::Contents loaded;
    fs::path folder;
    assert(!frondSession::loadPath(machine, loaded, folder));
    assert(FolderBytes(machine) == before);
}

// Crash before, between and after each save step: the folder loads as the old or the new card,
// byte for byte, or is refused and left as it was.
static void JournalCrashInjection() {
    const auto dir = FixtureDir();
    const auto machine = dir / "machine";
    std::vector<uint8_t> oldPayload, newPayload;

    JournalSetup(machine, oldPayload, newPayload);
    WriteFile(machine / ".frondsd.tmp-partial", {1, 2, 3});  // a journal temp cut short
    SaveUntil(machine, Stop::none);
    CheckLoadsAs(machine, oldPayload, 1);

    JournalSetup(machine, oldPayload, newPayload);
    SaveUntil(machine, Stop::journal);
    CheckLoadsAs(machine, oldPayload, 1);  // descriptor N discards the journal

    for (auto stop : {Stop::frond, Stop::pages, Stop::header, Stop::all}) {
        JournalSetup(machine, oldPayload, newPayload);
        SaveUntil(machine, stop);
        CheckLoadsAs(machine, newPayload, 2);  // descriptor N+1 replays it
    }

    // Replaying twice gives the same bytes.
    JournalSetup(machine, oldPayload, newPayload);
    SaveUntil(machine, Stop::header);
    const auto journal = frondSession::sdJournalPath(machine, 2);
    const auto kept = ReadAll(journal);
    CheckLoadsAs(machine, newPayload, 2);
    WriteFile(journal, kept);
    CheckLoadsAs(machine, newPayload, 2);

    // Refused: no journal, a damaged or short journal, a descriptor two ahead, a sidecar ahead.
    JournalSetup(machine, oldPayload, newPayload);
    SaveUntil(machine, Stop::frond);
    const auto good = ReadAll(journal);
    fs::remove(journal);
    RefusedAsIs(machine);
    auto damaged = good;
    damaged[frondSession::journalHeaderBytes + 20] ^= 1;
    WriteFile(journal, damaged);
    RefusedAsIs(machine);
    WriteFile(journal, std::vector<uint8_t>(good.begin(), good.end() - 1));
    RefusedAsIs(machine);
    WriteFile(journal, good);
    fs::path session;
    assert(frondMachine::sessionFile(machine, session));
    const auto published = ReadAll(session);
    assert(frondSession::saveV7(session, JournalContents(3)));
    RefusedAsIs(machine);
    WriteFile(session, published);
    CheckLoadsAs(machine, newPayload, 2);
    assert(frondSession::saveV7(session, JournalContents(1)));
    RefusedAsIs(machine);  // sidecar at 2, .frond at 1, nothing to replay

    // A .frond at N+1 with the journal still to apply replays it.
    JournalSetup(machine, oldPayload, newPayload);
    SaveUntil(machine, Stop::journal);
    assert(frondSession::saveV7(session, JournalContents(2)));
    {
        frondSession::SdJournal parsed;
        assert(frondSession::readSdJournal(journal, parsed) && parsed.from == 1 && parsed.to == 2);
        assert(parsed.records > 0 && parsed.table.size() == 3);
    }
    CheckLoadsAs(machine, newPayload, 2);
    fs::remove_all(dir);
}

static uint64_t PublishedGeneration(const fs::path& machine) {
    fs::path session;
    assert(frondMachine::sessionFile(machine, session));
    const auto bytes = ReadAll(session);
    frondSession::Contents parsed;
    assert(frondSession::parseFolderSession(bytes.data(), bytes.size(), parsed) && parsed.media.size() == 1);
    return parsed.media[0].contentGeneration;
}

static uint64_t SidecarGeneration(const fs::path& machine) {
    frondSdFile::Info info;
    assert(frondMachine::readSdHeader(machine / frondMachine::sdSibling, info) == frondMachine::SiblingStatus::ok);
    return info.generation;
}

// The live card reads as `payload`: the written sectors and others in the same chunks.
static void LiveReadsAs(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> got(SD_SECTOR_SIZE);
    for (uint32_t sector : {0u, 1u, 8u, 2040u, 2047u, 2048u, 2049u, 3000u, 4096u, uint32_t(kJournalSectors - 1)}) {
        assert(sdCardRead(sector, got.data()));
        assert(!std::memcmp(got.data(), payload.data() + size_t(sector) * SD_SECTOR_SIZE, SD_SECTOR_SIZE));
    }
}

static void WriteLive(uint32_t sector, uint8_t value, std::vector<uint8_t>& payload) {
    const auto bytes = SdSector(value);
    assert(sdCardWrite(sector, bytes.data()));
    std::memcpy(payload.data() + size_t(sector) * SD_SECTOR_SIZE, bytes.data(), bytes.size());
}

// Applying a journal fails partway (ENOSPC, EIO) after the .frond at 2 is published. The save
// counts, the live card keeps every page, and generation 2 is never written again: a reload
// replays journal 2, and the next save makes a full copy at 3 from the live card.
static void FailedApplyKeepsCard() {
    const auto dir = FixtureDir();
    const auto machine = dir / "machine";
    std::vector<uint8_t> oldPayload, newPayload;
    for (int variant = 0; variant < 4; ++variant) {
        JournalSetup(machine, oldPayload, newPayload);
        frondSession::sdJournalFailAfter() = 1;
        assert(SaveCard(machine));
        frondSession::sdJournalFailAfter() = -1;
        assert(PublishedGeneration(machine) == 2 && SidecarGeneration(machine) == 1);
        assert(fs::exists(frondSession::sdJournalPath(machine, 2)));
        assert(sdCardBackedBy((machine / frondMachine::sdSibling).c_str()));
        LiveReadsAs(newPayload);
        const auto saved = newPayload;
        if (variant == 0) {  // a crash now
            sdCardReset();
            CheckLoadsAs(machine, saved, 2);
            continue;
        }
        WriteLive(3000, 0x5a, newPayload);
        if (variant == 3) {  // the next save fails before publishing, then a crash
            fs::path session;
            assert(frondMachine::sessionFile(machine, session));
            const auto published = ReadAll(session);
            fs::remove(session);
            fs::create_directories(session);
            assert(!SaveCard(machine));
            fs::remove(session);
            WriteFile(session, published);
            LiveReadsAs(newPayload);
            sdCardReset();
            CheckLoadsAs(machine, saved, 2);
            continue;
        }
        // Both copy the card at 3; variant 2 with the journal hook still set, which a copy never uses.
        if (variant == 2) frondSession::sdJournalFailAfter() = 0;
        assert(SaveCard(machine));
        frondSession::sdJournalFailAfter() = -1;
        assert(PublishedGeneration(machine) == 3);
        assert(ReadAll(machine / frondMachine::sdSibling) == V2File(newPayload, 3));
        assert(frondSession::sdJournals(machine).empty() && !HasGenerations(machine));
        assert(sdCardBackedBy((machine / frondMachine::sdSibling).c_str()));
        LiveReadsAs(newPayload);
        WriteLive(5, 0x5b, newPayload);
        assert(SaveCard(machine));  // incremental again
        assert(ReadAll(machine / frondMachine::sdSibling) == V2File(newPayload, 4));
        sdCardReset();
        CheckLoadsAs(machine, newPayload, 4);
    }
    fs::remove_all(dir);
}

// The card can't move onto the file a save left at sd.frondsd: a full copy renamed over it while a
// snapshot is held (the card stays on the old, deleted inode), or the hook failing the move after a
// journal. The next save copies the card whole, so a page written back to its old bytes, which
// the overlay keeps as a "same as durable" tomb, is still in the saved card.
static void AdoptFailureCopies() {
    const auto dir = FixtureDir();
    const auto machine = dir / "machine";
    std::vector<uint8_t> oldPayload, newPayload;
    for (int variant = 0; variant < 2; ++variant) {
        JournalSetup(machine, oldPayload, newPayload);
        const auto sidecar = machine / frondMachine::sdSibling;
        if (variant == 0) {
            const auto pin = sdCardSnapshot();
            assert(pin && sdCardPinned());
            assert(SaveCard(machine));  // pinned: a full copy, and the move is refused
            assert(ReadAll(sidecar) == V2File(newPayload, 2));
        } else {
            frondNative::sdAdoptFailForTest() = true;
            assert(SaveCard(machine));  // a journal applied in place
            frondNative::sdAdoptFailForTest() = false;
            assert(ReadAll(sidecar) == V2File(newPayload, 2));
        }
        assert(PublishedGeneration(machine) == 2 && SidecarGeneration(machine) == 2);
        assert(sdCardRunsOn(sidecar.c_str()) && !sdCardBackedBy(sidecar.c_str()));
        LiveReadsAs(newPayload);
        // Sector 2047 back to its generation 1 bytes, sector 3000 new.
        std::vector<uint8_t> back(oldPayload.begin() + 2047 * SD_SECTOR_SIZE, oldPayload.begin() + 2048 * SD_SECTOR_SIZE);
        assert(sdCardWrite(2047, back.data()));
        std::memcpy(newPayload.data() + 2047 * SD_SECTOR_SIZE, back.data(), back.size());
        WriteLive(3000, 0x5c, newPayload);
        assert(SaveCard(machine));
        assert(PublishedGeneration(machine) == 3);
        assert(ReadAll(sidecar) == V2File(newPayload, 3));
        assert(frondSession::sdJournals(machine).empty());
        assert(sdCardBackedBy(sidecar.c_str()));  // moved onto the copy
        LiveReadsAs(newPayload);
        WriteLive(5, 0x5d, newPayload);
        assert(SaveCard(machine));  // incremental again
        assert(ReadAll(sidecar) == V2File(newPayload, 4));
        sdCardReset();
        CheckLoadsAs(machine, newPayload, 4);
    }
    fs::remove_all(dir);
}

// A full copy crashes before or after its .frond is published, before the copy is renamed onto
// sd.frondsd. The folder loads as the old or the new card, and a work-folder load copies it.
static void FullCopyCrash() {
    const auto dir = FixtureDir();
    const auto machine = dir / "machine";
    std::vector<uint8_t> oldPayload, newPayload;
    for (bool published : {false, true}) {
        // A card with a snapshot held is copied, not journalled.
        JournalSetup(machine, oldPayload, newPayload);
        {
            auto pin = sdCardSnapshot();
            frondNative::SdSave sd;
            frondMachine::Media media;
            assert(frondNative::beginSdSave(machine, media, true, sd));
            assert(sd.journal.empty() && sd.full == frondSession::sdFullPath(machine, 2) && fs::exists(sd.full));
            if (published) {
                auto contents = TinyContents();
                contents.media.push_back(media);
                assert(frondSession::saveFolderFromTemps(machine, contents, {fs::path()}));
            }
        }
        sdCardReset();
        assert(SidecarGeneration(machine) == 1);
        if (published) {
            const auto before = FolderBytes(machine);
            const auto work = dir / "work";
            fs::remove_all(work);
            fs::create_directories(work);
            frondSession::Contents loaded;
            fs::path folder;
            assert(frondSession::loadPath(machine, loaded, folder, frondSession::msPhysicalBytes, work));
            assert(folder == work && ReadAll(work / frondMachine::sdSibling) == V2File(newPayload, 2));
            assert(FolderBytes(machine) == before);
        }
        CheckLoadsAs(machine, published ? newPayload : oldPayload, published ? 2 : 1);
    }

    // The first save of a new card, with no sd.frondsd yet.
    fs::remove_all(machine);
    fs::create_directories(machine);
    assert(frondSession::saveFolder(machine, TinyContents()));
    sdCardInitialize(kJournalSectors, "ram-card");
    std::vector<uint8_t> payload(kJournalSectors * SD_SECTOR_SIZE, 0);
    WriteLive(7, 0x71, payload);
    WriteLive(uint32_t(kJournalSectors - 1), 0x72, payload);
    {
        frondNative::SdSave sd;
        frondMachine::Media media;
        assert(frondNative::beginSdSave(machine, media, true, sd));
        assert(media.contentGeneration == 1 && !sd.full.empty());
        auto contents = TinyContents();
        contents.media.push_back(media);
        assert(frondSession::saveFolderFromTemps(machine, contents, {fs::path()}));
    }
    sdCardReset();
    assert(!fs::exists(machine / frondMachine::sdSibling));
    {
        frondSession::Contents loaded;
        fs::path folder;
        assert(frondSession::loadPath(machine, loaded, folder));
        assert(loaded.media.size() == 1 && loaded.media[0].contentGeneration == 1);
        assert(ReadAll(machine / frondMachine::sdSibling) == V2File(payload, 1));
        assert(frondSession::sdJournals(machine).empty());
    }
    // A damaged copy is refused and left as it was.
    sdCardInitialize(kJournalSectors, "ram-card");
    WriteLive(9, 0x73, payload);
    {
        frondNative::SdSave sd;
        frondMachine::Media media;
        assert(frondNative::beginSdSave(machine, media, true, sd));
        assert(media.contentGeneration == 2 && !sd.full.empty());
        auto contents = TinyContents();
        contents.media.push_back(media);
        assert(frondSession::saveFolderFromTemps(machine, contents, {fs::path()}));
        auto bytes = ReadAll(sd.full);
        bytes[40] ^= 1;
        WriteFile(sd.full, bytes);
    }
    sdCardReset();
    RefusedAsIs(machine);
    fs::remove_all(dir);
}

// A save that fails after its .frond is published, here on a leftover Memory Stick that won't
// go, still finishes its card file: the journal is applied or the full copy renamed into place.
static void PublishedThenFails() {
    const auto dir = FixtureDir();
    const auto machine = dir / "machine";
    std::vector<uint8_t> oldPayload, newPayload;
    for (bool copy : {false, true}) {
        JournalSetup(machine, oldPayload, newPayload);
        fs::create_directories(machine / frondMachine::msSibling / "x");
        {
            std::shared_ptr<const CardSnapshot> pin;
            if (copy) pin = sdCardSnapshot();  // a card with a snapshot held is copied
            assert(!SaveCard(machine));
        }
        assert(PublishedGeneration(machine) == 2 && SidecarGeneration(machine) == 2);
        assert(ReadAll(machine / frondMachine::sdSibling) == V2File(newPayload, 2));
        assert(frondSession::sdJournals(machine).empty());
        LiveReadsAs(newPayload);
        sdCardReset();
        fs::remove_all(machine / frondMachine::msSibling);
        CheckLoadsAs(machine, newPayload, 2);
    }
    fs::remove_all(dir);
}

// A save left unsettled in one folder, then one left unsettled in another: the next save to the
// first folder copies the card past journal 2 and never writes generation 2 again.
static void UnsettledInTwoFolders() {
    const auto dir = FixtureDir();
    const auto machine = dir / "machine";
    const auto other = dir / "other";
    std::vector<uint8_t> oldPayload, newPayload;
    JournalSetup(machine, oldPayload, newPayload);
    frondSession::sdJournalFailAfter() = 1;
    assert(SaveCard(machine));
    frondSession::sdJournalFailAfter() = -1;
    const auto journal2 = ReadAll(frondSession::sdJournalPath(machine, 2));
    fs::create_directories(other);
    assert(frondSession::saveFolder(other, TinyContents()));
    fs::create_directories(other / frondMachine::sdSibling / "x");  // the copy can't be renamed
    assert(SaveCard(other));
    assert(PublishedGeneration(other) == 1 && fs::exists(frondSession::sdFullPath(other, 1)));
    const auto saved = newPayload;
    WriteLive(3000, 0x5a, newPayload);
    assert(ReadAll(frondSession::sdJournalPath(machine, 2)) == journal2);
    assert(SaveCard(machine));
    assert(PublishedGeneration(machine) == 3);
    assert(ReadAll(machine / frondMachine::sdSibling) == V2File(newPayload, 3));
    assert(frondSession::sdJournals(machine).empty());
    LiveReadsAs(newPayload);
    sdCardReset();
    CheckLoadsAs(machine, newPayload, 3);
    fs::remove_all(other / frondMachine::sdSibling);
    {
        frondSession::Contents loaded;
        fs::path folder;
        assert(frondSession::loadPath(other, loaded, folder));
        assert(ReadAll(other / frondMachine::sdSibling) == V2File(saved, 1));
    }
    fs::remove_all(dir);
}

// A save left unsettled under the live card, then a page written back to its bytes before that
// save: the next save copies the live card at 3 instead of replaying journal 2 under it, so the
// page keeps its older bytes. Same for a full copy left unrenamed.
static void UnsettledUnderLiveCard() {
    const auto dir = FixtureDir();
    const auto machine = dir / "machine";
    std::vector<uint8_t> oldPayload, newPayload;
    const uint32_t last = uint32_t(kJournalSectors - 1);
    for (bool copy : {false, true}) {
        JournalSetup(machine, oldPayload, newPayload);
        if (copy) {
            auto pin = sdCardSnapshot();  // a card with a snapshot held is copied
            frondNative::SdSave sd;
            frondMachine::Media media;
            assert(frondNative::beginSdSave(machine, media, true, sd) && !sd.full.empty());
            auto contents = TinyContents();
            contents.media.push_back(media);
            assert(frondSession::saveFolderFromTemps(machine, contents, {fs::path()}));
            assert(fs::exists(frondSession::sdFullPath(machine, 2)));
        } else {
            frondSession::sdJournalFailAfter() = 1;  // the last sector's record is never applied
            assert(SaveCard(machine));
            frondSession::sdJournalFailAfter() = -1;
            assert(fs::exists(frondSession::sdJournalPath(machine, 2)));
        }
        assert(PublishedGeneration(machine) == 2 && SidecarGeneration(machine) == 1);
        struct stat before {};
        assert(stat((machine / frondMachine::sdSibling).c_str(), &before) == 0);
        // Back to the bytes sd.frondsd still holds, then another change elsewhere.
        std::vector<uint8_t> back(oldPayload.begin() + size_t(last) * SD_SECTOR_SIZE,
                                  oldPayload.begin() + size_t(last + 1) * SD_SECTOR_SIZE);
        assert(sdCardWrite(last, back.data()));
        std::memcpy(newPayload.data() + size_t(last) * SD_SECTOR_SIZE, back.data(), back.size());
        WriteLive(3000, 0x5a, newPayload);
        { auto pin = sdCardSnapshot(); }  // flushes the page as one that matches sd.frondsd
        LiveReadsAs(newPayload);
        assert(SaveCard(machine));
        assert(PublishedGeneration(machine) == 3);
        assert(ReadAll(machine / frondMachine::sdSibling) == V2File(newPayload, 3));
        assert(frondSession::sdJournals(machine).empty() && !HasGenerations(machine));
        struct stat after {};
        assert(stat((machine / frondMachine::sdSibling).c_str(), &after) == 0 && after.st_ino != before.st_ino);
        assert(sdCardBackedBy((machine / frondMachine::sdSibling).c_str()));
        LiveReadsAs(newPayload);
        WriteLive(5, 0x5b, newPayload);
        assert(SaveCard(machine));  // incremental again, on the copy
        assert(ReadAll(machine / frondMachine::sdSibling) == V2File(newPayload, 4));
        LiveReadsAs(newPayload);
        sdCardReset();
        CheckLoadsAs(machine, newPayload, 4);
    }
    fs::remove_all(dir);
}

// Another process's cleanup between creating a candidate and locking it: a new one is made.
static void CandidateRace() {
    const auto dir = FixtureDir();
    const auto machine = dir / "machine";
    fs::create_directories(machine);
    int calls = 0;
    fs::path first;
    frondSdCandidate::beforeLockHook() = [&](const fs::path& path) {
        if (calls++ == 0) {
            first = path;
            frondSdCandidate::cleanup(machine);
        }
    };
    frondSdCandidate::Candidate made;
    std::vector<frondSdCandidate::LayoutSector> layout;
    assert(frondSdCandidate::create(machine, kJournalSectors * SD_SECTOR_SIZE, layout, made).ok);
    frondSdCandidate::beforeLockHook() = nullptr;
    assert(calls == 2 && !fs::exists(first) && made.path != first && fs::exists(made.path));
    frondSdCandidate::cleanup(machine);  // locked, so kept
    assert(fs::exists(made.path) && LiveCandidate(machine) == made.path);
    frondSdCandidate::discard(made);
    fs::remove_all(dir);
}

// A v7 folder with a v1 sidecar loads, its first v8 save rewrites the sidecar once as v2, and the
// next save is incremental.
static void V7FixtureMigrates() {
    const auto dir = FixtureDir();
    const auto machine = dir / "machine";
    fs::create_directories(machine);
    const size_t sectors = 4096;
    std::vector<uint8_t> payload(sectors * SD_SECTOR_SIZE);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 7);
    std::vector<uint8_t> v1;
    assert(frondSdFile::encode(payload, v1));
    WriteFile(machine / frondMachine::sdSibling, v1);
    auto contents = TinyContents();
    contents.media.push_back(TinySd(payload.size()));
    std::vector<uint8_t> media;
    assert(frondSession::encodeMedia(contents.media, media, frondMachine::descriptorBytesV7) && media.size() == 92);
    SessionFile5 v7;
    v7.SetDeviceName("palm-tx").SetDisplayMode(1).SetRamSize(contents.ramSize)
        .SetNor(contents.nor.size(), contents.nor.data()).SetMemory(contents.memory.size(), contents.memory.data())
        .SetSavestate(contents.savestate.size(), contents.savestate.data()).SetMedia(media.size(), media.data());
    assert(v7.SerializeAs(7));
    const auto* bytes = static_cast<const uint8_t*>(v7.GetSerializedSession());
    WriteFile(machine / "machine.frond", std::vector<uint8_t>(bytes, bytes + v7.GetSerializedSessionSize()));
    assert(bytes[4] == 7);

    frondSession::Contents loaded;
    fs::path folder;
    assert(frondSession::loadPath(machine, loaded, folder));
    assert(loaded.media.size() == 1 && loaded.media[0].contentGeneration == 0 && loaded.nor == contents.nor);
    char id[SD_CARD_ID_MAX_LEN + 1]{};
    std::memcpy(id, loaded.media[0].identity.data(), SD_CARD_ID_MAX_LEN);
    assert(sdCardInitializeFromFile((machine / frondMachine::sdSibling).c_str(), payload.size(),
                                    frondSdFile::headerBytes, id));
    const auto first = SdSector(0x31);
    assert(sdCardWrite(5, first.data()));
    std::memcpy(payload.data() + 5 * SD_SECTOR_SIZE, first.data(), SD_SECTOR_SIZE);
    struct stat st {};
    assert(stat((machine / frondMachine::sdSibling).c_str(), &st) == 0);
    const auto v1Inode = st.st_ino;
    assert(SaveCard(machine));
    assert(ReadAll(machine / "machine.frond")[4] == 8);
    assert(ReadAll(machine / frondMachine::sdSibling) == V2File(payload, 1));
    assert(stat((machine / frondMachine::sdSibling).c_str(), &st) == 0 && st.st_ino != v1Inode);
    const auto v2Inode = st.st_ino;

    const auto second = SdSector(0x32);
    assert(sdCardWrite(4000, second.data()));
    std::memcpy(payload.data() + 4000 * SD_SECTOR_SIZE, second.data(), SD_SECTOR_SIZE);
    assert(SaveCard(machine));
    assert(stat((machine / frondMachine::sdSibling).c_str(), &st) == 0 && st.st_ino == v2Inode);
    assert(ReadAll(machine / frondMachine::sdSibling) == V2File(payload, 2));
    assert(frondSession::sdJournals(machine).empty() && !HasGenerations(machine));
    std::vector<uint8_t> got(SD_SECTOR_SIZE);
    assert(sdCardRead(4000, got.data()) && got == second);
    sdCardReset();
    frondSession::Contents again;
    assert(frondSession::loadPath(machine, again, folder));
    assert(again.media.size() == 1 && again.media[0].contentGeneration == 2);
    fs::remove_all(dir);
}

// A damaged chunk fails the reads that touch it, which the guest sees as an I/O error. Other
// chunks still read, and a full check refuses the card.
static void LazyChunkCheck() {
    const auto dir = FixtureDir();
    const auto machine = dir / "machine";
    fs::create_directories(machine);
    auto payload = JournalPayload();
    auto file = V2File(payload, 1);
    file[frondSdFile::headerBytes + (1u << 20) + 100] ^= 0xff;  // chunk 1
    WriteFile(machine / frondMachine::sdSibling, file);
    auto contents = JournalContents(1);
    frondSession::Contents loaded;
    fs::path folder;
    assert(frondSession::saveV7(machine / "machine.frond", contents));
    assert(frondSession::loadPath(machine, loaded, folder));  // header and table are fine
    assert(frondMachine::checkSiblings(machine, contents.media, true) == frondMachine::SiblingStatus::malformed);
    char id[SD_CARD_ID_MAX_LEN + 1]{};
    assert(sdCardInitializeFromFile((machine / frondMachine::sdSibling).c_str(), payload.size(),
                                    frondSdFile::headerBytes, id));
    std::vector<uint8_t> got(SD_SECTOR_SIZE);
    assert(sdCardRead(0, got.data()) && !std::memcmp(got.data(), payload.data(), SD_SECTOR_SIZE));
    assert(!sdCardRead(2048, got.data()));
    assert(!sdCardRead(4095, got.data()));
    const auto bytes = SdSector(0x77);
    assert(!sdCardWrite(3000, bytes.data()));  // a partial page reads the bad chunk first
    assert(sdCardRead(4096, got.data()));
    assert(!std::memcmp(got.data(), payload.data() + 4096 * SD_SECTOR_SIZE, SD_SECTOR_SIZE));
    auto pin = sdCardSnapshot();
    assert(pin && !pin->Read(uint64_t(2100) * SD_SECTOR_SIZE, SD_SECTOR_SIZE, got.data()));
    pin.reset();
    sdCardReset();
    fs::remove_all(dir);
}

// Classic sticks of 4 and 64 MB keep their geometry through a save and load; a descriptor or
// sidecar of another size is refused.
static void MsSizesRoundTrip() {
    for (uint32_t megabytes : {4u, 64u}) {
        const auto dir = FixtureDir();
        const auto machine = dir / "sony-clie-tg50";
        fs::create_directories(machine);
        const auto& g = *frondMsGeometry::byMegabytes(megabytes);
        auto physical = frondMs::create(g);
        physical[size_t(g.PhysicalBytes()) - 1] = 0x42;
        assert(frondSession::writeMsFile(machine / frondMachine::msSibling, physical.data(), physical.size()));
        const auto sidecar = ReadAll(machine / frondMachine::msSibling);
        assert(sidecar.size() == frondMs::fileBytes(g));
        std::vector<uint8_t> decoded;
        assert(frondMs::decode(sidecar.data(), sidecar.size(), decoded) && decoded == physical);
        auto contents = TinyContents();
        contents.deviceName = "sony-clie-tg50";
        frondMachine::Media ms;
        ms.kind = frondMachine::kindMs;
        ms.name = frondMachine::msSibling;
        ms.inserted = true;
        ms.logicalSize = g.LogicalBytes();
        ms.physicalSize = g.PhysicalBytes();
        contents.media.push_back(ms);
        assert(frondSession::saveFolder(machine, contents));
        frondSession::Contents loaded;
        fs::path folder;
        assert(frondSession::loadPath(machine, loaded, folder));
        assert(loaded.media.size() == 1 && loaded.media[0].kind == frondMachine::kindMs);
        assert(loaded.media[0].logicalSize == (uint64_t(megabytes) << 20));
        assert(loaded.media[0].physicalSize == g.PhysicalBytes());
        assert(ReadAll(machine / frondMachine::msSibling) == sidecar);

        // Descriptors must name one of the six sizes, with a matching logical size.
        std::vector<uint8_t> bytes;
        auto odd = ms;
        odd.logicalSize = frondMachine::msLogicalBytes;
        assert(!frondMachine::encodeDescriptor(odd, bytes));
        odd = ms;
        odd.physicalSize += 521;
        assert(!frondMachine::encodeDescriptor(odd, bytes));
        odd.physicalSize = uint64_t(16384) * 32 * 521;  // 256 MB is not a classic size
        odd.logicalSize = 256ull << 20;
        assert(!frondMachine::encodeDescriptor(odd, bytes));

        // A descriptor for the other size, then a header naming the other geometry, are refused.
        fs::path session;
        assert(frondMachine::sessionFile(machine, session));
        const auto previous = ReadAll(session);
        const auto& other = *frondMsGeometry::byMegabytes(megabytes == 4 ? 64 : 4);
        auto mismatch = contents;
        mismatch.media[0].logicalSize = other.LogicalBytes();
        mismatch.media[0].physicalSize = other.PhysicalBytes();
        assert(frondMachine::checkSiblings(machine, mismatch.media) == frondMachine::SiblingStatus::mismatch);
        assert(!frondSession::saveFolder(machine, mismatch));
        assert(ReadAll(session) == previous);
        auto wrongHeader = sidecar;
        frondSdFile::put32(wrongHeader.data() + 12, other.blocks);
        frondSdFile::put32(wrongHeader.data() + 16, other.pagesPerBlock);
        WriteFile(machine / frondMachine::msSibling, wrongHeader);
        assert(frondMachine::checkSiblings(machine, contents.media) == frondMachine::SiblingStatus::malformed);
        assert(!frondSession::loadPath(machine, loaded, folder));
        fs::remove_all(dir);
    }
}

// A Memory Stick Pro (kind 3) is an SD-style sidecar named ms.frondmsp, with its own journals.
static bool SaveProCard(const fs::path& folder, bool finish = true) {
    auto contents = TinyContents();
    contents.deviceName = "sony-clie-tg50";
    std::vector<fs::path> temps;
    frondNative::SdSave sd;
    frondMachine::Media media;
    if (!frondNative::beginSdSave(folder, media, true, sd, frondMachine::kindMsPro, true)) return false;
    assert(media.kind == frondMachine::kindMsPro && media.name == frondMachine::msProSibling && media.writeProtected);
    temps.emplace_back();
    contents.media.push_back(media);
    if (finish) return frondNative::publishWithSd(folder, contents, temps, sd);
    bool published = false;
    return frondSession::saveFolderFromTemps(folder, contents, temps, &published) && published;
}

static void MsProSession() {
    const auto dir = FixtureDir();
    const auto machine = dir / "sony-clie-tg50";
    fs::create_directories(machine);
    const uint64_t bytes = uint64_t(256) << 20;
    const auto boot = SdSector(0x77);
    std::vector<frondSdCandidate::LayoutSector> layout{{63, boot.data()}};
    frondSdCandidate::Candidate made;
    assert(frondSdCandidate::create(machine, bytes, layout, made).ok);
    assert(frondSdCandidate::attach(made, bytes, frondSdCandidate::randomIdentity()));
    const auto first = SdSector(0x42), last = SdSector(0x24);
    const uint32_t lastSector = uint32_t(bytes / SD_SECTOR_SIZE) - 1;
    assert(sdCardWrite(0, first.data()) && sdCardWrite(lastSector, last.data()));

    // The first save copies the candidate to ms.frondmsp; no sd.frondsd appears.
    assert(SaveProCard(machine));
    assert(fs::exists(machine / frondMachine::msProSibling) && !fs::exists(machine / frondMachine::sdSibling));
    assert(sdCardBackedBy((machine / frondMachine::msProSibling).c_str()));
    assert(frondMachine::packableName(frondMachine::msProSibling));
    frondSession::Contents loaded;
    fs::path folder;
    assert(frondSession::loadPath(machine, loaded, folder));
    assert(loaded.media.size() == 1 && loaded.media[0].kind == frondMachine::kindMsPro);
    assert(loaded.media[0].physicalSize == bytes && loaded.media[0].contentGeneration == 1);

    // The next save journals as ms.frondmsp.journal-2; left unapplied, a load replays it.
    const auto changed = SdSector(0x99);
    assert(sdCardWrite(1, changed.data()));
    assert(SaveProCard(machine, false));
    const auto journal = machine / "ms.frondmsp.journal-2";
    assert(fs::exists(journal) && frondMachine::ignoredName(journal.filename().string()));
    assert(!frondMachine::packableName(journal.filename().string()));
    sdCardReset();
    assert(frondSession::loadPath(machine, loaded, folder));
    assert(!fs::exists(journal) && loaded.media[0].contentGeneration == 2);
    frondSdFile::Info info;
    assert(frondMachine::readSdHeader(machine / frondMachine::msProSibling, info) == frondMachine::SiblingStatus::ok);
    assert(info.generation == 2);
    char id[SD_CARD_ID_MAX_LEN + 1]{};
    std::memcpy(id, loaded.media[0].identity.data(), SD_CARD_ID_MAX_LEN);
    assert(sdCardInitializeFromFile((machine / frondMachine::msProSibling).c_str(), bytes, frondSdFile::headerBytes, id));
    std::vector<uint8_t> got(SD_SECTOR_SIZE);
    assert(sdCardRead(0, got.data()) && got == first);
    assert(sdCardRead(1, got.data()) && got == changed);
    assert(sdCardRead(63, got.data()) && got == boot);
    assert(sdCardRead(lastSector, got.data()) && got == last);

    // An in-place save while running on ms.frondmsp journals and applies.
    assert(sdCardWrite(2, changed.data()) && SaveProCard(machine));
    assert(frondMachine::readSdHeader(machine / frondMachine::msProSibling, info) == frondMachine::SiblingStatus::ok);
    assert(info.generation == 3 && SidecarPayload(machine / frondMachine::msProSibling)[2 * SD_SECTOR_SIZE] == 0x99);

    // Descriptors: only the Pro sizes, under ms.frondmsp; an SD descriptor can't name it.
    std::vector<uint8_t> encoded;
    auto pro = loaded.media[0];
    pro.contentGeneration = 3;
    assert(frondMachine::encodeDescriptor(pro, encoded));
    frondMachine::Media decoded;
    assert(frondMachine::decodeDescriptor(encoded.data(), encoded.size(), decoded) && decoded.kind == frondMachine::kindMsPro);
    auto odd = pro;
    odd.physicalSize = odd.logicalSize = 128ull << 20;
    assert(!frondMachine::encodeDescriptor(odd, encoded));
    odd = pro;
    odd.name = frondMachine::sdSibling;
    assert(!frondMachine::encodeDescriptor(odd, encoded));
    for (unsigned mib : frondMsPro::cardMibs) {
        odd = pro;
        odd.physicalSize = odd.logicalSize = uint64_t(mib) << 20;
        assert(frondMachine::encodeDescriptor(odd, encoded));
    }
    // A classic stick beside a Pro is refused: there is one slot.
    WriteFile(machine / frondMachine::msSibling, {1});
    assert(frondMachine::checkSiblings(machine, {pro}) == frondMachine::SiblingStatus::extra);
    fs::remove(machine / frondMachine::msSibling);
    assert(frondMachine::checkSiblings(machine, {pro}) == frondMachine::SiblingStatus::ok);
    sdCardReset();
    fs::remove_all(dir);
}

// Session v9: internal NAND in nand.frondnand with a kind 4 descriptor and an empty NAND section.
// A v8 session still loads its embedded NAND, a v9 one that embeds NAND is refused, the file
// goes with a work-folder copy, and a NAND journal a save left is replayed at load.
static void NandV9Folder() {
    const auto dir = FixtureDir();
    const auto machine = dir / "palm-e2";
    fs::create_directories(machine);
    constexpr uint64_t raw = 528 * 4096;  // 2 MiB of main bytes, 64 KiB spare
    const uint64_t stored = frondNand::storedBytes(raw);
    assert(stored == (2u << 20) + 65536 && frondNand::sideOffset(raw) == (2u << 20));
    std::vector<uint8_t> bytes(static_cast<size_t>(stored));
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(i * 7 + i / 512);
    frondMachine::Media media;
    frondNand::fillMedia(media, raw);
    assert(frondMachine::validMedia(media) && media.logicalSize == (2u << 20));
    auto identity = media.identity;
    assert(frondSession::writeSdFromReader(
        machine / frondMachine::nandSibling, stored,
        [&](uint64_t off, uint64_t n, void* dest) { return std::memcpy(dest, bytes.data() + off, n), true; }, identity, 1,
        frondSession::nandShape(raw)));
    assert(identity == media.identity);
    media.contentGeneration = 1;
    auto contents = TinyContents();
    contents.deviceName = "palm-e2";
    contents.nand.clear();
    contents.media.push_back(media);
    assert(frondSession::saveFolder(machine, contents));
    fs::path session;
    assert(frondMachine::sessionFile(machine, session));
    auto saved = ReadAll(session);
    assert(saved[4] == 9);
    SessionFile5 probe;
    assert(probe.Deserialize(saved.size(), saved.data()) && probe.GetVersion() == 9 && probe.GetNandSize() == 0);
    frondSession::Contents loaded;
    fs::path folder;
    assert(frondSession::loadPath(machine, loaded, folder) && loaded.nand.empty());
    assert(loaded.media.size() == 1 && loaded.media[0].kind == frondMachine::kindNand &&
           loaded.media[0].physicalSize == raw && loaded.media[0].name == frondMachine::nandSibling);
    assert(frondMachine::checkSiblings(machine, loaded.media, true) == frondMachine::SiblingStatus::ok);

    // A header for another size, or the SD magic, doesn't match.
    auto other = media;
    frondNand::fillMedia(other, raw * 2);
    other.contentGeneration = 1;
    assert(frondMachine::checkSiblings(machine, {other}) == frondMachine::SiblingStatus::mismatch);
    assert(!frondNand::openFile(machine / frondMachine::nandSibling, raw * 2));
    // Missing, or there with no descriptor: refused.
    fs::rename(machine / frondMachine::nandSibling, dir / "nand.keep");
    assert(!frondSession::loadPath(machine, loaded, folder));
    fs::rename(dir / "nand.keep", machine / frondMachine::nandSibling);
    assert(frondMachine::checkSiblings(machine, {}) == frondMachine::SiblingStatus::extra);
    assert(frondMachine::packableName(frondMachine::nandSibling));
    assert(frondMachine::ignoredName("nand.frondnand.journal-2") && frondMachine::ignoredName(".frondnand.tmp-x"));

    // v9 never embeds NAND.
    uint8_t one[] = {1};
    SessionFile5 embedded;
    embedded.SetDeviceName("palm-e2").SetNor(1, one).SetNand(1, one).SetRamSize(16u << 20);
    assert(!embedded.SerializeAs(9) && embedded.SerializeAs(8));
    std::vector<uint8_t> forged(static_cast<const uint8_t*>(embedded.GetSerializedSession()),
                                static_cast<const uint8_t*>(embedded.GetSerializedSession()) + embedded.GetSerializedSessionSize());
    SessionFile5 back;
    assert(back.Deserialize(forged.size(), forged.data()) && back.GetNandSize() == 1);
    forged[4] = 9;
    assert(!back.Deserialize(forged.size(), forged.data()));

    // A journal from a save that published its .frond at 2 and stopped is replayed at load.
    {
        auto file = frondNand::openFile(machine / frondMachine::nandSibling, raw);
        assert(file);
        const std::vector<uint8_t> page(512, 0x3c), spare(16, 0x00);
        assert(file->Write(4096, page.size(), page.data()) &&
               file->Write(frondNand::sideOffset(raw) + 8 * 16, spare.size(), spare.data()));
        std::memcpy(bytes.data() + 4096, page.data(), page.size());
        std::memcpy(bytes.data() + frondNand::sideOffset(raw) + 8 * 16, spare.data(), spare.size());
        frondNative::SdSave save;
        save.kind = frondMachine::kindNand;
        auto next = media;
        auto pin = file->Snapshot();
        assert(frondNative::journalStore(machine, next, save,
                                         [&](const frondNative::StoreChangeFn& fn) { return file->ForEachOverlayIn(*pin, fn); }));
        assert(next.contentGeneration == 2 && !save.journal.empty());
        contents.media = {next};
        assert(frondSession::saveFolderFromTemps(machine, contents, {fs::path()}));
    }
    FileCardBacking::DiscardGenerations(machine / frondMachine::nandSibling);
    // A copy into a work folder takes the file and its journal.
    const auto work = dir / "work";
    fs::create_directories(work);
    assert(frondSession::loadPath(machine, loaded, folder, frondSession::msPhysicalBytes, work) && folder == work);
    auto storedOf = [&](const fs::path& path) {
        const auto file = ReadAll(path);
        assert(file.size() == 64 + stored + 4 * frondSdFile::chunkCount(stored));
        return std::vector<uint8_t>(file.begin() + 64, file.begin() + 64 + long(stored));
    };
    assert(storedOf(work / frondMachine::nandSibling) == bytes);
    assert(frondSession::loadPath(machine, loaded, folder) && loaded.media[0].contentGeneration == 2);
    assert(storedOf(machine / frondMachine::nandSibling) == bytes);
    assert(frondSession::sdJournals(machine, frondMachine::kindNand).empty());
    fs::remove_all(dir);
}

int main() {
    RoundTrip();
    RefusedSiblingsLeaveBytes();
    TruncatedHeader();
    LegacyV6();
    LoneV6Bin();
    MsExtractStub();
    MsSizesRoundTrip();
    MsProSession();
    IdentityAndFailedPublish();
    SavesKeepCardsConsistent();
    CandidateCards();
    OutsideRunsFromWorkFolder();
    NewWritesAreV7();
    JournalCrashInjection();
    FailedApplyKeepsCard();
    FullCopyCrash();
    AdoptFailureCopies();
    PublishedThenFails();
    UnsettledInTwoFolders();
    UnsettledUnderLiveCard();
    CandidateRace();
    V7FixtureMigrates();
    LazyChunkCheck();
    NandV9Folder();
    std::puts("Session v7 and streamed v6 extraction checks passed");
}
