// Firmware-free TG50 wiring, retention, state rollback and callback lifetime checks.
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include "CPU.h"
#include "audio_queue.h"
#include "mem.h"
#include "pxa_GPIO.h"
#include "mmiodev_TG50uc.h"
#include "soc_PXA.h"

static uint32_t read(ArmMem *mem, uint32_t address, unsigned size = 4) {
    uint32_t value{};
    assert(memAccess(mem, address, size, false, &value));
    return value;
}
static void write(ArmMem *mem, uint32_t address, uint32_t value, unsigned size = 4) {
    assert(memAccess(mem, address, size, true, &value));
}
static std::vector<uint8_t> state(SocPXA &soc) {
    assert(soc.Save());
    auto data = soc.GetSavestate();
    return {static_cast<uint8_t *>(data.data), static_cast<uint8_t *>(data.data) + data.size};
}
static void matrix() {
    auto *mem = memInit();
    std::array<uint32_t, 64> keys{};
    keys[1] = keyIdHard1;
    auto *uc = tg50ucInit(mem, nullptr, -1, keys.data());
    constexpr uint32_t base = 0x08000000;
    assert(read(mem, base + 8) == 0xff00);
    assert(read(mem, base + 0xc) == 4);
    tg50ucSetKeyPressed(uc, keyIdHard1, true);
    assert(read(mem, base + 8) == 0xff00);
    write(mem, base + 0xa8, 0x0200);
    write(mem, base + 0x128, 0x0200);
    write(mem, base + 0x28, 1);
    assert(read(mem, base + 8) == 0xfd00);
    assert(read(mem, base + 0xc8) == 0x0200);
    write(mem, base + 0xc8, 0x0200);
    assert(read(mem, base + 0xc8) == 0);
    write(mem, base + 0x88, 0x0200);
    write(mem, base + 0x28, 0);
    assert(read(mem, base + 8) == 0xff00);
    assert(read(mem, base + 0xc8) == 0x0200);
    write(mem, base + 0x28, 1);
    write(mem, base + 0x210, 0xff, 1);
    assert(read(mem, base + 0x28) == 1);
    assert(read(mem, base + 0x128) == 0x0200);
    assert(read(mem, base + 0xc8) == 0x0200);
    assert(!(read(mem, base + 8) & 0x0200));
    tg50ucReset(uc);
    assert(read(mem, base + 8) == 0xff00);
    assert(read(mem, base + 0xc8) == 0);
    assert(read(mem, base + 0x28) == 0);
    assert(read(mem, base + 0x128) == 0);
    // Key ID zero is a matrix hole, not a key that presses every unused position.
    tg50ucSetKeyPressed(uc, 0, true);
    write(mem, base + 0x28, 2);
    assert((read(mem, base + 8) & 0xff00) == 0xff00);
    assert(!memRegionRemove(mem, base, 0x220, &keys));
    assert(memRegionRemove(mem, base, 0x220, uc));
    assert(!memRegionRemove(mem, base, 0x220, uc));
    tg50ucDestroy(uc);
    uint32_t value{};
    assert(!memAccess(mem, base, 4, false, &value));
}
static void transportOutputs() {
    auto* mem=memInit();std::array<uint32_t,64> keys{};
    auto* uc=tg50ucInit(mem,nullptr,-1,keys.data());
    const uint16_t masks[4]={0x400,0x18,0,0};unsigned events=0;
    tg50ucSetOutputObserver(uc,masks,[](void* p){++*static_cast<unsigned*>(p);},&events);
    write(mem,0x08000040,0x400);assert(events==0);
    write(mem,0x08000028,1);write(mem,0x08000048,1);assert(events==0);
    write(mem,0x08000020,0x400);assert(events==1 && tg50ucGpioIsOutput(uc,0,10));
    write(mem,0x08000040,0x400);assert(events==1);
    write(mem,0x08000020,0);assert(events==2 && !tg50ucGpioIsOutput(uc,0,10));
    write(mem,0x08000024,0x18);assert(events==3);
    write(mem,0x08000044,0x10);assert(events==4);
    tg50ucReset(uc);assert(events==5);tg50ucReset(uc);assert(events==5);
    assert(!tg50ucGpioIsOutput(nullptr,0,0) && !tg50ucGpioIsOutput(uc,4,0));
    tg50ucDestroy(uc);
}
static uint8_t codecRead(ArmMem *mem, uint8_t reg) {
    constexpr uint32_t base = 0x40301680;
    write(mem, base + 8, 0x20);
    write(mem, base + 16, 0x49);
    write(mem, base + 8, reg);
    write(mem, base + 16, 0x48);
    write(mem, base + 8, 0x21);
    write(mem, base + 16, 0x49);
    write(mem, base + 16, 0x4a);
    return read(mem, base + 8);
}
static void codecWrite(ArmMem *mem, uint8_t reg, uint8_t value) {
    constexpr uint32_t base = 0x40301680;
    write(mem, base + 8, 0x20);
    write(mem, base + 16, 0x49);
    write(mem, base + 8, reg);
    write(mem, base + 16, 0x48);
    write(mem, base + 8, value);
    write(mem, base + 16, 0x4a);
}
static void audioBoard() {
    std::vector<uint8_t> rom(4096);
    const uint32_t branch = 0xeafffffe;
    memcpy(rom.data(), &branch, 4);
    SocPXA soc(deviceTypeTG50, 16u << 20, rom.data(), rom.size(), nullptr, 0, -1, 0);
    auto *mem = cpuGetMem(soc.GetCpu());
    write(mem, 0xa0000000, 0xeafffffe);
    cpuSetReg(soc.GetCpu(), 15, 0xa0000000);
    write(mem, 0x40e0000c, 1u << 16);
    write(mem, 0x40e00058, 2);
    write(mem, 0x40b00000, 0x7f);
    write(mem, 0x40b00004, 144);
    write(mem, 0x40b00008, 288);
    auto *queue = audioQueueCreate(8192);
    soc.SetAudioQueue(queue);
    auto run = [&](SocPXA &owner) {
        audioQueueClear(queue);
        owner.Run(100000, 1000000);
        std::vector<uint32_t> frames(audioQueuePendingSamples(queue));
        assert(!frames.empty());
        assert(audioQueuePopChunk(queue,frames.data(),frames.size())==frames.size());
        return frames;
    };
    run(soc);
    // Active serial PCM continues unchanged while the external panel is dark.
    auto beforePanelAudio = state(soc);
    codecWrite(mem, 0, 0x80); codecWrite(mem, 1, 0x61);
    codecWrite(mem, 4, 0x6a); codecWrite(mem, 6, 0x10); codecWrite(mem, 5, 0x20);
    write(mem, 0x40400004, 8); write(mem, 0x40400000, 0x7701);
    write(mem, 0x40400080, 0xf83003e8);
    write(mem, 0x08000024, 1u << 9); write(mem, 0x08000044, 1u << 9);
    write(mem, 0x44000000, 1);
    auto panelState = state(soc);
    const auto visibleAudio = run(soc);
    assert(soc.Load(panelState.size(), panelState.data()));
    assert(soc.SetHoldSwitch(true));
    write(mem, 0x08000064, 1u << 9);
    assert(!soc.LcdEnabled() && !soc.PowerSleeping());
    assert(run(soc) == visibleAudio);
    assert(read(mem, 0x40400000) & 1);
    assert(read(mem, 0x44000000) & 1);
    assert(soc.Load(beforePanelAudio.size(), beforePanelAudio.data()));
    auto saved = state(soc);
    const auto expected = run(soc);
    bool nonzero{}; for(auto frame:expected) nonzero |= bool(frame); assert(nonzero);
    auto generation = audioQueueGetState(queue).generation;
    assert(soc.Load(saved.size(),saved.data()));
    assert(audioQueueGetState(queue).generation>generation && !audioQueuePendingSamples(queue));
    assert(run(soc)==expected);
    SocPXA fresh(deviceTypeTG50, 16u << 20, rom.data(), rom.size(), nullptr, 0, -1, 0);
    auto *freshMem = cpuGetMem(fresh.GetCpu());
    write(freshMem, 0xa0000000, 0xeafffffe);
    assert(fresh.Load(saved.size(),saved.data()));
    fresh.SetAudioQueue(queue);
    assert(run(fresh)==expected);
    // AF2 alone cannot route an input pin into the host speaker.
    write(freshMem, 0x40e0000c, 0);
    for(auto frame:run(fresh)) assert(!frame);
    write(freshMem, 0x40e0000c, 1u << 16);
    write(freshMem, 0x41300004, read(freshMem,0x41300004) & ~1u);
    for(auto frame:run(fresh)) assert(!frame);
    // Sleep exit resets PWM registers while retaining the external board and codec.
    write(freshMem, 0x41300004, read(freshMem,0x41300004) | 1u);
    write(freshMem, 0x40b00004, 144);
    write(freshMem, 0x40b00008, 288);
    codecWrite(freshMem, 7, 0x17);
    write(freshMem, 0x08000028, 1);
    write(freshMem, 0x080000a8, 0x100);
    write(freshMem, 0x08000128, 0x100);
    write(freshMem, 0x40f0000c, 1u << 14);
    write(freshMem, 0x40f00014, 1u << 14);
    write(freshMem, 0xa0000000, 0xee070e10);
    cpuInvalidateCodeCache(fresh.GetCpu());
    cpuSetReg(fresh.GetCpu(), ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC | ARM_SR_I | ARM_SR_F);
    cpuSetReg(fresh.GetCpu(), 15, 0xa0000000);
    cpuSetReg(fresh.GetCpu(), 0, 3);
    cpuCyclePure(fresh.GetCpu());
    assert(fresh.PowerSleeping());
    fresh.Run(100000,1000000);
    const auto beforeWakeGeneration = audioQueueGetState(queue).generation;
    fresh.KeyDown(keyIdPower);
    fresh.Run(100000,1000000);
    assert(!fresh.PowerSleeping());
    assert(audioQueueGetState(queue).generation > beforeWakeGeneration);
    assert(read(freshMem,0x40b00004)==0 && read(freshMem,0x40b00008)==0);
    assert(codecRead(freshMem, 7)==0x17);
    auto resumed = state(fresh);
    assert(fresh.Load(resumed.size(),resumed.data()));
    for(auto frame:run(fresh)) assert(!frame);
    fresh.SetAudioQueue(nullptr); soc.SetAudioQueue(nullptr);
}
class ControlSoc : public SocPXA {
 public:
    using SocPXA::SocPXA;
    using SocGeneric<SocPXA>::PumpEventQueues;
    void key(KeyId key, bool down) {
        if (down) KeyDown(key); else KeyUp(key);
        PumpEventQueues();
    }
};
static void controls() {
    std::vector<uint8_t> rom(0xc70000);
    ControlSoc soc(deviceTypeTG50, 16u << 20, rom.data(), rom.size(), nullptr, 0, -1, 0);
    auto *mem = cpuGetMem(soc.GetCpu());
    constexpr uint32_t uc = 0x08000000, gpio = 0x40e00000, jog = 1u << 21;
    auto scan = [&](KeyId key, unsigned row, unsigned sense) {
        write(mem, uc + 0x28, 0);
        soc.key(key, true);
        assert((read(mem, uc + 8) & 0xff00) == 0xff00);
        for (unsigned driven = 0; driven < 8; ++driven) {
            write(mem, uc + 0x28, 1u << driven);
            assert((read(mem, uc + 8) & 0xff00) ==
                   (driven == row ? (0xff00u ^ (1u << (sense + 8))) : 0xff00u));
        }
        write(mem, uc + 0x28, 1u << row);
        soc.key(key, true);
        assert(!(read(mem, uc + 8) & (1u << (sense + 8))));
        soc.key(key, false);
        assert((read(mem, uc + 8) & 0xff00) == 0xff00);
    };
    const KeyId hard[16] = {keyIdPower,keyIdHard1,keyIdHard2,keyInvalid,
        keyIdHard3,keyInvalid,keyIdHard4,keyIdVoice,keyIdUp,keyIdDown,
        keyIdHome,keyIdBack,keyInvalid,keyIdJogPress,keyIdGraffiti,keyInvalid};
    for (unsigned i = 0; i < 16; ++i) if (hard[i]) scan(hard[i], i / 8, i % 8);
    for (unsigned row = 2; row < 8; ++row)
        for (unsigned column = 0; column < 7; ++column)
            scan(tg50Key(row, column), row, column < 2 ? 1 - column : column);
    // Shared sense lines preserve the UC's high-drive precedence.
    soc.key(tg50Key(2, 2), true); soc.key(tg50Key(3, 2), true);
    write(mem, uc + 0x28, 12);
    assert(!(read(mem, uc + 8) & 0x400));
    write(mem, uc + 0x48, 8);
    assert(read(mem, uc + 8) & 0x400);
    write(mem, uc + 0x68, 8);
    soc.key(tg50Key(2, 2), false);
    assert(!(read(mem, uc + 8) & 0x400));
    soc.key(tg50Key(3, 2), false);
    assert(read(mem, uc + 8) & 0x400);
    soc.key(keyIdPower, true); assert(read(mem, gpio) & 1);
    soc.key(keyIdPower, false); assert(read(mem, gpio) & 1);
    write(mem, uc + 0xa0, 16); write(mem, uc + 0x120, 16);
    soc.key(keyIdHold, true);
    assert(!(read(mem, uc) & 16) && (read(mem, uc + 0xc0) & 16));
    assert(!(read(mem, gpio) & (1u << 13)) && (read(mem, gpio) & (1u << 14)));
    // The external lock switch does not suppress keyboard input in the host.
    soc.key(tg50Key(2, 1), true); write(mem, uc + 0x28, 4);
    assert(!(read(mem, uc + 8) & 0x100));
    soc.key(tg50Key(2, 1), false);
    soc.key(keyIdHold, false); assert(read(mem, uc) & 16);
    write(mem, gpio + 0x3c, jog); write(mem, gpio + 0x48, 0xffffffff);
    soc.key(keyIdJogUp, true);
    assert(!(read(mem, gpio) & jog) && (read(mem, gpio) & (1u << 22)));
    assert(read(mem, gpio + 0x48) & jog);
    auto held = state(soc);
    write(mem, gpio + 0x48, jog); soc.key(keyIdJogUp, true);
    assert(!(read(mem, gpio + 0x48) & jog));
    auto malformed = held; malformed.resize(malformed.size() - 4);
    assert(!soc.Load(malformed.size(), malformed.data()));
    soc.key(keyIdJogUp, true); assert(!(read(mem, gpio + 0x48) & jog));
    soc.key(keyIdJogUp, false); assert(read(mem, gpio) & jog);
    soc.key(keyIdJogUp, true); assert(read(mem, gpio + 0x48) & jog);
    soc.key(keyIdJogDown, true);
    assert(!(read(mem, gpio) & (1u << 22)) && (read(mem, gpio + 0x48) & jog));
    soc.key(keyIdJogDown, false); soc.key(keyIdJogUp, false);
    assert(read(mem, gpio) & jog);
    assert(soc.Load(held.size(), held.data()));
    assert(read(mem, gpio + 0x48) & jog);
    write(mem, gpio + 0x48, jog); soc.key(keyIdJogUp, true);
    assert(read(mem, gpio + 0x48) & jog);
    soc.Reset(); assert(read(mem, gpio) & jog);
    write(mem, gpio + 0x3c, jog); soc.key(keyIdJogUp, true);
    assert(read(mem, gpio + 0x48) & jog);
    puts("TG50 42-key matrix, hard controls, hold, jog edges/coalescing and rollback passed");
}
// Insert after ControlSoc/controls in tests/tg50-state.cpp; call from main.
static std::vector<uint32_t> modelChunk(const std::vector<uint8_t>& bytes, uint32_t kind) {
    const auto *words = reinterpret_cast<const uint32_t*>(bytes.data());
    size_t offset = 1 + 2 * words[0];
    for (unsigned i = 0; i < words[0]; ++i) {
        const auto size = words[2 + 2*i];
        if (words[1 + 2*i] == kind) return {words + offset, words + offset + size};
        offset += size;
    }
    assert(false); return {};
}
static void holdAndPanel() {
    std::vector<uint8_t> rom(0xc70000);
    ControlSoc soc(deviceTypeTG50, 16u << 20, rom.data(), rom.size(), nullptr, 0, -1, 0);
    auto *mem = cpuGetMem(soc.GetCpu());
    constexpr uint32_t uc = 0x08000000, gpio = 0x40e00000;
    constexpr uint32_t io = 1u << 13, keys = 1u << 14;
    assert(soc.HoldSwitchSupported() && !soc.HoldSwitchOn());
    write(mem, gpio + 0x3c, io | keys);
    write(mem, uc + 0xa0, 16); write(mem, uc + 0x120, 16);
    write(mem, uc + 0x28, 4);
    write(mem, uc + 0xa8, 0x100); write(mem, uc + 0x128, 0x100);
    assert(soc.SetHoldSwitch(true) && soc.HoldSwitchOn());
    assert(!(read(mem, gpio) & io) && (read(mem, gpio) & keys));
    assert((read(mem, gpio + 0x48) & (io | keys)) == io);
    soc.key(tg50Key(2, 1), true);
    assert(!(read(mem, gpio) & (io | keys)));
    auto pending = state(soc);
    write(mem, uc + 0xc0, 16);
    assert((read(mem, gpio) & io) && !(read(mem, gpio) & keys));
    write(mem, uc + 0xc8, 0x100);
    assert((read(mem, gpio) & (io | keys)) == (io | keys));
    // A late failed load retains the physical switch and both independent pending IRQs.
    assert(soc.Load(pending.size(), pending.data()));
    const auto live = state(soc);
    auto malformed = pending; malformed.resize(malformed.size() - 4);
    assert(!soc.Load(malformed.size(), malformed.data()));
    assert(state(soc) == live && soc.HoldSwitchOn());
    ControlSoc fresh(deviceTypeTG50, 16u << 20, rom.data(), rom.size(), nullptr, 0, -1, 0);
    assert(fresh.Load(pending.size(), pending.data()) && fresh.HoldSwitchOn());
    auto *other = cpuGetMem(fresh.GetCpu());
    assert(!(read(other, gpio) & (io | keys)));
    assert(fresh.SetHoldSwitch(false) && !fresh.HoldSwitchOn());
    write(other, uc + 0xc0, 16); write(other, uc + 0xc8, 0x100);
    assert((read(other, gpio) & (io | keys)) == (io | keys));
    // The physical panel does not alter the PXA controller's programmed DMA state.
    write(other, uc + 0x24, (1u << 9) | (1u << 11));
    write(other, uc + 0x44, (1u << 9) | (1u << 11));
    write(other, 0x44000000, 1);
    assert(fresh.LcdEnabled());
    const auto controller = modelChunk(state(fresh), 0x80);
    write(other, uc + 0x64, 1u << 9);
    assert(!fresh.LcdEnabled() && !fresh.PowerSleeping());
    assert(read(other, 0x44000000) & 1);
    assert(modelChunk(state(fresh), 0x80) == controller);
    write(other, uc + 0x44, 1u << 9);
    write(other, uc + 0x64, 1u << 11);
    assert(fresh.LcdEnabled()); // Backlight-off is distinct from panel power.
    assert(fresh.SetHoldSwitch(true));
    fresh.Reset();
    assert(fresh.HoldSwitchOn());
    // An independent port B source shares only the I/O IRQ with A.
    auto *bus = memInit();
    auto *ic = pxaIcInit(fresh.GetCpu(), bus, &fresh, 0);
    auto *pins = pxaGpioInit(bus, ic, 0);
    std::array<uint32_t,64> map{};
    auto *ioController = tg50ucInit(bus, pins, 14, map.data(), 13);
    tg50ucGpioSetInState(ioController, 1, 6, true);
    write(bus, uc + 0xa4, 1u << 6); write(bus, uc + 0x124, 1u << 6);
    tg50ucGpioSetInState(ioController, 1, 6, false);
    assert(!(read(bus, gpio) & io) && (read(bus, gpio) & keys));
    assert(read(bus, uc + 0xc4) & (1u << 6));
    write(bus, uc + 0xc4, 1u << 6);
    assert((read(bus, gpio) & (io | keys)) == (io | keys));
    write(bus, uc + 0x84, 1u << 6); write(bus, uc + 0x124, 0);
    tg50ucGpioSetInState(ioController, 1, 6, true);
    assert(read(bus, uc + 0xc4) & (1u << 6));
    assert((read(bus, gpio) & (io | keys)) == (io | keys));
    write(bus, uc + 0x124, 1u << 6);
    assert(!(read(bus, gpio) & io) && (read(bus, gpio) & keys));
    tg50ucDestroy(ioController);
    memDeinit(bus);
    puts("TG50 independent UC IRQs, persistent Hold and external panel power passed");
}

int main() {
    controls();
    holdAndPanel();
    audioBoard();
    matrix();
    transportOutputs();
    std::vector<uint8_t> rom(4096);
    ArmMem *retainedBus{};
    std::vector<uint8_t> saved, pending;
    {
        SocPXA soc(deviceTypeTG50, 16u << 20, rom.data(), rom.size(), nullptr, 0, -1, 0);
        auto *mem = cpuGetMem(soc.GetCpu());
        retainedBus = mem;
        assert(soc.GetRamSize() == (16u << 20));
        assert(soc.GetMemoryData().size == (17u << 20) + 65536);
        assert(soc.GetDeviceType() == deviceTypeTG50);
        assert(codecRead(mem, 1) == 0x80);
        assert(codecRead(mem, 3) == 3);
        codecWrite(mem, 7, 0x55);
        assert(codecRead(mem, 7) == 0x15);
        assert(read(mem, 0x08000100) == 0x4741);
        assert(read(mem, 0x14000008, 2) == 0x4455);
        write(mem, 0x14000008, 0xffff, 2);
        assert(read(mem, 0x14000008, 2) == (0x4455 & 0x8a00) + 0x75ff);
        write(mem, 0xa0000010, 0x12345678);
        assert(read(mem, 0xa1100010) == 0x12345678);
        write(mem, 0xa1000010, 0x87654321);
        assert(read(mem, 0xa2100010) == 0x87654321);
        write(mem, 0xac000000, 0x31415926);
        write(mem, 0xac0003fc, 0x27182818);
        write(mem, 0x40900004, 1234567);
        soc.Reset();
        assert(read(mem, 0x40f00030) == 8);
        assert(read(mem, 0x40900004) == 1234567);
        assert(read(mem, 0xac000000) == 0x31415926);
        assert(read(mem, 0xac0003fc) == 0x27182818);
        assert(read(mem, 0xa1000010) == 0x87654321);
        codecWrite(mem, 7, 0x37);
        // Save half-written I2C transactions, including each slave's address latch.
        for (unsigned address : {0x20u, 0xe6u}) {
            write(mem, 0x40301688, address);
            write(mem, 0x40301690, 0x49);
            write(mem, 0x40301688, address == 0x20 ? 7 : 2);
            write(mem, 0x40301690, 0x48);
            auto mid = state(soc);
            write(mem, 0x40301688, 0xff);
            write(mem, 0x40301690, 0x4a);
            assert(soc.Load(mid.size(), mid.data()));
            write(mem, 0x40301688, 0x37);
            write(mem, 0x40301690, 0x4a);
            assert(!(read(mem, 0x40301698) & 2));
        }
        assert(codecRead(mem, 7) == 0x17);
        write(mem, 0x14000040, 0x1234, 2);
        write(mem, 0x14000050, 0x5678, 2);
        write(mem, 0x140002c0, 0x9abc, 2);
        write(mem, 0x140002d0, 0x0247, 2);
        saved = state(soc);
        write(mem, 0xac000000, 0xdeadbeef);
        codecWrite(mem, 7, 0x61);
        write(mem, 0x08000028, 0x0001);
        write(mem, 0x080000a8, 0x0200);
        write(mem, 0x08000128, 0x0200);
        write(mem, 0x40e00030, (1u << 14) | (1u << 9));
        write(mem, 0x40e0003c, (1u << 14) | (1u << 9));
        write(mem, 0xa0000000, 0xeafffffe); // Explicit RAM test loop, no synthetic ROM execution.
        cpuInvalidateCodeCache(soc.GetCpu());
        cpuSetReg(soc.GetCpu(), ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC | ARM_SR_I | ARM_SR_F);
        cpuSetReg(soc.GetCpu(), 15, 0xa0000000);
        soc.KeyDown(keyIdHard1);
        soc.PenDown(80, 90);
        soc.Run(100000, 1000000);
        assert(!(read(mem, 0x40e00000) & (1u << 14)));
        assert(!(read(mem, 0x40e00000) & (1u << 9)));
        assert(read(mem, 0x080000c8) == 0x0200);
        write(mem, 0x08000128, 0);
        assert(read(mem, 0x40e00000) & (1u << 14));
        write(mem, 0x08000128, 0x0200);
        assert(!(read(mem, 0x40e00000) & (1u << 14)));
        write(mem, 0x40e00048, 0xffffffff);
        pending = state(soc);
        // A truncated late chunk must put SRAM and an earlier UC mutation back.
        auto malformed = saved;
        malformed.resize(malformed.size() - 4);
        assert(!soc.Load(malformed.size(), malformed.data()));
        assert(read(mem, 0xac000000) == 0xdeadbeef);
        assert(codecRead(mem, 7) == 0x01);
        assert(read(mem, 0x08000028) == 1);
        assert(!(read(mem, 0x08000008) & 0x0200));
        assert(!(read(mem, 0x40e00000) & ((1u << 14) | (1u << 9))));
        assert(read(mem, 0x080000c8) == 0x0200);
        assert(read(mem, 0x40e00048) == 0);
        // Corrupt the final model chunk's version after parsing has succeeded.
        auto corrupt = saved;
        auto *words = reinterpret_cast<uint32_t *>(corrupt.data());
        const uint32_t count = words[0];
        size_t offset = 1 + 2 * count;
        bool changed{};
        for (unsigned i = 0; i < count; ++i) {
            if (words[1 + 2 * i] == 0x01f0) { words[offset] = 99; changed = true; break; }
            offset += words[2 + 2 * i];
        }
        // Audio chunk failures must restore the running CPU, scheduler and every model.
        for (const auto type : {0x00b0u, 0x01d0u, 0x0210u, 0x10210u, 0x20210u}) {
            auto invalidAudio = saved;
            auto *audioWords = reinterpret_cast<uint32_t *>(invalidAudio.data());
            size_t audioOffset = 1 + 2 * audioWords[0];
            bool audioChanged{};
            for (unsigned i = 0; i < audioWords[0]; ++i) {
                if (audioWords[1 + 2*i] == (type & 0xffff)) {
                    if (type == 0x00b0) audioWords[audioOffset + 2] |= 255;
                    else if (type == 0x01d0) audioWords[audioOffset + 23] = 65537;
                    else if (type == 0x0210) audioWords[audioOffset + 1] = 0x80;
                    else if (type == 0x10210) audioWords[audioOffset] = 99;
                    else audioWords[audioOffset + 11] ^= 1;
                    audioChanged = true;
                    break;
                }
                audioOffset += audioWords[2 + 2*i];
            }
            const auto live = state(soc);
            assert(audioChanged && !soc.Load(invalidAudio.size(), invalidAudio.data()));
            assert(state(soc) == live);
        }
        assert(changed && !soc.Load(corrupt.size(), corrupt.data()));
        assert(read(mem, 0xac000000) == 0xdeadbeef);
        assert(codecRead(mem, 7) == 0x01);
        assert(read(mem, 0x08000028) == 1);
        assert(!(read(mem, 0x08000008) & 0x0200));
        assert(!(read(mem, 0x40e00000) & ((1u << 14) | (1u << 9))));
        assert(read(mem, 0x080000c8) == 0x0200);
        assert(read(mem, 0x40e00048) == 0);
        assert(soc.Load(saved.size(), saved.data()));
        assert(read(mem, 0xac000000) == 0x31415926);
        assert(codecRead(mem, 7) == 0x17);
        assert(read(mem, 0x14000040, 2) == 0x1234);
        assert(read(mem, 0x14000050, 2) == 0x5678);
        assert(read(mem, 0x140002c0, 2) == 0x9abc);
        assert(read(mem, 0x140002d0, 2) == 0x0246);
    }
    uint32_t value{};
    for (uint32_t address : {0x08000000u, 0x14000000u, 0xac000000u})
        assert(!memAccess(retainedBus, address, 4, false, &value));
    // A new owning SoC restores the separately serialized SRAM, not old pointers.
    SocPXA fresh(deviceTypeTG50, 16u << 20, rom.data(), rom.size(), nullptr, 0, -1, 0);
    assert(fresh.Load(saved.size(), saved.data()));
    assert(read(cpuGetMem(fresh.GetCpu()), 0xac000000) == 0x31415926);
    assert(codecRead(cpuGetMem(fresh.GetCpu()), 7) == 0x17);
    assert(read(cpuGetMem(fresh.GetCpu()), 0xac0003fc) == 0x27182818);
    assert(fresh.Load(pending.size(), pending.data()));
    auto *mem = cpuGetMem(fresh.GetCpu());
    assert(!(read(mem, 0x40e00000) & (1u << 14)));
    assert(read(mem, 0x40e00000) & (1u << 9));
    assert(read(mem, 0x08000008) & 0x0200);
    assert(read(mem, 0x080000c8) == 0x0200);
    write(mem, 0x080000c8, 0x0200);
    assert(read(mem, 0x40e00000) & (1u << 14));
    write(mem, 0x08000210, 0xff, 1);
    assert(read(mem, 0x40e00000) & (1u << 14));
    puts("TG50 matrix, retention, state rollback and teardown passed");
}
