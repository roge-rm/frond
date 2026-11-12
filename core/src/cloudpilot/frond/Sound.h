#ifndef FROND_68K_SOUND_H
#define FROND_68K_SOUND_H

// The DragonBall piezo: the PWM's frequency and duty cycle as a square wave, rendered in guest
// time from the cycle each change happened at. Mono signed 16-bit at kRate.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

class Piezo68k {
   public:
    static constexpr int kRate = 44100;
    // Peak level of the pulses at full loudness, below full scale.
    static constexpr double kLevel = 20000;

    // From EmHAL::onPwmChange: a frequency under 0 turns the tone off.
    void Change(uint64_t cycle, double frequency, double duty) {
        if (cycle < rendered) cycle = rendered;
        changes.push_back({cycle, frequency, duty});
        ++changeCount;
        if (frequency > 0) lastFrequency = frequency;
    }

    // Samples from the last call up to cycle, at clocksPerSecond guest clocks a second. With no
    // out, moves on without making samples.
    void Render(uint64_t cycle, double clocksPerSecond, std::vector<int16_t>* out) {
        if (cycle <= rendered || clocksPerSecond <= 0) return;
        const double perSample = clocksPerSecond / kRate;
        while (true) {
            // Run to the next change or the end.
            const uint64_t until = changes.empty() || changes.front().cycle > cycle ? cycle : changes.front().cycle;
            const double samples = (until - rendered) / perSample + fraction;
            const uint64_t whole = uint64_t(samples);
            fraction = samples - whole;
            if (out)
                for (uint64_t i = 0; i < whole; ++i) out->push_back(Sample());
            rendered = until;
            if (until == cycle && (changes.empty() || changes.front().cycle > cycle)) break;
            const auto change = changes.front();
            changes.pop_front();
            frequency = change.frequency;
            duty = change.duty < 0 ? 0 : change.duty > 1 ? 1 : change.duty;
        }
    }

    // Drops pending changes and silences the tone, for a reset or session load.
    void Restart(uint64_t cycle) {
        changes.clear();
        rendered = cycle;
        frequency = -1;
        fraction = 0;
    }

    bool Playing() const { return frequency > 0 || !changes.empty(); }
    uint64_t Changes() const { return changeCount; }
    double LastFrequency() const { return lastFrequency; }

   private:
    struct ToneChange {
        uint64_t cycle;
        double frequency, duty;
    };

    int16_t Sample() {
        if (frequency <= 0) {
            phase = 0;
            return 0;
        }
        phase += frequency / kRate;
        phase -= std::floor(phase);
        // The guest sets the volume by the duty cycle; the narrow pulses it uses (4 to 7% at full
        // volume) are scaled up so they're heard. AC coupled, so the level doesn't jump when the
        // duty changes.
        const double narrow = duty < 0.5 ? duty : 1 - duty;
        const double loudness = std::min(1.0, std::sqrt(8 * narrow));
        const double span = duty < 0.5 ? 1 - duty : duty;
        return int16_t(kLevel * loudness * ((phase < duty ? 1.0 : 0.0) - duty) / span);
    }

    std::deque<ToneChange> changes;
    uint64_t rendered{0};
    uint64_t changeCount{0};
    double fraction{0};
    double frequency{-1}, duty{0.5}, phase{0};
    double lastFrequency{0};
};

// A mono 16-bit WAV file written as samples arrive; the header is filled in on Close.
class WavWriter68k {
   public:
    bool Open(const std::string& path) {
        file = std::fopen(path.c_str(), "wb");
        if (!file) return false;
        uint8_t header[44]{};
        return std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
    }
    void Write(const std::vector<int16_t>& samples) {
        if (!file || samples.empty()) return;
        // Little-endian hosts only, like the rest of the runner.
        frames += std::fwrite(samples.data(), 2, samples.size(), file);
    }
    bool Close() {
        if (!file) return true;
        auto put32 = [](uint8_t* at, uint32_t v) {
            for (int i = 0; i < 4; ++i) at[i] = uint8_t(v >> (8 * i));
        };
        uint8_t header[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                              16,  0,   0,   0,   1, 0, 1, 0};
        put32(header + 4, uint32_t(36 + frames * 2));
        put32(header + 24, Piezo68k::kRate);
        put32(header + 28, Piezo68k::kRate * 2);
        header[32] = 2;
        header[34] = 16;
        std::memcpy(header + 36, "data", 4);
        put32(header + 40, uint32_t(frames * 2));
        const bool ok = std::fseek(file, 0, SEEK_SET) == 0 && std::fwrite(header, 1, 44, file) == 44;
        const bool closed = std::fclose(file) == 0;
        file = nullptr;
        return ok && closed;
    }

   private:
    std::FILE* file{nullptr};
    uint64_t frames{0};
};

#endif
