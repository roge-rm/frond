#include "CardBacking.h"
#include "FrondSdImage.h"
#include "MachineFolder.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void WriteFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    assert(out);
    if (!bytes.empty()) assert(out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

static std::vector<uint8_t> TinyPayload() { return std::vector<uint8_t>(512, 0xa5); }

static frondMachine::Media SdCard(uint64_t bytes, bool inserted = true) {
    frondMachine::Media media;
    media.kind = frondMachine::kindSd;
    media.name = frondMachine::sdSibling;
    media.inserted = inserted;
    media.writeProtected = false;
    media.identity.fill(0x11);
    media.logicalSize = bytes;
    media.physicalSize = bytes;
    return media;
}

static frondMachine::Media MsCard() {
    frondMachine::Media media;
    media.kind = frondMachine::kindMs;
    media.name = frondMachine::msSibling;
    media.inserted = true;
    media.identity.fill(0x22);
    media.logicalSize = frondMachine::msLogicalBytes;
    media.physicalSize = frondMachine::msPhysicalBytes;
    return media;
}

static void RangeChecks() {
    assert(cardRangeOk(100, 0, 100));
    assert(cardRangeOk(100, 100, 0));
    assert(cardRangeOk(0, 0, 0));
    assert(!cardRangeOk(100, 101, 0));
    assert(!cardRangeOk(100, 50, 51));
    assert(!cardRangeOk(100, 0, 101));
    size_t host = 99;
    assert(cardRangeOk(100, 40, 10, &host) && host == 10);
    assert(!cardRangeOk(8, std::numeric_limits<uint64_t>::max(), 0));
    assert(!cardRangeOk(8, 0, std::numeric_limits<uint64_t>::max()));

    RamCardBacking card(std::vector<uint8_t>(16, 0x3c));
    uint8_t buf[16];
    std::memset(buf, 0xee, sizeof(buf));
    assert(card.Read(16, 0, nullptr));
    assert(card.Write(16, 0, nullptr));
    assert(card.Read(0, 0, nullptr));
    assert(!card.Read(16, 1, buf) && buf[0] == 0xee);
    assert(!card.Read(17, 0, buf));
    assert(!card.Write(15, 2, buf));
    assert(!card.Read(0, 16, nullptr));
    assert(!card.Write(0, 4, nullptr));
    const uint8_t poke[] = {1, 2, 3, 4};
    assert(card.Write(4, 4, poke));
    assert(card.Read(4, 4, buf) && std::memcmp(buf, poke, 4) == 0);
    assert(card.Bytes()[3] == 0x3c && card.Bytes()[8] == 0x3c);
    assert(!card.Write(std::numeric_limits<uint64_t>::max(), 1, poke));
    assert(card.Bytes()[4] == 1);
}

static void SdImageCodec() {
    const auto payload = TinyPayload();
    std::vector<uint8_t> file, decoded;
    assert(frondSdFile::encode(payload, file));
    assert(file.size() == frondSdFile::headerBytes + payload.size());
    assert(frondSdFile::decode(file.data(), file.size(), decoded) && decoded == payload);

    auto bad = file;
    bad[0] = 'X';
    assert(!frondSdFile::decode(bad.data(), bad.size(), decoded) && decoded == payload);

    bad = file;
    frondSdFile::put32(bad.data() + 8, 2);
    assert(!frondSdFile::decode(bad.data(), bad.size(), decoded));

    bad = file;
    frondSdFile::put32(bad.data() + 12, 256);
    assert(!frondSdFile::decode(bad.data(), bad.size(), decoded));

    bad = file;
    bad[28] = 1;
    assert(!frondSdFile::decode(bad.data(), bad.size(), decoded));

    bad = file;
    bad.back() ^= 1;
    assert(!frondSdFile::decode(bad.data(), bad.size(), decoded));

    bad = file;
    bad.pop_back();
    assert(!frondSdFile::decode(bad.data(), bad.size(), decoded));

    bad = file;
    bad.push_back(0);
    assert(!frondSdFile::decode(bad.data(), bad.size(), decoded));

    bad = file;
    frondSdFile::put64(bad.data() + 16, 513);
    assert(!frondSdFile::decode(bad.data(), bad.size(), decoded));

    std::vector<uint8_t> rejected;
    assert(!frondSdFile::encode(nullptr, 0, rejected));
    assert(!frondSdFile::encode(payload.data(), 513, rejected));
    assert(!frondSdFile::encode(reinterpret_cast<const uint8_t*>(1), frondSdFile::maxPayloadBytes + 512, rejected));
}

static fs::path FixtureDir() {
    const auto dir = fs::path("build") / "card-backing-fixtures";
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

static void MachineFolder() {
    const auto dir = FixtureDir();
    const auto payload = TinyPayload();
    std::vector<uint8_t> sdFile, descriptor;
    assert(frondSdFile::encode(payload, sdFile));
    const auto sd = SdCard(payload.size());
    assert(frondMachine::encodeDescriptor(sd, descriptor));
    frondMachine::Media parsed;
    assert(frondMachine::decodeDescriptor(descriptor.data(), descriptor.size(), parsed));
    assert(parsed.kind == frondMachine::kindSd && parsed.name == frondMachine::sdSibling);
    assert(parsed.logicalSize == payload.size());

    const auto machine = dir / "palm-tx";
    fs::create_directories(machine);
    WriteFile(machine / "palm-tx.frond", {'v', '7'});
    WriteFile(machine / frondMachine::sdSibling, sdFile);
    fs::path session;
    assert(frondMachine::sessionFile(machine, session) && session.filename() == "palm-tx.frond");
    const auto android = dir / "android";
    fs::create_directories(android);
    WriteFile(android / frondMachine::androidSession, {'v', '7'});
    assert(frondMachine::sessionFile(android, session) && session.filename() == "session.frond");
    assert(frondMachine::checkSiblings(machine, {sd}) == frondMachine::SiblingStatus::ok);
    const auto packed = frondMachine::packableFiles(machine);
    assert(packed.size() == 2);

    WriteFile(machine / "sd.frondsd.tmp-1", {1});
    WriteFile(machine / ".gen-1", {2});
    assert(frondMachine::ignoredName("id.frond.tmp-abc"));
    assert(frondMachine::ignoredName(".gen-3"));
    assert(!frondMachine::packableName("sd.frondsd.tmp-1"));
    assert(frondMachine::checkSiblings(machine, {sd}) == frondMachine::SiblingStatus::ok);
    assert(frondMachine::packableFiles(machine).size() == 2);

    fs::remove(machine / frondMachine::sdSibling);
    assert(frondMachine::checkSiblings(machine, {sd}) == frondMachine::SiblingStatus::missing);

    WriteFile(machine / frondMachine::sdSibling, sdFile);
    assert(frondMachine::checkSiblings(machine, {}) == frondMachine::SiblingStatus::extra);

    auto mismatch = sd;
    mismatch.name = frondMachine::msSibling;
    std::vector<uint8_t> badDesc;
    assert(!frondMachine::encodeDescriptor(mismatch, badDesc));
    assert(frondMachine::checkSiblings(machine, {mismatch}) == frondMachine::SiblingStatus::mismatch);

    auto wrongSize = sd;
    wrongSize.logicalSize = 1024;
    wrongSize.physicalSize = 1024;
    assert(frondMachine::checkSiblings(machine, {wrongSize}) == frondMachine::SiblingStatus::mismatch);

    auto damaged = sdFile;
    damaged[0] = 'X';
    WriteFile(machine / frondMachine::sdSibling, damaged);
    assert(frondMachine::checkSiblings(machine, {sd}) == frondMachine::SiblingStatus::malformed);

    WriteFile(machine / frondMachine::msSibling, {'x'});
    WriteFile(machine / frondMachine::sdSibling, sdFile);
    assert(frondMachine::checkSiblings(machine, {sd}) == frondMachine::SiblingStatus::extra);
    assert(frondMachine::checkSiblings(machine, {MsCard()}) == frondMachine::SiblingStatus::mismatch);
    fs::remove(machine / frondMachine::msSibling);
    assert(frondMachine::checkSiblings(machine, {MsCard()}) == frondMachine::SiblingStatus::missing);

    auto trailing = descriptor;
    trailing.push_back(0);
    assert(!frondMachine::decodeDescriptor(trailing.data(), trailing.size(), parsed));
    descriptor[36] = 2;
    assert(!frondMachine::decodeDescriptor(descriptor.data(), descriptor.size(), parsed));

    fs::remove_all(dir);
}

void CardBackingGenerations();

int main() {
    RangeChecks();
    SdImageCodec();
    MachineFolder();
    CardBackingGenerations();
    std::puts("Card backing and sidecar checks passed");
}
