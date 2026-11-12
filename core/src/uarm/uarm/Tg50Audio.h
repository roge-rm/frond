#ifndef FROND_TG50_AUDIO_H
#define FROND_TG50_AUDIO_H
#include <algorithm>
#include <array>
#include <cstdint>

// Digital PWM and codec mixing, with one 44100 Hz output timeline.
class Tg50Audio {
public:
    static constexpr uint32_t rate = 44100;
    static constexpr uint32_t pwmClock = 3686400;
    static constexpr int32_t pwmGain = 8192;
    struct PwmRegisters {
        uint32_t control{}, duty{}, period{};
    };
    struct State {
        PwmRegisters programmed{}, active{};
        uint64_t pwmPhase{}, hostPhase{};
        uint32_t clock{}, running{}, stopping{};
        uint32_t dacCount{};
        std::array<uint32_t, 16> dac{};
    };

    void WritePwm(PwmRegisters next) {
        state.programmed = {next.control & 0x7f, next.duty & 0x7ff, next.period & 0x3ff};
        if (!state.running || !state.active.period) Latch();
    }
    void Clock(bool enabled) {
        if (enabled == bool(state.clock)) return;
        state.clock = enabled;
        if (enabled) {
            if (!state.running) { state.pwmPhase = 0; Latch(); }
            state.running = 1;
            state.stopping = 0;
        } else if (state.programmed.control & 0x40) {
            state.running = state.stopping = 0;
            state.pwmPhase = 0;
        } else {
            state.stopping = state.running;
        }
    }
    void SetMuxOutput(bool enabled) { muxOutput = enabled; }
    bool PushDac(uint32_t frame) {
        if (state.dacCount == state.dac.size()) return false;
        state.dac[state.dacCount++] = frame;
        return true;
    }
    void CodecBoundary() {
        state.dacCount = 0;
        state.dac.fill(0);
    }
    void Reset() { state = {}; muxOutput = false; }
    const State &GetState() const { return state; }
    bool Load(const State &next) {
        if (!Valid(next)) return false;
        state = next;
        return true;
    }
    static bool Valid(const State &next) {
        const auto regsValid = [](const PwmRegisters &r) {
            return r.control <= 0x7f && r.duty <= 0x7ff && r.period <= 0x3ff;
        };
        for (size_t i = next.dacCount; i < next.dac.size(); ++i)
            if (next.dac[i]) return false;
        const uint64_t period = Period(next.active);
        return regsValid(next.programmed) && regsValid(next.active) &&
            next.pwmPhase < period && next.hostPhase < 1000000000 &&
            next.clock <= 1 && next.running <= 1 && next.stopping <= 1 &&
            next.dacCount <= next.dac.size() && (!next.clock || next.running) &&
            (!next.stopping || (!next.clock && next.running)) &&
            (!next.running || next.clock || next.stopping) &&
            (next.running || !next.pwmPhase);
    }
    template <typename Emit> void Advance(uint64_t ns, Emit emit) {
        // Scheduler quanta are 20 us; bounding slices also makes direct tests safe.
        while (ns) {
            const auto slice = std::min<uint64_t>(ns, 1000000);
            ns -= slice;
            state.hostPhase += slice * rate;
            while (state.hostPhase >= 1000000000) {
                state.hostPhase -= 1000000000;
                const int32_t beep = NextPwmSample();
                uint32_t dac{};
                if (state.dacCount) {
                    dac = state.dac[0];
                    for (unsigned i = 1; i < state.dacCount; ++i) state.dac[i-1] = state.dac[i];
                    state.dac[--state.dacCount] = 0;
                }
                const auto left = Saturate(int32_t(int16_t(dac)) + beep);
                const auto right = Saturate(int32_t(int16_t(dac >> 16)) + beep);
                emit(uint16_t(left) | (uint32_t(uint16_t(right)) << 16));
            }
        }
    }

private:
    static int16_t Saturate(int32_t value) {
        return static_cast<int16_t>(std::clamp(value, -32768, 32767));
    }
    static uint64_t Period(const PwmRegisters &r) {
        return uint64_t((r.control & 63) + 1) * (r.period + 1) * rate;
    }
    static uint64_t High(const PwmRegisters &r) {
        const auto count = (r.duty & 0x400) ? r.period + 1 : std::min(r.duty & 0x3ff, r.period + 1);
        return uint64_t((r.control & 63) + 1) * count * rate;
    }
    static uint64_t Integral(uint64_t position, uint64_t period, uint64_t high) {
        return (position / period) * high + std::min(position % period, high);
    }
    void Latch() {
        state.active = state.programmed;
        state.pwmPhase = 0;
    }
    int32_t NextPwmSample() {
        if (!state.running) return 0;
        uint64_t remaining = pwmClock;
        int64_t weighted{};
        while (remaining && state.running) {
            const uint64_t period = Period(state.active), high = High(state.active);
            // Only one pending register update or graceful stop can split this frame.
            const bool boundary = state.stopping ||
                state.active.control != state.programmed.control ||
                state.active.duty != state.programmed.duty || state.active.period != state.programmed.period;
            const uint64_t span = boundary ? std::min(remaining, period-state.pwmPhase) : remaining;
            if (state.active.period && high && high < period) {
                const uint64_t highs = Integral(state.pwmPhase + span, period, high) -
                                       Integral(state.pwmPhase, period, high);
                // Remove the digital duty-cycle mean. This models no analogue filter or gain.
                weighted += int64_t(2 * pwmGain) * int64_t(highs) -
                    int64_t(2 * pwmGain) * int64_t(high * span / period);
            }
            state.pwmPhase = (state.pwmPhase + span) % period;
            remaining -= span;
            if (boundary && !state.pwmPhase) {
                if (state.stopping) { state.running = state.stopping = 0; break; }
                Latch();
            }
        }
        return muxOutput ? static_cast<int32_t>(weighted / pwmClock) : 0;
    }

    State state{};
    bool muxOutput{}; // Derived from GPIO16 direction/alternate function after load.
};
#endif
