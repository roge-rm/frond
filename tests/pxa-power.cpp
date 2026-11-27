// Firmware-free PXA27x and PXA25x sleep, wake qualification, reset domains and state
// compatibility.
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>
#include <vector>

#include "CPU.h"
#include "audio_queue.h"
#include "mem.h"
#include "savestate/savestateAll.h"
#include "sdcard.h"
#include "soc_PXA.h"
#include "spitz_nand.h"
#include "storage/Io.h"

#include "patch_dispatch.h"
#include "syscall.h"

class FirmwarePowerSoc : public SocPXA {
 public:
    using SocPXA::SocPXA;
    bool patched(uint32_t syscall) {
        return patchDispatchHasPatch(patchDispatch, (syscall >> 14) * 4, syscall & 0xfff);
    }
    unsigned queuedKeys() const { return keyEventQueue->GetSize(); }
};

static void firmwarePower() {
    for (bool variant : {false, true}) {
        std::vector<uint8_t> rom((variant ? 6u : 8u) << 20), nand(34603008, 0xff);
        auto word = [&](size_t at, uint32_t value) { std::memcpy(rom.data() + at, &value, 4); };
        word(0x20008, 0xfeedbeef); word(0x20080, deviceCompanyPalm5); word(0x20084, 0x68737072);
        if (variant) {
            std::memcpy(rom.data() + 0x20010, "rePalm", 7);
            std::memcpy(rom.data() + 0x20030, "Dmitry.GR", 10);
            word(0x2006c, rom.size() - 0x20000);
        }
        FirmwarePowerSoc soc(deviceTypeE2, 32u << 20, rom.data(), rom.size(), nand.data(), nand.size(), -1, 0);
        assert(soc.PowerButtonSupported() == !variant);
        assert(soc.patched(SYSCALL_SYS_SLEEP) == variant);
        assert(soc.patched(SYSCALL_SYS_SET_AUTO_OFF_TIME) == variant);
        soc.KeyDown(keyIdPower); soc.KeyUp(keyIdPower);
        assert(soc.queuedKeys() == (variant ? 0u : 2u));
        soc.KeyDown(keyIdHard1); soc.KeyUp(keyIdHard1);
        assert(soc.queuedKeys() == (variant ? 2u : 4u));
    }
}

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
// PXA25x (E2): GPIO0-15 wake, sleep-exit reset with RCSR[SMR], and SDRAM held out of
// the address map until MDCNFG enables a partition, so boot-time RAM clears are lost.
static void pxa25xSleep() {
    // MIRROR is MARK in the RAM probe mirror above the 32 MiB of RAM.
    static constexpr uint32_t MDCNFG = 0x48000000, MARK = 0xa0002000, MIRROR = MARK + (32u << 20);
    std::vector<uint8_t> rom(8u << 20), nand(34603008, 0xff);
    const uint32_t branch = 0xeafffffe;
    memcpy(rom.data(), &branch, 4);
    SocPXA soc(deviceTypeE2, 32 * 1024 * 1024, rom.data(), rom.size(),
               nand.data(), nand.size(), -1, 0);
    write(soc, PM + 0x0c, 1u << 2);  // power key is GPIO2, active low
    write(soc, PM + 0x10, 0);
    write(soc, PM + 0x14, 1u << 2);
    write(soc, PM + 0x08, 0x5ca1ab1e);
    write(soc, MARK, 0x12345678);
    assert(read(soc, MIRROR) == 0x12345678);
    sleep(soc);
    run(soc, 2000);
    assert(soc.PowerSleeping());
    soc.Wakeup(26);  // ordinary IRQs do not end power sleep
    assert(soc.PowerSleeping());
    power(soc, true);
    assert(!soc.PowerSleeping());
    assert(read(soc, PM + 0x18) == (1u << 2));
    assert(read(soc, PM + 0x30) & 4);
    assert((read(soc, PM + 4) & 0x31) == 0x31);
    assert(read(soc, PM + 8) == 0x5ca1ab1e);
    assert(read(soc, MARK) == 0 && read(soc, MIRROR) == 0);
    write(soc, MARK, 0xdeadbeef);  // dropped while SDRAM is held
    write(soc, MIRROR, 0xfeedface);  // the mirror is held too

    // A held session restores held, and a version 0 memory controller chunk never holds.
    auto saved = snapshot(soc);
    SocPXA fresh(deviceTypeE2, 32 * 1024 * 1024, rom.data(), rom.size(),
                 nand.data(), nand.size(), -1, 0);
    assert(fresh.Load(saved.size(), saved.data()));
    assert(read(fresh, MARK) == 0 && read(fresh, MIRROR) == 0);
    auto old = legacy(saved, ChunkType::pxaMemCtrlr, 0, 1);
    assert(fresh.Load(old.size(), old.data()));
    write(fresh, MARK, 0x0badf00d);
    assert(read(fresh, MARK) == 0x0badf00d && read(fresh, MIRROR) == 0x0badf00d);

    write(soc, MDCNFG, 0x00000ac8);  // no DE bits: still held
    assert(read(soc, MARK) == 0 && read(soc, MIRROR) == 0);
    write(soc, MDCNFG, 0x00001bcd);
    assert(read(soc, MARK) == 0x12345678 && read(soc, MIRROR) == 0x12345678);
    write(soc, MIRROR, 0x5eed5eed);
    assert(read(soc, MARK) == 0x5eed5eed);
    write(soc, MARK, 0x12345678);
    power(soc, false);

    // Each edge needs its own enable: a press is a falling edge.
    write(soc, PM + 0x0c, 1u << 2);
    write(soc, PM + 0x10, 0);
    write(soc, PM + 0x14, 0);
    sleep(soc);
    power(soc, true);
    assert(soc.PowerSleeping());
    write(soc, PM + 0x10, 1u << 2);
    power(soc, false);
    assert(!soc.PowerSleeping() && read(soc, MARK) == 0);
    write(soc, MDCNFG, 0x00001bcd);

    // Soft reset releases a hold, and old power manager and GPIO chunks still load.
    sleep(soc); power(soc, true); power(soc, false);
    soc.Reset();
    write(soc, MARK, 0x600df00d);
    assert(read(soc, MARK) == 0x600df00d);
    saved = snapshot(soc);
    old = legacy(saved, ChunkType::pxaPwrClk, 1, 4);
    old = legacy(old, ChunkType::pxaGpio, 0, 9);
    assert(soc.Load(old.size(), old.data()));
    old = legacy(old, ChunkType::pxaPwrClk, 0, 37);
    assert(soc.Load(old.size(), old.data()));
    assert(!soc.PowerSleeping());
}

// Run one coprocessor instruction at the start of RAM with r0 = in and return r1.
static uint32_t coproc(SocPXA& soc, uint32_t instruction, uint32_t in = 0) {
    auto cpu = soc.GetCpu();
    write(soc, 0xa0000000, instruction);
    write(soc, 0xa0000004, 0xeafffffe);
    cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu, ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC | ARM_SR_I | ARM_SR_F);
    cpuSetReg(cpu, 15, 0xa0000000);
    cpuSetReg(cpu, 0, in);
    cpuSetReg(cpu, 1, 0xdeadbeef);
    cpuCyclePure(cpu);
    return cpuGetRegExternal(cpu, 1);
}
static uint32_t chunkVersion(const std::vector<uint8_t>& state, ChunkType type) {
    const auto* words = reinterpret_cast<const uint32_t*>(state.data());
    size_t offset = 1 + words[0] * 2;
    for (unsigned i = 0; i < words[0]; ++i) {
        if (words[1 + 2 * i] == uint32_t(type)) return words[offset];
        offset += words[2 + 2 * i];
    }
    assert(false);
    return 0;
}
static void pxa27xCycleCounter(SocPXA& soc) {
    constexpr uint32_t readPmnc = 0xee101e11, writePmnc = 0xee000e11;
    constexpr uint32_t readCcnt = 0xee111e11, writeCcnt = 0xee010e11;
    constexpr uint32_t xsc2 = 0x14000000;
    assert(coproc(soc, readPmnc) == xsc2);
    assert(coproc(soc, readCcnt) == 0);
    coproc(soc, writeCcnt, 123);
    run(soc, 1);
    assert(coproc(soc, readCcnt) == 123);

    coproc(soc, writePmnc, 5);  // reset CCNT and count every core cycle
    assert(coproc(soc, readPmnc) == (xsc2 | 1));
    const uint32_t count = coproc(soc, readCcnt);
    assert(count < 100);
    run(soc, 1);
    const uint32_t full = coproc(soc, readCcnt) - count;
    assert(full >= 1000);

    coproc(soc, writePmnc, 13);  // reset CCNT and divide the clock by 64
    assert(coproc(soc, readPmnc) == (xsc2 | 9));
    const uint32_t start = coproc(soc, readCcnt);
    run(soc, 1);
    const uint32_t divided = coproc(soc, readCcnt) - start;
    assert(divided > 0 && divided * 64 + 128 >= full && divided * 64 <= full + 128);

    auto saved = snapshot(soc);
    const auto restored = coproc(soc, readCcnt);
    run(soc, 1);
    assert(coproc(soc, readCcnt) > restored);
    assert(soc.Load(saved.size(), saved.data()));
    assert(coproc(soc, readCcnt) == restored);
    assert(coproc(soc, readPmnc) == (xsc2 | 9));

    coproc(soc, writePmnc, 0);
    coproc(soc, writeCcnt, 0xfffffff0);
    coproc(soc, writePmnc, 1);
    run(soc, 1);
    assert(coproc(soc, readCcnt) < 2000);  // architectural 32-bit wrap
    coproc(soc, writePmnc, 0);
    const auto stopped = coproc(soc, readCcnt);
    run(soc, 1);
    assert(coproc(soc, readCcnt) == stopped);
}

// PXA27x CCSR, CLKCFG read back and cache type; PXA25x keeps its old values.
static void clocks() {
    static constexpr uint32_t CCCR = 0x41300000, CCSR = 0x4130000c;
    static constexpr uint32_t READ_CLKCFG = 0xee161e10, WRITE_CLKCFG = 0xee060e10,
                              READ_CACHE_TYPE = 0xee101f30;
    const uint32_t branch = 0xeafffffe;
    {
        std::vector<uint8_t> rom(16u << 20), nand(132u << 20, 0xff);
        memcpy(rom.data(), &branch, 4);
        SocPXA soc(deviceTypeTX, 32u << 20, rom.data(), rom.size(), nand.data(), nand.size(), -1, 2);
        assert(read(soc, CCCR) == 0x190 && read(soc, CCSR) == 0x30000190);
        assert(coproc(soc, READ_CLKCFG) == 0);
        coproc(soc, WRITE_CLKCFG, 0xb);  // T, F and B: frequency change to turbo
        assert(coproc(soc, READ_CLKCFG) == 9);  // F reads 0
        coproc(soc, WRITE_CLKCFG, 6);  // F and HT
        assert(coproc(soc, READ_CLKCFG) == 4);
        coproc(soc, WRITE_CLKCFG, 9);
        // HT and B are saved; a version 2 chunk loads with B set and HT clear.
        auto saved = snapshot(soc);
        assert(chunkVersion(saved, ChunkType::pxaPwrClk) == 4);
        coproc(soc, WRITE_CLKCFG, 0);
        assert(soc.Load(saved.size(), saved.data()) && coproc(soc, READ_CLKCFG) == 9);
        coproc(soc, WRITE_CLKCFG, 5);
        auto old = legacy(snapshot(soc), ChunkType::pxaPwrClk, 2, 2);
        assert(soc.Load(old.size(), old.data()) && coproc(soc, READ_CLKCFG) == 9);
        // A CCCR write takes effect at the next CLKCFG write, so a state saved between them keeps the old clock.
        coproc(soc, WRITE_CLKCFG, 0xb);
        const uint64_t hz = soc.CoreHz();
        write(soc, CCCR, 0x02000210);
        assert(soc.CoreHz() == hz);
        saved = snapshot(soc);
        SocPXA copy(deviceTypeTX, 32u << 20, rom.data(), rom.size(), nand.data(), nand.size(), -1, 2);
        assert(copy.Load(saved.size(), saved.data()) && copy.CoreHz() == hz);
        assert(read(copy, CCCR) == 0x02000210);
        coproc(soc, WRITE_CLKCFG, 0xb);
        coproc(copy, WRITE_CLKCFG, 0xb);
        assert(soc.CoreHz() != hz && copy.CoreHz() == soc.CoreHz());
        // A version 3 chunk has no active CCCR and loads with the saved one.
        write(copy, CCCR, 0x190);
        coproc(copy, WRITE_CLKCFG, 0xb);
        assert(copy.CoreHz() == hz);
        old = legacy(saved, ChunkType::pxaPwrClk, 3, 1);
        assert(copy.Load(old.size(), old.data()) && copy.CoreHz() == soc.CoreHz());
        pxa27xCycleCounter(soc);
        soc.Reset();
        assert(coproc(soc, 0xee111e11) == 0);  // reset clears CCNT
        assert(coproc(soc, READ_CLKCFG) == 0);  // reset clears CLKCFG
        write(soc, CCCR, 0x02000210);
        assert(read(soc, CCSR) == 0x30000210);
        assert(coproc(soc, READ_CACHE_TYPE) == 0x0b1aa1aa);
    }
    std::vector<uint8_t> rom(8u << 20), nand(34603008, 0xff);
    memcpy(rom.data(), &branch, 4);
    SocPXA soc(deviceTypeE2, 32u << 20, rom.data(), rom.size(), nand.data(), nand.size(), -1, 0);
    assert(read(soc, CCCR) == 0x122 && read(soc, CCSR) == 0);
    coproc(soc, WRITE_CLKCFG, 0xf);
    assert(coproc(soc, READ_CLKCFG) == 0);
    assert(coproc(soc, 0xee111e11) == 0xdeadbeef);  // XSC2 CCNT is undefined on PXA25x
    assert(chunkVersion(snapshot(soc), ChunkType::pxaPwrClk) == 4);
    assert(coproc(soc, READ_CACHE_TYPE) == 0x0b16a16a);
}

static void spitzKeyboardAndHinge() {
    constexpr unsigned strobes[] = {88, 23, 24, 25, 26, 27, 52, 103, 107, 108, 114};
    constexpr unsigned senses[] = {12, 17, 91, 34, 36, 38, 39};
    constexpr uint32_t bankOffset[] = {0, 4, 8, 0x100};
    auto bit = [](unsigned pin) { return 1u << (pin % 32); };
    std::vector<uint8_t> rom(8u << 20, 0xff), nand(spitzNandRawSize, 0xff);
    const uint32_t branch = 0xeafffffe;
    std::memcpy(rom.data(), &branch, sizeof(branch));
    SocPXA soc(deviceTypeSpitz, 64u << 20, rom.data(), rom.size(), nand.data(), nand.size(), -1, 2);
    auto level = [&](unsigned pin) { return bool(read(soc, GPIO + bankOffset[pin / 32]) & bit(pin)); };

    bool seen[keyIdCount]{};
    unsigned count = 0;
    for (unsigned strobe = 0; strobe < std::size(strobes); ++strobe) {
        const unsigned pin = strobes[strobe];
        const uint32_t bank = bankOffset[pin / 32];
        write(soc, GPIO + bank + 0x0c, bit(pin));  // GPDR
        write(soc, GPIO + bank + 0x18, bit(pin));  // GPSR
        for (unsigned sense = 0; sense < std::size(senses); ++sense) {
            const KeyId key = spitzKey(sense, strobe);
            if (key == keyInvalid) continue;
            assert(!seen[key]);
            seen[key] = true;
            ++count;
            soc.KeyDown(key);
            run(soc, 100);
            assert(level(0));
            for (unsigned other : senses) assert(level(other) == (other == senses[sense]));
            soc.KeyUp(key);
            run(soc, 100);
            assert(!level(0) && !level(senses[sense]));
        }
        write(soc, GPIO + bank + 0x24, bit(pin));  // GPCR
        write(soc, GPIO + bank + 0x0c, 0);
    }
    assert(count == 64 && !seen[keyIdPower] && !seen[keyIdHotSync]);

    write(soc, GPIO + 0x0c, bit(23));
    write(soc, GPIO + 0x18, bit(23));
    soc.KeyDown(spitzKey(0, 1));
    soc.KeyDown(spitzKey(4, 1));
    run(soc, 100);
    assert(level(0) && level(12) && level(36));
    soc.KeyUp(spitzKey(0, 1));
    run(soc, 100);
    assert(level(0) && !level(12) && level(36));
    power(soc, true);
    soc.KeyUp(spitzKey(4, 1));
    run(soc, 100);
    assert(level(0) && level(95) && !level(36));
    power(soc, false);
    assert(!level(0) && !level(95));

    assert(!level(96) && !level(97));
    write(soc, GPIO + 0x130, bit(96) | bit(97));  // GRER3
    assert(soc.SetSpitzLid(false, true));
    assert(soc.SpitzSwivelled() && !soc.SpitzLidClosed() && level(96) && !level(97));
    assert(read(soc, GPIO + 0x148) & bit(96));  // GEDR3
    auto saved = snapshot(soc);
    assert(soc.SetSpitzLid(true, false));
    assert(!soc.SpitzSwivelled() && soc.SpitzLidClosed() && level(96) && level(97));
    assert(read(soc, GPIO + 0x148) & bit(97));
    soc.Reset();
    assert(soc.SpitzLidClosed() && level(96) && level(97));
    assert(soc.Load(saved.size(), saved.data()));
    assert(soc.SpitzSwivelled() && !soc.SpitzLidClosed() && level(96) && !level(97));
}

static void spitzBoard() {
    std::vector<uint8_t> rom(8u << 20, 0xff), nand(spitzNandRawSize, 0xff);
    // A restored machine may have damaged boot blocks and still have usable RAM and CPU state.
    SocPXA soc(deviceTypeSpitz, 64u << 20, rom.data(), rom.size(), nand.data(), nand.size(), -1, 2, true);
    write(soc, 0xa0000000, 0xeafffffe);
    cpuSetReg(soc.GetCpu(), 15, 0xa0000000);

    // The Left key bridges GPIO52 and GPIO39, in the same GPIO register bank.
    constexpr uint32_t strobe = 1u << (52 - 32), sense = 1u << (39 - 32);
    write(soc, GPIO + 0x10, strobe);  // GPDR1
    write(soc, GPIO + 0x34, sense);   // GRER1
    soc.KeyDown(keyIdLeft);
    run(soc, 100);
    assert(!(read(soc, GPIO + 4) & sense));
    write(soc, GPIO + 0x1c, strobe);  // GPSR1
    assert(read(soc, GPIO + 4) & sense);
    assert(read(soc, GPIO + 0x4c) & sense);  // GEDR1
    write(soc, GPIO + 0x28, strobe);  // GPCR1
    assert(!(read(soc, GPIO + 4) & sense));

    constexpr uint32_t irq = 1u << 11, select = 1u << 14, ssp = 0x41700000;
    write(soc, GPIO + 0x0c, select);  // GPDR0
    write(soc, GPIO + 0x18, select);  // GPSR0
    write(soc, GPIO + 0x3c, irq);     // GFER0
    write(soc, ssp, 0x6ab);          // Sharp's 12-bit SSP2 format
    assert(read(soc, GPIO) & irq);
    soc.PenDown(479, 639);
    run(soc, 100);
    assert(!(read(soc, GPIO) & irq));
    assert(read(soc, GPIO + 0x48) & irq);

    auto transfer = [&](uint32_t command) {
        write(soc, ssp + 0x10, command);
        run(soc, 1);
        return read(soc, ssp + 0x10);
    };
    assert(transfer(0x93) == 0);  // inactive select
    write(soc, GPIO + 0x24, select);
    assert(transfer(0x93) == 0);
    assert(transfer(0xd3) == 250);   // panel Y falls toward the right
    assert(transfer(0xb3) == 3750);  // panel X rises toward the bottom
    assert(transfer(0xc3) == 1000);  // Z1
    assert(transfer(0x90) == 1300);  // Z2

    // A full session replacement releases the host pen but keeps the queued conversion.
    auto saved = snapshot(soc);
    assert(soc.Load(saved.size(), saved.data()));
    assert(read(soc, GPIO) & irq);
    assert(transfer(0x93) == 250);
    assert(transfer(0xd3) == 0);

    soc.PenDown(0, 0);
    run(soc, 100);
    assert(transfer(0x93) == 0);
    assert(transfer(0xd3) == 3750);
    assert(transfer(0xb3) == 250);
    soc.PenUp();
    run(soc, 100);
    assert(read(soc, GPIO) & irq);

    assert(!soc.SpitzTabletKeyDown(5));
    assert(soc.SpitzTabletKeyDown(0));
    run(soc, 100);
    assert(!(read(soc, GPIO) & irq));
    transfer(0xd3);
    assert(transfer(0x93) == 3892);  // 665 panel pixels down, beyond the LCD
    assert(transfer(0xb3) == 3400);  // 48 panel pixels from the left
    soc.PenUp();
    run(soc, 100);
    assert(read(soc, GPIO) & irq);
}

static void spitzUsbDevice() {
    std::vector<uint8_t> rom(8u << 20, 0xff), nand(spitzNandRawSize, 0xff);
    SocPXA soc(deviceTypeSpitz, 64u << 20, rom.data(), rom.size(), nand.data(), nand.size(), -1, 2, true);
    constexpr uint32_t vbus = 1u << (35 - 32), host = 1u << (37 - 32), role = 1u << (41 - 32);
    constexpr uint32_t udc = 0x40600000, cken = 0x41300004, usbClock = 1u << 11;
    constexpr uint32_t pullup = 0x20010;
    const UsbDevicePort port(soc.GetUsbController());
    assert(port && !soc.GetUsbPort());
    assert(!soc.UsbTestCableAttached() && (read(soc, GPIO + 4) & (vbus | host | role)) == role);
    write(soc, GPIO + 0x34, vbus);  // GRER1
    write(soc, GPIO + 0x40, vbus);  // GFER1

    assert(soc.SetUsbTestCable(true) && soc.UsbTestCableAttached());
    assert((read(soc, GPIO + 4) & (vbus | host | role)) == (vbus | role));
    assert(read(soc, GPIO + 0x4c) & vbus);  // GEDR1
    write(soc, GPIO + 0x4c, vbus);

    // Sharp masks IERS. A Port 2 connection needs EP0 interrupts and the clock.
    write(soc, cken, read(soc, cken) & ~usbClock);
    write(soc, udc + 4, 3);
    write(soc, udc + 8, 0xb0000000);
    write(soc, udc, 1);
    write(soc, udc + 0x20, pullup);
    assert(!port.Enabled() && !port.Reset());
    write(soc, cken, read(soc, cken) | usbClock);
    assert(port.Enabled() && port.Listening() && port.Reset());
    const uint8_t setup[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};
    assert(port.Setup(setup) == UsbPacketResult::Ack);
    assert(read(soc, udc + 0x100) & 0x80);  // SETUP arrived at EP0
    assert(read(soc, udc + 0x0c) & 1);

    // Switching the transceiver off and on makes a new USB connection even
    // when the cable and UDE are both still on.
    const uint32_t generation = port.EnableCount();
    write(soc, udc + 0x20, 0);
    assert(!port.Listening() && port.Setup(setup) == UsbPacketResult::Disabled);
    write(soc, udc + 0x20, pullup);
    assert(port.Listening() && port.EnableCount() == generation + 1);
    write(soc, GPIO + 0x10, host);  // GPDR1
    write(soc, GPIO + 0x1c, host);  // GPSR1
    assert(!port.Listening());
    write(soc, GPIO + 0x28, host);  // GPCR1
    assert(port.Listening());
    write(soc, cken, read(soc, cken) & ~usbClock);
    assert(!port.Listening());
    write(soc, cken, read(soc, cken) | usbClock);
    assert(port.Listening());

    auto saved = snapshot(soc);
    assert(soc.SetUsbTestCable(false) && !soc.UsbTestCableAttached());
    assert(read(soc, GPIO + 0x4c) & vbus);
    assert(!port.Enabled() && !port.Frame());
    write(soc, GPIO + 0x4c, vbus);

    // Loading the session from a normal launch cannot reattach the probe cable.
    assert(soc.Load(saved.size(), saved.data()));
    assert(!soc.UsbTestCableAttached() && !(read(soc, GPIO + 4) & vbus));
    assert(read(soc, GPIO + 0x4c) & vbus);
    assert(!port.Listening());

    auto unplugged = snapshot(soc);
    assert(soc.SetUsbTestCable(true));
    assert(soc.Load(unplugged.size(), unplugged.data()));
    assert(soc.UsbTestCableAttached() && (read(soc, GPIO + 4) & vbus));
    assert(port.Listening());

    auto refused = legacy(unplugged, ChunkType::spitzPanel, 0, 1);
    assert(!soc.Load(refused.size(), refused.data()));
    assert(soc.UsbTestCableAttached() && (read(soc, GPIO + 4) & vbus));
    assert(port.Listening());
}

static void spitzCodecControl() {
    std::vector<uint8_t> rom(8u << 20, 0xff), nand(spitzNandRawSize, 0xff);
    SocPXA soc(deviceTypeSpitz, 64u << 20, rom.data(), rom.size(), nand.data(), nand.size(), -1, 2, true);
    constexpr uint32_t data = 0x40301688, control = 0x40301690, status = 0x40301698;
    auto send = [&](uint8_t value, uint32_t flags) {
        write(soc, data, value);
        write(soc, control, 0x48 | flags);  // Enable the bus and transfer one byte.
        return !(read(soc, status) & 2);
    };
    struct CodecState {
        std::array<uint16_t, 43> registers{};
        void Load(SavestateLoader<ChunkType>& loader) {
            auto chunk = loader.GetChunkOrFail(ChunkType::wm8750, 2, "WM8750");
            if (!chunk) return;
            LoadChunkHelper helper(*chunk);
            uint8_t phase{}, first{};
            helper.Do(decltype(helper)::Pack8() << phase << first);
            for (auto& reg : registers) helper.Do16(reg);
        }
    } codec;
    auto readRegisters = [&] {
        auto state = snapshot(soc);
        SavestateLoader<ChunkType> loader;
        assert(loader.Load(state.data(), state.size(), codec));
    };

    assert(!send(0x37, 1));  // The codec does not accept reads.
    write(soc, control, 0x50);
    assert(!send(0x34, 1));
    write(soc, control, 0x50);
    assert(send(0x36, 1));
    assert(send((26 << 1) | 1, 0));
    auto pending = snapshot(soc);
    assert(send(0x80, 2));
    readRegisters();
    assert(codec.registers[26] == 0x180);
    assert(soc.Load(pending.size(), pending.data()));
    assert(send(0x40, 2));
    readRegisters();
    assert(codec.registers[26] == 0x140);
    assert(send(0x36, 1));
    assert(send(15 << 1, 0));
    assert(send(0, 2));
    readRegisters();
    assert(codec.registers[26] == 0 && codec.registers[5] == 8);

    auto setRegister = [&](unsigned reg, uint16_t value) {
        assert(send(0x36, 1));
        assert(send((reg << 1) | (value >> 8), 0));
        assert(send(value & 0xff, 2));
    };
    auto* queue = audioQueueCreate(128);
    soc.SetAudioQueue(queue);
    soc.SetPcmOutputEnabled(true);
    write(soc, 0xa0000000, 0xeafffffe);
    cpuSetReg(soc.GetCpu(), 15, 0xa0000000);
    write(soc, 0x41300004, read(soc, 0x41300004) | 0x100);
    constexpr uint32_t i2s = 0x40400000;
    setRegister(7, 2);       // Standard 16-bit I²S.
    write(soc, i2s + 0x60, 26);
    write(soc, i2s + 4, 8);   // Disable receive.
    write(soc, i2s, 0x7705);  // Master, enabled.
    write(soc, i2s + 0x80, 0x20004000);
    auto lastFrame = [&] {
        std::array<uint32_t, 128> frames{};
        run(soc, 1);
        const size_t count = audioQueuePopChunk(queue, frames.data(), frames.size());
        assert(count >= 42 && count <= 46);
        return frames[count - 1];
    };
    assert(lastFrame() == 0); // Power-on mute.
    setRegister(5, 0);
    setRegister(24, 0x10);
    setRegister(25, 0xc0);
    setRegister(26, 0x1f8);
    setRegister(34, 0x100);
    setRegister(37, 0x100);
    assert(lastFrame() == 0x30003000);
    setRegister(10, 0);
    assert(lastFrame() == 0x30003000);  // Left volume waits for a volume update.
    setRegister(11, 0x1ff);
    assert(lastFrame() == 0x10001000);
    setRegister(10, 0x1ff);
    assert(soc.SetHeadphones(true));
    assert(read(soc, GPIO + 0x100) & (1u << (116 - 96)));
    assert(lastFrame() == 0);
    constexpr uint32_t scoop = 0x10800000;
    write(soc, scoop + 0x20, 0x30);
    write(soc, scoop + 0x24, 0x30);
    assert(lastFrame() == 0x20004000);
    auto playing = snapshot(soc);
    setRegister(5, 8);
    assert(lastFrame() == 0);
    assert(soc.Load(playing.size(), playing.data()));
    assert(lastFrame() == 0x20004000);
    soc.SetPcmOutputEnabled(false);
    run(soc, 1);
    assert(audioQueuePendingSamples(queue) == 0);
    soc.SetAudioQueue(nullptr);

    struct MicSource { unsigned pulls{}, disarms{}; } mic;
    const AudioInputSource input{
        &mic,
        [](void* context, AudioInputRoute route, uint32_t n, uint32_t d) -> int16_t {
            assert(route == AudioInputRoute::externalMic && n == 576000 && d == 26);
            ++static_cast<MicSource*>(context)->pulls;
            return -512;
        },
        nullptr,
        [](void* context) { ++static_cast<MicSource*>(context)->disarms; },
    };
    soc.SetMicrophoneSource(&input);
    setRegister(0, 0x117);
    setRegister(1, 0x117);
    setRegister(25, 0xfe);
    setRegister(32, 0xc0);
    setRegister(33, 0xc0);
    write(soc, i2s + 4, 0);
    run(soc, 1);
    assert(!soc.MicrophoneDemand().active && mic.pulls == 0);
    assert(read(soc, i2s + 0x80) == 0);
    write(soc, i2s + 4, 8);
    constexpr uint32_t scoop2 = 0x08800040;
    write(soc, scoop2 + 0x20, 0x200);
    write(soc, scoop2 + 0x24, 0x200);
    assert(!soc.MicrophoneDemand().active);
    write(soc, i2s + 4, 0);
    assert(soc.MicrophoneDemand().active);
    run(soc, 1);
    assert(mic.pulls > 0 && read(soc, i2s + 0x80) == 0xfe00fe00);
    auto recording = snapshot(soc);
    assert(soc.Load(recording.size(), recording.data()));
    assert(mic.disarms == 1);
    const unsigned pulls = mic.pulls;
    write(soc, i2s + 4, 8);
    write(soc, i2s + 4, 0);
    run(soc, 1);
    assert(mic.pulls == pulls && read(soc, i2s + 0x80) == 0);
}


static void spitzPower() {
    constexpr uint32_t ssp = 0x41700000, scoop = 0x10800000, rtc = 0x40900000;
    constexpr uint32_t adcSelect = 1u << 20, powerGood = 1u << 21;
    constexpr uint32_t doorLocked = 1u << (90 - 64), full = 1u << (101 - 96), acAbsent = 1u << (115 - 96);
    constexpr uint32_t powerOutputs = (1u << 1) | (1u << 3) | (1u << 7) | (1u << 9);
    std::vector<uint8_t> rom(8u << 20, 0xff), nand(spitzNandRawSize, 0xff);
    const uint32_t branch = 0xeafffffe;
    std::memcpy(rom.data(), &branch, sizeof(branch));
    SocPXA soc(deviceTypeSpitz, 64u << 20, rom.data(), rom.size(), nand.data(), nand.size(), -1, 2);
    assert(soc.BatterySupported());
    assert(read(soc, GPIO) & powerGood);
    assert(read(soc, GPIO + 8) & doorLocked);
    assert(!(read(soc, GPIO + 0x100) & (acAbsent | full)));

    assert(soc.SetSpitzPower(50, false, true));
    assert(read(soc, GPIO + 0x100) & acAbsent);
    assert(soc.SetSpitzBatteryDoorLocked(false));
    assert(!(read(soc, GPIO + 8) & doorLocked));
    assert(soc.SetSpitzBatteryDoorLocked(true));
    assert(read(soc, GPIO + 8) & doorLocked);

    write(soc, GPIO + 0x0c, adcSelect);
    write(soc, GPIO + 0x18, adcSelect);
    write(soc, ssp, 0x6a7);
    auto transfer = [&](uint32_t command) {
        write(soc, ssp + 0x10, command);
        run(soc, 1);
        return read(soc, ssp + 0x10);
    };
    auto measure = [&](uint32_t command) {
        write(soc, GPIO + 0x24, adcSelect);
        assert(transfer(command) == 0);
        const uint32_t high = transfer(0);
        const uint32_t low = transfer(0);
        write(soc, GPIO + 0x18, adcSelect);
        return (high << 2) | (low >> 6);
    };
    constexpr uint32_t remoteIrq = 1u << 13, jack = 1u << (116 - 96);
    constexpr uint32_t scoop1 = 0x08800040, pullup = 1u << 2;
    assert(!soc.SetSpitzRemoteKey(SpitzRemoteKey::play, true));
    assert(measure(0x8f) == 0);
    auto badUnpluggedRemote = legacy(snapshot(soc), ChunkType::spitzPanel, 0, 1);
    soc.SetSpitzRemote(true);
    assert(soc.HeadphonesIn() && (read(soc, GPIO + 0x100) & jack));
    assert(measure(0x8f) == 255 && !(read(soc, GPIO) & remoteIrq));
    write(soc, scoop1 + 0x20, pullup);
    write(soc, scoop1 + 0x24, pullup);
    assert(!(read(soc, GPIO) & remoteIrq));
    write(soc, GPIO + 0x30, remoteIrq);
    for (const auto& [key, value] : {
             std::pair{SpitzRemoteKey::volumeDown, 220u}, {SpitzRemoteKey::mute, 185u},
             {SpitzRemoteKey::previous, 151u}, {SpitzRemoteKey::volumeUp, 119u},
             {SpitzRemoteKey::next, 89u}, {SpitzRemoteKey::play, 59u}, {SpitzRemoteKey::stop, 27u}}) {
        assert(soc.SetSpitzRemoteKey(key, true));
        assert((read(soc, GPIO) & remoteIrq) && (read(soc, GPIO + 0x48) & remoteIrq));
        assert(measure(0x8f) == value);
        assert(soc.SetSpitzRemoteKey(key, false));
        assert(!(read(soc, GPIO) & remoteIrq));
        write(soc, GPIO + 0x48, remoteIrq);
    }
    assert(soc.SetSpitzRemoteKey(SpitzRemoteKey::play, true));
    assert(!soc.Load(badUnpluggedRemote.size(), badUnpluggedRemote.data()));
    assert(soc.SpitzRemoteInserted() && measure(0x8f) == 59);
    auto pressedRemote = snapshot(soc);
    soc.SetSpitzRemote(false);
    assert(!soc.HeadphonesIn() && !(read(soc, GPIO + 0x100) & jack));
    assert(soc.SetHeadphones(true));
    auto badPluggedRemote = legacy(pressedRemote, ChunkType::spitzPanel, 0, 1);
    assert(!soc.Load(badPluggedRemote.size(), badPluggedRemote.data()));
    assert(!soc.SpitzRemoteInserted() && soc.HeadphonesIn() && (read(soc, GPIO + 0x100) & jack));
    assert(soc.Load(pressedRemote.size(), pressedRemote.data()));
    assert(soc.SpitzRemoteInserted() && soc.HeadphonesIn() && measure(0x8f) == 255);
    assert(soc.SetHeadphones(true));
    assert(!soc.SpitzRemoteInserted() && soc.HeadphonesIn() && measure(0x8f) == 0);
    assert(soc.SetHeadphones(false));

    assert(measure(0xcf) == 191);  // Battery voltage, 50% unplugged.
    assert(measure(0xaf) == 0);    // Thermistor without power.
    assert(measure(0xef) == 0);    // Unplugged adapter.

    assert(soc.SetBattery({BatteryPower::Mode::level, 50, true}));
    write(soc, scoop + 0x20, powerOutputs);
    write(soc, scoop + 0x24, powerOutputs);
    SpitzPowerState state{};
    assert(soc.GetSpitzPower(state));
    assert(state.batteryPercent == 50 && state.acConnected && state.batteryDoorLocked);
    assert(state.chargeEnabled && state.chargeLed && state.diskLed);
    assert(measure(0xcf) == 194);  // Charging curve.
    assert(measure(0xaf) == 210);
    assert(measure(0xef) == 128);
    assert(!(read(soc, GPIO + 0x100) & full));
    assert(soc.SetSpitzPower(100, true, true));
    assert(read(soc, GPIO + 0x100) & full);
    assert(soc.SetSpitzPower(0, false, true));
    assert(!(read(soc, GPIO) & powerGood));
    assert(soc.SetSpitzPower(50, false, false));

    // The guest can set a Unix counter. The clock persists across a pin reset
    // and a session load. A load restores the door and keeps the host charger setting.
    const uint32_t wall = std::time(nullptr);
    assert(read(soc, rtc) - wall < 60);
    constexpr uint32_t setTime = 946684800;  // 2000-01-01 UTC.
    write(soc, rtc, setTime);
    write(soc, rtc + 4, setTime + 30);
    auto saved = snapshot(soc);
    soc.Reset();
    assert(read(soc, rtc) - setTime < 60 && read(soc, rtc + 4) == setTime + 30);
    assert(!(read(soc, GPIO + 8) & doorLocked));
    assert(read(soc, GPIO + 0x100) & acAbsent);
    write(soc, rtc, wall);
    assert(soc.SetSpitzPower(100, true, true));
    assert(soc.Load(saved.size(), saved.data()));
    assert(read(soc, rtc) - setTime < 60 && read(soc, rtc + 4) == setTime + 30);
    assert(!(read(soc, GPIO + 8) & doorLocked));
    assert(!(read(soc, GPIO + 0x100) & acAbsent));
    assert(soc.GetSpitzPower(state) && !state.batteryDoorLocked && state.acConnected);

    write(soc, PM + 0x0c, 1u << 31);
    write(soc, rtc + 8, 4);  // Enable the alarm interrupt.
    sleep(soc);
    auto asleep = snapshot(soc);
    assert(soc.Load(asleep.size(), asleep.data()));
    assert(soc.PowerSleeping());
    write(soc, rtc, setTime + 30);  // Bring the counter to the match without waiting for wall time.
    run(soc, 1100);
    assert(!soc.PowerSleeping());
    assert(read(soc, PM + 0x18) & (1u << 31));
    assert(read(soc, rtc + 8) & 1);
}

static void spitzDirectWake() {
    constexpr uint32_t resume = 0xa0004000, rtc = 0x40900000;
    std::vector<uint8_t> rom(8u << 20, 0xff), nand(spitzNandRawSize, 0xff);
    SocPXA soc(deviceTypeSpitz, 64u << 20, rom.data(), rom.size(), nand.data(), nand.size(), -1, 2, true);
    write(soc, resume, 0xeafffffe);
    write(soc, PM + 8, resume);
    write(soc, PM + 0x0c, 1u << 31);
    const auto alarm = read(soc, rtc) + 30;
    write(soc, rtc + 4, alarm);
    sleep(soc);
    write(soc, rtc, alarm);
    run(soc, 1100);
    assert(!soc.PowerSleeping());
    assert(cpuGetRegExternal(soc.GetCpu(), 15) == resume);
    assert(read(soc, PM + 0x30) & 4);
    assert(!(read(soc, PM + 4) & 0x30));
    assert(read(soc, 0x48000000) == 0x1ac81ac9);
}

// Two ATA sockets and an SD slot, each with distinct media and an in-flight transfer.
static void spitzStorage() {
    constexpr uint32_t external = 0x20000000, internal = 0x30000000, attr = 0x08000000;
    constexpr uint32_t scoopCf = 0x10800000, scoopDrive = 0x08800040, mmc = 0x41100000;
    constexpr uint32_t sdDetect = 1u << 9, sdProtect = 1u << (81 - 64);
    constexpr uint32_t cfDetect = 1u << (94 - 64), cfIrq = 1u << (105 - 96), driveIrq = 1u << (106 - 96);
    constexpr auto microdriveId = "0123456789abcdef0123456789abcdef";

    struct RawDrive {
        char path[40] = "/tmp/frond-spitz-storage.XXXXXX";
        RawDrive() {
            frondStorage::Fd fd(mkstemp(path));
            assert(fd.fd >= 0);
            std::array<uint8_t, 2048> data{};
            data.fill(0x32);
            assert(frondStorage::writeAllAt(fd.fd, data.data(), data.size(), 0));
        }
        ~RawDrive() { frondStorage::removePath(path); }
    } drive, clone;

    const uint32_t branch = 0xeafffffe;
    std::vector<uint8_t> rom(8u << 20), nand(spitzNandRawSize, 0xff);
    std::memcpy(rom.data(), &branch, sizeof(branch));
    auto newBoard = [&] {
        return std::make_unique<SocPXA>(deviceTypeSpitz, 64u << 20, rom.data(), rom.size(),
                                        nand.data(), nand.size(), -1, 2);
    };

    std::vector<uint8_t> oldSnapshot;
    {
        auto soc = newBoard();
        assert(soc->FlushMicrodrive());
        assert(soc->MountMicrodrive(drive.path, true));
        // After v1: both media IDs, external capacity and task file, Ethernet and Wi-Fi flags.
        constexpr unsigned suffixWords = 8 + 1 + 8 + 4 + 18 + 512 / 4;
        oldSnapshot = legacy(snapshot(*soc), ChunkType::spitzPcmcia, 1, suffixWords);
    }
    {
        auto soc = newBoard();
        assert(soc->MountMicrodrive(clone.path, true, microdriveId));
        assert(!soc->Load(oldSnapshot.size(), oldSnapshot.data()));
    }

    std::vector<uint8_t> portableSnapshot;
    {
        auto board = newBoard();
        auto& soc = *board;
        assert(soc.MountMicrodrive(drive.path, true, microdriveId));
        assert(soc.Load(oldSnapshot.size(), oldSnapshot.data()));

        auto access8 = [&](uint32_t address, bool store, uint8_t value = 0) {
            assert(memAccess(cpuGetMem(soc.GetCpu()), address, 1, store, &value));
            return value;
        };
        auto ata = [&](uint32_t base, uint8_t sector, uint8_t op) {
            access8(base + 6, true, 0xe0);
            access8(base + 2, true, 1);
            access8(base + 3, true, sector);
            access8(base + 7, true, op);
        };
        auto mmcCommand = [&](unsigned command, uint32_t argument, unsigned flags) {
            write(soc, mmc + 0x30, command);
            write(soc, mmc + 0x34, argument >> 16);
            write(soc, mmc + 0x38, argument & 0xffff);
            write(soc, mmc + 0x10, flags);
            return read(soc, mmc + 4);
        };

        assert(read(soc, GPIO) & sdDetect);
        assert(read(soc, GPIO + 8) & cfDetect);
        assert(read(soc, scoopCf + 8) == 4);
        assert(read(soc, external + attr) == 0xffffffff);

        auto& cf = sdCardStore(1);
        cf.initialize(32, "compact-flash");
        std::array<uint8_t, 512> sector{};
        sector.fill(0x65);
        assert(cf.write(1, sector.data()));
        assert(soc.InsertCompactFlash(false));
        assert(!soc.InsertCompactFlash(false));
        assert(!(read(soc, GPIO + 8) & cfDetect));
        write(soc, scoopCf + 0x0c, 0x82);
        write(soc, scoopDrive + 0x0c, 0x82);
        assert(read(soc, scoopCf + 8) == 0xb2);
        assert(access8(external + attr, false) == 1);
        assert(access8(internal + attr, false) == 1);
        access8(external + attr + 0x200, true, 1);
        access8(internal + attr + 0x200, true, 1);

        sdCardInitialize(8192, "sd-card");
        sector.fill(0x11);
        assert(sdCardWrite(1, sector.data()));
        soc.SetSdWriteProtect(true);
        soc.SdInsert();
        assert(!(read(soc, GPIO) & sdDetect));
        assert(read(soc, GPIO + 8) & sdProtect);

        write(soc, mmc, 2);
        assert(mmcCommand(8, 0x1aa, 1) & 2);  // inserted but rail is off
        write(soc, scoopCf + 0x0c, 0x86);
        assert(!(mmcCommand(8, 0x1aa, 1) & 2));
        assert(mmcCommand(0, 0, 0) & 0x2000);
        for (unsigned attempt = 0; attempt < 21; ++attempt) {
            assert(!(mmcCommand(55, 0, 1) & 2));
            assert(!(mmcCommand(41, 0x00300000, 3) & 2));
        }
        assert(read(soc, mmc + 0x3c) & 0x80);
        assert(!(mmcCommand(2, 0, 2) & 2));
        assert(!(mmcCommand(3, 0, 1) & 2));
        assert(!(mmcCommand(7, 3 << 16, 1) & 2));
        write(soc, mmc + 0x1c, 512);
        write(soc, mmc + 0x20, 1);
        assert(!(mmcCommand(17, 512, 5) & 2));

        ata(external, 1, 0x20);
        ata(internal, 1, 0x20);
        assert(!(read(soc, GPIO + 0x100) & (cfIrq | driveIrq)));
        assert(access8(external + 7, false) & 8);
        assert(read(soc, GPIO + 0x100) & cfIrq);
        assert(!(read(soc, GPIO + 0x100) & driveIrq));

        assert(read(soc, external) == 0x65656565);
        assert(read(soc, internal) == 0x32323232);
        assert(read(soc, mmc + 0x40) == 0x11111111);
        const auto threeTransfers = snapshot(soc);
        for (unsigned i = 0; i < 127; ++i) {
            assert(read(soc, external) == 0x65656565);
            assert(read(soc, internal) == 0x32323232);
            assert(read(soc, mmc + 0x40) == 0x11111111);
        }
        auto restore = threeTransfers;
        assert(soc.Load(restore.size(), restore.data()));
        assert(soc.CompactFlashInserted() && soc.SdInserted());
        for (unsigned i = 0; i < 127; ++i) {
            assert(read(soc, external) == 0x65656565);
            assert(read(soc, internal) == 0x32323232);
            assert(read(soc, mmc + 0x40) == 0x11111111);
        }

        // WP affects both the Sharp switch and the card protocol, on the independent SD bus.
        write(soc, mmc + 0x20, 1);
        assert(!(mmcCommand(24, 512, 0xd) & 2));
        assert(read(soc, mmc + 0x3c) & 4);
        soc.SetSdWriteProtect(false);
        assert(!(read(soc, GPIO + 8) & sdProtect));
        write(soc, mmc + 0x20, 1);
        assert(!(mmcCommand(24, 512, 0xd) & 2));
        for (unsigned i = 0; i < 128; ++i) write(soc, mmc + 0x44, 0x44444444);
        assert(sdCardRead(1, sector.data()) && sector.front() == 0x44 && sector.back() == 0x44);

        // Partial PIO writes stay on the matching card and are lost on physical removal.
        ata(external, 1, 0x30);
        for (unsigned i = 0; i < 16; ++i) write(soc, external, 0x77777777);
        soc.SetCompactFlashWriteProtect(true);
        assert((access8(external + 7, false) & 9) == 1);
        ata(external, 1, 0x30);
        assert((access8(external + 7, false) & 9) == 1);
        soc.SetCompactFlashWriteProtect(false);
        ata(external, 1, 0x30);
        for (unsigned i = 0; i < 64; ++i) write(soc, external, 0x77777777);

        // Turning off SD's bit on the shared SCOOP does not reset an active CF transfer.
        write(soc, scoopCf + 0x0c, 0x82);
        assert(!(read(soc, GPIO) & sdDetect));
        assert(mmcCommand(8, 0x1aa, 1) & 2);
        for (unsigned i = 0; i < 64; ++i) write(soc, external, 0x77777777);
        assert(cf.read(1, sector.data()) && sector.front() == 0x77 && sector.back() == 0x77);
        ata(external, 1, 0x30);
        write(soc, external, 0x88888888);
        soc.EjectCompactFlash();
        assert(read(soc, GPIO + 8) & cfDetect);
        assert(read(soc, external) == 0xffffffff);
        assert(cf.read(1, sector.data()) && sector.front() == 0x77);
        ata(internal, 1, 0x20);
        assert(read(soc, internal) == 0x32323232);
        soc.SdEject();
        assert(read(soc, GPIO) & sdDetect);

        assert(!soc.Load(restore.size(), restore.data()));
        soc.SetCompactFlashSwapOnLoad(true);
        assert(soc.Load(restore.size(), restore.data()));
        soc.SetCompactFlashSwapOnLoad(false);
        assert(!soc.CompactFlashInserted());
        assert(soc.SdInserted());
        soc.SdEject();
        assert(soc.InsertCompactFlash(false));
        soc.Reset();
        assert(soc.CompactFlashInserted());
        assert(!(read(soc, GPIO + 8) & cfDetect));
        assert(read(soc, external) == 0xffffffff);  // physically attached, power reset
        soc.EjectCompactFlash();

        // A managed Microdrive changes only its overlay until the session publishes.
        write(soc, scoopDrive + 0x0c, 0x82);
        access8(internal + attr + 0x200, true, 1);
        ata(internal, 2, 0x30);
        for (unsigned i = 0; i < 128; ++i) write(soc, internal, 0x9b9b9b9b);
        access8(internal + 7, true, 0xe7);
        assert(!(access8(internal + 7, false) & 1));
        auto readBase = [&] {
            frondStorage::Fd fd(open(drive.path, O_RDONLY | O_CLOEXEC));
            assert(fd.fd >= 0 && frondStorage::readAllAt(fd.fd, sector.data(), sector.size(), 1024));
            return sector.front();
        };
        ata(internal, 2, 0x20);
        assert(read(soc, internal) == 0x9b9b9b9b);
        assert(readBase() == 0x32);
        assert(!soc.WriteMicrodriveBase(2048, sector.data(), sector.size()));
        unsigned journalPages = 0;
        assert(soc.ForEachMicrodriveChange([&](uint64_t offset, uint64_t length, const uint8_t* data) {
            ++journalPages;
            assert(offset == 0 && length == 2048 && data[1024] == 0x9b);
            return soc.WriteMicrodriveBase(offset, data, length);
        }));
        assert(journalPages == 1 && soc.FlushMicrodrive() && soc.ResetMicrodriveOverlay());
        assert(soc.ForEachMicrodriveChange([](uint64_t, uint64_t, const uint8_t*) {
            return false;  // no changes after rebase
        }));
        assert(readBase() == 0x9b);
        portableSnapshot = snapshot(soc);

        // The Sharp driver uses CMD1 for MMC and assigns its own RCA with CMD3.
        assert(soc.SetSpitzMmcCard(true));
        soc.SdInsert();
        assert(!soc.SetSpitzMmcCard(false));
        write(soc, mmc, 2);
        write(soc, scoopCf + 0x0c, 4);
        assert(mmcCommand(55, 0, 1) & 2);
        assert(mmcCommand(8, 0x1aa, 1) & 2);
        assert(mmcCommand(0, 0, 0) & 0x2000);
        assert(!(mmcCommand(1, 0, 3) & 2));
        assert(!(read(soc, mmc + 0x3c) & 0x80));
        for (unsigned attempt = 0; attempt < 21; ++attempt)
            assert(!(mmcCommand(1, 0x00ff8000, 3) & 2));
        assert(read(soc, mmc + 0x3c) & 0x80);
        assert(!(mmcCommand(2, 0, 2) & 2));
        assert(!(mmcCommand(3, 0x1234 << 16, 1) & 2));
        assert(!(mmcCommand(9, 0x1234 << 16, 2) & 2));
        assert((read(soc, mmc + 0x3c) & 0xff) == 0x4c);
        assert(!(mmcCommand(7, 0x1234 << 16, 1) & 2));
        write(soc, mmc + 0x1c, 512);
        write(soc, mmc + 0x20, 1);
        assert(!(mmcCommand(17, 512, 5) & 2));
        assert(read(soc, mmc + 0x40) == 0x44444444);
        auto mmcState = snapshot(soc);

        soc.Reset();
        assert(soc.SpitzMmcCard() && soc.SdInserted());
        soc.SdEject();
        assert(soc.SetSpitzMmcCard(false));
        assert(soc.Load(mmcState.size(), mmcState.data()));
        assert(soc.SpitzMmcCard() && soc.SdInserted());
        assert(read(soc, mmc + 0x40) == 0x44444444);
        soc.SdEject();
        assert(soc.FlushMicrodrive());
        sdCardReset();
        cf.reset();
    }

    assert(std::filesystem::copy_file(drive.path, clone.path, std::filesystem::copy_options::overwrite_existing));
    {
        auto soc = newBoard();
        assert(soc->MountMicrodrive(clone.path, true, "fedcba9876543210fedcba9876543210"));
        assert(!soc->Load(portableSnapshot.size(), portableSnapshot.data()));
    }
    auto soc = newBoard();
    assert(soc->MountMicrodrive(clone.path, true, microdriveId));
    assert(soc->Load(portableSnapshot.size(), portableSnapshot.data()));
    assert(read(*soc, internal) == 0x9b9b9b9b);  // Resume the saved in-flight PIO read on the copy.
}

static void spitzWifiSocket() {
    constexpr uint32_t io = 0x20000000, attribute = 0x28000000, scoop = 0x10800000;
    constexpr uint32_t detect = 1u << (94 - 64), irq = 1u << (105 - 96);
    constexpr uint32_t idleWord = 0xeafffffe;
    std::vector<uint8_t> rom(8u << 20), nand(spitzNandRawSize, 0xff);
    std::memcpy(rom.data(), &idleWord, sizeof(idleWord));
    SocPXA soc(deviceTypeSpitz, 64u << 20, rom.data(), rom.size(), nand.data(), nand.size(), -1, 2);
    auto* memory = cpuGetMem(soc.GetCpu());
    auto attr = [&](uint32_t address, bool store = false, uint8_t value = 0) {
        assert(memAccess(memory, attribute + address, 1, store, &value));
        return value;
    };
    auto word = [&](uint32_t address, bool store = false, uint16_t value = 0) {
        assert(memAccess(memory, io + address, 2, store, &value));
        return value;
    };

    assert(soc.SetSpitzCfNetwork(SpitzCfNetwork::wifi));
    assert(soc.GetSpitzCfNetwork() == SpitzCfNetwork::wifi);
    assert(!(read(soc, GPIO + 8) & detect));
    assert(attr(0) == 0xff && word(0) == 0xffff);
    write(soc, scoop + 0x0c, 0x82);
    assert(attr(0) == 1 && word(0) == 0xffff);
    attr(0x3e0, true, 0x41);
    assert(attr(0x3e0) == 0x41 && word(0) == 0);
    word(0, true, 0);  // INITIALIZE completion asserts the GPIO only while masked in.
    assert(read(soc, GPIO + 0x100) & irq);
    word(0x32, true, 0x10);
    assert(!(read(soc, GPIO + 0x100) & irq));
    word(0x34, true, 0x10);
    assert(read(soc, GPIO + 0x100) & irq);
    word(0x28, true, 0x4a2d);

    auto saved = snapshot(soc);
    // Make the malformed Wi-Fi fixture before a wider Ethernet chunk can enlarge the save layout.
    auto truncated = legacy(saved, ChunkType::spitzPcmcia, 4, 1);
    assert(!soc.Load(truncated.size(), truncated.data()));
    assert(soc.GetSpitzCfNetwork() == SpitzCfNetwork::wifi && word(0x28) == 0x4a2d);
    assert(soc.SetSpitzCfNetwork(SpitzCfNetwork::off));
    assert(read(soc, GPIO + 8) & detect);
    assert(soc.Load(saved.size(), saved.data()));
    assert(soc.GetSpitzCfNetwork() == SpitzCfNetwork::wifi && word(0x28) == 0x4a2d);

    auto& cf = sdCardStore(1);
    cf.initialize(32, "wifi-exclusive-fixture");
    assert(!soc.InsertCompactFlash(false));
    assert(soc.SetSpitzCfNetwork(SpitzCfNetwork::off));
    assert(soc.InsertCompactFlash(false));
    assert(!soc.SetSpitzCfNetwork(SpitzCfNetwork::wifi));
    assert(!soc.Load(saved.size(), saved.data()));
    assert(soc.GetSpitzCfNetwork() == SpitzCfNetwork::off && soc.CompactFlashInserted());
    soc.EjectCompactFlash();
    cf.reset();

    assert(soc.SetSpitzCfNetwork(SpitzCfNetwork::ethernet));
    auto wiredV3 = legacy(snapshot(soc), ChunkType::spitzPcmcia, 3, 1);
    assert(soc.SetSpitzCfNetwork(SpitzCfNetwork::off));
    assert(soc.Load(wiredV3.size(), wiredV3.data()));
    assert(soc.GetSpitzCfNetwork() == SpitzCfNetwork::ethernet);
    assert(soc.SetSpitzCfNetwork(SpitzCfNetwork::off));

    puts("PASS: Spitz Wi-Fi socket power, IRQ, v4 reopen, v3 Ethernet and CF exclusivity");
}

int main() {
    spitzBoard();
    spitzUsbDevice();
    spitzCodecControl();
    spitzStorage();
    spitzWifiSocket();
    spitzKeyboardAndHinge();
    firmwarePower();
    spitzPower();
    spitzDirectWake();
    clocks();
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
    auto old = legacy(saved, ChunkType::pxaPwrClk, 1, 4);
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
        if (words[1 + 2 * i] == uint32_t(ChunkType::pxaPwrClk)) words[offset + size - 3] = 1;  // wakePending, before CLKCFG and the active CCCR
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
    pxa25xSleep();
    puts("PASS: PXA27x and PXA25x sleep wake, reset retention, legacy and fresh-state restore");
}
