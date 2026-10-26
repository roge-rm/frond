// Firmware-free PXA27x sleep, wake qualification, reset domains and state compatibility.
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "CPU.h"
#include "mem.h"
#include "soc_PXA.h"

static constexpr uint32_t PM = 0x40f00000, GPIO = 0x40e00000;
static void write(SocPXA& soc, uint32_t address, uint32_t value) {
    assert(memAccess(cpuGetMem(soc.GetCpu()), address, 4, true, &value));
}
static uint32_t read(SocPXA& soc, uint32_t address) {
    uint32_t value;
    assert(memAccess(cpuGetMem(soc.GetCpu()), address, 4, false, &value));
    return value;
}
static void run(SocPXA& soc, unsigned ms) { assert(soc.Run(ms * 1000, 1000000) > 0); }
static void sleep(SocPXA& soc, unsigned mode = 3) {
    auto cpu = soc.GetCpu();
    write(soc, 0xa0000000, 0xee070e10);  // MCR p14,0,r0,c7,c0,0
    write(soc, 0xa0000004, 0xeafffffe);
    cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu, ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC | ARM_SR_I | ARM_SR_F);
    cpuSetReg(cpu, 15, 0xa0000000);
    cpuSetReg(cpu, 0, mode);
    cpuCyclePure(cpu);
    assert(soc.PowerSleeping() == (mode == 3));
}
static std::vector<uint8_t> snapshot(SocPXA& soc) {
    assert(soc.Save());
    auto data = soc.GetSavestate();
    return {static_cast<uint8_t*>(data.data), static_cast<uint8_t*>(data.data) + data.size};
}
static void power(SocPXA& soc, bool down) {
    if (down) soc.KeyDown(keyIdPower); else soc.KeyUp(keyIdPower);
    run(soc, 150);
}
// Rebuild a legacy chunk (versions before sleep fields), preserving all other chunks.
static std::vector<uint8_t> legacy(const std::vector<uint8_t>& source, ChunkType type,
                                  uint32_t version, unsigned removedWords) {
    const auto* old = reinterpret_cast<const uint32_t*>(source.data());
    std::vector<uint32_t> result(old, old + 1 + old[0] * 2);
    size_t offset = result.size();
    bool found = false;
    for (unsigned i = 0; i < old[0]; ++i) {
        unsigned size = old[2 + 2 * i];
        if (old[1 + 2 * i] == uint32_t(type)) {
            assert(size > removedWords);
            size -= removedWords;
            result[2 + 2 * i] = size;
            found = true;
        }
        const size_t start = result.size();
        result.insert(result.end(), old + offset, old + offset + size);
        if (old[1 + 2 * i] == uint32_t(type)) result[start] = version;
        offset += old[2 + 2 * i];
    }
    assert(found);
    const auto* bytes = reinterpret_cast<uint8_t*>(result.data());
    return {bytes, bytes + result.size() * 4};
}
int main() {
    std::vector<uint8_t> rom(16 * 1024 * 1024), nand(132 * 1024 * 1024, 0xff);
    const uint32_t branch = 0xeafffffe;
    memcpy(rom.data(), &branch, 4);
    SocPXA soc(deviceTypeTX, 32 * 1024 * 1024, rom.data(), rom.size(),
               nand.data(), nand.size(), -1, 2);

    // Ordinary pending IRQs do not reject or terminate power sleep. IRQ idle still works.
    write(soc, 0x40a00000, 100);
    write(soc, 0x40a0001c, 1);
    run(soc, 10);
    write(soc, PM + 0x0c, 0);
    write(soc, PM + 0x50, 0);
    sleep(soc);
    const auto pc = cpuGetRegExternal(soc.GetCpu(), 15);
    const auto timer = read(soc, 0x40a00010);
    run(soc, 5000);
    soc.Wakeup(26);
    assert(soc.PowerSleeping() && cpuGetRegExternal(soc.GetCpu(), 15) == pc);
    assert(read(soc, 0x40a00010) == timer);
    assert(!(read(soc, PM + 0x30) & 4));
    power(soc, true); power(soc, false);
    assert(soc.PowerSleeping());
    soc.Reset();
    sleep(soc, 1);
    soc.Wakeup(26);
    run(soc, 10);
    assert(cpuGetRegExternal(soc.GetCpu(), 15) == 0xa0000004);

    // Rising and falling wake masks are independent of GPIO IRQ masks.
    soc.Reset();
    write(soc, PM + 0x0c, 1);
    write(soc, PM + 0x10, 1);
    write(soc, PM + 0x14, 0);
    write(soc, PM + 0x08, 0xabc12345);
    write(soc, PM + 0x34, 0x100);  // Retain SRAM bank 0 only, reset OS timer.
    write(soc, 0xa0002000, 0x12345678);
    write(soc, 0x5c000000, 0xabcdef01);
    write(soc, 0x5c010000, 0x76543210);
    write(soc, 0x58000000, 0x101);
    write(soc, 0x41300004, 0x12345);
    write(soc, 0x40a0001c, 1);
    write(soc, GPIO + 0x0c, 1u << 20);
    write(soc, PM + 0x20, 1u << 20);
    sleep(soc);
    power(soc, true);  // falling edge not enabled
    assert(soc.PowerSleeping());
    power(soc, false);
    assert(!soc.PowerSleeping());
    assert(read(soc, PM + 0x18) == 1);
    assert(read(soc, PM + 0x30) & 4);
    assert((read(soc, PM + 4) & 0x71) == 0x71);
    assert(read(soc, PM + 8) == 0xabc12345 && read(soc, PM + 0x0c) == 1);
    assert(read(soc, 0xa0002000) == 0x12345678);
    assert(read(soc, 0x5c000000) == 0xabcdef01 && read(soc, 0x5c010000) == 0);
    assert(read(soc, 0x58000000) == 0 && read(soc, 0x40a0001c) == 0);
    assert(read(soc, 0x41300004) == 0x12345);
    assert(read(soc, GPIO + 0x0c) == 0 && read(soc, PM + 0x20) == 0);
    assert(read(soc, GPIO) & (1u << 20));  // PGSR pad hold survives register reset.
    write(soc, PM + 4, 0x10);
    assert(!(read(soc, GPIO) & (1u << 20)));
    write(soc, PM + 0x18, 1);
    assert(read(soc, PM + 0x18) == 0);

    soc.Reset();
    write(soc, PM + 0x0c, 1);
    write(soc, PM + 0x10, 0);
    write(soc, PM + 0x14, 1);
    write(soc, GPIO + 0x0c, 1);  // Output pins are not wake inputs.
    sleep(soc); power(soc, true); power(soc, false);
    assert(soc.PowerSleeping());

    // Keypad wake needs PKWR, input direction, keyboard mux, and enabled scanning.
    soc.Reset();
    write(soc, PM + 0x0c, 0);
    write(soc, PM + 0x50, 1u << 18);  // GPIO101 = MKIN1 (Hard2)
    write(soc, GPIO + 0x6c, 1u << 10);  // GAFR3_L GPIO101 AF1
    write(soc, 0x41500000, 0x20001000 | (3u << 26) | (2u << 23));
    sleep(soc);
    soc.KeyDown(keyIdHard1); run(soc, 150);  // MKIN0 masked
    assert(soc.PowerSleeping());
    soc.KeyDown(keyIdHard2); run(soc, 150);
    assert(!soc.PowerSleeping());
    assert(read(soc, PM + 0x54) == (1u << 18));
    write(soc, PM + 0x54, 1u << 18);
    assert(read(soc, PM + 0x54) == 0 && read(soc, PM + 0x50) == (1u << 18));

    for (bool wrongMux : {false, true}) {
        soc.Reset();
        write(soc, PM + 0x0c, 0);
        write(soc, PM + 0x50, 1u << 18);
        write(soc, GPIO + 0x6c, wrongMux ? 0 : 1u << 10);
        write(soc, 0x41500000, wrongMux ? 0x2d001000 : 0);
        sleep(soc);
        soc.KeyDown(keyIdHard2); run(soc, 150);
        assert(soc.PowerSleeping());
    }

    // Manual scans select MS0 at bit 13, include the last configured column,
    // and retain valid rows. Hard1 is row 0 / column 1; Hard2 is row 1 / column 0.
    soc.Reset();
    soc.KeyDown(keyIdHard1); soc.KeyDown(keyIdHard2); run(soc, 150);
    write(soc, 0x41500000, 0x1000 | (1u << 26) | (1u << 23) | (1u << 13));
    assert((read(soc, 0x41500018) & 0xff) == 2);
    write(soc, 0x41500000, 0x1000 | (1u << 26) | (1u << 23) | (1u << 14));
    assert((read(soc, 0x41500018) & 0xff) == 1);
    write(soc, 0x41500000, 0x1000 | (1u << 23) | (1u << 13));
    assert((read(soc, 0x41500018) & 0xff) == 0);  // row 1 outside configured rows

    // PSLR retains the timer state, but its 3.25-MHz clock still stops in sleep.
    soc.Reset();
    write(soc, PM + 0x34, 4);
    write(soc, PM + 0x1c, 0x40);  // retain power I2C domain
    write(soc, PM + 0x80, 0x1234);
    write(soc, PM + 0x0c, 1); write(soc, PM + 0x10, 0); write(soc, PM + 0x14, 1);
    write(soc, 0x40a00000, 0x12345678);
    write(soc, 0x40a0001c, 1);
    sleep(soc);
    const auto retainedTimer = read(soc, 0x40a00010);
    run(soc, 2000);
    assert(read(soc, 0x40a00010) == retainedTimer);
    power(soc, true);
    assert(!soc.PowerSleeping());
    assert(read(soc, 0x40a00000) == 0x12345678 && read(soc, 0x40a0001c) == 1);
    assert(read(soc, PM + 0x80) == 0x1234);

    soc.Reset();
    write(soc, PM + 0x0c, 1); write(soc, PM + 0x10, 1); write(soc, PM + 0x14, 0);
    power(soc, true);
    sleep(soc);
    auto saved = snapshot(soc);
    SocPXA fresh(deviceTypeTX, 32 * 1024 * 1024, rom.data(), rom.size(),
                 nand.data(), nand.size(), -1, 2);
    assert(fresh.Load(saved.size(), saved.data()));
    run(fresh, 5000);
    assert(fresh.PowerSleeping());
    power(fresh, true); power(fresh, false);
    assert(!fresh.PowerSleeping());
    soc.Reset();
    saved = snapshot(soc);
    auto old = legacy(saved, ChunkType::pxaPwrClk, 1, 2);
    old = legacy(old, ChunkType::pxaGpio, 0, 9);
    assert(soc.Load(old.size(), old.data()));
    assert(!soc.PowerSleeping());
    old = legacy(old, ChunkType::pxaPwrClk, 0, 37);
    assert(soc.Load(old.size(), old.data()));
    assert(!soc.PowerSleeping() && read(soc, PM + 0x50) == 0);

    auto invalid = snapshot(soc);
    auto* words = reinterpret_cast<uint32_t*>(invalid.data());
    size_t offset = 1 + 2 * words[0];
    for (unsigned i = 0; i < words[0]; ++i) {
        const unsigned size = words[2 + 2 * i];
        if (words[1 + 2 * i] == uint32_t(ChunkType::pxaPwrClk)) words[offset + size - 1] = 1;
        offset += size;
    }
    write(soc, PM + 8, 0xcafef00d);
    assert(!soc.Load(invalid.size(), invalid.data()));  // pending wake without sleep
    assert(!soc.PowerSleeping() && read(soc, PM + 8) == 0xcafef00d);

    // Legacy RTAR alarm wakes even with ALE=0 and all interrupt-controller masks clear.
    soc.Reset();
    write(soc, PM + 0x0c, 0x80000000);
    write(soc, 0x40900008, 0);
    const uint32_t now = read(soc, 0x40900000);
    write(soc, 0x40900004, now + 1);
    sleep(soc);
    fresh.Reset();
    write(fresh, PM + 0x0c, 0);
    write(fresh, 0x40900008, 4);  // CPU alarm IRQ alone is not a sleep wake source.
    write(fresh, 0x40900004, now + 1);
    sleep(fresh);
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    run(soc, 1100);
    assert(!soc.PowerSleeping());
    assert(read(soc, PM + 0x18) == 0x80000000);
    assert(read(soc, 0x40900008) == 0);  // no enabled alarm or 1-Hz CPU interrupt
    assert(read(soc, 0x40900004) == now + 1);
    run(fresh, 1100);
    assert(fresh.PowerSleeping());
    assert(read(fresh, 0x40900008) & 1);
    puts("PASS: PXA27x qualified sleep wake, reset retention, legacy and fresh-state restore");
}
