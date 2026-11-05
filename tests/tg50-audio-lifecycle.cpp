// SDL driver lifecycle on an actual TG50 SoC: start, muted draining, pause, and the background
// panel's output fence with its one queue clear.
#include "tg50-audio-fixture.h"
#include "audio_queue.h"
#include "SdlAudioDriver.h"
#include <cstdio>
static bool failOpen{}, closed{};
extern "C" SDL_AudioDeviceID SDL_OpenAudioDevice(const char*, int, const SDL_AudioSpec* requested,
                                                 SDL_AudioSpec* actual, int) {
    *actual = *requested; actual->samples=16; return failOpen ? 0 : 1;
}
extern "C" void SDL_PauseAudioDevice(SDL_AudioDeviceID, int) {}
extern "C" void SDL_CloseAudioDevice(SDL_AudioDeviceID) { closed=true; }
int main() {
    AudioMachine m; m.configure(); auto* queue=audioQueueCreate(256); m.soc.SetAudioQueue(queue);
    {
        failOpen=true; SdlAudioDriver failed(&m.soc,queue);
        failed.Start(); assert(!failed.Running() && !m.soc.PcmOutputEnabled());
    }
    failOpen=false;
    {
        SdlAudioDriver driver(&m.soc,queue); driver.Start();
        assert(driver.Running() && m.soc.PcmOutputEnabled());
        // Muted output still drains the queue at the device's pace.
        for (unsigned i=0;i<128;++i) audioQueuePush(queue,0x10001000);
        driver.SetMuted(true);
        uint32_t stream[16]{}; const auto consumed=audioQueueAccounting(queue).consumed;
        for (unsigned i=0;i<16;++i) driver.AudioCallback(reinterpret_cast<uint8_t*>(stream),sizeof(stream));
        for (auto value:stream) assert(!value);
        assert(audioQueueAccounting(queue).consumed>consumed);
        driver.SetMuted(false);
        // Pause detaches the producer; Start clears and re-primes.
        driver.Pause(); assert(!m.soc.PcmOutputEnabled());
        const auto paused=audioQueueAccounting(queue).committed;
        m.run(); assert(audioQueueAccounting(queue).committed==paused);
        driver.Start(); assert(m.soc.PcmOutputEnabled() && !audioQueuePendingSamples(queue));
        m.run(); assert(audioQueuePendingSamples(queue)>0);

        // The background panel fences the producer and clears the queue once.
        const auto pending=audioQueuePendingSamples(queue);
        const auto beforeHidden=audioQueueAccounting(queue);
        const auto generation=audioQueueGetState(queue).generation;
        const auto hiddenTime=m.soc.GetTime();
        driver.SetHeadless(true);
        assert(!audioQueuePendingSamples(queue) && audioQueueGetState(queue).generation!=generation);
        assert(audioQueueAccounting(queue).discarded-beforeHidden.discarded==pending);
        const auto hiddenGeneration=audioQueueGetState(queue).generation;
        assert(m.soc.PcmOutputEnabled());
        m.run(); assert(m.soc.GetTime()>hiddenTime && !audioQueuePendingSamples(queue));
        // Setting it again doesn't clear again, and nothing is committed while hidden.
        driver.SetHeadless(true); m.run();
        assert(audioQueueGetState(queue).generation==hiddenGeneration);
        assert(audioQueueAccounting(queue).committed==beforeHidden.committed);
        // Reattaching the producer starts from an empty queue.
        driver.SetHeadless(false); assert(!audioQueuePendingSamples(queue));
        m.run(); assert(audioQueuePendingSamples(queue)>0);
        driver.Abandon(); assert(!driver.Running());
    }
    assert(!closed); // Abandon leaves the device to its platform owner.
    std::puts("TG50 audio lifecycle: muted drain, pause, background panel fence and clear PASS");
}
