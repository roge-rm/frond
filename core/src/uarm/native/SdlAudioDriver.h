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

    // Runs the SDL device calls through run, which runs them on the SDL thread and waits.
    // Android's background pause holds the device lock on that thread until resume.
    void SetDeviceThread(std::function<void(const std::function<void()>&)> run) { deviceThread = std::move(run); }

   private:
    void OnDeviceThread(const std::function<void()>& work) { deviceThread ? deviceThread(work) : work(); }

    SoC* soc;
    std::function<void(const std::function<void()>&)> deviceThread;
    AudioQueue* audioQueue;
    SDL_AudioDeviceID audioDevice;

    bool initialized{false};
    bool headless{};

    std::atomic<bool> audioBuffering{true};
    std::atomic<bool> audioBackpressure{false};
    std::atomic<bool> muted{false};
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
