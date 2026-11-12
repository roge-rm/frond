#ifndef FROND_SDL_MICROPHONE_H
#define FROND_SDL_MICROPHONE_H
#include <SDL.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include "audio_input.h"

// Callback frames are mono signed16 at 48 kHz. The emulator never waits for input.
// A fixed 250 ms ring drops oldest frames on overflow. Guest time consumes at its
// declared rate, with zero on underrun; no correction changes the guest clock.
class MicrophoneQueue {
public:
    static constexpr size_t capacity = 12000;
    void Fence(bool value) { std::lock_guard<std::mutex> lock(mutex); fenced = value; }
    void Configure(const AudioInputDemand& d) {
        std::lock_guard<std::mutex> lock(mutex);
        if (fenced) return;
        if (active != d.active || route != d.route || numerator != d.rateNumerator || denominator != d.rateDenominator) {
            Clear(); ++generation; active = d.active; route = d.route; numerator = d.rateNumerator; denominator = d.rateDenominator;
            // 48-tap windowed sinc, cutoff follows the lower Nyquist frequency.
            const double rate = denominator ? double(numerator) / denominator : 0;
            const double cutoff = std::min(0.45, rate / 48000 * 0.45);
            double sum = 0;
            for (size_t i = 0; i < filter.size(); ++i) {
                const double x = double(i) - 23.5;
                filter[i] = std::sin(2 * 3.141592653589793 * cutoff * x) / (3.141592653589793 * x) *
                            (0.54 - 0.46 * std::cos(2 * 3.141592653589793 * i / 47));
                sum += filter[i];
            }
            if (sum) for (auto& c : filter) c /= sum;
        }
    }
    void Reset() {
        std::lock_guard<std::mutex> lock(mutex);
        Clear(); active = false; fenced = false; ++generation;
    }
    void Publish(const int16_t* input, size_t frames, uint64_t expectedGeneration) {
        std::lock_guard<std::mutex> lock(mutex);
        if (fenced || !active || expectedGeneration != generation) return;
        for (size_t i = 0; i < frames; ++i) {
            if (count == capacity) { read = (read + 1) % capacity; --count; ++dropped; }
            samples[(read + count) % capacity] = input[i]; ++count;
        }
    }
    int16_t Pull(AudioInputRoute requestedRoute, uint32_t n, uint32_t d) {
        std::lock_guard<std::mutex> lock(mutex);
        if (fenced || !active || requestedRoute != route || n != numerator || d != denominator || !n || !d) return 0;
        // Linear interpolation follows the anti-alias filter. Phase is in host frames.
        const size_t consumed = size_t(phase);
        if (count < consumed + 2) { Clear(); ++underruns; return 0; }
        for (size_t i = 0; i < consumed; ++i) {
            history[historyAt] = samples[read]; historyAt = (historyAt + 1) % history.size();
            read = (read + 1) % capacity; --count;
        }
        phase -= consumed;
        double current = 0, next = 0;
        for (size_t j = 0; j < filter.size(); ++j) {
            current += filter[j] * history[(historyAt + j) % history.size()];
            next += filter[j] * (j + 1 == filter.size() ? samples[read] : history[(historyAt + j + 1) % history.size()]);
        }
        const double value = current * (1 - phase) + next * phase;
        phase += 48000.0 * d / n;
        return int16_t(std::round(std::clamp(value, -32768.0, 32767.0)));
    }
    uint64_t Generation() const { std::lock_guard<std::mutex> lock(mutex); return generation; }
    size_t Pending() const { std::lock_guard<std::mutex> lock(mutex); return count; }
    uint64_t Dropped() const { std::lock_guard<std::mutex> lock(mutex); return dropped; }
    uint64_t Underruns() const { std::lock_guard<std::mutex> lock(mutex); return underruns; }
private:
    void Clear() { read = count = historyAt = 0; phase = 0; history.fill(0); }
    mutable std::mutex mutex;
    std::array<double, capacity> samples{};
    std::array<double, 48> filter{}, history{};
    size_t read{}, count{}, historyAt{};
    double phase{};
    bool active{}, fenced{};
    AudioInputRoute route{};
    uint32_t numerator{}, denominator{};
    uint64_t generation{}, dropped{}, underruns{};
};

class SdlMicrophone {
public:
    ~SdlMicrophone() {
        deviceThread = {};
        CloseOnDeviceThread();
        if (detach) detach();
    }
    std::function<void()> detach;
    std::function<bool()> prepare;
    // The guest is awake, so capture can come back after a sleep.
    std::function<bool()> awake;
    void SetDeviceThread(std::function<void(const std::function<void()>&)> run) { deviceThread = std::move(run); }
    bool Arm(uint64_t expectedEpoch = UINT64_MAX) {
        error.clear();
        preparing = true;
        const bool ready = !prepare || prepare();
        preparing = false;
        if (!ready) { error = "Wake the device to enable capture"; return false; }
        OnDeviceThread([&] {
            CloseOnDeviceThread();
            if (expectedEpoch != UINT64_MAX && expectedEpoch != epoch.load()) { error = "Capture request expired"; return; }
            if (background) { error = "Return to the device to enable capture"; return; }
            SDL_AudioSpec wanted{}; wanted.freq = 48000; wanted.format = AUDIO_S16SYS;
            wanted.channels = 1; wanted.samples = 512; wanted.callback = Callback; wanted.userdata = this;
            SDL_AudioSpec actual{};
            const auto opened = SDL_OpenAudioDevice(nullptr, 1, &wanted, &actual, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
            if (!opened) { error = SDL_GetError(); return; }
            device.store(opened);
            SDL_PauseAudioDevice(opened, 0);
        });
        const bool on = device.load() != 0;
        if (on) wanted = true;
        return on;
    }
    // Capture off, and the user's choice with it.
    void Disarm() { wanted = false; Close(); }
    // Capture off from the host side while the user's choice stays, so it comes back once the guest is awake.
    void Suspend() { Close(); }
    uint64_t Epoch() const { return epoch.load(); }
    bool ArmOnForeground(uint64_t expectedEpoch) {
        bool accepted = false;
        OnDeviceThread([&] {
            if (expectedEpoch != epoch.load()) return;
            pendingEpoch = expectedEpoch; pendingArm = true; accepted = true;
        });
        return accepted;
    }
    // SDL app events are emitted in the SDL pump before Android pauses device locks.
    void AppEventOnDeviceThread(uint32_t type) {
        if (type == SDL_APP_WILLENTERBACKGROUND) { background = true; ++epoch; pendingArm = false; wanted = false; CloseOnDeviceThread(); }
        else if (type == SDL_APP_DIDENTERFOREGROUND) background = false;
    }
    void CloseOnDeviceThread() {
        const auto closing = device.exchange(0);
        queue.Reset();
        if (closing) SDL_CloseAudioDevice(closing);
    }
    void Removed(SDL_AudioDeviceID id) { if (device.load() == id) { Disarm(); error = "Capture device disconnected"; } }
    // A capture the guest dropped by sleeping or resetting comes back once the guest is awake, while the
    // user still has the microphone on. A failed re-arm leaves it off.
    void Service(const AudioInputDemand& demand) {
        if (pendingArm.load() && !background.load() && pendingArm.exchange(false)) Arm(pendingEpoch);
        else if (wanted.load() && !device.load() && !background.load() && awake && awake() && !Arm()) wanted = false;
        queue.Configure(demand);
    }
    std::string State() const { return device.load() ? "on" : error.empty() ? "off" : "off " + error; }
    size_t Pending() const { return queue.Pending(); }
    uint64_t Dropped() const { return queue.Dropped(); }
    uint64_t Underruns() const { return queue.Underruns(); }
    AudioInputSource Source() {
        return {this, [](void* p, AudioInputRoute r, uint32_t n, uint32_t d) {
                    return static_cast<SdlMicrophone*>(p)->queue.Pull(r, n, d); },
                [](void* p, bool f) { static_cast<SdlMicrophone*>(p)->queue.Fence(f); },
                [](void* p) { static_cast<SdlMicrophone*>(p)->Close(); }};
    }
private:
    static void Callback(void* p, uint8_t* stream, int bytes) {
        auto& self = *static_cast<SdlMicrophone*>(p);
        self.queue.Publish(reinterpret_cast<int16_t*>(stream), size_t(bytes) / sizeof(int16_t), self.queue.Generation());
    }
    void OnDeviceThread(const std::function<void()>& work) { deviceThread ? deviceThread(work) : work(); }
    // Capture off from the guest's side (sleep, reset, a load); the user's choice stays.
    void Close() { pendingArm = false; OnDeviceThread([&] { if (!preparing) ++epoch; CloseOnDeviceThread(); }); }
    MicrophoneQueue queue;
    std::function<void(const std::function<void()>&)> deviceThread;
    std::atomic<SDL_AudioDeviceID> device{0};
    std::atomic<bool> background{false}, pendingArm{false};
    // The user turned capture on and hasn't turned it off; the emulation thread re-arms it after a sleep.
    std::atomic<bool> wanted{false};
    std::atomic<uint64_t> epoch{0};
    uint64_t pendingEpoch{};
    bool preparing{}; // emulator thread, while attaching its source
    std::string error;
};
#endif
