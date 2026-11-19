#ifndef FROND_UDA1380_H
#define FROND_UDA1380_H

// The NXP UDA1380 codec on the rx1950's IIC bus (address 0x1a) and IIS, as wavedev.dll drives it. Registers
// are 16 bits, written as a register byte then the value's high and low bytes, the register stepping on for
// each further pair; reads give the same pairs. What the model uses:
//   0x02 power: PON_HP (bit 13) the headphone driver, PON_DAC (10), PON_ADCL (2) and PON_ADCR (0)
//   0x10 master volume and 0x11 mixer volume (channel 1 in the low byte), 0.25 dB a step down from 0 dB
//   0x13 mute: MTM (bit 14) the master mute, MT1 (bit 3) channel 1, the IIS input
//   0x20 decimator volume, 0.5 dB a step as a signed byte; 0x21 bit 15 MT_ADC mutes the ADC
//   0x7f any write resets the registers
// ASSUMPTION "UDA1380 registers": bit positions and volume steps from the UDA1380 data sheet as remembered,
// checked only against what wavedev.dll writes (its init table matches the reset values below); left is the
// high byte of the volume registers. Volume steps are taken as 6 dB per 24 (0.25 dB) or 12 (0.5 dB) steps,
// so the gains are exact halvings and the same on every host.

#include <cstdint>

#include "s3c2442_blocks.h"

class Uda1380 : public S3cIicDevice {
   public:
    static constexpr uint8_t address = 0x1a;
    Uda1380() { reset(); }
    void reset() {
        for (auto& r : regs) r = 0;
        regs[0x00] = 0x0502;
        regs[0x03] = 0x3f3f;
        regs[0x04] = 0x0202;
        regs[0x11] = 0xff00;
        regs[0x13] = 0x4808;
        pointer = 0;
        phase = idle;
        high = 0;
    }

    void start(uint8_t addressByte) override {
        if ((addressByte >> 1) != address) phase = idle;
        else phase = (addressByte & 1) ? reading : pointerNext;
        readHigh = true;
    }
    void write(uint8_t byte) override {
        if (phase == pointerNext) {
            pointer = byte & 0x7f;
            phase = valueHigh;
        } else if (phase == valueHigh) {
            high = byte;
            phase = valueLow;
        } else if (phase == valueLow) {
            set(pointer, uint16_t(high << 8 | byte));
            ++writes;
            pointer = (pointer + 1) & 0x7f;
            phase = valueHigh;
        }
    }
    uint8_t read() override {
        if (phase != reading) return 0xff;
        const uint16_t v = regs[pointer];
        if (readHigh) {
            readHigh = false;
            return uint8_t(v >> 8);
        }
        readHigh = true;
        pointer = (pointer + 1) & 0x7f;
        return uint8_t(v);
    }
    void stop() override { phase = idle; }

    void set(uint8_t reg, uint16_t value) {
        if (reg == 0x7f) {
            reset();
            return;
        }
        regs[reg & 0x7f] = value;
    }
    uint16_t reg(uint8_t r) const { return regs[r & 0x7f]; }

    // The DAC path playing: powered and neither mute on.
    bool dacOn() const { return (regs[0x02] & 0x400) && !(regs[0x13] & 0x4000) && !(regs[0x13] & 8); }
    bool headphoneDriverOn() const { return regs[0x02] & 0x2000; }
    bool adcOn() const { return (regs[0x02] & 5) && !(regs[0x21] & 0x8000); }
    // One IIS frame through the DAC's digital volume, master then mixer channel 1.
    void play(int16_t& left, int16_t& right) const {
        left = scale(left, attenuation(regs[0x10] >> 8) * uint64_t(attenuation(regs[0x11] & 0xff)) >> 16);
        right = scale(right, attenuation(regs[0x10] & 0xff) * uint64_t(attenuation(regs[0x11] & 0xff)) >> 16);
    }
    // A microphone sample through the decimator volume, left from the high byte. ASSUMPTION "rx1950 mic
    // level": the host's full scale stands for the ADC's full scale after the LNA and VGA, so their gains
    // aren't applied again. ASSUMPTION "mic on both channels": the mic feeds the left and right samples.
    void record(int16_t sample, int16_t out[2]) const {
        out[0] = scale(sample, boost(int8_t(regs[0x20] >> 8)));
        out[1] = scale(sample, boost(int8_t(regs[0x20] & 0xff)));
    }

    uint64_t writes = 0;
    template <class A> void state(A& a) {
        a.DoBuffer16(regs, 0x80).Do8(pointer).Do8(phase).Do8(high).DoBool(readHigh).Do64(writes);
    }

   private:
    static int16_t scale(int32_t sample, uint32_t gain16) {
        const int64_t v = int64_t(sample) * gain16 / 65536;
        return int16_t(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
    }
    // 0.25 dB a step down; 0xfc and above are silent, as -78 dB and below is.
    static uint32_t attenuation(uint32_t steps) {
        static constexpr uint32_t frac[24] = {65536, 63677, 61870, 60115, 58409, 56752, 55142, 53577,
                                              52057, 50580, 49145, 47751, 46396, 45080, 43801, 42558,
                                              41350, 40177, 39037, 37930, 36854, 35808, 34792, 33805};
        if (steps >= 0xfc) return 0;
        return frac[steps % 24] >> (steps / 24);
    }
    // 0.5 dB a step, signed: +24 dB (48) down to -63.5 dB (-127); -128 is silent.
    static uint32_t boost(int8_t steps) {
        static constexpr uint32_t frac[12] = {65536, 69419, 73533, 77890, 82505, 87394,
                                              92572, 98057, 103868, 110022, 116541, 123447};
        if (steps == -128) return 0;
        int s = steps > 48 ? 48 : steps;
        if (s >= 0) return frac[s % 12] << (s / 12);
        const int down = -s;
        // Each 12 steps down halves; the rest steps back up from there.
        const int halvings = (down + 11) / 12;
        return (frac[halvings * 12 - down] >> halvings);
    }

    enum : uint8_t { idle, pointerNext, valueHigh, valueLow, reading };
    uint16_t regs[0x80];
    uint8_t pointer, phase, high;
    bool readHigh = true;
};

#endif
