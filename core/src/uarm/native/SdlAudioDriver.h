#ifndef _SDL_AUDIO_DRIVER_
#define _SDL_AUDIO_DRIVER_

#include <SDL.h>

#include <atomic>
#include <functional>

class SoC;
struct AudioQueue;

class SdlAudioDriver {
   public:
    SdlAudioDriver(SoC* soc, AudioQueue* audioQueue, bool lowLatencyStartup = false,
                   uint16_t periodFrames = 512);
    ~SdlAudioDriver();

    void Start();
    void Pause();
    bool GetAudioBackpressure() const;

    // Plays silence while still draining the queue at the device's pace, so guest audio timing,
    // backpressure and the low-latency path behave the same as when playing.
    void SetMuted(bool value);
    void SetHeadless(bool);
    bool Muted() const { return muted.load(std::memory_order_relaxed); }
    bool Running() const { return initialized; }
    uint64_t GetUnderrunCount() const;

    // Leaves the device open at destruction. SDL's Android pause holds the device lock on the
    // main thread until resume, so closing it while in the background never returns.
    void Abandon();

    void AudioCallback(uint8_t* stream, int len);

    // Mixes more sound into each period on the callback thread, after the guest's: (stereo frames, interleaved;
    // the period's frame count). It runs every period, with guest audio or silence, muted and headless too (its
    // sound is dropped then). Set it before Start; ClearMixer takes it out while the device runs.
    void SetMixer(std::function<void(int16_t*, size_t)> mix) {
        mixer = std::move(mix);
        mixerOn.store(bool(mixer));
    }
    // Waits for a callback inside the mixer to leave it, without the device's lock, which Android's background
    // pause holds.
    void ClearMixer();

    // Runs the SDL device calls through run, which runs them on the SDL thread and waits.
    // Android's background pause holds the device lock on that thread until resume.
    void SetDeviceThread(std::function<void(const std::function<void()>&)> run) { deviceThread = std::move(run); }

   private:
    void OnDeviceThread(const std::function<void()>& work) { deviceThread ? deviceThread(work) : work(); }

    SoC* soc;
    std::function<void(const std::function<void()>&)> deviceThread;
    std::function<void(int16_t*, size_t)> mixer;
    AudioQueue* audioQueue;
    SDL_AudioDeviceID audioDevice{0};

    bool initialized{false};
    bool headless{};

    std::atomic<bool> audioBuffering{true};
    std::atomic<bool> audioBackpressure{false};
    std::atomic<bool> muted{false};
    // The callback runs the mixer only while mixerOn, and says so in inMixer (both sequentially consistent, so
    // ClearMixer sees it inside or it sees mixerOn off).
    std::atomic<bool> mixerOn{false}, inMixer{false};
    std::atomic<bool> headlessOut{false};  // headless, for the callback
    std::atomic<uint64_t> underrunCount{0};
    std::atomic<bool> starting{true};
    bool lowLatencyStartup;
    uint16_t periodFrames;
    uint64_t queueGeneration{0};  // owned by the SDL callback thread

    uint32_t bufferThresholdStart{0};
    uint32_t bufferThresholdStop{0};
    uint32_t bufferThresholdStartup{0};
    uint32_t backpressureThresholdStart{0};
    uint32_t backpressureThresholdStop{0};
};

#endif  //_SDL_AUDIO_DRIVER_
