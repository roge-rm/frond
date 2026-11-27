#ifndef _I2CDEV_WM8750_H_
#define _I2CDEV_WM8750_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>

#include "audio_input.h"
#include "audio_queue.h"
#include "cputil.h"
#include "pxa_I2C.h"
#include "savestate/savestateAll.h"

class Wm8750 {
   public:
    bool Attach(PxaI2c* bus) {
        return pxaI2cDeviceAdd(bus, [](void* context, ActionI2C action, uint_fast8_t value) {
            return static_cast<Wm8750*>(context)->Transfer(action, value);
        }, this);
    }

    void SetQueue(AudioQueue* newQueue) {
        if (queue == newQueue) return;
        if (queue) Boundary();
        queue = newQueue;
        if (queue) audioQueueClear(queue);
    }

    void Boundary() {
        resampler = {};
        if (queue) audioQueueClear(queue);
    }

    void Output(uint32_t frame, bool playing, bool msbJustified, uint32_t numerator,
                uint32_t denominator, bool headphones, uint16_t scoopPins) {
        if (!queue || !playing || !numerator || !denominator ||
            msbJustified || !I2sFormat()) return;

        const uint16_t power = registers[26];
        int32_t dac[2]{}, mixed[2]{};
        if (!(registers[5] & 8) && (registers[25] & 0x40)) {
            for (unsigned ch = 0; ch < 2; ++ch) {
                if (!(power & (0x100 >> ch))) continue;
                dac[ch] = Scale(static_cast<int16_t>(frame >> (16 * ch)), dacGain[gains[ch]]);
                if (registers[5] & 0x80) dac[ch] /= 2;
            }
            for (unsigned out = 0; out < 2; ++out) {
                for (unsigned in = 0; in < 2; ++in) {
                    if (registers[34 + 2 * out + in] & 0x100) mixed[out] += dac[in];
                }
            }
        }

        if (headphones) {
            for (unsigned ch = 0; ch < 2; ++ch) {
                const bool active = (power & (0x40 >> ch)) && (scoopPins & (0x10 << ch));
                mixed[ch] = active ? Scale(mixed[ch], pinGain[gains[2 + ch]]) : 0;
            }
        } else {
            const int32_t left = power & 0x10 ? Scale(mixed[0], pinGain[gains[4]]) : 0;
            const int32_t right = power & 8 ? Scale(mixed[1], pinGain[gains[5]]) : 0;
            // The mono speaker is driven across the two OUT2 pins.
            mixed[0] = mixed[1] = (left + (registers[24] & 0x10 ? right : -right)) / 2;
        }
        Resample(std::clamp(mixed[0], -32768, 32767), std::clamp(mixed[1], -32768, 32767),
                 numerator, denominator);
    }

    void SetInputSource(const AudioInputSource* source) {
        if (input.disarm) input.disarm(input.context);
        input = source ? *source : AudioInputSource{};
        if (inputFences && input.fence) input.fence(input.context, true);
    }

    void FenceInput(bool fence) {
        const bool wasFenced = inputFences != 0;
        if (fence) ++inputFences;
        else if (inputFences) --inputFences;
        if (wasFenced != bool(inputFences) && input.fence)
            input.fence(input.context, inputFences != 0);
    }

    AudioInputDemand InputDemand(bool receive, bool bias, uint32_t numerator,
                                 uint32_t denominator) const {
        const bool active = receive && bias && numerator && denominator && I2sFormat() &&
            (AdcConnected(0) || AdcConnected(1));
        return {active, inputFences != 0, AudioInputRoute::externalMic, numerator, denominator};
    }

    uint32_t InputFrame(const AudioInputDemand& demand) {
        if (!demand.active || demand.fenced || !input.pull) return 0;
        const int32_t sample = input.pull(input.context, demand.route,
                                          demand.rateNumerator, demand.rateDenominator);
        uint32_t frame = 0;
        for (unsigned ch = 0; ch < 2; ++ch) {
            if (!AdcConnected(ch)) continue;
            const uint8_t pga = inputGains[ch];
            const auto boost = (registers[32 + ch] >> 4) & 3;
            int32_t level = Scale(Scale(sample, pgaGain[pga & 0x3f]), micGain[boost]);
            level = Scale(std::clamp(level, -32768, 32767), adcGain[inputGains[2 + ch]]);
            if (registers[5] & (0x20 << ch)) level = -level;
            if (registers[5] & 0x100) level /= 2;
            frame |= uint32_t(uint16_t(std::clamp(level, -32768, 32767))) << (16 * ch);
        }
        return frame;
    }

    template <typename T>
    void Save(T& savestate) const {
        auto chunk = savestate.GetChunk(ChunkType::wm8750, 2);
        if (!chunk) ERR("cannot allocate WM8750 state");
        SaveChunkHelper helper(*chunk);
        auto state = *this;
        state.Serialize(helper);
        state.SerializeOutput(helper);
        helper.DoBuffer(state.inputGains.data(), state.inputGains.size());
    }

    template <typename T>
    void Load(T& loader) {
        Wm8750 state;
        if (!loader.HasChunk(ChunkType::wm8750)) {
            ApplyState(state);
            return;
        }
        uint32_t version{};
        auto chunk = loader.GetChunkOrFail(ChunkType::wm8750, 2, "WM8750", version);
        if (!chunk) return;
        LoadChunkHelper helper(*chunk);
        state.Serialize(helper);
        if (version >= 1) state.SerializeOutput(helper);
        else state.LatchOutputGains();
        if (version >= 2) helper.DoBuffer(state.inputGains.data(), state.inputGains.size());
        else state.LatchInputGains();
        if (chunk->HasError() || state.phase > complete || !state.resampler.Valid() ||
            std::any_of(state.gains.begin() + 2, state.gains.end(),
                        [](uint8_t value) { return value > 127; }) ||
            std::any_of(state.registers.begin(), state.registers.end(),
                        [](uint16_t value) { return value > 0x1ff; })) {
            loader.NotifyError();
            return;
        }
        ApplyState(state);
    }

   private:
    static constexpr unsigned resetRegister = 15;
    static constexpr std::array<uint16_t, 43> resetValues{
        0x097, 0x097, 0x079, 0x079, 0x000, 0x008, 0x000, 0x00a,
        0x000, 0x000, 0x0ff, 0x0ff, 0x00f, 0x00f, 0x000, 0x000,
        0x000, 0x07b, 0x000, 0x032, 0x000, 0x0c3, 0x0c3, 0x0c0,
        0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
        0x000, 0x000, 0x050, 0x050, 0x050, 0x050, 0x050, 0x050,
        0x079, 0x079, 0x079,
    };
    enum Phase : uint8_t { idle, address, firstByte, secondByte, complete };
    uint8_t phase{idle}, first{};
    std::array<uint16_t, resetValues.size()> registers = resetValues;

    AudioQueue* queue{};
    AudioInputSource input{};
    unsigned inputFences{};
    std::array<uint8_t, 4> inputGains{0x97, 0x97, 195, 195};
    std::array<uint8_t, 6> gains{255, 255, 121, 121, 121, 121};
    struct Resampler {
        uint32_t numerator{}, denominator{}, phase{};
        int64_t left{}, right{};

        bool Valid() const {
            if (!numerator) return !denominator && !phase && !left && !right;
            return numerator == 576000 && denominator > 0 && denominator <= 127 && phase < numerator &&
                left >= -32768ll * phase && left <= 32767ll * phase &&
                right >= -32768ll * phase && right <= 32767ll * phase;
        }
    } resampler;

    template <unsigned Size>
    static std::array<int32_t, Size> GainTable(unsigned firstCode, int unity, double stepDb) {
        std::array<int32_t, Size> table{};
        for (unsigned code = firstCode; code < Size; ++code)
            table[code] = std::lround(65536.0 * std::pow(10.0, (int(code) - unity) * stepDb / 20.0));
        return table;
    }

    void ApplyState(Wm8750& state) {
        state.queue = queue;
        state.input = input;
        state.inputFences = inputFences;
        *this = state;
        if (queue) audioQueueClear(queue);
    }

    bool I2sFormat() const { return (registers[7] & 0x4f) == 2; }

    bool AdcConnected(unsigned ch) const {
        constexpr uint16_t vrefAndMicBias = 0x42;
        const uint16_t power = vrefAndMicBias | (0x28 >> ch);
        if ((registers[25] & power) != power || (inputGains[ch] & 0x80)) return false;
        const unsigned select = (registers[32 + ch] >> 6) & 3;
        // The external mic drives LINPUT1, including differential input pair 1.
        return (select == 0 && ch == 0) || (select == 3 && !(registers[31] & 0x100));
    }

    static int32_t Scale(int32_t sample, int32_t gain) {
        return int64_t(sample) * gain / 65536;
    }

    inline static const auto dacGain = GainTable<256>(1, 255, 0.5);
    inline static const auto pinGain = GainTable<128>(48, 121, 1.0);

    inline static const auto pgaGain = GainTable<64>(0, 23, 0.75);
    inline static const auto adcGain = GainTable<256>(1, 195, 0.5);
    static constexpr int32_t micGain[] = {65536, 292739, 655360, 1847055};

    void LatchInputGains() {
        for (unsigned ch = 0; ch < 2; ++ch) {
            inputGains[ch] = registers[ch] & 0xff;
            inputGains[2 + ch] = registers[21 + ch] & 0xff;
        }
    }

    void LatchOutputGains() {
        for (unsigned ch = 0; ch < 2; ++ch) {
            gains[ch] = registers[10 + ch] & 0xff;
            gains[2 + ch] = registers[2 + ch] & 0x7f;
            gains[4 + ch] = registers[40 + ch] & 0x7f;
        }
    }

    void WriteRegister(unsigned reg, uint16_t value) {
        if (reg == resetRegister) {
            registers = resetValues;
            LatchOutputGains();
            LatchInputGains();
            Boundary();
        } else if (reg < registers.size()) {
            registers[reg] = value;
            if (!(value & 0x100)) return;
            const unsigned pair = reg & ~1u;
            if (pair == 10 || pair == 2 || pair == 40) {
                const unsigned index = pair == 10 ? 0 : pair == 2 ? 2 : 4;
                const uint16_t mask = pair == 10 ? 0xff : 0x7f;
                gains[index] = registers[pair] & mask;
                gains[index + 1] = registers[pair + 1] & mask;
            } else if (reg <= 1 || reg == 21 || reg == 22) {
                const unsigned base = reg <= 1 ? 0 : 21;
                const unsigned index = reg <= 1 ? 0 : 2;
                inputGains[index] = registers[base] & 0xff;
                inputGains[index + 1] = registers[base + 1] & 0xff;
            }
        }
    }

    void Resample(int32_t left, int32_t right, uint32_t numerator, uint32_t denominator) {
        if (resampler.numerator != numerator || resampler.denominator != denominator) {
            Boundary();
            resampler.numerator = numerator;
            resampler.denominator = denominator;
        }
        // Each wire frame spans 44100 * denominator units at the host rate.
        uint32_t remaining = 44100 * denominator;
        while (remaining) {
            const uint32_t span = std::min(remaining, numerator - resampler.phase);
            remaining -= span;
            resampler.phase += span;
            resampler.left += int64_t(left) * span;
            resampler.right += int64_t(right) * span;
            if (resampler.phase == numerator) {
                const auto l = uint16_t(resampler.left / numerator);
                const auto r = uint16_t(resampler.right / numerator);
                audioQueuePush(queue, l | (uint32_t(r) << 16));
                resampler.phase = 0;
                resampler.left = resampler.right = 0;
            }
        }
    }

    uint_fast8_t Transfer(ActionI2C action, uint8_t value) {
        switch (action) {
            case i2cStart:
            case i2cRestart:
                phase = address;
                break;
            case i2cStop:
                phase = idle;
                break;
            case i2cTx:
                return WriteByte(value);
            case i2cRx:
                break;
        }
        return 0;
    }

    bool WriteByte(uint8_t value) {
        switch (phase) {
            case address:
                // The SL-C3000 straps the write-only codec to 7-bit address 0x1b.
                phase = value == 0x36 ? firstByte : idle;
                return phase == firstByte;
            case firstByte:
                first = value;
                phase = secondByte;
                return true;
            case secondByte: {
                const unsigned reg = first >> 1;
                WriteRegister(reg, ((first & 1) << 8) | value);
                phase = complete;
                return true;
            }
            default:
                return false;
        }
    }

    template <typename T>
    void Serialize(T& helper) {
        helper.Do(typename T::Pack8() << phase << first);
        for (auto& reg : registers) helper.Do16(reg);
    }

    template <typename T>
    void SerializeOutput(T& helper) {
        helper.DoBuffer(gains.data(), gains.size());
        helper.Do32(resampler.numerator).Do32(resampler.denominator).Do32(resampler.phase)
            .Do64(resampler.left).Do64(resampler.right);
    }
};

#endif
