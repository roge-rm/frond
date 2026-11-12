// The Zodiac's YMF768 model: the register answers the ROM's driver waits for, the sequencer FIFO and its
// interrupt line, a known note sequence against a stored hash and its pitch, and through the machine on CS4:
// FM added to SSI1's words the same in Exact and Fast, a state saved mid-note carrying on the same, and a
// state saved without the chip loading with it reset.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <utility>
#include <vector>
#include "CPU.h"
#include "imx1_boot_probe.h"
#include "zodiac_ymf768.h"

static void put(Imx1Ymf768& y, unsigned reg, unsigned value) { y.write(false, uint8_t(reg)); y.write(true, uint8_t(value)); }
static uint8_t get(Imx1Ymf768& y, unsigned reg) { y.write(false, uint8_t(reg)); return y.read(true); }

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
    put(y, 0x06, 0);
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
        int32_t l = 0, r = 0;
        y.render(l, r);
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
                for (unsigned i = 0; i < frames; ++i) { int32_t l = 0, r = 0; y.render(l, r); most = std::max(most, std::abs(l)); }
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
        for (unsigned i = 0; i < frames; ++i) { int32_t l = 0, r = 0; y.render(l, r); h = fnv(fnv(h, l), r); }
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

// A guest that plays zeros over and over through DMA channel 10 into SSI1 (as tests/zodiac-sound.cpp), then
// programs the YMF768 on CS4 with byte stores, keys a note on and spins.
struct Guest {
    std::vector<uint32_t> code;
    std::vector<std::pair<size_t, uint32_t>> literals;
    explicit Guest(bool note) {
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
        if (note) {
            ymf(0x0f, 1); ymf(0x10, 31); ymf(0x0f, 0);
            for (unsigned n = 0; n < 2; ++n) {
                const unsigned s = 0x10 + 4 * n;
                ymf(s, 0x21); ymf(s + 0x20, n ? 0 : 24); ymf(s + 0x40, 0xf2); ymf(s + 0x60, 0x25); ymf(s + 0x80, 0);
            }
            ymf(0xd0, 0x10); ymf(0xb0, 580 & 0xff); ymf(0xc0, 2 | 4 << 2 | 0x20);
        }
        code.push_back(0xeafffffe);  // b .
        for (const auto& literal : literals) {
            code[literal.first] |= uint32_t((code.size() - literal.first - 2) * 4);
            code.push_back(literal.second);
        }
    }
    void load(unsigned reg, uint32_t value) { literals.emplace_back(code.size(), value); code.push_back(0xe59f0000 | reg << 12); }
    void store(uint32_t address, uint32_t value) { load(0, address); load(1, value); code.push_back(0xe5801000); }
    void storeByte(uint32_t address, uint32_t value) { load(0, address); load(1, value); code.push_back(0xe5c01000); }
    void ymf(unsigned reg, unsigned value) { storeByte(0x15000000, reg); storeByte(0x15000001, value); }
    const uint8_t* rom() const { return reinterpret_cast<const uint8_t*>(code.data()); }
    uint32_t size() const { return uint32_t(code.size() * 4); }
};
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
static Imx1Machine* create(const Guest& g, bool fast, bool chip) {
    Imx1ProbeOptions o{};
    o.bootstrapPeripherals = true;
    o.cs4Port0InputKnown = true;
    auto* m = imx1MachineCreate(g.rom(), g.size(), Imx1ProbeEntry::physicalFlash, false, o);
    assert(m);
    if (chip) imx1MachineEnableYmf768(m);
    if (fast) assert(imx1MachineSetFastEngine(m, true) && imx1MachineFastEngine(m));
    return m;
}
static void run(Imx1Machine* m, uint32_t instructions, uint32_t slice) {
    while (instructions) {
        const uint32_t piece = instructions < slice ? instructions : slice;
        const auto r = imx1MachineRunSlice(m, piece, 100000);
        if (r.status != Imx1MachineStatus::yielded) {
            fprintf(stderr, "stopped: %s pc %08x address %08x\n", imx1ProbeStopName(r.result.stop), r.result.trace.pc, r.result.address);
            assert(false);
        }
        instructions -= piece;
    }
}

static void machine() {
    const Guest guest(true);
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

// A state saved without the chip (an older session) loads into a machine with it, which starts it reset.
static void olderState() {
    const Guest guest(false);
    auto* m = create(guest, false, false);
    run(m, 200000, 5000);
    std::vector<uint8_t> state;
    assert(imx1MachineSaveState(m, state) == Imx1StateStatus::ok);
    imx1MachineStop(m);
    auto* loaded = create(guest, false, true);
    Capture d;
    imx1MachineSetPcmOutput(loaded, Capture::hook, &d);
    assert(imx1MachineLoadState(loaded, state.data(), state.size()).status == Imx1StateStatus::ok);
    run(loaded, 200000, 5000);
    assert(!d.bytes.empty() && d.nonzero() == 0);
    imx1MachineStop(loaded);
}

int main() {
    unsetenv("FROND_JIT"); cpuSetJitSync(true); setenv("FROND_JIT_HOT", "1", 1);
    registers();
    pitch();
    fourOperators();
    const uint32_t h = sequence();
    fprintf(stderr, "sequence hash %08x\n", h);
    assert(h == 0x479a9839u && sequence() == h);
    machine();
    olderState();
    std::puts("PASS: YMF768 register answers, FIFO and interrupt, A4 pitch, four operators, the stored note sequence, Exact == Fast on CS4, saved mid-note, older states");
}
