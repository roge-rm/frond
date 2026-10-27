#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "CPU.h"
#include "ReadOnlyGuest.h"
#include "soc_PXA.h"

class InputSoc : public SocPXA {
 public:
    using SocPXA::SocPXA;
    using SocGeneric<SocPXA>::PumpEventQueues;
    int serialDepth = 0;
    bool failSave = false;
    bool Save() override { return !failSave && SocPXA::Save(); }
    void BeginSerialCheckpoint() override { ++serialDepth; SocPXA::BeginSerialCheckpoint(); }
    void EndSerialCheckpoint() override { --serialDepth; SocPXA::EndSerialCheckpoint(); }
    void expectedQueues() {
        assert(penEventQueue->GetSize() == 3 && keyEventQueue->GetSize() == 3);
        auto p = penEventQueue->Pop(); assert(p.penDown && p.x == 21 && p.y == 22);
        p = penEventQueue->Pop(); assert(!p.penDown);
        p = penEventQueue->Pop(); assert(p.penDown && p.x == 31 && p.y == 32);
        auto k = keyEventQueue->Pop(); assert(k.keyDown && k.key == keyIdHard2);
        k = keyEventQueue->Pop(); assert(!k.keyDown && k.key == keyIdHard2);
        k = keyEventQueue->Pop(); assert(k.keyDown && k.key == keyIdHard3);
    }
    void noInput() {
        assert(!penEventQueue->GetSize() && !keyEventQueue->GetSize());
        assert(!penDown && jammedKey == keyInvalid);
    }
    void seedTiming() { eventQueueTicks = 101; eventQueueTicksAtPenDown = 100; }
    void expectedTiming() {
        assert(penDown && eventQueueTicks == 101 && eventQueueTicksAtPenDown == 100);
        assert(jammedKey == keyIdUp && releaseJammedKeyAt == 500000000);
    }
    void customSchedule() {
        scheduler->ScheduleTask(SCHEDULER_TASK_TIMER, 12345, 7);
        scheduler->ScheduleTask(SCHEDULER_TASK_PCM, 23456, 3);
    }
    std::vector<uint8_t> schedule() {
        Savestate<ChunkType> state;
        assert(state.Save(*scheduler));
        const auto* bytes = static_cast<const uint8_t*>(state.GetBuffer());
        // One chunk: count + TOC + version, followed by eight 32-byte tasks.
        std::vector<uint8_t> result;
        for (unsigned task : {SCHEDULER_TASK_TIMER, SCHEDULER_TASK_PCM})
            result.insert(result.end(), bytes + 16 + task * 32, bytes + 16 + task * 32 + 28);
        return result;
    }
};
static uint32_t read(InputSoc& soc, uint32_t address) {
    uint32_t value{};
    assert(cpuMemOpExternal(soc.GetCpu(), &value, address, 4, false)); return value;
}
static void write(InputSoc& soc, uint32_t address, uint32_t value) {
    assert(cpuMemOpExternal(soc.GetCpu(), &value, address, 4, true));
}
static void test(DeviceType5 type) {
    const bool tx = type == deviceTypeTX;
    std::vector<uint8_t> rom(tx ? 16u << 20 : 8u << 20);
    std::vector<uint8_t> nand(tx ? 132u << 20 : 34603008, 0xff);
    InputSoc soc(type, 32u << 20, rom.data(), rom.size(), nand.data(), nand.size(), -1, tx ? 2 : 0);
    write(soc, 0xa0003000, 0xeafffffe);
    cpuSetReg(soc.GetCpu(), 15, 0xa0003000);
    // Configure direct PENDOWN on E2; TX also retains raw ADC coordinates.
    write(soc, 0x40500200 + 0x4c * 2, 0xf836);
    write(soc, 0x40500200 + 0x56 * 2, 0xf836);
    soc.PenDown(11, 12); soc.KeyDown(keyIdHard1); soc.PumpEventQueues();
    soc.JamKey(keyIdUp, 500); soc.seedTiming();
    soc.PenDown(21, 22); soc.PenUp(); soc.PenDown(31, 32);
    soc.KeyDown(keyIdHard2); soc.KeyUp(keyIdHard2); soc.KeyDown(keyIdHard3);
    soc.customSchedule();
    read(soc, 0x40500200 + 0x7a * 2); // AC97 returns the previous read first
    assert(read(soc, 0x40500200 + 0x7a * 2) & 0x8000);
    const auto schedule = soc.schedule();
    const auto gpio = read(soc, 0x40e00000), pen = read(soc, 0x40e00004);
    if (!tx) { assert(!(gpio & (1u << 11))); assert(pen & (1u << 18)); }
    for (bool exception : {false, true}) {
        try {
            ReadOnlyGuest checkpoint(soc);
            assert(soc.networkSimulation.Frozen() && soc.serialDepth == 1);
            soc.PumpEventQueues(); soc.PumpEventQueues(); soc.PumpEventQueues();
            soc.PenUp(); soc.KeyUp(keyIdHard1); soc.PumpEventQueues();
            soc.Run(500, 100000000);
            if (exception) throw std::runtime_error("speculative failure");
        } catch (const std::runtime_error&) { assert(exception); }
        assert(!soc.RestoringCheckpoint() && !soc.networkSimulation.Frozen() && !soc.serialDepth);
        assert(read(soc, 0x40e00000) == gpio && read(soc, 0x40e00004) == pen);
        read(soc, 0x40500200 + 0x7a * 2); // AC97 returns the previous read first
    assert(read(soc, 0x40500200 + 0x7a * 2) & 0x8000);
        soc.expectedTiming(); assert(soc.schedule() == schedule);
        // Inspect order without consuming the original queue snapshot for repeat.
        SoC::InputCheckpoint inputs(soc);
        soc.expectedQueues();
        assert(soc.Save()); auto b = soc.GetSavestate();
        assert(inputs.Restore(b.size, b.data));
    }
    // Full restore uses ordinary stale-input clearing, even after checkpoint rollback.
    assert(soc.Save()); auto b = soc.GetSavestate();
    std::vector<uint8_t> state(static_cast<uint8_t*>(b.data), static_cast<uint8_t*>(b.data) + b.size);
    assert(soc.Load(state.size(), state.data())); soc.noInput();
    if (!tx) {
        assert(read(soc, 0x40e00000) & (1u << 11));
        assert(!(read(soc, 0x40e00004) & (1u << 18)));
    }
    // Commit keeps RAM/CPU/time/input changes and releases all scopes immediately.
    {
        ReadOnlyGuest checkpoint(soc);
        write(soc, 0xa0004000, 0x12345678);
        cpuSetReg(soc.GetCpu(), 0, 456);
        soc.PenDown(21, 22); soc.PenUp(); soc.PenDown(31, 32);
        soc.KeyDown(keyIdHard2); soc.KeyUp(keyIdHard2); soc.KeyDown(keyIdHard3);
        soc.Run(500, 100000000);
        checkpoint.Commit(); checkpoint.Commit();
        assert(!soc.networkSimulation.Frozen() && !soc.serialDepth);
        assert(cpuGetRegExternal(soc.GetCpu(), 0) == 456);
        soc.expectedQueues();
        const auto now = soc.GetTime();
        { ReadOnlyGuest next(soc); write(soc, 0xa0004000, 0x87654321); }
        assert(read(soc, 0xa0004000) == 0x12345678 && soc.GetTime() == now);
    }
    assert(read(soc, 0xa0004000) == 0x12345678);
    // Constructor failure unwinds both freezes without beginning recovery.
    soc.failSave = true;
    try { ReadOnlyGuest failed(soc); assert(false); } catch (const std::runtime_error&) {}
    assert(!soc.networkSimulation.Frozen() && !soc.serialDepth);
    soc.failSave = false;
    { ReadOnlyGuest next(soc); next.Commit(); }
    std::printf("PASS: %s checkpoint input, timing, full restore, commit and scope balance\n", tx ? "TX" : "E2");
}
int main() { test(deviceTypeTX); test(deviceTypeE2); }
