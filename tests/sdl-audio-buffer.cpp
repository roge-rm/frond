// Exercise the production SDL audio callback without a host audio thread.
#include <cassert>
#include <cstdio>
#include <vector>
#include <SDL.h>
#include "SdlAudioDriver.h"
#include "soc_PXA.h"
#include "audio_queue.h"

static AudioQueue* queue;
static SDL_AudioSpec requested;
static bool closed = false;
static unsigned actualPeriod = 512;
static void push(unsigned count) {
    for (unsigned i = 0; i < count; ++i) audioQueuePush(queue, 0x10001000);
}
extern "C" SDL_AudioDeviceID SDL_OpenAudioDevice(const char*, int, const SDL_AudioSpec* want,
                                                 SDL_AudioSpec* got, int) {
    requested = *want;
    *got = *want;
    got->samples = actualPeriod;
    return 1;
}
extern "C" void SDL_PauseAudioDevice(SDL_AudioDeviceID, int pause) {
    if (!pause) {
        // A callback may run as soon as the device is unpaused.
        push(600);
        std::vector<uint32_t> stream(actualPeriod);
        requested.callback(requested.userdata, reinterpret_cast<uint8_t*>(stream.data()),
                           stream.size() * sizeof(uint32_t));
        for (auto sample : stream) assert(sample == 0);
    }
}
extern "C" void SDL_CloseAudioDevice(SDL_AudioDeviceID) { closed = true; }

int main() {
    std::vector<uint8_t> rom(8 * 1024 * 1024), nand(33 * 1024 * 1024, 0xff);
    SocPXA soc(DeviceType5::deviceTypeE2, 16 * 1024 * 1024, rom.data(), rom.size(),
               nand.data(), nand.size(), -1, 0);
    queue = audioQueueCreate(16384);
    soc.SetAudioQueue(queue);
    {
        SdlAudioDriver driver(&soc, queue, true);
        driver.Start();
        assert(soc.PcmOutputEnabled());
        assert(driver.GetUnderrunCount() == 0);
        assert(audioQueuePendingSamples(queue) == 600);
        uint32_t stream[512];
        auto callback = [&]() {
            driver.AudioCallback(reinterpret_cast<uint8_t*>(stream), sizeof(stream));
        };
        push(1000);
        callback();
        for (auto sample : stream) assert(sample == 0x10001000);
        // Backpressure stays on between the high and low water marks.
        push(3500);
        callback();
        assert(driver.GetAudioBackpressure());
        while (audioQueuePendingSamples(queue) >= 2560) {
            callback();
            assert(driver.GetAudioBackpressure());
        }
        callback();
        assert(!driver.GetAudioBackpressure());
        while (audioQueuePendingSamples(queue) >= 512) callback();
        callback();
        for (auto sample : stream) assert(sample == 0);
        assert(driver.GetUnderrunCount() == 1);
        // Refill before restarting after an underrun.
        audioQueueClear(queue);
        push(600);
        callback();
        for (auto sample : stream) assert(sample == 0);
        assert(driver.GetUnderrunCount() == 1);
        push(1000);
        callback();
        for (auto sample : stream) assert(sample == 0x10001000);
        driver.Pause();
        assert(!soc.PcmOutputEnabled());
        driver.Start();
        assert(soc.PcmOutputEnabled());
        assert(audioQueuePendingSamples(queue) == 600);
        driver.Pause();
        assert(!soc.PcmOutputEnabled());
    }
    queue = audioQueueCreate(7350);
    soc.SetAudioQueue(queue);
    actualPeriod = 1024;
    {
        SdlAudioDriver driver(&soc, queue);
        driver.Start();
        assert(soc.PcmOutputEnabled());
        push(7000);
        assert(audioQueuePendingSamples(queue) == 7600);
        std::vector<uint32_t> stream(actualPeriod);
        driver.AudioCallback(reinterpret_cast<uint8_t*>(stream.data()), stream.size() * 4);
        for (auto sample : stream) assert(sample == 0x10001000);
        driver.Pause();
        assert(!soc.PcmOutputEnabled());
    }
    audioQueueClear(queue);
    audioQueuePush(queue, 1);
    audioQueuePush(queue, 2);
    audioQueueEnsureCapacity(queue, 20000);
    uint32_t saved[2];
    assert(audioQueuePopChunk(queue, saved, 2) == 2 && saved[0] == 1 && saved[1] == 2);
    assert(closed);
    audioQueueClear(queue);
    actualPeriod = 512;
    {
        SdlAudioDriver driver(&soc, queue);
        driver.Start();
        assert(soc.PcmOutputEnabled());
        uint32_t stream[512];
        auto callback = [&]() {
            driver.AudioCallback(reinterpret_cast<uint8_t*>(stream), sizeof(stream));
        };
        push(2000);
        callback();
        for (auto sample : stream) assert(sample == 0);
        push(600);
        callback();
        for (auto sample : stream) assert(sample == 0x10001000);
        push(1600);
        callback();
        assert(driver.GetAudioBackpressure());
        callback();
        assert(driver.GetAudioBackpressure());
        callback();
        assert(driver.GetAudioBackpressure());
        callback();
        assert(!driver.GetAudioBackpressure());
        audioQueueClear(queue);
        callback();
        assert(driver.GetUnderrunCount() == 1);
        push(1600);
        callback();
        for (auto sample : stream) assert(sample == 0x10001000);
        driver.Pause();
        assert(!soc.PcmOutputEnabled());
    }
    puts("PASS: SDL startup, bounded audio buffering, backpressure hysteresis and underrun recovery");
}
