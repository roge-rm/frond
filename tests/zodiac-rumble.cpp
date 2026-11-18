// The Zodiac's rumble motor and LED on the YMF768's bank 1 register 48 (bit 1 motor, bit 0 LED), as the ROM
// sets them (0x200b5bf0, 0x200b5c88: bank 1, 0d = 0d, 49 = 3, then 48). On the model: each bit on and off
// alone, bank 0's 48 apart, the register reading back as written. Through the machine, Exact and Fast: a guest
// turning the motor on, then off, as imx1MachineAttention reports it.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>
#include "CPU.h"
#include "imx1_boot_probe.h"
#include "zodiac_ymf768.h"

static void put(Imx1Ymf768& y, unsigned reg, unsigned value) { y.write(false, uint8_t(reg)); y.write(true, uint8_t(value)); }
static uint8_t get(Imx1Ymf768& y, unsigned reg) { y.write(false, uint8_t(reg)); return y.read(true); }

// The ROM's setter: the shadow byte with the bit changed, written after 0d and 49.
static void romSet(Imx1Ymf768& y, uint8_t outputs) {
    put(y, 0x0f, 1); put(y, 0x0d, 0x0d); put(y, 0x49, 3); put(y, 0x48, outputs);
}

static void model() {
    Imx1Ymf768 y;
    assert(!y.led() && !y.motor());
    romSet(y, 2);
    assert(y.motor() && !y.led() && get(y, 0x48) == 2);
    romSet(y, 3);
    assert(y.motor() && y.led() && get(y, 0x48) == 3);
    romSet(y, 1);
    assert(!y.motor() && y.led() && get(y, 0x48) == 1);
    romSet(y, 0);
    assert(!y.motor() && !y.led());
    // Bank 0's 48 is something else.
    put(y, 0x0f, 0); put(y, 0x48, 3);
    assert(!y.motor() && !y.led());
    put(y, 0x0f, 1);
    assert(get(y, 0x48) == 0);
    romSet(y, 2);
    y.reset();
    assert(!y.motor());
}

struct Program {
    std::vector<uint32_t> code;
    std::vector<std::pair<size_t, uint32_t>> literals;
    void load(unsigned reg, uint32_t value) { literals.emplace_back(code.size(), value); code.push_back(0xe59f0000 | reg << 12); }
    void storeByte(uint32_t address, uint32_t value) { load(0, address); load(1, value); code.push_back(0xe5c01000); }
    void ymf(unsigned reg, unsigned value) { storeByte(0x15000000, reg); storeByte(0x15000001, value); }
    void romSet(uint8_t outputs) { ymf(0x0f, 1); ymf(0x0d, 0x0d); ymf(0x49, 3); ymf(0x48, outputs); }
    void finish() {
        code.push_back(0xeafffffe);  // b .
        for (const auto& literal : literals) {
            code[literal.first] |= uint32_t((code.size() - literal.first - 2) * 4);
            code.push_back(literal.second);
        }
    }
    const uint8_t* rom() const { return reinterpret_cast<const uint8_t*>(code.data()); }
    uint32_t size() const { return uint32_t(code.size() * 4); }
};

static Imx1MachineAttention runGuest(const Program& g, bool fast) {
    Imx1ProbeOptions o{};
    o.bootstrapPeripherals = true;
    o.cs4Port0InputKnown = true;
    auto* m = imx1MachineCreate(g.rom(), g.size(), Imx1ProbeEntry::physicalFlash, false, o);
    assert(m);
    imx1MachineEnableYmf768(m);
    assert(!imx1MachineAttention(m).motor);
    if (fast) assert(imx1MachineSetFastEngine(m, true));
    for (unsigned i = 0; i < 10; ++i) {
        const auto r = imx1MachineRunSlice(m, 5000, 100000);
        if (r.status != Imx1MachineStatus::yielded) {
            fprintf(stderr, "stopped: %s pc %08x\n", imx1ProbeStopName(r.result.stop), r.result.trace.pc);
            assert(false);
        }
    }
    const auto attention = imx1MachineAttention(m);
    imx1MachineStop(m);
    return attention;
}

static void machine() {
    Program on, onOff, led;
    on.romSet(2); on.finish();
    onOff.romSet(2); onOff.romSet(0); onOff.finish();
    led.romSet(1); led.finish();
    for (unsigned fast = 0; fast < 2; ++fast) {
        auto a = runGuest(on, fast);
        assert(a.motor && !a.led);
        a = runGuest(onOff, fast);
        assert(!a.motor && !a.led);
        a = runGuest(led, fast);
        assert(!a.motor && a.led);
    }
    assert(!imx1MachineAttention(nullptr).motor);
}

int main() {
    model();
    machine();
    printf("zodiac rumble: ok\n");
}
