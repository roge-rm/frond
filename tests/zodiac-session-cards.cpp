// A Zodiac session with a card in each slot: the native save writes sd.frondsd and sd2.frondsd beside
// the .frond, and a session start (the cards opened into their stores, slot 2's put in before the state
// loads) has both back with their bytes, slot 2 attached and its write protection kept. A second save
// with slot 2 ejected loads with it out, and a session with one card loads with slot 2 empty. Made-up
// ROM, no firmware.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "CardSwap.h"
#include "MachineSession.h"
#include "SessionIo.h"
#include "sdcard.h"
#include "soc_IMX1.h"

namespace {
namespace fs = std::filesystem;

void put32le(std::vector<uint8_t>& rom, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) rom[at + i] = uint8_t(v >> (8 * i)); }
void put32be(std::vector<uint8_t>& rom, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) rom[at + i] = uint8_t(v >> (24 - 8 * i)); }

// The made-up ROM of test-zodiac-normal-runtime: the identity layout, and a branch to itself at 0.
std::vector<uint8_t> rom() {
    constexpr size_t card = 0x20000;
    std::vector<uint8_t> rom(card + 0x1000);
    put32le(rom, card + 0x78, 0x400);
    put32le(rom, card + 0xac, 0x300);
    put32be(rom, card + 0x300 + 4, 0x9a24);
    put32be(rom, card + 0x400 + 4, 0u - 0x9a24);
    size_t at = card + 0x408, value = 0;
    for (size_t length : {100, 128, 128, 128, 92}) {
        rom[at++] = uint8_t(0x80 | (length - 1));
        for (size_t i = 0; i < length; ++i, ++value) rom[at++] = uint8_t(value * 7 + 3);
    }
    put32le(rom, 0, 0xeafffffe);
    return rom;
}

void run(SocIMX1& soc) { for (int i = 0; i < 4; ++i) soc.Run(1000000, 100000000); }

std::vector<uint8_t> card(uint8_t fill) {
    std::vector<uint8_t> bytes(32u << 20);
    for (size_t sector = 0; sector < 16; ++sector) std::memset(bytes.data() + sector * 512, fill + sector, 512);
    return bytes;
}

uint8_t firstByte(SdCardStore& store, uint32_t sector) {
    std::vector<uint8_t> bytes(SD_SECTOR_SIZE);
    assert(store.read(sector, bytes.data()));
    return bytes[0];
}

// As the session start does it: the folder's cards opened, slot 2's card put in, then the state loaded.
void startSession(SocIMX1& soc, const fs::path& folder, frondSession::Contents& contents,
                  const frondSession::CardSwap* swap = nullptr) {
    soc.SdEject();
    soc.Sd2Eject();
    run(soc);
    sdCardReset();
    sdCardStore(1).reset();
    frondNative::slotCards() = {};
    fs::path loaded;
    assert(frondSession::loadPath(folder, contents, loaded, frondSession::msPhysicalBytes, {}, swap));
    assert(frondNative::attachFolderMedia(contents, loaded));
    bool secondIn = false;
    for (const auto& media : contents.media) {
        if (media.kind == frondMachine::kindSd) frondNative::slotCards().sdProtected = media.writeProtected;
        if (media.kind == frondMachine::kindSd2) {
            secondIn = media.inserted;
            frondNative::slotCards().sd2Protected = media.writeProtected;
        }
    }
    soc.SetSd2WriteProtect(frondNative::slotCards().sd2Protected);
    if (secondIn) assert(soc.Sd2Insert());
    assert(soc.Load(contents.savestate.size(), contents.savestate.data()));
    soc.SetSdWriteProtect(frondNative::slotCards().sdProtected);
    soc.SetSd2WriteProtect(frondNative::slotCards().sd2Protected);
    if (!soc.CardSwappedOnLoad() && !soc.SdInserted() && sdCardInitialized()) soc.SdInsert();
}
}  // namespace

int main() {
    const fs::path root = fs::absolute("build/zodiac-session-cards");
    fs::remove_all(root);
    fs::create_directories(root);
    std::string error;
    const auto image = rom();
    SocIMX1* soc = SocIMX1::Create(image.data(), image.size(), error);
    assert(soc && error.empty());
    auto& second = sdCardStore(1);

    // Both slots: slot 1's card and slot 2's, protected, each with its own bytes.
    sdCardInitializeWithBytes(card(0x10), "11111111111111111111111111111111");
    second.initializeWithBytes(card(0x80), "22222222222222222222222222222222");
    soc->SdInsert();
    assert(soc->Sd2Insert() && soc->Sd2Inserted() && soc->SecondCardAttached() && soc->HasSecondSdSlot());
    frondNative::slotCards().sd2Protected = true;
    soc->SetSd2WriteProtect(true);
    run(*soc);
    std::vector<uint8_t> sector(SD_SECTOR_SIZE, 0xc3);
    assert(second.write(20, sector.data()));
    const auto folder = root / "two";
    fs::create_directories(folder);
    assert(frondNative::saveMachine(folder / "two.frond", *soc));
    assert(fs::is_regular_file(folder / frondMachine::sdSibling) && fs::is_regular_file(folder / frondMachine::sd2Sibling));

    frondSession::Contents contents;
    startSession(*soc, folder, contents);
    assert(contents.media.size() == 2);
    run(*soc);
    assert(!soc->Halted() && soc->SdInserted() && soc->CardAttached());
    assert(soc->Sd2Inserted() && soc->SecondCardAttached() && frondNative::slotCards().sd2Protected);
    assert(std::string(second.getId()) == "22222222222222222222222222222222");
    assert(std::string(sdCardGetId()) == "11111111111111111111111111111111");
    assert(firstByte(second, 3) == 0x83 && firstByte(second, 20) == 0xc3 && firstByte(sdCardStore(0), 3) == 0x13);
    std::puts("PASS: a Zodiac session saved with a card in each slot loads with both in");

    // Slot 2 ejected, saved again (a journal now the card runs on sd2.frondsd): loads with it out.
    assert(second.runsOn((folder / frondMachine::sd2Sibling).c_str()));
    sector.assign(SD_SECTOR_SIZE, 0xd4);
    assert(second.write(21, sector.data()));
    soc->Sd2Eject();
    run(*soc);
    assert(!soc->Sd2Inserted() && !soc->SecondCardAttached());
    assert(frondNative::saveMachine(folder / "two.frond", *soc));
    startSession(*soc, folder, contents);
    run(*soc);
    assert(contents.media.size() == 2 && second.initialized() && !soc->Sd2Inserted() && !soc->SecondCardAttached());
    assert(frondNative::slotCards().sd2Protected && !frondNative::slotCards().sdProtected);
    assert(soc->Sd2Insert());
    soc->Sd2Eject();
    assert(firstByte(second, 21) == 0xd4 && soc->SdInserted());
    std::puts("PASS: one saved with slot 2 ejected loads with its card out");

    // A session with one card: slot 2 stays empty, and its store is never touched.
    second.reset();
    const auto one = root / "one";
    fs::create_directories(one);
    assert(frondNative::saveMachine(one / "one.frond", *soc));
    assert(!fs::exists(one / frondMachine::sd2Sibling));
    startSession(*soc, one, contents);
    run(*soc);
    assert(contents.media.size() == 1 && !second.initialized() && !soc->Sd2Inserted() && !soc->SecondCardAttached());
    assert(soc->SdInserted() && !soc->Halted());
    std::puts("PASS: a session with one card loads with slot 2 empty");

    // Slot 2's card taken out, then saved: sd2.frondsd stays in the folder and the next start loads with slot 2 empty.
    sdCardInitializeWithBytes(card(0x10), "11111111111111111111111111111111");
    second.initializeWithBytes(card(0x80), "22222222222222222222222222222222");
    soc->SdInsert();
    assert(soc->Sd2Insert());
    run(*soc);
    const auto taken = root / "taken";
    fs::create_directories(taken);
    assert(frondNative::saveMachine(taken / "taken.frond", *soc));
    soc->Sd2Eject();
    run(*soc);
    frondNative::CardIdentity takenId{};
    assert(frondNative::takeOutFileCard(taken, frondMachine::kindSd2, false, takenId).ok);
    assert(frondNative::saveMachine(taken / "taken.frond", *soc));
    assert(fs::is_regular_file(taken / frondMachine::sd2Sibling));
    const frondSession::CardSwap swap;
    startSession(*soc, taken, contents, &swap);
    run(*soc);
    assert(contents.media.size() == 1 && !second.initialized() && !soc->Sd2Inserted() && soc->SdInserted() && !soc->Halted());
    std::puts("PASS: a session saved after slot 2's card was taken out loads with slot 2 empty");
    fs::remove_all(root);
    return 0;
}
