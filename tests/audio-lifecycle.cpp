// Firmware-free audio lifecycle checks with the production SDL callback and SoC.
#include <algorithm>
#include <atomic>
#include <thread>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <SDL.h>
#include "MainLoop.h"
#include "SdlAudioDriver.h"
#include "audio_queue.h"
#include "mem.h"
#include "soc_PXA.h"

static uint64_t wallUsec = 1000000;
extern "C" uint64_t timestampUsec() { return wallUsec; }
extern "C" void uarmAbort() { std::abort(); }
extern "C" SDL_AudioDeviceID SDL_OpenAudioDevice(const char*, int, const SDL_AudioSpec* want,
                                                 SDL_AudioSpec* got, int) {
    *got = *want;
    return 1;
}
extern "C" void SDL_PauseAudioDevice(SDL_AudioDeviceID, int) {}
extern "C" void SDL_CloseAudioDevice(SDL_AudioDeviceID) {}

struct Machine {
    std::vector<uint8_t> rom = std::vector<uint8_t>(16 * 1024 * 1024);
    std::vector<uint8_t> nand = std::vector<uint8_t>(132 * 1024 * 1024, 0xff);
    SocPXA soc{deviceTypeTX, 32 * 1024 * 1024, rom.data(), uint32_t(rom.size()),
               nand.data(), nand.size(), -1, 2};
    AudioQueue* queue = audioQueueCreate(16384);
    Machine() {
        soc.SetAudioQueue(queue);
        soc.SetPcmOutputEnabled(true);
        loop();
    }
    void write(uint32_t addr, uint32_t value) {
        assert(memAccess(cpuGetMem(soc.GetCpu()), addr, 4, true, &value));
    }
    uint32_t read(uint32_t addr) {
        uint32_t value;
        assert(memAccess(cpuGetMem(soc.GetCpu()), addr, 4, false, &value));
        return value;
    }
    void loop() {
        write(0xa0000000, 0xeafffffe);
        cpuInvalidateCodeCache(soc.GetCpu());
        cpuSetReg(soc.GetCpu(), ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC | ARM_SR_I | ARM_SR_F);
        cpuSetReg(soc.GetCpu(), 15, 0xa0000000);
    }
    void push(unsigned count, uint32_t value = 0x12341234) {
        for (unsigned i = 0; i < count; ++i) audioQueuePush(queue, value);
    }
    void run(unsigned ms) { assert(soc.Run(ms * 1000, 1000000) > 0); }
    void sleep(unsigned mode = 3) {
        write(0x40f0000c, 1); write(0x40f00010, 0); write(0x40f00014, 1);
        write(0xa0000000, 0xee070e10); // MCR p14,0,r0,c7,c0,0
        cpuInvalidateCodeCache(soc.GetCpu());
        cpuSetReg(soc.GetCpu(), 0, mode);
        cpuSetReg(soc.GetCpu(), 15, 0xa0000000);
        cpuCyclePure(soc.GetCpu());
        assert(soc.PowerSleeping() == (mode == 3));
    }
    std::vector<uint8_t> save() {
        assert(soc.Save()); auto data = soc.GetSavestate();
        return {static_cast<uint8_t*>(data.data), static_cast<uint8_t*>(data.data) + data.size};
    }
};

static void sleepTest() {
    Machine m;
    m.push(4000);
    m.sleep(1); // IRQ idle keeps peripheral clocks and queued PCM live.
    assert(audioQueuePendingSamples(m.queue) == 4000);
    const auto idleTimer = m.read(0x40a00010);
    m.run(10);
    assert(m.read(0x40a00010) != idleTimer && audioQueuePendingSamples(m.queue) >= 4000);
    m.soc.Reset(); m.loop();
    m.push(4000);
    m.sleep();
    // No pre-sleep host PCM may play after the guest stops its audio clock.
    assert(audioQueuePendingSamples(m.queue) == 0);
    auto saved = m.save();
    const auto timer = m.read(0x40a00010), pc = cpuGetRegExternal(m.soc.GetCpu(), 15);
    m.run(1000);
    assert(audioQueuePendingSamples(m.queue) == 0);
    assert(m.read(0x40a00010) == timer && cpuGetRegExternal(m.soc.GetCpu(), 15) == pc);
    assert(m.soc.Load(saved.size(), saved.data()) && m.soc.PowerSleeping());
    m.run(1000);
    assert(audioQueuePendingSamples(m.queue) == 0 && m.read(0x40a00010) == timer);
    m.soc.KeyDown(keyIdPower); m.run(150);
    assert(!m.soc.PowerSleeping());
    std::vector<uint32_t> resumed(audioQueuePendingSamples(m.queue));
    assert(audioQueuePopChunk(m.queue, resumed.data(), resumed.size()) == resumed.size());
    for (auto sample : resumed) assert(sample == 0); // synthetic ROM emits only silence
    puts("PASS: idle retains clocks/PCM; power sleep discards host PCM and stops clocks across restore; wake has no stale PCM");
}

static void stateTest() {
    Machine m;
    auto saved = m.save();
    m.push(4000); m.soc.Reset();
    assert(m.soc.PcmOutputEnabled() && audioQueuePendingSamples(m.queue) == 0);
    m.push(4000); assert(m.soc.Load(saved.size(), saved.data()));
    assert(audioQueuePendingSamples(m.queue) == 0);
    m.write(0x40f00008, 0xcafef00d);
    m.push(4000);
    // A truncated state fails, restores the live guest, and releases speculative PCM.
    assert(!m.soc.Load(saved.size() - 4, saved.data()));
    assert(m.read(0x40f00008) == 0xcafef00d && m.soc.PcmOutputEnabled());
    assert(audioQueuePendingSamples(m.queue) == 0);
    puts("PASS: reset, load and rejected-load rollback discard host PCM and retain live output configuration");
}

static void sdlTest() {
    Machine m;
    SdlAudioDriver driver(&m.soc, m.queue);
    driver.Start();
    uint32_t stream[512];
    auto callback = [&]() { driver.AudioCallback(reinterpret_cast<uint8_t*>(stream), sizeof(stream)); };
    auto silent = [&]() { for (auto sample : stream) assert(sample == 0); };
    auto signal = [&](uint32_t value) { for (auto sample : stream) assert(sample == value); };
    auto saved = m.save();
    m.push(5000); callback(); signal(0x12341234);
    assert(driver.GetAudioBackpressure());
    assert(m.soc.Load(saved.size(), saved.data()));
    m.push(600, 0x56785678); callback();
    // Restored output must acquire its startup reservoir before playback resumes.
    silent();
    assert(!driver.GetAudioBackpressure() && driver.GetUnderrunCount() == 0);
    assert(audioQueuePendingSamples(m.queue) == 600);
    m.push(3000, 0x56785678); callback(); signal(0x56785678);
    m.soc.Reset(); m.push(600); callback(); silent();
    assert(driver.GetUnderrunCount() == 0);
    m.push(3000); callback(); signal(0x12341234);
    assert(!m.soc.Load(saved.size() - 4, saved.data()));
    m.push(600); callback(); silent();
    assert(driver.GetUnderrunCount() == 0 && !driver.GetAudioBackpressure());
    // Explicit pause/start obeys the same startup reservoir, with no stale PCM.
    driver.Pause(); assert(!m.soc.PcmOutputEnabled());
    driver.Start(); assert(audioQueuePendingSamples(m.queue) == 0);
    m.push(600); callback(); silent();
    driver.Pause();
    puts("PASS: SDL re-primes after restore/reset/rejected load without false underruns or stale backpressure; pause/start clears PCM");
}

static void queueTest() {
    Machine m;
    m.push(2);
    const auto before = audioQueueGetState(m.queue);
    audioQueueEnsureCapacity(m.queue, 20000);
    assert(audioQueueGetState(m.queue).generation == before.generation);
    audioQueueClear(m.queue); m.push(2, 0x56785678);
    uint32_t samples[2] = {};
    assert(audioQueuePopChunkIfGeneration(m.queue, samples, 2, before.generation) == 0);
    const auto after = audioQueueGetState(m.queue);
    assert(after.pendingSamples == 2 && after.generation != before.generation);
    assert(audioQueuePopChunkIfGeneration(m.queue, samples, 2, after.generation) == 2);
    assert(samples[0] == 0x56785678 && samples[1] == samples[0]);
    // Exercise replacement between the consumer's snapshot and guarded pop.
    // A producer owns clear/push; the consumer must see only its observed generation.
    std::atomic<bool> done{false};
    std::thread producer([&]() {
        for (unsigned i = 0; i < 1000; ++i) {
            audioQueueClear(m.queue);
            const auto state = audioQueueGetState(m.queue);
            m.push(64, uint32_t(state.generation));
        }
        done.store(true);
    });
    do {
        const auto state = audioQueueGetState(m.queue);
        uint32_t block[64];
        const auto popped = audioQueuePopChunkIfGeneration(m.queue, block,
            std::min<size_t>(64, state.pendingSamples), state.generation);
        for (size_t i = 0; i < popped; ++i) assert(block[i] == uint32_t(state.generation));
    } while (!done.load());
    producer.join();
    puts("PASS: stale snapshots cannot consume replacement PCM; concurrent clear/push/pop preserve stream generations");
}

static void fastTest() {
    Machine m;
    auto cpu = m.soc.GetCpu();
    const uint32_t entry=0xa0012000, table=0xa0010000, globals=0xa0011004;
    const uint32_t object=0xa0016000, pcm=0xa0018000;
    m.write(globals-4,table); m.write(globals,globals); m.write(table+0x320,entry);
    m.write(entry,0xe92d4200); m.write(entry+4,0xe5999000); m.write(entry+8,0xe5999000);
    m.write(entry+32,0xe59fe100); m.write(entry+36,0xe0800100); m.write(entry+40,0xe08ee009);
    m.write(entry+44,0xe08e0100); m.write(entry+48,0xe5900004);
    m.write(entry-0x5f8,0xe4920004); m.write(entry-0x5f4,0xe1a005c0); m.write(entry-0x5f0,0xe250cc7f);
    m.write(entry+40+0x100,0x3ffc); m.write(0xa0015004,object);
    m.write(object,0x12); m.write(object+4,44100); m.write(object+8,2);
    m.write(object+40,1024 | (1024u << 16));
    for (unsigned i=0;i<4096;i+=4) m.write(pcm+i,0xfc1803e8);
    assert(m.soc.EnableFastAudio());
    auto call = [&]() {
        m.write(0xa0000000,0xe519c004); // LDR ip,[r9,#-4]
        m.write(0xa0000004,0xe59cf320); // LDR pc,[ip,#0x320]
        cpuInvalidateCodeCache(cpu);
        cpuSetReg(cpu,ARM_REG_NUM_CPSR,ARM_SR_MODE_SVC | ARM_SR_I | ARM_SR_F);
        cpuSetReg(cpu,0,0); cpuSetReg(cpu,1,pcm); cpuSetReg(cpu,2,4096);
        cpuSetReg(cpu,9,globals); cpuSetReg(cpu,15,0xa0000000);
        cpuCyclePure(cpu); cpuCyclePure(cpu);
        assert(m.soc.FastAudioActive() && audioQueuePendingSamples(m.queue)==1536);
    };
    call();
    // Both preview PCM and bypass state belong to the pre-sleep stream.
    m.sleep();
    assert(!m.soc.FastAudioActive() && audioQueuePendingSamples(m.queue)==0);
    auto saved=m.save();
    assert(m.soc.Load(saved.size(),saved.data()) && m.soc.PowerSleeping());
    assert(!m.soc.FastAudioActive() && audioQueuePendingSamples(m.queue)==0);
    m.soc.KeyDown(keyIdPower); m.run(150);
    assert(!m.soc.PowerSleeping() && !m.soc.FastAudioActive());
    // A fresh qualified HAL call can activate preview again after wake.
    call();
    m.soc.SetPcmOutputEnabled(false);
    assert(audioQueuePendingSamples(m.queue)==0);
    m.soc.SetPcmOutputEnabled(true);
    assert(audioQueuePendingSamples(m.queue)==0);
    call();
    m.soc.Reset();
    assert(!m.soc.FastAudioActive() && audioQueuePendingSamples(m.queue)==0);
    puts("PASS: fast preview sleep/load/reset discard PCM and bypass state; qualified output restarts after wake and audio toggle");
}

static void clocksTest() {
    Machine m;
    MainLoop loop(&m.soc);
    loop.SetCyclesPerSecondLimit(1000000);
    wallUsec += 20000; loop.Cycle(wallUsec);
    const auto time = m.soc.GetTime();
    const auto timer = m.read(0x40a00010);
    const auto pc = cpuGetRegExternal(m.soc.GetCpu(), 15);
    m.push(4000);
    const auto pending = audioQueuePendingSamples(m.queue);
    // This is the native main loop's audio backpressure path, not codec-only suspension.
    for (unsigned i = 0; i < 8; ++i) {
        wallUsec += 100000;
        assert(loop.Cycle(wallUsec, true) == 0);
        assert(m.soc.GetTime() == time && m.read(0x40a00010) == timer);
        assert(cpuGetRegExternal(m.soc.GetCpu(), 15) == pc);
        assert(audioQueuePendingSamples(m.queue) == pending);
    }
    audioQueueClear(m.queue);
    wallUsec += loop.GetTimesliceSizeUsec(); loop.Cycle(wallUsec);
    assert(m.soc.GetTime() > time && m.soc.GetTime() - time <= 17000000);
    const auto ticks = uint32_t(m.read(0x40a00010) - timer);
    assert(ticks > 50000 && ticks < 56000); // one guest slice, no host-stall catch-up
    puts("PASS: whole-guest backpressure freezes CPU, scheduler, timer and PCM; resume advances one bounded guest slice");
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "all";
    if (mode == "all" || mode == "sleep") sleepTest();
    if (mode == "all" || mode == "state") stateTest();
    if (mode == "all" || mode == "sdl") sdlTest();
    if (mode == "all" || mode == "clocks") clocksTest();
    if (mode == "all" || mode == "fast") fastTest();
    if (mode == "all" || mode == "queue") queueTest();
}
