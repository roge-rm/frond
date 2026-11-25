#ifndef FROND_ZODIAC_AUDIO_H
#define FROND_ZODIAC_AUDIO_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <numeric>

#include "audio_queue.h"
#include "imx1_boot_probe.h"

// The Zodiac's sound on its way to the host. SSI1 sends two 16-bit signed slots a frame; slot 0 (the frame
// sync slot) is taken as left (ASSUMPTION "SSI1 slot 0 is left"). Frames are averaged down or up to the
// host's 44100 Hz (a box filter, as the AK4534 path does) and gated by the amplifier. Everything is
// counted in frames, so the same words give the same host frames in Exact and Fast.

// The speaker volume as a 16.16 gain from the LCDC's contrast PWM (PWMR 0x0020502c, pulse width in bits 0-7).
// The i.MX1's own LCD controller drives no panel on the Zodiac, and its contrast output sets the volume: the
// ROM's volume update (2009f3ac) takes the volume (0-64, 0 while muted, from 200a1514) to a step 0-30 and writes
// PWMR = 0x500 | table[step], one table for the speaker and one for headphones (200d3b3c and 200d3b5b, flag at
// module byte 6c2). Both go from 0xff at step 0 down to 0 at step 30 in even steps, so a wider pulse is
// quieter. ASSUMPTION "contrast PWM is a DC volume control": the filtered level sets an amplifier's gain linear
// in dB, 0.2 dB a count (the speaker's steps 0.9 dB, its lowest 34 dB down), pulse width 0 as the full level and
// 0xff as off. CC_EN and the LCDC enable aren't looked at: the ROM turns the LCDC off for each write and CC_EN
// off at its sound power-down, and the level is taken to hold through those.
inline uint32_t zodiacVolumeGain(uint32_t pwmr) {
    struct Table {
        uint32_t gain[256];
        constexpr Table() : gain{} {
            // 10^(-0.2 / 20) in 0.32 fixed point, applied once a count; integer, so the same on every host.
            constexpr uint64_t step = 4197201904u;
            uint64_t level = uint64_t(1) << 32;
            for (unsigned n = 0; n < 255; ++n) {
                gain[n] = uint32_t((level + (uint64_t(1) << 15)) >> 16);
                level = (level * step + (uint64_t(1) << 31)) >> 32;
            }
            gain[255] = 0;
        }
    };
    static constexpr Table table;
    return table.gain[pwmr & 0xff];
}

// The board's gain on the sound as 16.16: the volume above, gated by the amplifier, GPIO D25. The ROM's audio
// module (20097dee) drives D25 high while nothing asks for mute and low at start-up and while muted (ASSUMPTION
// "D25 enables the speaker amplifier"). A pin that isn't a driven GPIO output counts as off. The PWM drives the
// power LED's brightness (ROM 20097854: level 3 on the charger, off on battery), so it plays no part here.
// Headphone detect (GPIO B16, read by 20097d40) stays at its fixed level, which leaves the speaker path on.
inline uint32_t zodiacAudioGain(const Imx1MachineAudio& a) {
    constexpr uint32_t d25 = 1u << 25;
    const bool driven = (a.gpioDDirection & d25) && (a.gpioDInUse & d25) && ((a.gpioDOutputSelect >> 18) & 3) == 3;
    return driven && (a.gpioDData & d25) ? zodiacVolumeGain(a.lcdcPwmr) : 0;
}

class ZodiacAudioOutput {
   public:
    static constexpr uint32_t hostRate = 44100;
    // A gain change takes this many input frames to reach its target, so volume steps and the amplifier
    // switching don't click: 1061 frames as the AK4534 path's full-range attenuation, about 24 ms.
    static constexpr uint32_t rampFrames = 1061;
    using Push = void (*)(void*, uint32_t);

    // Frames a second coming in, numerator / denominator. A change starts the stream afresh; zero holds
    // output back until a rate is known.
    void setRate(uint64_t numerator, uint64_t denominator) {
        if (numerator == rawNumerator && denominator == rawDenominator) return;
        rawNumerator = numerator;
        rawDenominator = denominator;
        if (numerator && denominator) {
            const uint64_t common = std::gcd(numerator, denominator);
            numerator /= common;
            denominator /= common;
        }
        // Keeps the sums in range: a frame's share times a sample stays well under 2^63.
        if (numerator > (uint64_t(1) << 40) || denominator > (uint64_t(1) << 40) / hostRate) numerator = denominator = 0;
        if (numerator == inNumerator && denominator == inDenominator) return;
        inNumerator = numerator;
        inDenominator = denominator;
        restart();
    }
    // The 16.16 gain to ramp to.
    void setGain(uint32_t gain16) {
        if (gain16 == target) return;
        target = gain16;
        ramp = rampFrames;
    }
    // Drops a half-made frame and the average and fades in from silence, as after a state load.
    void restart() {
        held = -1;
        phase = 0;
        sum[0] = sum[1] = 0;
        gain = 0;
        ramp = target ? rampFrames : 0;
    }
    // One SSI1 word. A slot 1 word with no slot 0 before it (the stream starting mid-frame) is dropped.
    void word(uint16_t value, unsigned slot, Push push, void* context) {
        if (slot == 0) {
            held = value;
            return;
        }
        if (held < 0) return;
        frame(int16_t(uint16_t(held)), int16_t(value), push, context);
        held = -1;
    }
    uint32_t currentGain() const { return gain; }

   private:
    void frame(int32_t left, int32_t right, Push push, void* context) {
        if (!inNumerator) return;
        if (ramp) {
            const int64_t delta = int64_t(target) - int64_t(gain);
            gain = uint32_t(int64_t(gain) + delta / ramp);
            if (!--ramp) gain = target;
        }
        const int64_t scaled[2] = {int64_t(left) * gain / 65536, int64_t(right) * gain / 65536};
        // Time in 1 / (inNumerator * 44100) s: an input frame is 44100 * inDenominator, an output one inNumerator.
        uint64_t remain = hostRate * inDenominator;
        while (remain) {
            const uint64_t portion = std::min(remain, inNumerator - phase);
            sum[0] += scaled[0] * int64_t(portion);
            sum[1] += scaled[1] * int64_t(portion);
            phase += portion;
            remain -= portion;
            if (phase == inNumerator) {
                const uint32_t packed = scale(sum[0]) | (scale(sum[1]) << 16);
                if (push) push(context, packed);
                phase = 0;
                sum[0] = sum[1] = 0;
            }
        }
    }
    uint32_t scale(int64_t total) const {
        int64_t v = total / int64_t(inNumerator);
        v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
        return uint16_t(int16_t(v));
    }

    uint64_t rawNumerator = 0, rawDenominator = 0;
    uint64_t inNumerator = 0, inDenominator = 0, phase = 0;
    int64_t sum[2] = {0, 0};
    int32_t held = -1;
    uint32_t gain = 0, target = 0, ramp = 0;
};

// SSI1's words from the machine to the host's audio queue, the part SocIMX1 hands imx1MachineSetPcmOutput.
// At each slot 0 word it reads the rate and volume from the machine, in guest time order, so they change at
// the same word in Exact and Fast. Frames are made while PCM output is off or suspended too, and only the
// push to the queue is skipped, so the stream is the same either way. All on the machine's thread.
struct ZodiacPcmSink {
    const Imx1Machine* machine = nullptr;
    struct AudioQueue* queue = nullptr;
    bool enabled = false, suspended = false;
    // Checkpointed guest work that may be rolled back runs while this is above 0; its sound isn't played.
    unsigned fenced = 0;
    ZodiacAudioOutput sound;
    // The gain the sound is ramping to, for music the host mixes in after (the SoundFont); any thread may read it.
    std::atomic<uint32_t> gain{65536};

    static void word(void* context, uint16_t value, unsigned slot) {
        auto& s = *static_cast<ZodiacPcmSink*>(context);
        if (slot == 0 && s.machine) {
            const auto audio = imx1MachineAudio(s.machine);
            s.sound.setRate(audio.rateNumerator, audio.rateDenominator);
            const uint32_t gain = zodiacAudioGain(audio);
            s.sound.setGain(gain);
            s.gain.store(gain, std::memory_order_relaxed);
        }
        s.sound.word(value, slot, push, &s);
    }
    static void push(void* context, uint32_t frame) {
        auto& s = *static_cast<ZodiacPcmSink*>(context);
        if (s.queue && s.enabled && !s.suspended && !s.fenced) audioQueuePush(s.queue, frame);
    }
    // After a state load or reset: the stream starts again at the new state's next frame.
    void restart() {
        sound.restart();
        if (queue) audioQueueClear(queue);
    }
};

#endif
