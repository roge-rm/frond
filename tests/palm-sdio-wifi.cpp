#include <cassert>
#include <cstdlib>

#include "NetworkSimulation.h"
#include "PalmSdioWifi.h"
#include "PalmPrismFrame.h"

extern "C" void uarmAbort() { std::abort(); }

static uint32_t command(SdioCard& card, uint8_t cmd, uint32_t arg) {
    uint8_t reply[6]{};
    assert(card.Command(cmd, arg, reply) != SdReplyNone);
    return uint32_t(reply[1]) << 24 | uint32_t(reply[2]) << 16 | uint32_t(reply[3]) << 8 | reply[4];
}

static uint32_t read(SdioCard& card, unsigned fn, unsigned address) {
    return command(card, 52, (fn << 28) | (address << 9));
}

static void write(SdioCard& card, unsigned fn, unsigned address, uint8_t value) {
    assert(command(card, 52, (1u << 31) | (fn << 28) | (address << 9) | value) == (0x1000 | value));
}

static void enumerate(SdioCard& card) {
    assert(command(card, 5, 0x00300000) == 0x90300000);
    assert(command(card, 3, 0) == 0x00030000);
    assert(command(card, 7, 0x00030000) == 0x1e00);
    assert(read(card, 0, 0x100) == 0x1007);
    assert(read(card, 0, 0x1002) == 0x1096);
    assert(read(card, 0, 0x1003) == 0x1002);
    assert(read(card, 0, 0x1004) == 0x1001);
    assert(read(card, 0, 0x1005) == 0x1001);
    write(card, 0, 2, 2);
}

static void genesis(SdioCard& card) {
    write(card, 1, 0x1c3e2, 0x0f);
    write(card, 1, 0x1c3e0, 0x45);
    write(card, 1, 0x1c83a, 0);
    write(card, 1, 0x1c83b, 0xfc);
    write(card, 1, 0x1c83c, 0x38);
    write(card, 1, 0x1c83d, 0);
}

// Seed the runtime state without relying on a copy of the original firmware.
static void setRuntime(Savestate<ChunkType>& snapshot, SdioCard& card, bool primary) {
    auto& chunk = *snapshot.GetChunk(ChunkType::palmSdioWifi);
    assert(chunk.Get32() == 1);  // Chunk version
    LoadChunkHelper helper(chunk);
    card.TransferState(helper);
    for (int i = 0; i < 3; ++i) chunk.Get64();  // Times and bulk digest
    chunk.Get32();  // Bulk byte count
    for (int i = 0; i < 10; ++i) chunk.Get16();  // Registers
    chunk.Get8();  // Function IRQ
    chunk.GetBool();  // Genesis
    chunk.PutBool(primary);
    chunk.PutBool(true);  // Station
    chunk.PutBool(false);  // Volatile download
    chunk.PutBool(true);  // Associated
}

int main() {
    NetworkSimulation firstNetwork, secondNetwork;
    PalmSdioWifi first(firstNetwork), second(secondNetwork);
    auto& a = first.Card();
    auto& b = second.Card();
    enumerate(a);
    enumerate(b);
    genesis(a);
    genesis(b);
    write(a, 1, 0x1c83e, 0x42);
    write(a, 1, 0x1c83c, 0x38);
    assert(read(a, 1, 0x1883e) == 0x1042);
    assert(read(b, 1, 0x1883e) == 0x1100);

    struct Restore {
        PalmSdioWifi& wifi;
        void Load(SavestateLoader<ChunkType>& loader) { wifi.Load(loader); }
    } restore{second};
    Savestate<ChunkType> snapshot;
    assert(snapshot.Save(first));
    SavestateLoader<ChunkType> loader;
    assert(loader.Load(snapshot.GetBuffer(), snapshot.GetSize(), restore));
    write(b, 1, 0x1c83c, 0x38);
    assert(read(b, 1, 0x1883e) == 0x1042);

    a.Reset();
    enumerate(a);
    genesis(a);
    assert(read(a, 1, 0x1883e) == 0x1100);
    // A truncated firmware stream must never switch the card to its runtime registers.
    write(a, 1, 0x1483e, 0x42);
    write(a, 1, 0x1c3e2, 7);
    write(a, 1, 0x1c3e0, 0x45);
    assert(read(a, 1, 0x18800) == 0x1100);
    a.Reset();
    enumerate(a);
    assert(snapshot.Save(first));
    setRuntime(snapshot, a, true);
    Restore restoreFirst{first};
    assert(loader.Load(snapshot.GetBuffer(), snapshot.GetSize(), restoreFirst));

    write(a, 1, 0x1c83a, 0);
    write(a, 1, 0x1c83b, 0xfe);
    write(a, 1, 0x1c83c, 0);
    write(a, 1, 0x1c83d, 0);
    for (int request = 0; request < 2; ++request) {
        write(a, 1, 0x1083e, 0);
        write(a, 1, 0x1083e, 4);
        for (int i = 0; i < 1024; ++i) {
            const auto reply = read(a, 1, 0x0c);
            assert((reply & ~0xffu) == 0x1000);
            if (request == 1) assert(reply == 0x10ff);
        }
    }

    Savestate<ChunkType> invalid;
    assert(invalid.Save(first));
    setRuntime(invalid, a, false);
    assert(snapshot.Save(first));
    assert(!loader.Load(invalid.GetBuffer(), invalid.GetSize(), restoreFirst));
    assert(loader.Load(snapshot.GetBuffer(), snapshot.GetSize(), restoreFirst));

    // Re-selecting the primary boot after association without a new upload
    // clears the station state together with primary.
    write(a, 1, 0x1c3e2, 7);
    write(a, 1, 0x1c3e0, 0x45);
    assert(read(a, 1, 0x18800) == 0x1100);
    assert(snapshot.Save(first));
    assert(!loader.Load(invalid.GetBuffer(), invalid.GetSize(), restoreFirst));
    assert(loader.Load(snapshot.GetBuffer(), snapshot.GetSize(), restoreFirst));
    assert(!a.IrqAsserted() && !b.IrqAsserted());

    PalmSdioWifi e3(firstNetwork, true), restoredE3(secondNetwork, true);
    auto& e3Card = e3.Card();
    enumerate(e3Card);
    assert(snapshot.Save(e3));
    setRuntime(snapshot, e3Card, true);
    Restore loadE3{e3};
    assert(loader.Load(snapshot.GetBuffer(), snapshot.GetSize(), loadE3));

    const std::array<uint8_t, 6> destination{2, 3, 5, 7, 11, 13};
    std::vector<uint8_t> ethernet(14);
    std::copy(destination.begin(), destination.end(), ethernet.begin());
    auto frame = palmPrismFrame::Receive(ethernet, destination);
    frame[15] = 1;
    std::copy(destination.begin(), destination.end(), frame.begin() + 30);
    for (uint8_t fid : {0, 1}) {
        write(e3Card, 1, 0x1c018, fid);
        write(e3Card, 1, 0x1c019, 1);
        write(e3Card, 1, 0x1c01c, 0);
        write(e3Card, 1, 0x1c01d, 0);
        for (auto byte : frame) write(e3Card, 1, 0x14836, byte);
        write(e3Card, 1, 0x1c002, fid);
        write(e3Card, 1, 0x1c003, 1);
        write(e3Card, 1, 0x1c000, 0x0b);
        write(e3Card, 1, 0x1c001, 1);
    }
    assert((read(e3Card, 1, 0x18830) & 8) == 0);
    assert(snapshot.Save(e3));
    Restore restoreQueue{restoredE3};
    assert(loader.Load(snapshot.GetBuffer(), snapshot.GetSize(), restoreQueue));
    auto& restoredCard = restoredE3.Card();
    restoredE3.Poll(0);
    assert(read(restoredCard, 1, 0x18822) == 0x1000);
    assert(read(restoredCard, 1, 0x18830) & 8);
    restoredE3.Poll(1);
    assert(read(restoredCard, 1, 0x18822) == 0x1000);
    write(restoredCard, 1, 0x1c034, 8);
    restoredE3.Poll(2);
    assert(read(restoredCard, 1, 0x18822) == 0x1001);
    assert(read(restoredCard, 1, 0x18830) & 8);
    write(restoredCard, 1, 0x1c034, 8);
    restoredE3.Poll(3);
    assert((read(restoredCard, 1, 0x18830) & 8) == 0);
}
