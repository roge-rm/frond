#include "SdlAudioDriver.h"

#include <iostream>
#include <thread>

#include "SDL_audio.h"
#include "SoC.h"
#include "audio_queue.h"

namespace {
    extern "C" void audioCallback(void* userData, uint8_t* stream, int len) {
        reinterpret_cast<SdlAudioDriver*>(userData)->AudioCallback(stream, len);
    }
}  // namespace

SdlAudioDriver::SdlAudioDriver(SoC* soc, AudioQueue* audioQueue, bool lowLatencyStartup,
                               uint16_t periodFrames)
    : soc(soc), audioQueue(audioQueue), lowLatencyStartup(lowLatencyStartup),
      periodFrames(periodFrames) {}

SdlAudioDriver::~SdlAudioDriver() {
    if (!initialized) ClearMixer();
    if (initialized) {
        SDL_CloseAudioDevice(audioDevice);
    }
}

void SdlAudioDriver::Start() {
    if (initialized) {
        soc->SetPcmOutputEnabled(true);
        audioBuffering = true;
        starting = true;
        audioBackpressure = false;
        OnDeviceThread([&] { SDL_PauseAudioDevice(audioDevice, 0); });
        playing = true;

        return;
    }

    SDL_AudioSpec audioSpecRequested = {.freq = 44100,
                                        .format = AUDIO_S16,
                                        .channels = 2,
                                        .silence = 0,
                                        .samples = periodFrames,
                                        .padding = 0,
                                        .size = 0,
                                        .callback = ::audioCallback,
                                        .userdata = this};

    SDL_AudioSpec audioSpecActual;

    OnDeviceThread([&] {
        audioDevice = SDL_OpenAudioDevice(nullptr, 0, &audioSpecRequested, &audioSpecActual,
                                          SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    });
    if (audioDevice == 0) {
        std::cout << "failed to open audio device" << std::endl;
        return;
    } else {
        std::cout << "audio running, period size " << audioSpecActual.samples << std::endl;
    }

    audioQueueEnsureCapacity(audioQueue, size_t(audioSpecActual.samples) * 8 + 44100 / 20);
    soc->SetPcmOutputEnabled(true);
    bufferThresholdStart = audioSpecActual.samples;
    bufferThresholdStop = audioSpecActual.samples * 3;
    bufferThresholdStartup = audioSpecActual.samples * (lowLatencyStartup ? 3 : 6);
    // Normal playback reserves an extra callback for frontend scheduling jitter.
    // The opt-in low-latency path keeps its existing reservoir.
    backpressureThresholdStart = audioSpecActual.samples * (lowLatencyStartup ? 7 : 8);
    backpressureThresholdStop = audioSpecActual.samples * (lowLatencyStartup ? 5 : 6);

    initialized = true;
    playing = true;
    OnDeviceThread([&] { SDL_PauseAudioDevice(audioDevice, 0); });
}

void SdlAudioDriver::Pause() {
    if (!initialized) return;

    soc->SetPcmOutputEnabled(false);
    OnDeviceThread([&] { SDL_PauseAudioDevice(audioDevice, 1); });
    playing = false;
}

bool SdlAudioDriver::GetAudioBackpressure() const {
    return audioBackpressure.load(std::memory_order_relaxed);
}

uint64_t SdlAudioDriver::GetUnderrunCount() const {
    return underrunCount.load(std::memory_order_relaxed);
}

void SdlAudioDriver::AudioCallback(uint8_t* stream, int len) {
    // 100 ms with nothing pushed: the guest's sound has stopped.
    constexpr uint64_t quietFlushFrames = 44100 / 10;
    size_t samplesRemaining = len >> 2;
    const auto queueState = audioQueueGetState(audioQueue);
    size_t samplesPending = queueState.pendingSamples;
    if (queueState.generation != queueGeneration) {
        queueGeneration = queueState.generation;
        audioBuffering = true;
        starting = true;
        audioBackpressure = false;
        drained = false;
    }
    if (queueState.committed != lastCommitted) {
        lastCommitted = queueState.committed;
        quietFrames = 0;
    } else if (quietFrames < quietFlushFrames) {
        quietFrames += samplesRemaining;
    }

    if (audioBuffering && samplesPending > (starting ? bufferThresholdStartup : bufferThresholdStop)) {
        audioBuffering = false;
        starting = false;
    }

    if (!audioBuffering && samplesPending < bufferThresholdStart) {
        audioBuffering = true;
        drained = true;
        drainedCommitted = queueState.committed;
    }
    // Running dry is an underrun when the guest's sound carries on; when it has stopped, it's just the end.
    if (drained && queueState.committed != drainedCommitted) {
        drained = false;
        underrunCount.fetch_add(1, std::memory_order_relaxed);
    }
    // Once the guest's sound has stopped, play what's left: the end of a sound, or a sound shorter than the
    // start threshold, that would otherwise wait for the next one.
    const bool flushing = audioBuffering && samplesPending && quietFrames >= quietFlushFrames;
    if (flushing) drained = false;

    if (!audioBackpressure && samplesPending > backpressureThresholdStart) audioBackpressure = true;
    if (audioBackpressure && samplesPending < backpressureThresholdStop) audioBackpressure = false;

    // While buffering nothing is popped, unless flushing, and the span gives the guest time of the next frame.
    const bool mixing = mixerOn.load();
    AudioQueueSpan span;
    if (!audioBuffering || mixing || flushing) {
        samplesRemaining -=
            audioQueuePopChunkIfGeneration(audioQueue, reinterpret_cast<uint32_t*>(stream),
                                          audioBuffering && !flushing ? 0 : samplesRemaining, queueGeneration,
                                          mixing ? &span : nullptr);
    }

    // The mixer gets the whole period, guest audio or silence, and runs muted and headless too, so its clock keeps
    // to the device's.
    if (samplesRemaining > 0) memset(stream + (len / 4 - samplesRemaining) * 4, 0, samplesRemaining * 4);
    inMixer.store(true);
    if (mixerOn.load()) mixer(reinterpret_cast<int16_t*>(stream), size_t(len) / 4, span);
    inMixer.store(false);
    if (muted.load(std::memory_order_relaxed) || headlessOut.load(std::memory_order_relaxed)) {
        memset(stream, 0, len);
        return;
    }
}

void SdlAudioDriver::ClearMixer() {
    if (!mixer) return;
    mixerOn.store(false);
    while (inMixer.load()) std::this_thread::yield();
    mixer = nullptr;
}

void SdlAudioDriver::SetMuted(bool value) { muted.store(value, std::memory_order_relaxed); }
void SdlAudioDriver::Abandon() {
    // The device stays open, so nothing it calls may outlive this.
    ClearMixer();
    initialized = false;
    playing = false;
}

void SdlAudioDriver::SetHeadless(bool value) {
    if (headless == value) return;
    headless = value;
    soc->SetAudioOutputFenced(value);
    headlessOut.store(value, std::memory_order_relaxed);
    // Frames from before the panel shouldn't play on return.
    if (value) audioQueueClear(audioQueue);
}
