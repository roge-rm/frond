// The Zodiac's YMF768 model. On the model: the register answers the ROM's driver waits for, the sequencer FIFO
// and its interrupt line, A4's pitch, four-operator channels, a known note sequence against a stored hash, the
// FM engine running on guest time with or without SSI1 words and in any batching, a save or copy changing
// nothing in the run, a loaded copy carrying on the same, and bad saved values refused. Through the machine:
// FM added to SSI1's words the same in Exact and Fast with a state saved mid-note; a guest that takes the
// FIFO tick as an interrupt on GPIO B15 through the AITC from a wait (idle skips bounded by the next tick)
// with SSI1 off and then on, alike in Exact, Fast and Exact with stepped idle; a B15 change the GPIO refuses
// stopping the run; and an older state without the chip loading with B15 as it was, then a reset giving sound.
#include <cassert>
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
    assert(get(y, 0x38) & 1);         // ready (0x200bdcfc)
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
        put(y, s, 0x21); put(y, s + 0x20, n ? 0 : tl); put(y, s + 0x40, 0xf2); put(y, s + 0x60, 0x25); put(y, s + 0x80, 0);
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
            ymf(s, 0x21); ymf(s + 0x20, n ? 0 : 24); ymf(s + 0x40, 0xf2); ymf(s + 0x60, 0x25); ymf(s + 0x80, 0);
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
// level (low here) until the guest first touches the chip, then follows the chip. Loading the power-on state,
// as Reset does, brings B15 high from the start. The note plays both times.
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
    auto r = run(loaded, 200000, 5000);
    assert(d.nonzero() > 100 && !(r.result.registers[8] & 0x8000) && (r.result.registers[7] & 0x8000));
    Capture e;
    imx1MachineSetPcmOutput(loaded, Capture::hook, &e);
    assert(imx1MachineLoadState(loaded, powerOn.data(), powerOn.size()).status == Imx1StateStatus::ok);
    r = run(loaded, 600000, 5000);
    assert(e.nonzero() > 100 && (r.result.registers[8] & 0x8000) && (r.result.registers[7] & 0x8000));
    imx1MachineStop(loaded);
}

int main() {
    unsetenv("FROND_JIT"); cpuSetJitSync(true); setenv("FROND_JIT_HOT", "1", 1);
    registers();
    pitch();
    fourOperators();
    const uint32_t h = sequence();
    fprintf(stderr, "sequence hash %08x\n", h);
    assert(h == 0xf1879438u && sequence() == h);
    guestTime();
    saveHasNoEffect();
    badState();
    machine();
    guestInterrupts();
    lineRefused();
    olderState();
    std::puts("PASS: YMF768 registers, FIFO and interrupt, A4 pitch, four operators, the stored note sequence, FM on "
              "guest time, saves change nothing, bad states refused, Exact == Fast on CS4 with a save mid-note, the "
              "B15 interrupt guest Exact == Fast == stepped idle, a refused B15 stops, older states");
}
