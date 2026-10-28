// Firmware-free partial transfers, card removal and malformed-state rejection.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "pxa_MMC.h"
#include "savestate/savestateAll.h"

static ArmMemAccessF registers;
static void* controller;
static std::array<uint8_t, 512> sector;
static unsigned reads, writes;
static bool rxRequest, txRequest;
static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
bool memRegionAdd(ArmMem*, uint32_t, uint32_t, ArmMemAccessF fn, void* context) {
    registers = fn; controller = context; return true;
}
void pxaIcInt(PxaIc*, uint_fast8_t, bool) {}
void pxaDmaExternalReq(PxaDma*, uint_fast8_t channel, bool active) {
    if (channel == DMA_CMR_MMC_RX) rxRequest = active;
    if (channel == DMA_CMR_MMC_TX) txRequest = active;
}
extern "C" void uarmAbort() { std::abort(); }
static bool readSector(uint32_t n, void* data) {
    ++reads;
    if (n) return false;
    std::memcpy(data, sector.data(), sector.size()); return true;
}
static bool writeSector(uint32_t n, const void* data) {
    ++writes;
    if (n) return false;
    std::memcpy(sector.data(), data, sector.size()); return true;
}
static void regWrite(unsigned offset, uint32_t value) {
    require(registers(controller, 0x41100000 + offset, 4, true, &value), "register write");
}
static uint32_t regRead(unsigned offset) {
    uint32_t value = 0;
    require(registers(controller, 0x41100000 + offset, 4, false, &value), "register read");
    return value;
}
static uint32_t command(VSD* card, unsigned cmd, uint32_t arg = 0) {
    uint8_t reply[17]{};
    vsdCommand(card, cmd, arg, reply);
    return uint32_t(reply[1]) << 24 | uint32_t(reply[2]) << 16 |
           uint32_t(reply[3]) << 8 | reply[4];
}
struct Fixture {
    VSD* card = vsdInit(readSector, writeSector, 65536);
    PxaMmc* mmc = pxaMmcInit(nullptr, nullptr, nullptr, true);
    Fixture() {
        pxaMmcInsert(mmc, card);
        command(card, 0);
        for (unsigned n = 0; n < 32; ++n) {
            command(card, 55);
            if (command(card, 41, 0x00300000) & 0x80000000u) break;
        }
        command(card, 2); command(card, 3); command(card, 7, 3u << 16);
    }
    template<class T> void Save(T& out) { vsdSave(card, out); pxaMmcSave(mmc, out); }
    void Load(SavestateLoader<ChunkType>& in) { vsdLoad(card, in); pxaMmcLoad(mmc, in); }
    void transfer(bool write, unsigned blocks = 1, bool dma = false) {
        regWrite(0, 1); regWrite(0x1c, 512); regWrite(0x20, blocks);
        regWrite(0x30, write ? 24 : blocks == 1 ? 17 : 18);
        regWrite(0x34, 0); regWrite(0x38, 0);
        regWrite(0x10, (write ? 13 : 5) | (dma ? 0x80 : 0)); regWrite(0, 2);
    }
    std::vector<uint32_t> snapshot() {
        Savestate<ChunkType> state; require(state.Save(*this), "snapshot");
        auto* data = reinterpret_cast<const uint32_t*>(state.GetBuffer());
        return {data, data + state.GetSize() / 4};
    }
    bool restore(std::vector<uint32_t>& state) {
        const auto before = sector; unsigned r = reads, w = writes;
        SavestateLoader<ChunkType> loader;
        bool ok = loader.Load(state.data(), state.size() * 4, *this);
        require(sector == before && reads == r && writes == w, "load accessed card backing");
        return ok;
    }
};
static void corrupt(std::vector<uint32_t>& state, ChunkType type, unsigned word, uint32_t value) {
    size_t offset = 1 + 2 * state[0];
    for (unsigned i = 0; i < state[0]; ++i) {
        unsigned size = state[2 + 2 * i];
        if (state[1 + 2 * i] == uint32_t(type)) {
            require(word < size, "bad mutation offset"); state[offset + word] = value; return;
        }
        offset += size;
    }
    throw std::runtime_error("missing chunk");
}
static uint32_t chunkWord(const std::vector<uint32_t>& state, ChunkType type, unsigned word) {
    size_t offset = 1 + 2 * state[0];
    for (unsigned i = 0; i < state[0]; ++i) {
        if (state[1 + 2 * i] == uint32_t(type)) return state[offset + word];
        offset += state[2 + 2 * i];
    }
    throw std::runtime_error("missing chunk");
}
static size_t chunkSize(const std::vector<uint32_t>& state, ChunkType type) {
    for (unsigned i = 0; i < state[0]; ++i)
        if (state[1 + 2 * i] == uint32_t(type)) return state[2 + 2 * i];
    throw std::runtime_error("missing chunk");
}
// Older layouts are the same words without the newer trailing fields, which are ignored.
static void downgrade(std::vector<uint32_t>& state, ChunkType type, uint32_t version) {
    corrupt(state, type, 0, version);
    corrupt(state, type, chunkSize(state, type) - 1, 0xffffffffu);
}
static uint32_t status(VSD* card) { return command(card, 13, 3u << 16); }
static void versions() {
    Fixture f;
    uint8_t reply[17]{};
    require(vsdCommand(f.card, 63, 0, reply) == SdReplyNone, "unknown command answered");
    auto state = f.snapshot();
    require(status(f.card) & 1u << 22, "illegal command not latched");
    require(!(status(f.card) & 1u << 22), "illegal command latched twice");
    require(f.restore(state) && (status(f.card) & 1u << 22), "latch lost in savestate");
    require(f.restore(state), "restore");
    auto old = state; downgrade(old, ChunkType::vsd, 1);
    require(f.restore(old) && !(status(f.card) & 1u << 22), "version 1 vsd state not loaded clear");
    require(chunkWord(state, ChunkType::vsd, 0) == 2, "vsd version");
    old = state; downgrade(old, ChunkType::vsd, 0);
    require(f.restore(old) && !(status(f.card) & 1u << 22), "version 0 vsd state not loaded clear");
    // A failed read raises DAT_ERR; older MMC states default the new mask bits to masked.
    regWrite(0, 1); regWrite(0x1c, 512); regWrite(0x20, 1); regWrite(0x30, 17);
    regWrite(0x34, 0); regWrite(0x38, 512); regWrite(0x10, 5); regWrite(0, 2);
    require((regRead(0x04) & 1) && (regRead(0x2c) & 0x100), "DAT_ERR not raised");
    state = f.snapshot();
    require(chunkWord(state, ChunkType::pxaMmc, 0) == 1, "mmc version");
    regWrite(0x28, 0);
    require(f.restore(state) && regRead(0x28) == 0x37f && (regRead(0x2c) & 0x100), "mmc v1 restore");
    old = state; downgrade(old, ChunkType::pxaMmc, 0);
    regWrite(0x28, 0);
    require(f.restore(old) && regRead(0x28) == 0x37f && (regRead(0x2c) & 0x100),
            "version 0 mmc state not loaded with new bits masked");
}
static void transfers() {
    for (unsigned i = 0; i < sector.size(); ++i) sector[i] = i;
    Fixture f; f.transfer(false);
    for (unsigned i = 0; i < 16; ++i) regRead(0x40);
    auto state = f.snapshot(); const auto count = reads;
    require(regRead(0x40) == 0x43424140, "partial read offset");
    require(f.restore(state), "read restore");
    require(regRead(0x40) == 0x43424140 && reads == count, "read replayed backing access");
    for (unsigned i = 17; i < 128; ++i) regRead(0x40);
    require(regRead(4) & 0x800, "read completion");
    f.transfer(true);
    for (unsigned i = 0; i < 32; ++i) regWrite(0x44, 0x11223344);
    state = f.snapshot(); const auto before = writes;
    for (unsigned i = 32; i < 128; ++i) regWrite(0x44, 0x55667788);
    require(writes == before + 1, "write completion count");
    require(f.restore(state) && writes == before + 1, "write restore replay");
    for (unsigned i = 32; i < 128; ++i) regWrite(0x44, 0x99aabbcc);
    require(writes == before + 2 && sector[0] == 0x44 && sector[128] == 0xcc &&
            sector[511] == 0x99, "partial write state lost");
}
static void malformed() {
    uint32_t expected;
    std::memcpy(&expected, sector.data(), 4);
    Fixture f; f.transfer(false);
    auto good = f.snapshot();
    for (auto test : std::array<std::array<uint32_t, 3>, 6>{{
             {{uint32_t(ChunkType::pxaMmc), 3, (65535u << 16) | 1}},
             {{uint32_t(ChunkType::pxaMmc), 11, 1025}},
             {{uint32_t(ChunkType::pxaMmc), 12, 1025}},
             {{uint32_t(ChunkType::pxaMmc), 12, 1024}},
             {{uint32_t(ChunkType::vsd), 2, 255u << 16}},
             {{uint32_t(ChunkType::vsd), 3, 65535}} }}) {
        auto bad = good; corrupt(bad, ChunkType(test[0]), test[1], test[2]);
        if (f.restore(bad)) {
            std::fprintf(stderr, "accepted chunk=%u word=%u value=%u\n", test[0], test[1], test[2]);
            throw std::runtime_error("malformed transfer state accepted");
        }
        require(f.restore(good), "good state rejected after failure");
    }
    auto writing = good;
    corrupt(writing, ChunkType::pxaMmc, 4, chunkWord(writing, ChunkType::pxaMmc, 4) | 8);
    corrupt(writing, ChunkType::pxaMmc, 11, 1);
    corrupt(writing, ChunkType::pxaMmc, 12, 1024);
    if (f.restore(writing)) {
        uint32_t value = 0;
        value = 1;
        require(registers(controller, 0x41100040, 4, false, &value) && value == 0, "write state exposed invalid RXFIFO");
    }
    require(f.restore(good), "recovery after write-state guard");
    require(regRead(0x40) == expected, "restored read");
}
static void eject() {
    Fixture f; f.transfer(false, 2, true); regRead(0x40);
    require(rxRequest, "read DMA not requested");
    const auto count = reads;
    pxaMmcInsert(f.mmc, nullptr);
    require(!rxRequest && !txRequest, "ejection left DMA requested");
    uint32_t value = 0;
    value = 1;
    require(registers(controller, 0x41100040, 4, false, &value) && value == 0, "ejected card exposes stale FIFO");
    require(reads == count, "ejection accessed backing");
    pxaMmcInsert(f.mmc, f.card); command(f.card, 12);
    f.transfer(false); regRead(0x40);
    Fixture writer; writer.transfer(true, 1, true);
    for (unsigned i = 0; i < 32; ++i) regWrite(0x44, 0x12345678);
    const auto before = writes;
    pxaMmcInsert(writer.mmc, nullptr);
    require(!rxRequest && !txRequest, "write ejection left DMA requested");
    require(registers(controller, 0x41100044, 4, true, &value), "ejected card FIFO write faulted");
    require((regRead(0x04) & 0x804) == 0x804, "ejected write did not end with a CRC error");
    require(writes == before, "partial ejected write reached backing");
    pxaMmcInsert(writer.mmc, writer.card); command(writer.card, 12);
    writer.transfer(true);
    for (unsigned i = 0; i < 128; ++i) regWrite(0x44, 0xaabbccdd);
    require(writes == before + 1 && sector[0] == 0xdd && sector[511] == 0xaa,
            "new transfer retained an ejected partial write");
    for (unsigned i = 0; i < sector.size(); ++i) sector[i] = i;
    Fixture fresh; fresh.transfer(false, 2); regRead(0x40);
    auto saved = fresh.snapshot();
    pxaMmcInsert(fresh.mmc, nullptr);
    require(fresh.restore(saved), "fresh disconnected restore");
    pxaMmcInsert(fresh.mmc, fresh.card);  // Successful remount keeps the restored FIFO offset.
    require(regRead(0x40) == 0x07060504, "remount discarded restored FIFO offset");
    pxaMmcInsert(fresh.mmc, nullptr);
    require(fresh.restore(saved), "fresh restore before failed remount");
    pxaMmcInsert(fresh.mmc, nullptr);  // Rejected backing even though no old pointer exists.
    value = 1;
    require(registers(controller, 0x41100040, 4, false, &value) && value == 0, "failed remount exposed restored FIFO");
}
int main(int argc, char** argv) {
    try {
        if (argc == 1) { versions(); transfers(); malformed(); eject(); }
        else if (!std::strcmp(argv[1], "malformed")) malformed();
        else if (!std::strcmp(argv[1], "eject")) eject();
        else throw std::runtime_error("unknown case");
        std::puts("PASS SD lifecycle: partial transfers, savestate versions, no load replay, malformed rejection and card removal");
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
