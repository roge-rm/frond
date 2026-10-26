#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "CPU.h"
#include "NetworkSimulation.h"
static uint8_t memory[65536]{};
static uint32_t d0, a0, sp = 0x100, pc = 0x400;
uint32_t paceGetPC() { return pc; }
void paceSetPC(uint32_t value) { pc = value; }
void cpuInvalidateCodeCache(ArmCpu*) {}
bool cpuMemOpExternal(ArmCpu*, void* data, uint32_t address, uint_fast8_t size, bool write) {
    if (address >= sizeof(memory) || size > sizeof(memory) - address) return false;
    if (write)
        std::memcpy(memory + address, data, size);
    else
        std::memcpy(data, memory + address, size);
    return true;
}
uint32_t paceGetAreg(uint8_t index) { return index == 7 ? sp : a0; }
void paceSetAreg(uint8_t index, uint32_t value) {
    if (index == 7)
        sp = value;
    else
        a0 = value;
}
void paceSetDreg(uint8_t, uint32_t value) { d0 = value; }
static void put(uint32_t address, uint32_t value, unsigned size = 4) {
    for (unsigned i = 0; i < size; i++) memory[address + size - 1 - i] = value >> (i * 8);
}
static uint32_t get(uint32_t address, unsigned size = 4) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = (v << 8) | memory[address + i];
    return v;
}
static void arg(unsigned offset, uint32_t value, unsigned size = 4) {
    put(0x100 + offset, value, size);
}
int main() {
    NetworkSimulation net;
    std::strcpy(reinterpret_cast<char*>(memory + 0x200), "Net.lib");
    arg(0, 0x200);
    arg(4, 0x300);
    assert(!net.Handle(nullptr, 0xa0ba));
    assert(!net.SetMode("bad"));
    assert(net.SetMode("connected"));
    assert(net.Handle(nullptr, 0xa0ba));
    assert(get(0x300, 2) == 0x7ffe);
    auto start = [&] {
        sp = 0x100;
        pc = 0x400;
        std::memset(memory + 0x100, 0, 48);
        arg(0, 0x7ffe, 2);
    };
    start();
    arg(2, 0x302);
    assert(net.Handle(nullptr, 0xa801));
    assert(d0 == 0 && get(0x302, 2) == 0);
    start();
    arg(2, 2, 1);
    arg(4, 1, 1);
    arg(12, 0x302);
    net.Handle(nullptr, 0xa807);
    assert(d0 == 4);
    start();
    arg(2, 4, 2);
    arg(4, 0x400);
    arg(8, 16, 2);
    arg(14, 0x302);
    put(0x400, 2, 2);
    put(0x402, 7, 2);
    put(0x404, 0x0a000002);
    net.Handle(nullptr, 0xa80c);
    assert(d0 == 0);
    std::memcpy(memory + 0x500, "hello", 5);
    start();
    arg(2, 4, 2);
    arg(4, 0x500);
    arg(8, 5, 2);
    arg(22, 0x302);
    net.Handle(nullptr, 0xa811);
    assert(d0 == 5);
    Savestate<ChunkType> state;
    assert(state.Save(net));
    auto receive = [&](uint32_t address = 0x600) {
        start();
        arg(2, 4, 2);
        arg(4, address);
        arg(8, 10, 2);
        arg(24, 0x302);
        net.Handle(nullptr, 0xa813);
    };
    receive(0xfffffff0);
    assert(d0 == UINT32_MAX && get(0x302, 2) == 0x1204);  // no queue consumption
    receive();
    assert(d0 == 5 && std::memcmp(memory + 0x600, "hello", 5) == 0);
    receive();
    assert(d0 == UINT32_MAX && get(0x302, 2) == 0x122f);
    SavestateLoader<ChunkType> loader;
    assert(loader.Load(state.GetBuffer(), state.GetSize(), net));
    receive();
    assert(d0 == 5);  // pending reply restored
    // Reuse a preallocated snapshot after changing queue occupancy.
    assert(state.Save(net));
    assert(net.SetMode("timeout"));
    start();
    arg(2, 0x302);
    net.Handle(nullptr, 0xa801);
    assert(d0 == 0);
    start();
    arg(2, 2, 1);
    arg(4, 1, 1);
    arg(12, 0x302);
    net.Handle(nullptr, 0xa807);
    assert(d0 == 4);
    start();
    arg(2, 4, 2);
    arg(4, 0x400);
    arg(8, 16, 2);
    arg(14, 0x302);
    net.Handle(nullptr, 0xa80c);
    assert(get(0x302, 2) == 0x1212);
    assert(net.SetMode("disconnected"));
    start();
    arg(2, 0x302);
    net.Handle(nullptr, 0xa801);
    assert(d0 == 0x120c);
    std::vector<uint32_t> corrupt(state.GetSize() / 4);
    std::memcpy(corrupt.data(), state.GetBuffer(), state.GetSize());
    // One chunk: count, type, size, chunk version, then mode.
    corrupt[4] = 99;
    assert(!loader.Load(corrupt.data(), state.GetSize(), net));
    uint32_t legacy = 0;
    assert(loader.Load(&legacy, sizeof(legacy), net));
    assert(net.GetMode() == NetworkSimulation::disabled);
    // Exercise the host path through the packed Palm ABI and verify the actual
    // task-delay trampoline. Model tests inspect code; the SDK probe executes it.
    assert(net.SetMode("internet"));
    start();
    arg(2, 0x302);
    net.Handle(nullptr, 0xa801);
    assert(d0 == 0);
    start();
    arg(2, 2, 1);
    arg(4, 2, 1);
    arg(12, 0x302);
    net.Handle(nullptr, 0xa807);
    assert(d0 == 4);
    start();
    arg(2, 4, 2);
    arg(4, 0x400);
    arg(8, 8, 2);
    arg(14, 0x302);
    put(0x400, 2, 2);
    put(0x402, 0, 2);
    put(0x404, 0x7f000001);
    net.Handle(nullptr, 0xa80b);
    assert(d0 == 0);
    auto waitReceive = [&] {
        start();
        arg(2, 4, 2);
        arg(4, 0x600);
        arg(8, 10, 2);
        arg(20, 1);
        arg(24, 0x302);
        net.Handle(nullptr, 0xa813);
    };
    waitReceive();
    assert(sp == 0xe0 && pc == 0xe4);
    assert(get(0xe0) == 1 && get(0xe4, 2) == 0x4e4f && get(0xe6, 2) == 0xa0a0);
    assert(get(0xea, 2) == 32 && get(0xee) == 0x3fc);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    waitReceive();
    assert(sp == 0x100 && get(0x302, 2) == 0x1212);
    assert(state.Save(net));
    {
        NetworkSimulation::Checkpoint checkpoint(net);
        assert(loader.Load(state.GetBuffer(), state.GetSize(), net));
        start();
        arg(2, 4, 2);
        arg(4, 0x600);
        arg(8, 10, 2);
        arg(24, 0x302);
        net.Handle(nullptr, 0xa813);
        assert(get(0x302, 2) == 0x122f);  // export freezes host I/O
    }
    assert(loader.Load(state.GetBuffer(), state.GetSize(), net));
    start();
    arg(2, 4, 2);
    arg(4, 0x600);
    arg(8, 10, 2);
    arg(24, 0x302);
    net.Handle(nullptr, 0xa813);
    assert(get(0x302, 2) == 0x1214);  // normal resume closes descriptors
    puts(
        "PASS: simulated NetLib packed ABI, echo, error profiles, invalid memory and pending reply "
        "restore");
}
