#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "LibertasCF.h"
extern "C" void uarmAbort() { std::abort(); }
struct Region {
    uint32_t address, size;
    ArmMemAccessF callback;
    void* context;
};
static std::vector<Region> regions;
static GpioChangedNotifF notifiers[121]{};
static void* contexts[121]{};
static bool levels[121]{}, inCallback = false, scheduled = false;
bool memRegionAdd(ArmMem*, uint32_t address, uint32_t size, ArmMemAccessF callback, void* context) {
    regions.push_back({address, size, callback, context});
    return true;
}
void pxaGpioSetNotif(PxaGpio*, uint_fast8_t pin, GpioChangedNotifF callback, void* context) {
    notifiers[pin] = callback;
    contexts[pin] = context;
}
void pxaGpioSetState(PxaGpio*, uint_fast8_t pin, bool value) {
    assert(!inCallback);
    levels[pin] = value;
}
PxaGpioState pxaGpioGetState(PxaGpio*, uint_fast8_t pin) {
    return levels[pin] ? PxaGpioStateHigh : PxaGpioStateLow;
}
uint32_t paceGetAreg(uint8_t) { return 0; }
uint32_t paceGetPC() { return 0; }
void paceSetAreg(uint8_t, uint32_t) {}
void paceSetPC(uint32_t) {}
void paceSetDreg(uint8_t, uint32_t) {}
void cpuInvalidateCodeCache(ArmCpu*) {}
bool cpuMemOpExternal(ArmCpu*, void*, uint32_t, uint_fast8_t, bool) { return false; }
static uint32_t access(uint32_t address, uint32_t value, unsigned size, bool write) {
    for (auto& region : regions)
        if (address >= region.address && address + size <= region.address + region.size) {
            assert(region.callback(region.context, address, size, write, &value));
            return value;
        }
    assert(false);
    return 0;
}
static uint32_t read(unsigned offset, unsigned size = 2) {
    return access(0x20000000 + offset, 0, size, false);
}
static void write(unsigned offset, uint32_t value, unsigned size = 2) {
    access(0x20000000 + offset, value, size, true);
}
static void pin(unsigned number, bool value) {
    bool old = levels[number];
    inCallback = true;
    notifiers[number](contexts[number], number, old, value);
    inCallback = false;
    levels[number] = value;
}
static void boot(LibertasCF& radio) {
    pin(106, true);
    pin(94, true);
    pin(108, true);
    pin(106, false);
    radio.Tick();
    access(0x28000200, 1, 1, true);
    assert(access(0x28000200, 0, 1, false) == 1);
    assert(read(0x3e) == 0);
    write(0x18, 256);
    for (unsigned i = 0; i < 128; ++i) write(0x1a, i);
    write(2, 4);
    assert(read(0x20, 1) == 4);
    write(0x18, 0);
    write(2, 4);
    assert(read(0x3e) == 0x5a00);
    const uint8_t header[] = {1, 0, 0, 0, 0, 0, 0, 0xc0, 16, 0, 0, 0, 0, 0, 0, 0};
    write(0x18, 17);
    for (unsigned i = 0; i < 16; i += 2) write(0x1a, header[i] | (unsigned(header[i + 1]) << 8));
    write(2, 4);
    assert(read(0x28) == 16);
    write(0x18, 17);
    for (unsigned i = 0; i < 8; ++i) write(0x1a, 0);
    write(2, 4);
    assert(read(0x28) == 16);
    write(0x18, 17);
    for (unsigned i = 0; i < 8; ++i) write(0x1a, i ? 0 : 4);
    write(2, 4);
    assert(read(0x20, 1) == 4);
    write(4, 0);
}
static void command(unsigned id, unsigned size = 8) {
    write(0x18, size);
    write(0x1a, id);
    write(0x1a, size);
    write(0x1a, 17);
    write(0x1a, 0);
    for (unsigned i = 8; i < size; i += 2) write(0x1a, id == 6 && i == 8 ? 3 : 0);
    write(0, 4);
    write(2, 4);
}
static std::vector<uint8_t> response() {
    assert(read(0x20) & 8);
    auto length = read(0x30);
    std::vector<uint8_t> out;
    assert(length >= 16 && length <= 4096);
    for (unsigned i = 0; i < length; ++i) out.push_back(read(0x12, 1));
    assert(!(read(0x20) & 8));
    return out;
}
static std::vector<uint8_t> arpTx() {
    // CF transmit descriptor followed by an Ethernet ARP request for 10.0.2.2.
    std::vector<uint8_t> b(66);
    b[8] = 24;
    b[12] = 42;
    const uint8_t mac[] = {2, 0x46, 0x72, 0x6f, 0x6e, 0x64};
    std::fill(b.begin() + 24, b.begin() + 30, 0xff);
    std::copy(mac, mac + 6, b.begin() + 30);
    const uint8_t header[] = {8, 6, 0, 1, 8, 0, 6, 4, 0, 1};
    std::copy(header, header + 10, b.begin() + 36);
    std::copy(mac, mac + 6, b.begin() + 46);
    b[52] = b[62] = 10;
    b[54] = b[64] = 2;
    b[55] = 15;
    b[65] = 2;
    return b;
}
static void transmit(const std::vector<uint8_t>& b) {
    write(0x14, b.size());
    for (auto byte : b) write(0x16, byte, 1);
    write(2, 1);
}
static std::vector<uint8_t> packet() {
    assert(read(0x20) & 2);
    auto length = read(0x24);
    assert(length >= 70 && length <= 1560);
    std::vector<uint8_t> b;
    for (unsigned i = 0; i < length; ++i) b.push_back(read(0x10, 1));
    // RX is acknowledged separately from reading its data and its interrupt.
    assert(read(0x20) & 2);
    write(2, 2);
    assert(b[32] == 0 && b[33] == length - 34);
    const uint8_t llc[] = {0xaa, 0xaa, 3, 0, 0, 0, 8, 6};
    assert(std::equal(llc, llc + 8, b.begin() + 34));
    assert(b[48] == 0 && b[49] == 2);  // ARP reply opcode
    return b;
}
int main() {
    NetworkSimulation mode;
    uint64_t now = 0;
    LibertasCF radio(nullptr, nullptr, {[](void*, uint32_t) { scheduled = true; }, nullptr},
                     {[](void* p) { return *static_cast<uint64_t*>(p); }, &now}, &mode);
    assert(read(0x20) == 0xffff);
    assert(mode.SetMode("wireless"));
    radio.Poll();
    boot(radio);
    command(3, 46);
    assert(!levels[116]);
    assert(read(0x22) & 8);
    // IRQ acknowledgement must preserve response data until the service task reads it.
    write(0x22, 8);
    assert(levels[116]);
    assert(read(0x20) & 8);
    auto hw = response();
    assert(hw[0] == 3 && hw[1] == 0x80 && hw[16] == 2 && hw[21] == 0x64 && hw[42] == 3);
    command(6, 16);
    Savestate<ChunkType> state;
    assert(state.Save(radio));
    auto scan = response();
    assert(scan[10] == 1);
    const char ssid[] = "Frond";
    bool found = false;
    for (size_t i = 0; i + 5 <= scan.size(); ++i)
        if (!memcmp(scan.data() + i, ssid, 5)) found = true;
    assert(found);
    SavestateLoader<ChunkType> loader;
    assert(loader.Load(state.GetBuffer(), state.GetSize(), radio));
    assert(response() == scan);  // in-flight command response survives resume
    assert(state.Save(radio));
    // Snapshot sizes stay fixed as FIFOs fill and drain.
    command(3, 46);
    assert(state.Save(radio));
    {
        NetworkSimulation::Checkpoint freeze(mode);
        assert(loader.Load(state.GetBuffer(), state.GetSize(), radio));
        assert(response() == hw);
    }
    command(0x12);
    response();
    auto tx = arpTx();
    for (unsigned round = 0; round < 16; ++round) {
        // A partially submitted Ethernet packet is device state, and survives
        // normal resume even though the old host router is discarded.
        write(0x14, tx.size());
        for (unsigned i = 0; i < 33; ++i) write(0x16, tx[i], 1);
        assert(state.Save(radio));
        assert(loader.Load(state.GetBuffer(), state.GetSize(), radio));
        for (size_t i = 33; i < tx.size(); ++i) write(0x16, tx[i], 1);
        write(2, 1);
        radio.Poll();
        auto first = packet();
        transmit(tx);
        transmit(tx);
        radio.Poll();
        assert(read(0x22) & 2);
        write(0x22, 2);
        assert(read(0x20) & 2);  // IRQ acknowledgement does not discard RX.
        for (unsigned i = 0; i < 7; ++i) assert(read(0x10, 1) == first[i]);
        assert(state.Save(radio));
        assert(loader.Load(state.GetBuffer(), state.GetSize(), radio));
        for (size_t i = 7; i < first.size(); ++i) assert(read(0x10, 1) == first[i]);
        write(2, 2);
        assert(packet() == first);  // Second queued packet retains its boundary.
        assert(!(read(0x20) & 2));
        {
            NetworkSimulation::Checkpoint freeze(mode);
            transmit(tx);
            radio.Poll();
            assert(!(read(0x20) & 2));  // No new external I/O during export.
        }
        transmit(tx);
        radio.Poll();
        assert(packet() == first);
        transmit(tx);
        radio.Poll();
        command(0x24);
        response();
        assert(!(read(0x20) & 2));  // Disassociation clears pending packets.
        transmit(tx);
        radio.Poll();
        assert(!(read(0x20) & 2));
        command(0x12);
        response();
        transmit(tx);
        radio.Poll();
        assert(packet() == first);
    }
    auto malformed = tx;
    malformed[8] = 23;  // Descriptor cannot point into itself.
    transmit(malformed);
    radio.Poll();
    assert(!(read(0x20) & 2));
    command(6, 16);
    write(0x22, 8);
    pin(106, true);
    radio.Tick();
    assert(read(0x20) == 0xffff);
    assert(mode.SetMode("disabled"));
    radio.Poll();
    assert(read(0x20) == 0xffff);
    uint32_t empty = 0;
    assert(loader.Load(&empty, sizeof(empty), radio));
    assert(scheduled);
    puts(
        "PASS: Marvell CF firmware handshake, IRQ/data acknowledgement, hardware identity, Frond "
        "scan, in-flight restore, 16 CF packet/partial FIFO restore and reassociation cycles, checkpoint freeze, malformed TX and reset/disable");
}
