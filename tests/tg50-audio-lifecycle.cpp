// SDL output eligibility, intentional mute consumption and pause on an actual TG50 SoC.
#include "tg50-audio-fixture.h"
#include "audio_queue.h"
#include "SdlAudioDriver.h"
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
        failed.Start(); assert(!failed.Running() && !audioQueuePreviewAllowed(queue));
        assert(!m.soc.PcmOutputEnabled());
    }
    failOpen=false;
    {
        SdlAudioDriver driver(&m.soc,queue); driver.Start();
        assert(driver.Running() && audioQueuePreviewAllowed(queue));
        for (unsigned i=0;i<128;++i) audioQueuePush(queue,0x10001000);
        const auto generation=audioQueueGetState(queue).generation;
        driver.Start(); assert(audioQueueGetState(queue).generation==generation);
        assert(audioQueuePendingSamples(queue)==128);
        driver.SetMuted(true); assert(!audioQueuePreviewAllowed(queue));
        uint32_t stream[16]; driver.AudioCallback(reinterpret_cast<uint8_t*>(stream),sizeof(stream));
        for (auto value:stream) assert(!value);
        assert(audioQueuePendingSamples(queue)==112 && audioQueueAccounting(queue).consumed==16);
        driver.SetMuted(false); assert(audioQueuePreviewAllowed(queue));
        driver.Pause(); assert(!audioQueuePreviewAllowed(queue) && !m.soc.PcmOutputEnabled());
        assert(!audioQueuePendingSamples(queue) && audioQueueAccounting(queue).discarded==112);
        driver.Start(); assert(audioQueuePreviewAllowed(queue));
        const auto hiddenTime=m.soc.GetTime();
        for(unsigned i=0;i<3;++i)audioQueuePush(queue,0x20002000);
        const auto beforeHidden=audioQueueAccounting(queue);
        driver.SetHeadless(true); assert(!audioQueuePreviewAllowed(queue));
        const auto hiddenGeneration=audioQueueGetState(queue).generation;
        assert(audioQueueAccounting(queue).discarded-beforeHidden.discarded==3);
        assert(m.soc.PcmOutputEnabled());
        m.run(); assert(m.soc.GetTime()>hiddenTime && !audioQueuePendingSamples(queue));
        driver.SetMuted(false); driver.Start(); assert(!audioQueuePreviewAllowed(queue));
        m.run(); assert(!audioQueuePendingSamples(queue));
        assert(audioQueueGetState(queue).generation==hiddenGeneration);
        assert(audioQueueAccounting(queue).committed==beforeHidden.committed);
        driver.SetMuted(true); driver.SetHeadless(false); assert(!audioQueuePreviewAllowed(queue));
        driver.SetMuted(false); assert(audioQueuePreviewAllowed(queue));
        m.run(); assert(audioQueuePendingSamples(queue)>0);
        driver.Abandon(); assert(!audioQueuePreviewAllowed(queue));
    }
    assert(!closed); // Abandon leaves the device to its platform owner.
}
