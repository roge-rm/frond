#ifndef FROND_ZODIAC_YMF768_H
#define FROND_ZODIAC_YMF768_H

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

// The Zodiac's Yamaha YMF768 on CS4: an index port at 0x15000000 and a data port at 0x15000001, two banks of
// 256 registers (common register 0f bit 0 picks the bank). There is no register manual; everything here is
// read from the ROM's driver (DAL 0x200b5c00..0x200c0e40).
//
// - Identity: bank 1 0e reads 3, bank 1 38 bit 0 (ready) reads 1, bank 1 00 bit 6 (busy) reads 0.
// - Sequencer FIFO: bank 0 register 0 takes 3-byte records. The ROM only ever writes 01 ff 00, a 1 tick wait,
//   and uses the FIFO as its 10 ms MIDI tick. Bank 1 register 2 low bits pick the tick (code 4 = 10 ms),
//   bank 0 register 6 bit 0 runs it. Bank 0 08 bit 0 asks for more records while fewer than two are queued,
//   and the interrupt line (GPIO B15, which the ROM sets to interrupt while low) is low while it asks and the
//   FIFO runs.
// - FM synthesis: OPL3 style operators (slots 10-2f, groups at +00 +20 +40 +60 +80 as OPL3's 20 40 60 80 e0),
//   F-number, block and key on in b0/c0 as OPL3's a0/b0, algorithm and feedback in d0. The operators,
//   envelopes and LFOs are ymfm's OPL3 (zodiac_ymf768_core.h); this layer maps the 16 channels onto it and
//   adds what OPL3 doesn't have: channel level e0 and pan f0, bank 1 channel level e0 and master level 10.
// The FM engine runs on guest time, 49716 samples a guest second whatever SSI1 does, so envelopes and notes go
// on while SSI1 is off. 49716 Hz is the rate the ROM's F-number table is tuned for (A4 = 580 in block 4 is
// 440 Hz there, 0x200d64bc), so the chip's clock is taken as its own crystal, not an i.MX1 clock. SSI1 words
// take the FM output at the guest time they finish, so it is the same in Exact and Fast. All integer.
struct Imx1Ymf768Core;

class Imx1Ymf768 {
   public:
    Imx1Ymf768();
    ~Imx1Ymf768();
    Imx1Ymf768(const Imx1Ymf768& other);
    Imx1Ymf768& operator=(const Imx1Ymf768& other);
    Imx1Ymf768(Imx1Ymf768&&) noexcept;
    Imx1Ymf768& operator=(Imx1Ymf768&&) noexcept;

    void reset();

    // One byte access to port 0 (index) or port 1 (data). Port 0 reads give 0; the ROM throws them away.
    uint8_t read(bool dataPort) const {
        if (!dataPort) return 0;
        return readRegister(bank, index);
    }
    void write(bool dataPort, uint8_t value);
    uint8_t readRegister(unsigned b, unsigned i) const {
        if (b == 1 && i == 0x0e) return 3;
        if (b == 1 && i == 0x38) return 1;
        if (b == 1 && i == 0x00) return regs[1][0] & ~0x40;
        if (b == 0 && i == 0x08) return request() ? 1 : 0;
        if (b == 0 && (i == 0x09 || i == 0x00)) return 0;
        return regs[b & 1][i & 0xff];
    }

    // The interrupt line: true while asserted (GPIO B15 low).
    bool interrupt() const { return running() && request(); }

    // Moves the FM engine and the sequencer clock on by system clocks, with the System PLL at
    // 32768 * numerator / denominator Hz. Returns true when the interrupt line changed.
    bool advance(uint64_t clocks, uint64_t numerator, uint64_t denominator) {
        if (!numerator || !denominator) return false;
        advanceEngine(clocks, numerator, denominator);
        if (!running()) return false;
        const bool before = interrupt();
        timerAcc += clocks * denominator * 1000;
        const uint64_t period = tickPeriod(numerator);
        if (timerAcc >= period) {
            // More ticks than records do nothing, which also bounds the work for a loaded remainder.
            const uint64_t ticks = std::min<uint64_t>(timerAcc / period, fifoCapacity + 1);
            timerAcc %= period;
            for (uint64_t n = 0; n < ticks; ++n) tick();
        }
        return interrupt() != before;
    }
    // System clocks until the interrupt line can change, UINT64_MAX when it can't. The FM engine never
    // changes the line, so an idle skip can pass over its samples.
    uint64_t clocksUntilChange(uint64_t numerator, uint64_t denominator) const {
        if (!running() || !numerator || !denominator || !fifoQueued) return UINT64_MAX;
        const uint64_t period = tickPeriod(numerator);
        const uint64_t ticks = fifoWait ? fifoWait : 1;
        const uint64_t needed = ticks * period - std::min(timerAcc, ticks * period);
        const uint64_t unit = denominator * 1000;
        return std::max<uint64_t>(1, (needed + unit - 1) / unit);
    }

    // The FM output now, added to left and right: the engine's last two samples, linear by the time since
    // the last one.
    void output(int32_t& left, int32_t& right) const {
        const int64_t fraction = enginePeriod ? int64_t((engineAcc << 16) / enginePeriod) : 0;
        left += last[0] + int32_t((int64_t(last[2] - last[0]) * fraction) >> 16);
        right += last[1] + int32_t((int64_t(last[3] - last[1]) * fraction) >> 16);
    }

    static constexpr uint32_t engineRate = 49716;
    static constexpr uint32_t stateVersion = 3;
    template <class A> void state(A& a) {
        a.DoBuffer(&regs[0][0], sizeof(regs));
        a.Do8(index).Do8(bank).Do8(fifoQueued).Do8(fifoPartial).Do8(fifoWait);
        a.Do64(timerAcc).Do64(engineAcc).Do64(enginePeriod);
        for (auto& v : last) a.Do32(reinterpret_cast<uint32_t&>(v));
        a.Do32(reinterpret_cast<uint32_t&>(pendingRight));
        // The accumulators stay under their periods, at most 32768 x 50 ms x 2^24 and 32768 x 2^24.
        a.Require(bank <= 1 && fifoQueued <= fifoCapacity && fifoPartial < 3 && fifoWait == (fifoQueued ? 1 : 0) &&
                  timerAcc < (uint64_t(1) << 46) && enginePeriod < (uint64_t(1) << 40) &&
                  engineAcc < std::max<uint64_t>(enginePeriod, 1), "ymf768");
        std::vector<uint8_t> engine;
        if (!A::loading) saveEngine(engine);
        a.DoVector(engine, engineStateBytes());
        a.Require(engine.size() == engineStateBytes() && (!A::loading || engineStateValid(engine)), "ymf768 engine");
        if (A::loading && a.ok()) { loadEngine(engine); quietOutput = false; }
    }

    // SSI1 word mixing: slot 0 makes a frame and keeps its right half for the slot 1 word.
    uint16_t mix(uint16_t word, unsigned slot) {
        int32_t extra;
        if (!slot) { int32_t l = 0, r = 0; output(l, r); extra = l; pendingRight = r; }
        else { extra = pendingRight; pendingRight = 0; }
        const int32_t v = std::clamp<int32_t>(int32_t(int16_t(word)) + extra, -32768, 32767);
        return uint16_t(int16_t(v));
    }

    // For tests.
    unsigned queuedRecords() const { return fifoQueued; }
    uint64_t engineSamples() const { return samples; }
    uint64_t engineQuietSamples() const { return quietSamples; }
    void setQuietSkipping(bool on) { quietSkipping = on; }
    std::vector<uint8_t> engineState() const { std::vector<uint8_t> v; saveEngine(v); return v; }
    static bool engineStateValid(const std::vector<uint8_t>& in);

   private:
    static constexpr uint8_t fifoCapacity = 8;

    bool running() const { return regs[0][6] & 1; }
    bool request() const { return fifoQueued < 2; }
    uint64_t tickPeriod(uint64_t numerator) const {
        // Bank 1 register 2 low bits, ASSUMPTION "YMF768 timebase codes", from the MA-2 family: code 4 is the
        // 10 ms the driver counts per interrupt (0x200bfc68).
        static constexpr uint64_t ms[8] = {1, 2, 4, 5, 10, 20, 40, 50};
        return 32768ull * numerator * ms[regs[1][2] & 7];
    }
    void fifoByte() {
        if (fifoQueued >= fifoCapacity) return;
        if (++fifoPartial < 3) return;
        fifoPartial = 0;
        if (!fifoQueued) fifoWait = 1;
        ++fifoQueued;
    }
    void tick() {
        if (!fifoQueued) return;
        if (fifoWait > 1) { --fifoWait; return; }
        --fifoQueued;
        fifoWait = fifoQueued ? 1 : 0;
    }

    // 4-op pairs: channel n (0-3, 8-11) with channel n+4 when its algorithm is 2..7.
    bool fourOp(unsigned ch) const { return (ch & 4) == 0 && (regs[0][0xd0 + ch] & 7) >= 2; }
    // Channels 3 and 11 have no OPL3 four-operator pair; they run as two FM pairs added.
    static bool emulatedPair(unsigned ch) { return ch == 3 || ch == 11; }
    void engineWrite(unsigned reg, uint8_t value);
    void syncAlgorithm(unsigned first);
    void syncFrequency(unsigned ch);
    void sample(int32_t& left, int32_t& right);
    void advanceEngine(uint64_t clocks, uint64_t numerator, uint64_t denominator);
    void saveEngine(std::vector<uint8_t>& out) const;
    void loadEngine(const std::vector<uint8_t>& in);
    static size_t engineStateBytes();

    uint8_t regs[2][256];
    uint8_t index, bank, fifoQueued, fifoPartial, fifoWait;
    uint64_t timerAcc;
    uint64_t engineAcc;     // system clocks x denominator x engineRate since the last sample, of enginePeriod
    uint64_t enginePeriod;  // 32768 x the System PLL numerator at the last advance, 0 before the first
    uint64_t samples;     // not saved, for tests
    uint64_t quietSamples;  // not saved, for tests: samples made without the output calls
    // The last sample was quiet and every channel's feedback input is 0 (sample). Not saved: false after a
    // load or copy only costs one full sample.
    bool quietOutput;
    bool quietSkipping = true;  // for tests: false runs every quiet sample in full
    int32_t last[4];      // the engine's last two samples, left and right, mixed
    int32_t pendingRight;
    std::unique_ptr<Imx1Ymf768Core> core;
};

#endif
