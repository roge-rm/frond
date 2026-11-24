// The Zodiac's YMF768 model. On the model: the register answers the ROM's driver waits for, the power steps in 38
// on guest time and their save, the sequencer FIFO and its interrupt line, A4's pitch, the MA-2 operator fields,
// four-operator channels, a known note sequence against a stored hash, the
// FM engine running on guest time with or without SSI1 words and in any batching, a save or copy changing
// nothing in the run, a loaded copy carrying on the same, quiet idling giving a full run's samples and state,
// and bad saved values refused. Through the machine:
// FM added to SSI1's words the same in Exact and Fast with a state saved mid-note; a guest that takes the
// FIFO tick as an interrupt on GPIO B15 through the AITC from a wait (idle skips bounded by the next tick)
// with SSI1 off and then on, alike in Exact, Fast and Exact with stepped idle; a B15 change the GPIO refuses
// stopping the run; and an older state without the chip loading with B15 as it was, then a reset giving sound.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <utility>
#include <vector>
#include "CPU.h"
#include "imx1_boot_probe.h"
#include "machine_state_archive.h"
#include "zodiac_ymf768.h"

static void put(Imx1Ymf768& y, unsigned reg, unsigned value) { y.write(false, uint8_t(reg)); y.write(true, uint8_t(value)); }
static uint8_t get(Imx1Ymf768& y, unsigned reg) { y.write(false, uint8_t(reg)); return y.read(true); }

// The ROM's clocks: System PLL 96 MHz (32768 x 46875 / 16), SSI1 frames of 17 x 128 system clocks.
static constexpr uint64_t pllNumerator = 46875, pllDenominator = 16, frameClocks = 17 * 128;
static void frame(Imx1Ymf768& y, int32_t& l, int32_t& r) {
    y.advance(frameClocks, pllNumerator, pllDenominator);
    l = r = 0;
    y.output(l, r);
}

static void registers() {
    Imx1Ymf768 y;
    put(y, 0x0f, 1);
    assert(get(y, 0x0e) == 3);        // identity (0x200bd8a8)
    assert(get(y, 0x38) == 3);        // clock running and powered up from reset (0x200bdcfc)
    put(y, 0x00, 0x46);
    assert(!(get(y, 0x00) & 0x40));   // never busy (0x200c0ccc)
    put(y, 0x0f, 0);
    assert(get(y, 0x0f) == 0 && get(y, 0x08) == 1);  // empty FIFO asks for records
    // Six 01 ff 00 records, as the ROM primes it (0x200c02ac).
    for (unsigned i = 0; i < 6; ++i) { put(y, 0, 1); put(y, 0, 0xff); put(y, 0, 0); }
    assert(y.queuedRecords() == 6 && get(y, 0x08) == 0 && !y.interrupt());
    // 10 ms ticks (bank 1 register 2 code 4), the FIFO run by bank 0 register 6 bit 0.
    put(y, 0x0f, 1); put(y, 0x02, 0x44); put(y, 0x0f, 0); put(y, 0x06, 1);
    assert(!y.interrupt());
    // System PLL 32768 * 3000 Hz: 983040 clocks a tick. Records go one a tick; the line asserts with one left.
    const uint64_t tick = 983040;
    assert(!y.advance(tick - 1, 3000, 1) && y.queuedRecords() == 6);
    assert(y.clocksUntilChange(3000, 1) == 1);
    assert(!y.advance(1, 3000, 1) && y.queuedRecords() == 5);
    assert(!y.advance(3 * tick, 3000, 1) && y.queuedRecords() == 2);
    assert(y.advance(tick, 3000, 1) && y.queuedRecords() == 1 && y.interrupt() && get(y, 0x08) == 1);
    put(y, 0, 1); put(y, 0, 0xff); put(y, 0, 0);
    assert(!y.interrupt());
    // Many ticks in one advance empty the FIFO and no more.
    assert(y.advance(100 * tick, 3000, 1) && y.queuedRecords() == 0 && y.interrupt());
    put(y, 0x06, 0);
    assert(y.clocksUntilChange(3000, 1) == UINT64_MAX);
    for (unsigned i = 0; i < 2; ++i) { put(y, 0, 1); put(y, 0, 0xff); put(y, 0, 0); }
    assert(!y.advance(10 * tick, 3000, 1) && y.queuedRecords() == 2 && !y.interrupt());
}

// A 2-operator voice on channel ch: the modulator at TL tl, the carrier at full level, attack 15, release 5.
static void voice(Imx1Ymf768& y, unsigned ch, unsigned tl, unsigned alg) {
    static const uint8_t slots[16] = {0, 1, 2, 3, 8, 9, 10, 11, 16, 17, 18, 19, 24, 25, 26, 27};
    put(y, 0x0f, 0);
    put(y, 0xd0 + ch, alg);
    for (unsigned n = 0; n < 2; ++n) {
        const unsigned s = 0x10 + slots[ch] + 4 * n;
        // MA-2 fields: MULT 1 and EGT; RR 5, DR 2; AR 15, SL 2; TL; sine.
        put(y, s, 0x14); put(y, s + 0x20, 0x52); put(y, s + 0x40, 0xf2); put(y, s + 0x60, (n ? 0 : tl) << 2); put(y, s + 0x80, 0);
    }
}
static void key(Imx1Ymf768& y, unsigned ch, unsigned note, bool on) {
    // The ROM's F-number table (0x200d64bc) for note 69 (A4) is 580 in block 4; other notes by semitone ratio.
    static const uint16_t fnum[12] = {345, 365, 387, 410, 435, 460, 488, 517, 547, 580, 615, 651};
    const unsigned f = fnum[note % 12], block = note / 12 - 1;
    put(y, 0xb0 + ch, f & 0xff);
    put(y, 0xc0 + ch, (f >> 8) | block << 2 | (on ? 0x20 : 0));
}
static uint32_t fnv(uint32_t h, int32_t v) {
    for (unsigned i = 0; i < 4; ++i) h = (h ^ ((uint32_t(v) >> (8 * i)) & 0xff)) * 16777619u;
    return h;
}

static void pitch() {
    Imx1Ymf768 y;
    put(y, 0x0f, 1); put(y, 0x10, 16); put(y, 0x0f, 0);
    voice(y, 0, 63, 0);
    key(y, 0, 69, true);
    // One second at SSI1's 44117.6 frames: about 440 rising zero crossings of a plain sine.
    int32_t last = 0;
    unsigned crossings = 0, frames = 44118;
    int32_t peak = 0;
    for (unsigned i = 0; i < frames; ++i) {
        int32_t l, r;
        frame(y, l, r);
        assert(l == r);  // pan centre
        if (last < 0 && l >= 0) ++crossings;
        last = l; peak = std::max(peak, l);
    }
    fprintf(stderr, "A4: %u crossings, peak %d\n", crossings, peak);
    assert(crossings >= 438 && crossings <= 442 && peak > 2000);
}

// The operator bytes in the MA-2 layout the ROM's voices use: the carrier's MULT in group 0 bits 4-7 sets the
// pitch, its TL in group 3 bits 2-7 (not 0-5) sets the level, its WS in group 4 bits 0-2 the waveform, and a
// modulator with MULT 0 (x 0.5) at a low TL puts a subharmonic under A4.
static void ma2Fields() {
    struct Run { unsigned crossings; int32_t peak; int32_t tail; double halfBelow; };
    auto run = [](uint8_t carrier0, uint8_t carrier3, uint8_t carrier4, uint8_t mod0, unsigned modTl,
                    uint8_t carrier1 = 0x52, uint8_t carrier2 = 0xf2) {
        Imx1Ymf768 y;
        put(y, 0x0f, 1); put(y, 0x10, 16); put(y, 0x0f, 0);
        voice(y, 0, modTl, 0);
        put(y, 0x10, mod0); put(y, 0x14, carrier0); put(y, 0x34, carrier1); put(y, 0x54, carrier2); put(y, 0x74, carrier3); put(y, 0x94, carrier4);
        key(y, 0, 69, true);
        std::vector<int32_t> v;
        for (unsigned i = 0; i < 44118; ++i) { int32_t l, r; frame(y, l, r); v.push_back(l); }
        Run out{0, 0, 0, 0};
        for (size_t i = 1; i < v.size(); ++i) {
            if (v[i - 1] < 0 && v[i] >= 0) ++out.crossings;
            out.peak = std::max(out.peak, std::abs(v[i]));
            if (i >= v.size() / 2) out.tail = std::max(out.tail, std::abs(v[i]));
        }
        // Energy at 220 Hz against 440 Hz over the last half second (Goertzel on SSI1's 44117.6 frames).
        auto power = [&](double hz) {
            const double w = 2 * 3.141592653589793 * hz / 44117.647;
            double a = 0, b = 0;
            for (size_t i = v.size() / 2; i < v.size(); ++i) { const double c = v[i] + 2 * std::cos(w) * a - b; b = a; a = c; }
            return a * a + b * b - 2 * std::cos(w) * a * b;
        };
        out.halfBelow = power(220) / power(440);
        return out;
    };
    const Run plain = run(0x14, 0x00, 0, 0x14, 63);
    const Run octave = run(0x24, 0x00, 0, 0x14, 63);
    const Run ksl = run(0x14, 0x03, 0, 0x14, 63);    // KSL 3 only: OPL3 key scaling, far above TL 63
    const Run quiet = run(0x14, 0xfc, 0, 0x14, 63);  // TL 63 in bits 2-7
    const Run tlHigh = run(0x14, 0xc0, 0, 0x14, 63);  // TL 48 in bits 2-7: quiet; read from bits 0-5 it would be 0
    const Run tlLow = run(0x14, 0x3c, 0, 0x14, 63);   // TL 15: mid; read from bits 0-5 it would be 60
    const Run half = run(0x14, 0x00, 1, 0x14, 63);   // half sine: positive halves only
    const Run decay = run(0x14, 0x00, 0, 0x14, 63, 0x5f, 0xf3);  // DR 15, SL 3: falls to SL and holds
    const Run sub = run(0x14, 0x00, 0, 0x04, 8);     // modulator MULT 0 at TL 8
    fprintf(stderr, "tails plain %d decay %d tl %d %d\n", plain.tail, decay.tail, tlHigh.peak, tlLow.peak);
    fprintf(stderr, "MA-2 fields: crossings %u %u, peaks %d %d %d %d, 220/440 %.3f %.3f\n", plain.crossings,
            octave.crossings, plain.peak, ksl.peak, quiet.peak, half.peak, plain.halfBelow, sub.halfBelow);
    assert(plain.crossings >= 438 && plain.crossings <= 442 && octave.crossings >= 876 && octave.crossings <= 884);
    assert(plain.peak > 2000 && ksl.peak > quiet.peak * 10 && quiet.peak < plain.peak / 50);
    assert(ksl.peak < plain.peak / 2);
    assert(tlHigh.peak < plain.peak / 20);
    assert(tlLow.peak > plain.peak / 8 && tlLow.peak < plain.peak / 2);
    // DR 15 to SL 3 (9 dB, about 0.35 of plain's level) holds; DR and SL swapped it reads about 0.45.
    assert(decay.tail > plain.peak * 30 / 100 && decay.tail < plain.peak * 40 / 100);
    assert(half.crossings < 5);
    assert(plain.halfBelow < 0.001 && sub.halfBelow > 0.01);
}

// The raw operator bytes land on OPL3's registers bit for bit, read back from the saved engine (11 bytes of
// engine, 9 of OPL3 LFO state, then the registers). Slot 0 is channel 0's modulator (OPL3 operator 0), slot 4
// its carrier (3), slot 8 channel 4's (OPL3 channel 3, operator 8).
static uint8_t oplReg(Imx1Ymf768& y, unsigned reg) { return y.engineState()[11 + 9 + reg]; }
static void operatorMapping() {
    Imx1Ymf768 y;
    auto setSlot = [&](unsigned s, uint8_t g0, uint8_t g1, uint8_t g2, uint8_t g3, uint8_t g4) {
        put(y, 0x10 + s, g0); put(y, 0x30 + s, g1); put(y, 0x50 + s, g2); put(y, 0x70 + s, g3); put(y, 0x90 + s, g4);
    };
    struct Case { unsigned slot, op; uint8_t g[5], r20, r40, r60, r80, re0; };
    const Case cases[] = {
        // Each group one bit at a time: MULT 5 and VIB; EGT; SUS and KSR; AM; DVB and DAM ignored.
        {0, 0, {0x58, 0x00, 0x00, 0x00, 0x08}, 0xc5, 0x00, 0x00, 0x00, 0x00},
        {0, 0, {0x04, 0x00, 0x00, 0x00, 0x00}, 0x20, 0x00, 0x00, 0x00, 0x00},
        {0, 0, {0x02, 0x00, 0x00, 0x00, 0xf0}, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0, 0, {0x01, 0x00, 0x00, 0x00, 0x00}, 0x10, 0x00, 0x00, 0x00, 0x00},
        // RR 9 DR 4, AR 11 SL 6, TL 37 KSL 2, WS 5: all different, so a swap shows.
        {4, 3, {0x00, 0x94, 0xb6, 0x96, 0x05}, 0x00, 0x80 | 37, 0xb4, 0x69, 0x05},
        {8, 8, {0x3c, 0x12, 0x34, 0xfd, 0x02}, 0x63, 0x7f, 0x32, 0x41, 0x02},
    };
    for (const Case& c : cases) {
        setSlot(c.slot, c.g[0], c.g[1], c.g[2], c.g[3], c.g[4]);
        // Channel 3 of OPL3 (slot 8, ymf channel 4) is operator offset 8.
        const unsigned base = c.op;
        assert(oplReg(y, 0x20 + base) == c.r20 && oplReg(y, 0x40 + base) == c.r40);
        assert(oplReg(y, 0x60 + base) == c.r60 && oplReg(y, 0x80 + base) == c.r80 && oplReg(y, 0xe0 + base) == c.re0);
    }
}

// Four-operator channels sound, on an OPL3 pair (0, 8) and on an emulated one (3, 11), and only through the
// first channel's key.
static void fourOperators() {
    for (unsigned first : {0u, 3u, 8u, 11u}) {
        for (unsigned alg : {4u, 5u, 6u, 7u}) {
            Imx1Ymf768 y;
            put(y, 0x0f, 1); put(y, 0x10, 31); put(y, 0x0f, 0);
            voice(y, first, 16, 0); voice(y, first + 4, 16, 0); put(y, 0xd0 + first, alg);
            auto peak = [&](unsigned frames) {
                int32_t most = 0;
                for (unsigned i = 0; i < frames; ++i) { int32_t l, r; frame(y, l, r); most = std::max(most, std::abs(l)); }
                return most;
            };
            key(y, first + 4, 60, true);
            assert(peak(2000) == 0);
            key(y, first, 60, true);
            assert(peak(2000) > 1000);
        }
    }
}

// A known sequence: three two-operator channels, FM and additive, and two four-operator ones, with key offs
// and a master level change.
static uint32_t sequence() {
    Imx1Ymf768 y;
    put(y, 0x0f, 1); put(y, 0x10, 31); put(y, 0x0f, 0);
    voice(y, 0, 20, 0x08); voice(y, 1, 30, 0x01); voice(y, 2, 10, 0x38);
    put(y, 0xf1, 20); put(y, 0xf2, 110);
    // Four operators: 8 with 12 on an OPL3 pair (MA-3 algorithm 5), 3 with 7 as two FM pairs added (4).
    voice(y, 8, 16, 0); voice(y, 12, 16, 0); put(y, 0xd8, 0x05);
    voice(y, 3, 16, 0); voice(y, 7, 16, 0); put(y, 0xd3, 0x14);
    uint32_t h = 2166136261u;
    auto run = [&](unsigned frames) {
        for (unsigned i = 0; i < frames; ++i) { int32_t l, r; frame(y, l, r); h = fnv(fnv(h, l), r); }
    };
    key(y, 0, 60, true); run(3000);
    key(y, 1, 64, true); run(3000);
    key(y, 2, 67, true); run(3000);
    key(y, 8, 48, true); key(y, 3, 72, true); run(3000);
    key(y, 8, 48, false); key(y, 3, 72, false); run(2000);
    key(y, 0, 60, false); run(2000);
    put(y, 0x0f, 1); put(y, 0x10, 12); put(y, 0x0f, 0);
    key(y, 1, 64, false); key(y, 2, 67, false); run(6000);
    return h;
}

// A note keyed and left 0.3 guest seconds with no output taken comes out where one sampled every SSI1 frame
// does, in samples made and in what it plays next; and the same in any batching of the clocks.
static Imx1Ymf768 heldNote() {
    Imx1Ymf768 y;
    put(y, 0x0f, 1); put(y, 0x10, 31); put(y, 0x0f, 0);
    voice(y, 0, 24, 0);
    key(y, 0, 57, true);
    return y;
}
static void guestTime() {
    const uint64_t clocks = 28800000;  // 0.3 s at 96 MHz
    Imx1Ymf768 silent = heldNote(), framed = heldNote(), odd = heldNote();
    silent.advance(clocks, pllNumerator, pllDenominator);
    uint64_t done = 0;
    for (; done + frameClocks <= clocks; done += frameClocks) { int32_t l, r; frame(framed, l, r); }
    framed.advance(clocks - done, pllNumerator, pllDenominator);
    for (uint64_t left = clocks, step = 1; left; step = step * 7 % 9973 + 1) {
        const uint64_t n = std::min(left, step);
        odd.advance(n, pllNumerator, pllDenominator);
        left -= n;
    }
    // 0.3 s of 49716 a second.
    assert(silent.engineSamples() == 14914 && framed.engineSamples() == 14914 && odd.engineSamples() == 14914);
    for (unsigned i = 0; i < 4000; ++i) {
        int32_t a[2], b[2], c[2];
        frame(silent, a[0], a[1]); frame(framed, b[0], b[1]); frame(odd, c[0], c[1]);
        assert(a[0] == b[0] && a[1] == b[1] && a[0] == c[0] && a[1] == c[1]);
    }
    // The note is part way through its envelope: not what a freshly keyed one plays.
    Imx1Ymf768 fresh = heldNote();
    bool differs = false;
    for (unsigned i = 0; i < 400; ++i) {
        int32_t a[2], b[2];
        frame(silent, a[0], a[1]); frame(fresh, b[0], b[1]);
        differs = differs || a[0] != b[0];
    }
    assert(differs);
}

// Saving (state() or a copy) changes nothing in the run, through key offs, quiet channels and ymfm's
// periodic prepare; a copy loaded from a save part way carries on sample for sample.
static Imx1Ymf768 threeVoices() {
    Imx1Ymf768 y;
    put(y, 0x0f, 1); put(y, 0x10, 31); put(y, 0x0f, 0);
    for (unsigned ch : {0u, 1u, 2u}) { voice(y, ch, 20, 0); put(y, 0xe0 + ch, 31); }
    return y;
}
static void saveHasNoEffect() {
    Imx1Ymf768 plain = threeVoices(), saved = threeVoices();
    std::vector<uint8_t> mid;
    unsigned saves = 0;
    auto save = [&](bool keep) {
        std::vector<uint8_t> bytes;
        StateWriter w(bytes);
        saved.state(w);
        const Imx1Ymf768 copy(saved);
        (void)copy;
        ++saves;
        if (keep) mid = bytes;
    };
    std::vector<int32_t> out[2];
    auto run = [&](unsigned frames, unsigned saveEvery) {
        for (unsigned i = 0; i < frames; ++i) {
            int32_t l, r;
            frame(plain, l, r); out[0].push_back(l); out[0].push_back(r);
            frame(saved, l, r); out[1].push_back(l); out[1].push_back(r);
            if (saveEvery && i % saveEvery == saveEvery - 1) save(false);
        }
    };
    for (auto* y : {&plain, &saved}) { key(*y, 0, 60, true); key(*y, 1, 64, true); }
    save(false);  // straight after register writes
    run(3000, 997);
    for (auto* y : {&plain, &saved}) key(*y, 0, 60, false);
    save(false);
    run(9000, 1499);  // channel 0 releases to quiet; ymfm's 4096 sample sweep passes
    for (auto* y : {&plain, &saved}) { key(*y, 2, 67, true); key(*y, 1, 64, false); }
    run(500, 0);
    save(true);  // well after the last write: no channel waits for a prepare
    const size_t from = out[1].size();
    run(12000, 2003);
    assert(saves > 10 && out[0] == out[1] && plain.engineState() == saved.engineState());
    // A model loaded from the save at `from` makes the same samples as the one it was saved from.
    Imx1Ymf768 loaded;
    StateReader reader(mid.data(), mid.size());
    loaded.state(reader);
    assert(reader.complete());
    for (size_t i = from; i < out[1].size(); i += 2) {
        int32_t l, r;
        frame(loaded, l, r);
        assert(l == out[1][i] && r == out[1][i + 1]);
    }
    assert(loaded.engineState() == saved.engineState());
}

// Quiet idling: once every operator is past ymfm's quiet level the model skips the output calls but still
// clocks the engine. Against a model that runs every sample in full: the same samples and engine state through
// silence from the start, notes, long silences (ymfm's 4096 sample sweep included), a four-operator change and
// a pan write while quiet, a key on with a slow attack and two saves loaded mid-silence; and most silent
// samples are skipped.
static Imx1Ymf768 quietVoices(bool skipping) {
    Imx1Ymf768 y;
    y.setQuietSkipping(skipping);
    put(y, 0x0f, 1); put(y, 0x10, 31); put(y, 0x0f, 0);
    voice(y, 0, 20, 0x08); voice(y, 1, 30, 0x01); voice(y, 8, 16, 0); voice(y, 12, 16, 0);
    // Release rate 10 (group 1, decay 2 as before), so notes end within the script.
    for (unsigned slot : {0x30u, 0x34u, 0x31u, 0x35u, 0x40u, 0x44u, 0x48u, 0x4cu}) put(y, slot, 0xa2);
    return y;
}
// Plays the script from frame `start` to the end, with the writes due at each frame, saving at `saveAt`.
static void quietScript(Imx1Ymf768& y, unsigned start, std::vector<int32_t>& out, unsigned saveAt,
                        std::vector<uint8_t>* saved) {
    static constexpr unsigned end = 36000;
    for (unsigned f = start; f < end; ++f) {
        switch (f) {
            case 2000: key(y, 0, 60, true); key(y, 1, 64, true); break;
            case 5000: key(y, 0, 60, false); key(y, 1, 64, false); break;
            case 14000: put(y, 0xd8, 0x05); put(y, 0xf0, 20); break;  // four operators on 8 and 12, a pan
            case 16000:  // a slow attack (rate 1, group 2 with SL 2) stays quiet for a while after its key on
                put(y, 0x50, 0x12); put(y, 0x54, 0x12); key(y, 0, 62, true); break;
            case 20000: key(y, 8, 48, true); break;
            case 23000: key(y, 0, 62, false); key(y, 8, 48, false); break;
        }
        if (saved && f == saveAt) { StateWriter w(*saved); y.state(w); }
        int32_t l, r;
        frame(y, l, r);
        out.push_back(l); out.push_back(r);
    }
}
static void quietIdle() {
    Imx1Ymf768 full = quietVoices(false), quick = quietVoices(true);
    std::vector<int32_t> fullOut, quickOut;
    std::vector<uint8_t> early, late;
    quietScript(full, 0, fullOut, 0, nullptr);
    quietScript(quick, 0, quickOut, 1000, &early);
    assert(fullOut == quickOut && full.engineState() == quick.engineState());
    assert(full.engineQuietSamples() == 0);
    fprintf(stderr, "quiet idle: %llu of %llu samples skipped\n", (unsigned long long)quick.engineQuietSamples(),
            (unsigned long long)quick.engineSamples());
    assert(quick.engineQuietSamples() > quick.engineSamples() / 2);
    // The speed log's sounding count: the same whether quiet samples are skipped or not, none before the first
    // key on, and every sample is either sounding or quiet.
    assert(full.engineSoundingSamples() == quick.engineSoundingSamples());
    assert(quick.engineSoundingSamples() > 0 && quick.engineSoundingSamples() + quick.engineQuietSamples() <= quick.engineSamples());
    {
        Imx1Ymf768 opening = quietVoices(true);
        for (unsigned f = 0; f < 1500; ++f) { int32_t l, r; frame(opening, l, r); }
        assert(opening.engineSamples() > 0 && opening.engineSoundingSamples() == 0);
    }
    // Saves in the opening silence and mid-silence after the release, loaded, carry on the same.
    {
        Imx1Ymf768 again = quietVoices(true);
        std::vector<int32_t> unused;
        quietScript(again, 0, unused, 12000, &late);
    }
    for (auto [bytes, at] : {std::pair{&early, 1000u}, std::pair{&late, 12000u}}) {
        Imx1Ymf768 loaded;
        StateReader reader(bytes->data(), bytes->size());
        loaded.state(reader);
        assert(reader.complete());
        std::vector<int32_t> out;
        quietScript(loaded, at, out, 0, nullptr);
        assert(std::equal(out.begin(), out.end(), fullOut.begin() + 2 * at) && loaded.engineState() == full.engineState());
    }
}

// A state whose engine holds operator registers as an older mapping wrote them plays as a fresh sync of its
// raw registers does, and one that already matches loads unchanged.
static void oldStateResync() {
    Imx1Ymf768 good = threeVoices();
    key(good, 0, 60, true); key(good, 1, 64, true);
    for (unsigned i = 0; i < 200; ++i) { int32_t l, r; frame(good, l, r); }
    std::vector<uint8_t> goodBytes, oldBytes;
    StateWriter wg(goodBytes);
    good.state(wg);
    // The same raw registers with the engine's operator registers as another mapping left them.
    oldBytes = goodBytes;
    const size_t engine = oldBytes.size() - good.engineState().size();
    for (unsigned op : {0u, 1u, 2u, 3u, 4u, 5u, 8u, 9u, 10u, 11u, 12u, 13u})
        for (unsigned base : {0x20u, 0x40u, 0x60u, 0x80u, 0xe0u}) oldBytes[engine + 11 + 9 + base + op] = 0x5a;
    assert(goodBytes != oldBytes);
    Imx1Ymf768 a, b;
    StateReader ra(goodBytes.data(), goodBytes.size()), rb(oldBytes.data(), oldBytes.size());
    a.state(ra); b.state(rb);
    assert(ra.complete() && rb.complete());
    // The good one loads as saved, and the old one comes out as the good one's engine registers.
    for (unsigned op : {0u, 3u, 8u, 11u})
        for (unsigned base : {0x20u, 0x40u, 0x60u, 0x80u, 0xe0u}) assert(oplReg(b, base + op) == oplReg(a, base + op));
    for (unsigned i = 0; i < 3000; ++i) {
        int32_t l1, r1, l2, r2, l3, r3;
        frame(good, l1, r1); frame(a, l2, r2); frame(b, l3, r3);
        assert(l1 == l2 && r1 == r2 && l2 == l3 && r2 == r3);
    }
}

// Bad values in a saved engine or model are refused before the engine sees them.
static void badState() {
    // ymfm's layout: 11 bytes of engine, 9 + 0x200 of OPL3 registers, 18 channels of 6, then 36 operators
    // of 10 (phase 4, attenuation 2, envelope state 1, SSG 1, key 1, key on 1), masks, prepare count, map.
    constexpr size_t operators = 11 + 9 + 0x200 + 18 * 6, extra = operators + 36 * 10, map = extra + 12;
    Imx1Ymf768 y;
    const auto good = y.engineState();
    assert(good.size() == map + 18 * 4 && Imx1Ymf768::engineStateValid(good));
    for (unsigned n = 0; n < 36; ++n)  // reset leaves every operator released and silent
        assert(good[operators + 10 * n + 6] == 4 && good[operators + 10 * n + 4] == 0xff && good[operators + 10 * n + 5] == 3);
    auto refused = [&](size_t at, uint8_t value) {
        auto bad = good;
        bad[at] = value;
        return !Imx1Ymf768::engineStateValid(bad);
    };
    assert(refused(operators + 10 * 5 + 6, 6));        // envelope state past eg_rate[]
    assert(refused(operators + 10 * 5 + 6, 0));        // OPL3 has no depress state
    assert(refused(operators + 10 * 5 + 5, 4));        // attenuation over 10 bits
    assert(refused(12, 0x40));                         // AM LFO counter past its 210 x 64 steps
    assert(refused(map + 3 * 4, 0));                   // an operator in two channels
    assert(refused(map + 2, 6));                       // half a four-operator map
    assert(refused(extra + 2, 0x04));                  // active mask past the channels
    assert(refused(0, 1));                             // envelope counter sub-steps
    auto shorter = good; shorter.pop_back();
    assert(!Imx1Ymf768::engineStateValid(shorter));
    // Through state(): the engine is the section's last bytes.
    std::vector<uint8_t> bytes;
    StateWriter w(bytes);
    y.state(w);
    {
        Imx1Ymf768 into;
        StateReader r(bytes.data(), bytes.size());
        into.state(r);
        assert(r.complete());
    }
    auto bad = bytes;
    bad[bad.size() - good.size() + operators + 10 * 7 + 6] = 9;
    Imx1Ymf768 into;
    StateReader r(bad.data(), bad.size());
    into.state(r);
    assert(!r.ok());
    bad = bytes;
    bad[256 * 2 + 2] = 3;  // three records queued with no wait
    StateReader r2(bad.data(), bad.size());
    into.state(r2);
    assert(!r2.ok());
}

// The power steps in bank 1 38 as the ROM's power routine (200bd468) writes 30: the clock (bit 0) 2 ms after 30 bit 0
// clears, powered up (bit 1) 10 ms after bits 3 and 4 clear, powered down (bit 2) 2 ms after they set, on guest time
// whatever the System PLL. A step saved under way loads and finishes at the same time; a state without the ready
// section loads with every step done, and bad ready values are refused.
static void powerSteps() {
    const uint64_t ms = 32768ull * pllNumerator / pllDenominator / 1000;  // 96000 System PLL clocks
    auto ready = [](Imx1Ymf768& y) { return get(y, 0x38); };
    Imx1Ymf768 y;
    put(y, 0x0f, 1);
    // Init (200bd92c): 30 = fe (bit 0 clear, 3 and 4 set) starts powering down; 38 bit 0 stays set.
    put(y, 0x30, 0xfe); put(y, 0x31, 3);
    assert(ready(y) == 1 && y.readyBusy());
    y.advance(2 * ms - 1, pllNumerator, pllDenominator);
    assert(ready(y) == 1);
    y.advance(1, pllNumerator, pllDenominator);
    assert(ready(y) == 5 && !y.readyBusy());
    // Sleep (200bd848): bit 0 set stops the clock at once.
    put(y, 0x30, 0xff);
    assert(ready(y) == 4 && !y.readyBusy());
    // Wake (200bd4bc, 200bd514): the clock 2 ms after bit 0 clears, then powered up 10 ms after 3 and 4 clear.
    put(y, 0x30, 0xfe);
    assert(ready(y) == 4 && y.readyBusy());
    y.advance(2 * ms, pllNumerator, pllDenominator);
    assert(ready(y) == 5);
    put(y, 0x30, 0xe6);
    assert(ready(y) == 1 && y.readyBusy());
    // Saved 4 ms in, at another PLL ratio: the time is guest time, so a half-speed PLL takes half the clocks.
    y.advance(4 * ms, pllNumerator, pllDenominator);
    std::vector<uint8_t> chip, steps;
    { StateWriter w(chip); y.state(w); }
    { StateWriter w(steps); y.stateReady(w); }
    Imx1Ymf768 loaded;
    { StateReader r(chip.data(), chip.size()); loaded.state(r); assert(r.complete()); }
    assert(get(loaded, 0x38) == 3 && !loaded.readyBusy());  // no ready section: done
    { StateReader r(chip.data(), chip.size()); loaded.state(r); assert(r.complete()); }
    { StateReader r(steps.data(), steps.size()); loaded.stateReady(r); assert(r.complete()); }
    for (auto* m : {&y, &loaded}) {
        assert(get(*m, 0x38) == 1);
        m->advance(3 * ms - 1, pllNumerator, pllDenominator * 2);
        assert(get(*m, 0x38) == 1);
        m->advance(1, pllNumerator, pllDenominator * 2);
        assert(get(*m, 0x38) == 3 && !m->readyBusy());
    }
    // Setting 3 and 4 again before the step ends drops it and starts powering down.
    put(y, 0x30, 0xfe); put(y, 0x30, 0xe6);
    y.advance(ms, pllNumerator, pllDenominator);
    put(y, 0x30, 0xfe);
    assert(ready(y) == 1 && y.readyBusy());
    y.advance(2 * ms, pllNumerator, pllDenominator);
    assert(ready(y) == 5 && !y.readyBusy());
    // Bad values: a step whose bit 30 wouldn't turn on, or time on a step not under way.
    put(y, 0x30, 0xe6);
    std::vector<uint8_t> good;
    { StateWriter w(good); y.stateReady(w); }
    auto refused = [&](size_t at, uint8_t value) {
        auto bad = good;
        bad[at] = value;
        Imx1Ymf768 into(y);
        StateReader r(bad.data(), bad.size());
        into.stateReady(r);
        return !r.ok();
    };
    assert(!refused(0, 2));
    assert(refused(0, 4) && refused(0, 8) && refused(1 + 8 * 2, 1));
    assert(y.readyRegisterReads() > 10);
}

// A System PLL numerator change in the middle of a step: the time counted so far stays the same guest time, also
// in a copy loaded from a state saved before the change.
static void powerStepsPllChange() {
    const uint64_t ms = 32768ull * pllNumerator / pllDenominator / 1000;  // 96000 System PLL clocks
    Imx1Ymf768 y;
    put(y, 0x0f, 1);
    put(y, 0x30, 0xff); put(y, 0x30, 0xe6);  // clock (2 ms) and power up (10 ms) under way
    assert(get(y, 0x38) == 0 && y.readyBusy());
    y.advance(ms, pllNumerator, pllDenominator);
    std::vector<uint8_t> chip, steps;
    { StateWriter w(chip); y.state(w); }
    { StateWriter w(steps); y.stateReady(w); }
    Imx1Ymf768 loaded;
    { StateReader r(chip.data(), chip.size()); loaded.state(r); assert(r.complete()); }
    { StateReader r(steps.data(), steps.size()); loaded.stateReady(r); assert(r.complete()); }
    // Twice the numerator: twice the clocks a millisecond.
    for (auto* m : {&y, &loaded}) {
        m->advance(2 * ms - 1, pllNumerator * 2, pllDenominator);
        assert(get(*m, 0x38) == 0);
        m->advance(1, pllNumerator * 2, pllDenominator);
        assert(get(*m, 0x38) == 1);
        m->advance(8 * 2 * ms - 1, pllNumerator * 2, pllDenominator);
        assert(get(*m, 0x38) == 1);
        m->advance(1, pllNumerator * 2, pllDenominator);
        assert(get(*m, 0x38) == 3 && !m->readyBusy());
    }
    // A numerator that doesn't divide evenly rounds the time so far down, so the step ends a clock late at most.
    put(y, 0x30, 0xff); put(y, 0x30, 0xe6);
    y.advance(ms / 3, pllNumerator, pllDenominator);
    const uint64_t odd = pllNumerator * 7 / 3 + 1;  // 109376
    const uint64_t left = (32768ull * odd * 2 - (ms / 3) * pllDenominator * 1000 * odd / pllNumerator) /
                          (pllDenominator * 1000);
    y.advance(left - 1, odd, pllDenominator);
    assert(get(y, 0x38) == 0);
    y.advance(2, odd, pllDenominator);
    assert(get(y, 0x38) == 1);
}

// Guest programs built from words with a literal pool at the end.
struct Program {
    std::vector<uint32_t> code;
    std::vector<std::pair<size_t, uint32_t>> literals;
    void load(unsigned reg, uint32_t value) { literals.emplace_back(code.size(), value); code.push_back(0xe59f0000 | reg << 12); }
    void store(uint32_t address, uint32_t value) { load(0, address); load(1, value); code.push_back(0xe5801000); }
    void storeByte(uint32_t address, uint32_t value) { load(0, address); load(1, value); code.push_back(0xe5c01000); }
    void ymf(unsigned reg, unsigned value) { storeByte(0x15000000, reg); storeByte(0x15000001, value); }
    size_t here() const { return code.size(); }
    // A branch with condition cond (0xe always) from here to word target.
    void branch(size_t target, uint32_t cond = 0xe) {
        code.push_back(cond << 28 | 0x0a000000 | (uint32_t(int32_t(target) - int32_t(code.size()) - 2) & 0xffffff));
    }
    void patchBranch(size_t at, size_t target) {
        code[at] = (code[at] & 0xff000000) | (uint32_t(int32_t(target) - int32_t(at) - 2) & 0xffffff);
    }
    void finish() {
        for (const auto& literal : literals) {
            code[literal.first] |= uint32_t((code.size() - literal.first - 2) * 4);
            code.push_back(literal.second);
        }
    }
    // Zeros played over and over through DMA channel 10 into SSI1 (as tests/zodiac-sound.cpp).
    void startPcm() {
        constexpr uint32_t ssi = 0x218000, dma = 0x209000, ch = 0x209300;
        for (uint32_t i = 0; i < 16; ++i) store(0x300000 + i * 4, 0);
        store(dma, 2); store(dma, 1); store(dma + 8, 0x3ff); store(dma + 0x1c, 0xffff);
        store(ch + 0xc, 8); store(ch + 0x10, 16); store(ch + 0xc, 0x2088);
        store(ch + 0x14, 8); store(ch + 0xc, 0x208c); store(ch + 0x18, 0);
        store(ch + 8, 64); store(ch + 4, ssi);
        store(ssi + 8, 0); store(ssi + 0x28, 0); store(ssi + 0x10, 0); store(ssi + 0x18, 0);
        store(ssi + 8, 0x9900); store(ssi + 0x14, 0x6100); store(ssi + 0x20, 4); store(ssi + 0xc, 0xe8);
        for (unsigned i = 0; i < 8; ++i) store(ssi, 0);
        store(ch, 0x300000); store(ch + 0xc, 0x208c); store(ch + 0xc, 0x208d);
        store(dma + 4, 0x400); store(ssi + 0xc, 0x2e8); store(ssi + 8, 0x9b00);
    }
    // A two-operator note on channel 0 with every level up.
    void note() {
        ymf(0x0f, 1); ymf(0x10, 31); ymf(0x0f, 0);
        for (unsigned n = 0; n < 2; ++n) {
            const unsigned s = 0x10 + 4 * n;
            ymf(s, 0x14); ymf(s + 0x20, 0x52); ymf(s + 0x40, 0xf2); ymf(s + 0x60, n ? 0 : 24 << 2); ymf(s + 0x80, 0);
        }
        ymf(0xd0, 0x10); ymf(0xb0, 580 & 0xff); ymf(0xc0, 2 | 4 << 2 | 0x20);
    }
    void record() { ymf(0x00, 1); storeByte(0x15000001, 0xff); storeByte(0x15000001, 0); }
    const uint8_t* rom() const { return reinterpret_cast<const uint8_t*>(code.data()); }
    uint32_t size() const { return uint32_t(code.size() * 4); }
};

// PCM zeros, the note on CS4, then spin; with readB, the spin reads GPIO port B's pins into r7.
static Program noteGuest(bool note, bool readB = false) {
    Program g;
    if (readB) {
        g.load(2, 0x21c124);
        g.code.push_back(0xe5928000);  // ldr r8, [r2]: SSR_B before the chip is touched
    }
    g.startPcm();
    if (note) g.note();
    if (readB) {
        g.load(2, 0x21c124);
        g.code.push_back(0xe5927000);  // ldr r7, [r2]
        g.branch(g.here() - 1);
    } else g.code.push_back(0xeafffffe);  // b .
    g.finish();
    return g;
}

struct Capture {
    std::vector<uint8_t> bytes;  // word low, word high, slot
    static void hook(void* context, uint16_t word, unsigned slot) {
        auto& c = *static_cast<Capture*>(context);
        c.bytes.push_back(uint8_t(word)); c.bytes.push_back(uint8_t(word >> 8)); c.bytes.push_back(uint8_t(slot));
    }
    size_t nonzero() const {
        size_t n = 0;
        for (size_t i = 0; i + 2 < bytes.size(); i += 3) n += (bytes[i] | bytes[i + 1]) != 0;
        return n;
    }
};
static Imx1Machine* create(const Program& g, bool fast, bool chip, Imx1ProbeEntry entry = Imx1ProbeEntry::physicalFlash,
                           const Imx1ProbeOptions* given = nullptr) {
    Imx1ProbeOptions o{};
    if (given) o = *given;
    o.bootstrapPeripherals = true;
    o.cs4Port0InputKnown = true;
    auto* m = imx1MachineCreate(g.rom(), g.size(), entry, false, o);
    assert(m);
    if (chip) imx1MachineEnableYmf768(m);
    if (fast) assert(imx1MachineSetFastEngine(m, true) && imx1MachineFastEngine(m));
    return m;
}
static Imx1MachineRun run(Imx1Machine* m, uint32_t instructions, uint32_t slice) {
    Imx1MachineRun r{};
    while (instructions) {
        const uint32_t piece = instructions < slice ? instructions : slice;
        r = imx1MachineRunSlice(m, piece, 100000);
        if (r.status != Imx1MachineStatus::yielded) {
            fprintf(stderr, "stopped: %s pc %08x address %08x\n", imx1ProbeStopName(r.result.stop), r.result.trace.pc, r.result.address);
            assert(false);
        }
        instructions -= piece;
    }
    return r;
}

static void machine() {
    const Program guest = noteGuest(true);
    std::vector<uint8_t> streams[2], resumed[2];
    for (unsigned fast = 0; fast < 2; ++fast) {
        Capture c;
        auto* m = create(guest, fast, true);
        imx1MachineSetPcmOutput(m, Capture::hook, &c);
        run(m, 600000, fast ? 7919 : 5000);
        // The note sounds in the words while the guest's own PCM is all zeros.
        fprintf(stderr, "%s: %zu words, %zu not zero\n", fast ? "fast" : "exact", c.bytes.size() / 3, c.nonzero());
        assert(c.nonzero() > 100);
        // Saved mid-note: a machine loading it makes the same words as this one going on.
        std::vector<uint8_t> state;
        assert(imx1MachineSaveState(m, state) == Imx1StateStatus::ok);
        const size_t w1 = c.bytes.size();
        run(m, 300000, fast ? 7919 : 5000);
        resumed[fast].assign(c.bytes.begin() + w1, c.bytes.end());
        streams[fast] = c.bytes;
        imx1MachineStop(m);
        Capture d;
        auto* loaded = create(guest, fast, true);
        imx1MachineSetPcmOutput(loaded, Capture::hook, &d);
        assert(imx1MachineLoadState(loaded, state.data(), state.size()).status == Imx1StateStatus::ok);
        run(loaded, 300000, 5000);
        assert(d.bytes == resumed[fast] && d.nonzero() > 100);
        imx1MachineStop(loaded);
    }
    // Exact and Fast make the same words.
    assert(streams[0] == streams[1] && resumed[0] == resumed[1]);
}

// The FM left out of the PCM output: the words are the guest's own (all zeros here), and the saved state is the
// same as a machine playing the FM; a state load keeps it muted.
static void fmMuted() {
    const Program guest = noteGuest(true);
    for (unsigned fast = 0; fast < 2; ++fast) {
        // One machine at a time.
        Capture plain, muted;
        std::vector<uint8_t> sa, sb;
        auto* a = create(guest, fast, true);
        imx1MachineSetPcmOutput(a, Capture::hook, &plain);
        run(a, 600000, fast ? 7919 : 5000);
        assert(imx1MachineSaveState(a, sa) == Imx1StateStatus::ok);
        imx1MachineStop(a);
        auto* b = create(guest, fast, true);
        imx1MachineSetPcmOutput(b, Capture::hook, &muted);
        imx1MachineSetFmOutputMuted(b, true);
        run(b, 600000, fast ? 7919 : 5000);
        assert(plain.nonzero() > 100 && muted.nonzero() == 0 && plain.bytes.size() == muted.bytes.size());
        assert(imx1MachineSaveState(b, sb) == Imx1StateStatus::ok && sa == sb);
        assert(imx1MachineLoadState(b, sa.data(), sa.size()).status == Imx1StateStatus::ok);
        const size_t before = muted.bytes.size();
        run(b, 300000, 5000);
        assert(muted.bytes.size() > before && muted.nonzero() == 0);
        imx1MachineSetFmOutputMuted(b, false);
        run(b, 300000, 5000);
        assert(muted.nonzero() > 100);
        imx1MachineStop(b);
    }
}

// The ROM's way of running the chip, from the reset vector: FIFO primed with six records and 10 ms ticks,
// GPIO B15 a low level interrupt through AITC source 12, the note keyed, then wait for interrupt. Each
// interrupt adds a record, clears B15's status and counts in r6. After quietTicks interrupts the PCM starts;
// after allTicks the guest stops on an undefined instruction.
static constexpr unsigned quietTicks = 4, allTicks = 9;
static Program irqGuest() {
    Program g;
    g.code.push_back(0xea000000);                        // 0x00 b start
    for (unsigned i = 1; i < 6; ++i) g.code.push_back(0xeafffffe);
    g.code.push_back(0xea000000);                        // 0x18 b handler
    g.code.push_back(0xeafffffe);
    g.patchBranch(0, g.here());
    g.store(0x21b000, 0x57000003);                       // CSCR as the ROM leaves it
    g.code.push_back(0xe3a06000);                        // mov r6, #0
    g.ymf(0x0f, 1); g.ymf(0x02, 0x44); g.ymf(0x0f, 0);
    for (unsigned i = 0; i < 6; ++i) g.record();
    g.note();
    g.store(0x21c128, 0xc0000000);                       // ICR1_B: B15 low level
    g.store(0x21c130, 0x8000);                           // IMR_B
    g.store(0x223008, 12);                               // AITC INTENNUM: GPIO port B
    g.ymf(0x06, 1);                                      // run the FIFO
    g.code.push_back(0xe321f013);                        // msr cpsr_c, #0x13: IRQs on
    const size_t quiet = g.here();
    g.code.push_back(0xee070f90);                        // wait for interrupt
    g.code.push_back(0xe3560000 | quietTicks);           // cmp r6, #quietTicks
    g.branch(quiet, 0xb);                                // blt
    g.code.push_back(0xe321f093);                        // IRQs off while r0, r1 are in use
    g.startPcm();
    g.code.push_back(0xe321f013);
    const size_t loud = g.here();
    g.code.push_back(0xee070f90);
    g.code.push_back(0xe3560000 | allTicks);
    g.branch(loud, 0xb);
    g.code.push_back(0xe7f000f0);                        // undefined: the run ends
    g.patchBranch(6, g.here());
    g.record();
    g.store(0x21c134, 0x8000);                           // ISR_B: clear B15
    g.code.push_back(0xe2866001);                        // add r6, r6, #1
    g.code.push_back(0xe25ef004);                        // subs pc, lr, #4
    g.finish();
    return g;
}
struct IrqRun { Capture words; uint64_t ticks = 0, idle = 0, irqs = 0, r6 = 0; };
static IrqRun irqRun(bool fast, bool stepped) {
    const Program g = irqGuest();
    Imx1ProbeOptions o{};
    o.gpioBInputKnown = true; o.gpioBInput = 0x08100000;  // B15 low, as SocIMX1's options
    o.idleStepped = stepped;
    auto* m = create(g, fast, true, Imx1ProbeEntry::resetAlias, &o);
    IrqRun out;
    imx1MachineSetPcmOutput(m, Capture::hook, &out.words);
    Imx1MachineRun r{};
    for (unsigned n = 0; n < 100000; ++n) {
        r = imx1MachineRunSlice(m, fast ? 997 : 500, UINT64_MAX);
        if (r.status != Imx1MachineStatus::yielded) break;
    }
    if (r.status != Imx1MachineStatus::terminal)
        fprintf(stderr, "irq guest: %s pc %08x\n", imx1ProbeStopName(r.result.stop), r.result.trace.pc);
    assert(r.status == Imx1MachineStatus::terminal);
    const auto status = imx1MachineGetStatus(m);
    out.ticks = status.time.ticks; out.idle = status.idleHclk; out.irqs = r.result.irqEntries; out.r6 = r.result.registers[6];
    imx1MachineStop(m);
    return out;
}
static void guestInterrupts() {
    const IrqRun exact = irqRun(false, false), fast = irqRun(true, false), stepped = irqRun(false, true);
    fprintf(stderr, "irq guest: %llu CLK32 ticks, %llu idle HCLK, %llu interrupts, %zu words, %zu not zero\n",
            (unsigned long long)exact.ticks, (unsigned long long)exact.idle, (unsigned long long)exact.irqs,
            exact.words.bytes.size() / 3, exact.words.nonzero());
    for (const IrqRun* r : {&exact, &fast, &stepped}) {
        assert(r->irqs == allTicks && r->r6 == allTicks && r->idle > 0);
        // Six records primed, the first interrupt when one is left (after 5 ticks), then one a tick.
        const double ms = r->ticks * 1000.0 / 32768;
        assert(ms > (5 + allTicks - 1) * 10 - 1 && ms < (5 + allTicks - 1) * 10 + 1);
    }
    assert(exact.words.bytes == fast.words.bytes && exact.words.bytes == stepped.words.bytes);
    assert(exact.ticks == fast.ticks && exact.ticks == stepped.ticks && exact.idle == stepped.idle);
    assert(exact.words.nonzero() > 100);
}

// A board whose port B was used before the chip came in can't move B15: the run stops on it.
// Both with the guest writing the chip and with it never touching it.
static void lineRefused() {
    for (unsigned write = 0; write < 2; ++write) {
        Program g;
        g.load(0, 0x21c100);
        g.code.push_back(0xe5901000);  // ldr r1, [r0]: DDIR_B, so port B has been accessed
        if (write) g.ymf(0x06, 1);
        g.code.push_back(0xeafffffe);
        g.finish();
        auto* m = create(g, false, false);
        run(m, 2, 2);
        imx1MachineEnableYmf768(m);
        const auto r = imx1MachineRunSlice(m, 1000, 100000);
        assert(r.status != Imx1MachineStatus::yielded && r.result.stop == Imx1ProbeStop::unsupportedPeripheral &&
               r.result.peripheralStatus == Imx1BootstrapAccess::sideEffect);
        imx1MachineStop(m);
    }
}

// A state saved without the chip (an older session) loads into a machine with it: B15 stays at the state's
// level (low here) until the guest first touches the chip, then follows the chip, also over a save and load
// before that. Loading the power-on state, as Reset does, brings B15 high from the start. The note plays both
// times.
static void olderState() {
    const Program guest = noteGuest(true, true);
    Imx1ProbeOptions o{};
    o.gpioBInputKnown = true; o.gpioBInput = 0x08100000;
    auto* m = create(guest, false, false, Imx1ProbeEntry::physicalFlash, &o);
    std::vector<uint8_t> state;
    assert(imx1MachineSaveState(m, state) == Imx1StateStatus::ok);
    imx1MachineStop(m);
    auto* loaded = create(guest, false, true, Imx1ProbeEntry::physicalFlash, &o);
    std::vector<uint8_t> powerOn;
    assert(imx1MachineSaveState(loaded, powerOn) == Imx1StateStatus::ok);
    Capture d;
    imx1MachineSetPcmOutput(loaded, Capture::hook, &d);
    assert(imx1MachineLoadState(loaded, state.data(), state.size()).status == Imx1StateStatus::ok);
    // Saved now, with the chip's section, and loaded again: B15 stays at the older state's level.
    std::vector<uint8_t> resaved, again;
    assert(imx1MachineSaveState(loaded, resaved) == Imx1StateStatus::ok);
    assert(imx1MachineLoadState(loaded, resaved.data(), resaved.size()).status == Imx1StateStatus::ok);
    assert(imx1MachineSaveState(loaded, again) == Imx1StateStatus::ok && again == resaved);
    assert(!(imx1MachineGpioInput(loaded, 1) & 0x8000));
    auto r = run(loaded, 200000, 5000);
    assert(d.nonzero() > 100 && !(r.result.registers[8] & 0x8000) && (r.result.registers[7] & 0x8000));
    Capture e;
    imx1MachineSetPcmOutput(loaded, Capture::hook, &e);
    assert(imx1MachineLoadState(loaded, powerOn.data(), powerOn.size()).status == Imx1StateStatus::ok);
    r = run(loaded, 600000, 5000);
    assert(e.nonzero() > 100 && (r.result.registers[8] & 0x8000) && (r.result.registers[7] & 0x8000));
    imx1MachineStop(loaded);
}

// A machine in the middle of a power step loads a state saved without the ready section (no step under way): every
// step reads as done at once. The guest starts the clock and the power up and then reads 38 into r5 over and over.
static void readyStepsDroppedOnLoad() {
    Program g;
    g.ymf(0x0f, 1); g.ymf(0x30, 0xff); g.ymf(0x30, 0xe6); g.storeByte(0x15000000, 0x38);
    const size_t loop = g.here();
    g.load(2, 0x15000001);
    g.code.push_back(0xe5d25000);  // ldrb r5, [r2]
    g.branch(loop);
    g.finish();
    const auto hasReady = [](const std::vector<uint8_t>& state) {
        static const uint8_t name[4] = {'Y', 'M', 'F', 'R'};
        return std::search(state.begin(), state.end(), name, name + 4) != state.end();
    };
    for (unsigned fast = 0; fast < 2; ++fast) {
        auto* done = create(g, fast, true);
        auto r = run(done, 1000, 1000);
        assert(r.result.registers[5] == 0);
        for (unsigned n = 0; n < 2000 && r.result.registers[5] != 3; ++n) r = run(done, 5000, 5000);
        assert(r.result.registers[5] == 3);
        std::vector<uint8_t> state;
        assert(imx1MachineSaveState(done, state) == Imx1StateStatus::ok && !hasReady(state));
        imx1MachineStop(done);
        auto* busy = create(g, fast, true);
        r = run(busy, 1000, 1000);
        assert(r.result.registers[5] == 0);
        std::vector<uint8_t> mid;
        assert(imx1MachineSaveState(busy, mid) == Imx1StateStatus::ok && hasReady(mid));
        assert(imx1MachineLoadState(busy, state.data(), state.size()).status == Imx1StateStatus::ok);
        r = run(busy, 50, 50);
        assert(r.result.registers[5] == 3);
        imx1MachineStop(busy);
    }
}

int main() {
    unsetenv("FROND_JIT"); cpuSetJitSync(true); setenv("FROND_JIT_HOT", "1", 1);
    registers();
    powerSteps();
    powerStepsPllChange();
    pitch();
    fourOperators();
    ma2Fields();
    operatorMapping();
    oldStateResync();
    operatorMapping();
    const uint32_t h = sequence();
    fprintf(stderr, "sequence hash %08x\n", h);
    assert(h == 0xf1879438u && sequence() == h);
    guestTime();
    saveHasNoEffect();
    quietIdle();
    badState();
    machine();
    fmMuted();
    guestInterrupts();
    lineRefused();
    olderState();
    readyStepsDroppedOnLoad();
    std::puts("PASS: YMF768 registers, FIFO and interrupt, power steps, A4 pitch, MA-2 operator fields, four operators, the stored note sequence, FM on "
              "guest time, saves change nothing, quiet idling matches a full run, bad states refused, Exact == Fast on CS4 with a save mid-note, FM left out of the sound with the same state, the "
              "B15 interrupt guest Exact == Fast == stepped idle, a refused B15 stops, older states, ready steps dropped by a state without them");
}
