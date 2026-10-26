#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "CPU.h"
#include "audio_queue.h"
#include "soc_PXA.h"
#include "sdcard.h"

static ArmCpu* cpu;
static void write32(uint32_t addr, uint32_t value) {
    assert(cpuMemOpExternal(cpu, &value, addr, 4, true));
}
static uint32_t read32(uint32_t addr) {
    uint32_t value = 0;
    assert(cpuMemOpExternal(cpu, &value, addr, 4, false));
    return value;
}
static void step(uint32_t instruction, uint32_t address) {
    write32(0xa0000000, instruction);
    cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu, ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC);
    cpuSetReg(cpu, 15, 0xa0000000);
    cpuSetReg(cpu, 0, address);
    cpuCyclePure(cpu);
    assert(cpuGetRegisters(cpu)[15] == 0xa0000004);
}
static void loadWmmx(unsigned reg, uint64_t value) {
    write32(0xa0001000, uint32_t(value));
    write32(0xa0001004, uint32_t(value >> 32));
    step(0xedd00100 | (reg << 12), 0xa0001000); // LDCL p1,cN,[r0]
}
static uint64_t readWmmx(unsigned reg) {
    step(0xedc00100 | (reg << 12), 0xa0001000); // STCL p1,cN,[r0]
    return uint64_t(read32(0xa0001000)) | (uint64_t(read32(0xa0001004)) << 32);
}
struct E2Versions {
    void Load(SavestateLoader<ChunkType>& loader) {
        for (auto type : {ChunkType::pxaLcd, ChunkType::pxaPwrClk, ChunkType::pxaTimr}) {
            uint32_t version = 99;
            assert(loader.GetChunkOrFail(type, 1, "E2 compatibility", version));
            assert(version == 0);
        }
    }
};
int main() {
    std::vector<uint8_t> rom(16 * 1024 * 1024);
    std::vector<uint8_t> nand(132 * 1024 * 1024, 0xff);
    SocPXA soc(DeviceType5::deviceTypeTX, 32 * 1024 * 1024, rom.data(), rom.size(),
               nand.data(), nand.size(), -1, 2);
    cpu = soc.GetCpu();
    assert(soc.GetUsbController());
    assert(soc.SetUsbTestCable(false) && (read32(0x40e00000) & (1u << 13)));
    assert(soc.SetUsbTestCable(true) && !(read32(0x40e00000) & (1u << 13)));
    auto audio = audioQueueCreate(1024);
    soc.SetAudioQueue(audio);
    cpuSetCPAR(cpu, 3);
    write32(0xa0000000, 0xeafffffe); // B .
    cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu, 15, 0xa0000000);
    uint32_t timerBefore = read32(0x40a00010);
    soc.Run(100000, 10000000); // 10 ms of emulated time
    uint32_t ticks = read32(0x40a00010) - timerBefore;
    // Scheduler periods have integer-nanosecond resolution.
    assert(ticks >= 32400 && ticks <= 32600);
    // A counter read inside a long CPU batch observes instruction progress,
    // not the counter at the previous scheduler dispatch (up to 32 ticks stale).
    write32(0xa0000000, 0xe5901000); // LDR r1,[r0]
    write32(0xa0000004, 0xeafffffd); // B back to LDR
    cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu, 0, 0x40a00010);
    cpuSetReg(cpu, 15, 0xa0000000);
    soc.Run(1024, 10000000);
    assert(read32(0x40a00010) - cpuGetRegExternal(cpu, 1) <= 2);
    assert(cpuCyclesInProgress(cpu) == 0);
    write32(0xa0000000, 0xe7fd6801); // Thumb LDR r1,[r0]; B back to LDR.
    cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu, 15, 0xa0000001);
    soc.Run(1024, 10000000);
    assert(read32(0x40a00010) - cpuGetRegExternal(cpu, 1) <= 2);
    assert(cpuCyclesInProgress(cpu) == 0);
    // TX-only register banks and the seventh LCD DMA channel must survive reload.
    const uint32_t registers[] = {0x58000000, 0x41500048, 0x40600004, 0x40600008,
        0x4060000c, 0x40600018, 0x40600020, 0x40600024, 0x40600404,
        0x40f00040, 0x40f00050, 0x40f00080, 0x40f000fc, 0x44000260};
    std::vector<uint32_t> expected;
    for (unsigned i = 0; i < sizeof(registers) / sizeof(registers[0]); ++i) {
        write32(registers[i], 0x13579bdfu + i * 0x1020304u);
        expected.push_back(read32(registers[i]));
    }
    write32(0xa5fffffc, 0x76543210);
    for (unsigned i = 0; i < 16; ++i) loadWmmx(i, 0x123456789abcdef0ull + i);
    // USB foundation: save a partially loaded EP0 packet through the real SoC.
    write32(0x40600000, 1);
    write32(0x40600300, 0x44332211);
    assert(!(read32(0x40600100) & 2));
    const auto savedTimer = read32(0x40a00010);
    assert(soc.Save());
    const auto saved = soc.GetSavestate();
    std::vector<uint8_t> snapshot(saved.size);
    std::memcpy(snapshot.data(), saved.data, saved.size);
    assert(soc.SetUsbTestCable(false));
    write32(0x40600100, 4); // discard the unsent EP0 packet before restore
    for (auto addr : registers) write32(addr, 0);
    write32(0xa5fffffc, 0);
    for (unsigned i = 0; i < 16; ++i) loadWmmx(i, 0);
    audioQueuePush(audio, 0x12345678);
    soc.PenDown(20, 20);
    soc.KeyDown(keyIdHard1);
    assert(soc.Load(snapshot.size(), snapshot.data()));
    assert(audioQueuePendingSamples(audio) == 0);
    assert(!(read32(0x40e00000) & (1u << 13))); // detector restored with guest state
    assert(read32(0x40600000) == 1 && !(read32(0x40600100) & 2));
    for (unsigned i = 0; i < 3; ++i) write32(0x40600300, 0x88776655);
    assert(read32(0x40600100) & 2); // restored four bytes + twelve new bytes
    assert(!read32(0x4060000c)); // no fabricated packet-complete IRQ
    assert(read32(0x40a00010) == savedTimer);
    for (unsigned i = 0; i < expected.size(); ++i) assert(read32(registers[i]) == expected[i]);
    assert(read32(0xa5fffffc) == ~0x76543210u);
    for (unsigned i = 0; i < 16; ++i) assert(readWmmx(i) == 0x123456789abcdef0ull + i);
    // A rejected snapshot restores the current state rather than leaving a partial load.
    assert(!soc.Load(snapshot.size() - 4, snapshot.data()));
    assert(read32(0x41500048) == expected[1]);
    auto missingKeypad = snapshot;
    auto* toc = reinterpret_cast<uint32_t*>(missingKeypad.data());
    bool removed = false;
    for (unsigned i = 0; i < toc[0]; ++i) {
        if (toc[1 + 2 * i] == static_cast<uint32_t>(ChunkType::pxa270kpc)) {
            toc[1 + 2 * i] = 0x7fffffff;
            removed = true;
        }
    }
    assert(removed);
    write32(0x41500048, 77);
    assert(!soc.Load(missingKeypad.size(), missingKeypad.data()));
    assert(read32(0x41500048) == 77);
    write32(0xa0002000, 0xfeedface);
    audioQueuePush(audio, 0x12345678);
    soc.Reset();
    assert(audioQueuePendingSamples(audio) == 0);
    assert(read32(0x58000000) == 0);
    assert(read32(0x41500048) == 100);
    assert(read32(0x40600004) == 0);
    assert(read32(0x40600000) == 0 && read32(0x40600100) == 0);
    write32(0x40600000, 1);
    for (unsigned i = 0; i < 4; ++i) write32(0x40600300, i);
    assert(read32(0x40600100) == 2); // cold reset cleared FIFO occupancy
    write32(0x40600000, 0);
    assert(read32(0x40f00080) == 0);
    assert(read32(0x44000260) == 0);
    assert(read32(0xa5fffffc) == 0xffffffffu);
    assert(read32(0xa0002000) == 0xfeedface);
    cpuSetCPAR(cpu, 3);
    for (unsigned i = 0; i < 16; ++i) assert(readWmmx(i) == 0);

    // A new process has no cached LCD frame, even when guest RAM is unchanged.
    const uint32_t descriptor = 0xa0003000, framebuffer = 0xa0100000;
    for (unsigned offset = 0; offset < 320 * 480 * 2; offset += 4)
        write32(framebuffer + offset, 0xf800f800);
    write32(descriptor, descriptor);
    write32(descriptor + 4, framebuffer);
    write32(descriptor + 8, 0);
    write32(descriptor + 12, 320 * 480 * 2);
    write32(0x44000200, descriptor);
    write32(0x4400000c, 4u << 24);
    write32(0x44000000, 1);
    write32(0xa0000000, 0xeafffffe); // B .
    cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu, 15, 0xa0000000);
    soc.Run(5000000, 100000000);
    uint32_t first, last;
    auto frame = soc.GetPendingFrame(first, last);
    assert(frame && frame[0] == 0xff0000ff && frame[320 * 480 - 1] == frame[0]);
    assert(soc.Save());
    auto lcdState = soc.GetSavestate();
    std::vector<uint8_t> lcdSnapshot(lcdState.size);
    std::memcpy(lcdSnapshot.data(), lcdState.data, lcdState.size);
    SocPXA fresh(DeviceType5::deviceTypeTX, 32 * 1024 * 1024, rom.data(), rom.size(),
                 nand.data(), nand.size(), -1, 2);
    auto sourceRam = soc.GetMemoryData(), destinationRam = fresh.GetMemoryData();
    assert(sourceRam.size == destinationRam.size);
    std::memcpy(destinationRam.data, sourceRam.data, sourceRam.size);
    assert(fresh.Load(lcdSnapshot.size(), lcdSnapshot.data()));
    fresh.Run(5000000, 100000000);
    frame = fresh.GetPendingFrame(first, last);
    assert(frame && frame[0] == 0xff0000ff && frame[320 * 480 - 1] == frame[0]);
    // E2 keeps the old timer and version-zero LCD/power wire format.
    SocPXA e2(DeviceType5::deviceTypeE2, 16 * 1024 * 1024, rom.data(), 8 * 1024 * 1024,
              nand.data(), 33 * 1024 * 1024, -1, 0);
    cpu = e2.GetCpu();
    const auto e2Pins = read32(0x40e00000);
    assert(!e2.GetUsbController() && !e2.SetUsbTestCable(true));
    assert(read32(0x40e00000) == e2Pins);
    write32(0xa0000000, 0xeafffffe);
    cpuInvalidateCodeCache(cpu);
    cpuSetReg(cpu, 15, 0xa0000000);
    timerBefore = read32(0x40a00010);
    e2.Run(100000, 10000000);
    ticks = read32(0x40a00010) - timerBefore;
    assert(ticks >= 36700 && ticks <= 37000);
    assert(e2.Save());
    auto e2State = e2.GetSavestate();
    E2Versions versions;
    SavestateLoader<ChunkType> loader;
    assert(loader.Load(e2State.data, e2State.size, versions));
    assert(e2.Load(e2State.size, e2State.data));
    // Cold reset preserves the physical card, including its active-low detect
    // input. It must not require an artificial eject/insert edge after boot.
    cpu = soc.GetCpu();
    sdCardInitialize(65536, "reset-card");
    uint8_t sector[512] = {};
    sector[0] = 0x5a;
    assert(sdCardWrite(65535, sector));
    soc.SdInsert();
    assert(soc.SdInserted() && !(read32(0x40e00000) & (1u << 14)));
    soc.Reset();
    assert(soc.SdInserted() && !(read32(0x40e00000) & (1u << 14)));
    assert(!(read32(0x40e0000c) & (1u << 14))); // detect remains an input
    assert(sdCardRead(65535, sector) && sector[0] == 0x5a);
    soc.SdEject();
    soc.Reset();
    assert(!soc.SdInserted() && (read32(0x40e00000) & (1u << 14)));
    assert(sdCardRead(65535, sector) && sector[0] == 0x5a);
    soc.SdInsert();
    assert(soc.SdInserted() && !(read32(0x40e00000) & (1u << 14)));
    soc.SdEject();
    sdCardReset();
    puts("PASS: TX cold reset preserves inserted/ejected card detect and stored sectors");
    puts("PASS: TX EP0 partial FIFO restore, packet readiness and cold reset through the real SoC");
    puts("PASS: E2 timer and version-zero LCD/power sessions");
    puts("PASS: TX peripheral/WMMX reload, truncated-state rollback and reset without RAM erasure");
}
