// The Zodiac's PCM sound out: SSI1's words through the machine's PCM output in the same order and values in
// Exact and Fast, at the frame rate imx1MachineAudio gives, carried on through a state saved mid-stream; the
// PWM volume and D25 amplifier line as a gain; and the host stage's slot pairing, averaging and scaling.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#include "CPU.h"
#include "imx1_boot_probe.h"
#include "zodiac_audio.h"

// A guest that fills 64 bytes of SRAM with a pattern, plays them over and over through DMA channel 10 into
// SSI1 (the ROM's set-up, in repeat), sets the PWM to the ROM's volume level 3 and port D's D23 to D27 as the
// ROM's audio module leaves them, with D25 as given, then spins.
struct Guest {
    std::vector<uint32_t> code;
    std::vector<std::pair<size_t, uint32_t>> literals;
    explicit Guest(bool d25) {
        constexpr uint32_t ssi = 0x218000, dma = 0x209000, ch = 0x209300, pwm = 0x208000, portD = 0x21c300;
        for (uint32_t i = 0; i < 16; ++i) store(0x300000 + i * 4, 0x01000100u * (i + 1) + 0x80008000u * (i & 1));
        store(dma, 2); store(dma, 1); store(dma + 8, 0x3ff); store(dma + 0x1c, 0xffff);
        store(ch + 0xc, 8); store(ch + 0x10, 16); store(ch + 0xc, 0x2088);
        store(ch + 0x14, 8); store(ch + 0xc, 0x208c); store(ch + 0x18, 0);
        store(ch + 8, 64); store(ch + 4, ssi);
        store(ssi + 8, 0); store(ssi + 0x28, 0); store(ssi + 0x10, 0); store(ssi + 0x18, 0);
        store(ssi + 8, 0x9900); store(ssi + 0x14, 0x6100); store(ssi + 0x20, 4); store(ssi + 0xc, 0xe8);
        for (unsigned i = 0; i < 8; ++i) store(ssi, 0);
        store(ch, 0x300000); store(ch + 0xc, 0x208c); store(ch + 0xc, 0x208d);
        store(dma + 4, 0x400); store(ssi + 0xc, 0x2e8); store(ssi + 8, 0x9b00);
        // PWMC CLK32 / 2 enabled, period 0x80, sample 0x33 (20097b56).
        store(pwm, 0x8010); store(pwm + 8, 0x80); store(pwm + 4, 0x33);
        // D24, D27 high, D23 and D26 low, D25 as given; all five GPIO outputs from the data register.
        store(portD + 0x1c, (1u << 24) | (1u << 27) | (d25 ? 1u << 25 : 0));
        store(portD + 8, 0x00ffc000); store(portD + 0x20, 0x0f800000); store(portD, 0x0f800000);
        code.push_back(0xeafffffe);  // b .
        for (const auto& literal : literals) {
            code[literal.first] |= uint32_t((code.size() - literal.first - 2) * 4);
            code.push_back(literal.second);
        }
    }
    void load(unsigned reg, uint32_t value) { literals.emplace_back(code.size(), value); code.push_back(0xe59f0000 | reg << 12); }
    void store(uint32_t address, uint32_t value) { load(0, address); load(1, value); code.push_back(0xe5801000); }
    const uint8_t* rom() const { return reinterpret_cast<const uint8_t*>(code.data()); }
    uint32_t size() const { return uint32_t(code.size() * 4); }
};

struct Capture {
    std::vector<uint8_t> bytes;  // word low, word high, slot
    static void hook(void* context, uint16_t word, unsigned slot) {
        auto& c = *static_cast<Capture*>(context);
        c.bytes.push_back(uint8_t(word)); c.bytes.push_back(uint8_t(word >> 8)); c.bytes.push_back(uint8_t(slot));
    }
    size_t words() const { return bytes.size() / 3; }
};

static Imx1Machine* create(const Guest& g, bool fast) {
    Imx1ProbeOptions o{};
    o.bootstrapPeripherals = true;
    auto* m = imx1MachineCreate(g.rom(), g.size(), Imx1ProbeEntry::physicalFlash, fast, o);
    assert(m);
    return m;
}
static void run(Imx1Machine* m, uint32_t instructions) {
    while (instructions) {
        const uint32_t piece = instructions < 5000 ? instructions : 5000;
        const auto r = imx1MachineRunSlice(m, piece, 100000);
        if (r.status != Imx1MachineStatus::yielded) {
            fprintf(stderr, "stopped: %s pc %08x address %08x\n", imx1ProbeStopName(r.result.stop), r.result.trace.pc, r.result.address);
            assert(false);
        }
        instructions -= piece;
    }
}

static void machineStream() {
    const Guest guest(true);
    std::vector<uint8_t> streams[2], resumed[2];
    for (unsigned fast = 0; fast < 2; ++fast) {
        Capture c;
        auto* m = create(guest, fast);
        imx1MachineSetPcmOutput(m, Capture::hook, &c);
        run(m, 400000);
        const auto audio = imx1MachineAudio(m);
        assert(audio.rateNumerator && audio.rateDenominator);
        // The words: the eight priming zeros, then the pattern's halfwords, low first, over and over.
        assert(c.words() > 200);
        // The first word loads at the first word boundary after TE, which in this guest's timing starts slot 1.
        assert(c.bytes[2] == 1);
        for (size_t i = 0; i < 8; ++i) assert(c.bytes[i * 3] == 0 && c.bytes[i * 3 + 1] == 0);
        for (size_t i = 8; i < c.words(); ++i) {
            const uint32_t n = uint32_t((i - 8) % 32), w = 0x01000100u * (n / 2 + 1) + 0x80008000u * ((n / 2) & 1);
            const uint16_t expected = uint16_t(n & 1 ? w >> 16 : w);
            assert((c.bytes[i * 3] | c.bytes[i * 3 + 1] << 8) == expected);
            assert(c.bytes[i * 3 + 2] == ((c.bytes[(i - 1) * 3 + 2]) ^ 1));
        }
        // Frames over guest time match the rate given (CLK32 ticks, 32768 a second).
        const auto t0 = imx1MachineGetStatus(m).time;
        const size_t w0 = c.words();
        run(m, 400000);
        const auto t1 = imx1MachineGetStatus(m).time;
        const double seconds = (double(t1.ticks - t0.ticks) + double(t1.numerator) / t1.denominator -
                                double(t0.numerator) / t0.denominator) / 32768.0;
        const double frames = double(c.words() - w0) / 2, expected = seconds * audio.rateNumerator / audio.rateDenominator;
        assert(frames > 100 && frames > expected - 2 && frames < expected + 2);
        // Saved mid-stream: a machine loading it makes the same words as this one going on.
        std::vector<uint8_t> state;
        assert(imx1MachineSaveState(m, state) == Imx1StateStatus::ok);
        const size_t w1 = c.words();
        run(m, 200000);
        resumed[fast].assign(c.bytes.begin() + w1 * 3, c.bytes.end());
        streams[fast] = c.bytes;
        imx1MachineStop(m);
        Capture d;
        auto* loaded = create(guest, fast);
        imx1MachineSetPcmOutput(loaded, Capture::hook, &d);
        const auto info = imx1MachineLoadState(loaded, state.data(), state.size());
        assert(info.status == Imx1StateStatus::ok);
        run(loaded, 200000);
        assert(d.bytes == resumed[fast] && !d.bytes.empty());
        imx1MachineStop(loaded);
    }
    // Exact and Fast make the same bytes.
    assert(streams[0] == streams[1] && resumed[0] == resumed[1]);
}

static void volumeAndAmplifier() {
    for (bool d25 : {true, false}) {
        const Guest guest(d25);
        for (unsigned fast = 0; fast < 2; ++fast) {
            auto* m = create(guest, fast);
            // The PWM's sample takes effect at its next period boundary, 260 CLK32 ticks on.
            run(m, 50000);
            assert(!imx1MachineAudio(m).pwmKnown && !zodiacAudioGain(imx1MachineAudio(m)));
            for (unsigned i = 0; i < 400 && !imx1MachineAudio(m).pwmKnown; ++i) run(m, 50000);
            const auto a = imx1MachineAudio(m);
            assert(a.pwmKnown && a.pwmHigh == 0x33 && a.pwmPeriod == 0x82);
            assert((a.gpioDDirection & 0x0f800000) == 0x0f800000 && (a.gpioDInUse & 0x0f800000) == 0x0f800000);
            assert(((a.gpioDData >> 25) & 1) == d25);
            assert(zodiacAudioGain(a) == (d25 ? (0x33u << 16) / 0x82 : 0u));
            imx1MachineStop(m);
        }
    }
    // The gain wants D25 driven high as a GPIO output from the data register, and a known PWM duty.
    Imx1MachineAudio a;
    a.pwmKnown = true; a.pwmHigh = 128; a.pwmPeriod = 130;
    a.gpioDDirection = a.gpioDInUse = a.gpioDData = 1u << 25; a.gpioDOutputSelect = 3u << 18;
    assert(zodiacAudioGain(a) == (128u << 16) / 130);
    auto b = a; b.gpioDDirection = 0; assert(!zodiacAudioGain(b));
    b = a; b.gpioDInUse = 0; assert(!zodiacAudioGain(b));
    b = a; b.gpioDOutputSelect = 1u << 18; assert(!zodiacAudioGain(b));
    b = a; b.pwmKnown = false; assert(!zodiacAudioGain(b));
    b = a; b.pwmHigh = 0; assert(!zodiacAudioGain(b));
}

static void hostStage() {
    std::vector<uint32_t> out;
    auto push = [](void* context, uint32_t frame) { static_cast<std::vector<uint32_t>*>(context)->push_back(frame); };
    auto left = [](uint32_t f) { return int16_t(uint16_t(f)); };
    auto right = [](uint32_t f) { return int16_t(uint16_t(f >> 16)); };
    ZodiacAudioOutput o;
    o.setGain(65536);
    // No rate yet: nothing comes out.
    o.word(100, 0, push, &out); o.word(200, 1, push, &out);
    assert(out.empty());
    // At the host's rate each frame comes out as it is; slot 0 is left. A slot 1 word with no slot 0 before
    // it is dropped.
    o.setRate(44100, 1);
    o.word(7, 1, push, &out);
    o.word(1000, 0, push, &out); o.word(uint16_t(-2000), 1, push, &out);
    assert(out.size() == 1 && left(out[0]) == 1000 && right(out[0]) == -2000);
    // Twice the host's rate (88200 as 176400/2): each output is the average of two frames.
    out.clear();
    o.setRate(176400, 2);
    for (int16_t v : {100, 300, -50, -150}) { o.word(uint16_t(v), 0, push, &out); o.word(uint16_t(-v), 1, push, &out); }
    assert(out.size() == 2 && left(out[0]) == 200 && right(out[0]) == -200 && left(out[1]) == -100 && right(out[1]) == 100);
    // Half the host's rate: each frame twice. The gain scales and clamps.
    out.clear();
    o.setRate(22050, 1); o.setGain(32768);
    o.word(1000, 0, push, &out); o.word(uint16_t(-1000), 1, push, &out);
    assert(out.size() == 2 && left(out[1]) == 500 && right(out[1]) == -500);
    out.clear();
    o.setGain(4 << 16);
    o.word(20000, 0, push, &out); o.word(uint16_t(-20000), 1, push, &out);
    assert(out.size() == 2 && left(out[0]) == 32767 && right(out[0]) == -32768);
    // The gain is 16.16: just under 1 takes full scale down one step.
    out.clear();
    o.setGain(65535);
    o.word(32767, 0, push, &out); o.word(uint16_t(-32767), 1, push, &out);
    assert(out.size() == 2 && left(out[0]) == 32766 && right(out[0]) == -32766);
    // A restart drops a held slot 0 word.
    out.clear();
    o.setGain(65536);
    o.word(5, 0, push, &out); o.restart(); o.word(6, 1, push, &out);
    assert(out.empty());
    // The Zodiac's rate (the System PLL at 96 MHz-odd over PERCLK3 / 17 and 128): 44117.6 frames a
    // second make 44100 a second, give or take one.
    o.setRate(2126249984, 48195);
    for (int i = 0; i < 44118; ++i) { o.word(1, 0, push, &out); o.word(1, 1, push, &out); }
    assert(out.size() >= 44099 && out.size() <= 44101);
    for (auto f : out) assert(left(f) == 1 && right(f) == 1);
}

int main() {
    unsetenv("FROND_JIT"); cpuSetJitSync(true); setenv("FROND_JIT_HOT", "1", 1);
    hostStage();
    volumeAndAmplifier();
    machineStream();
    std::puts("PASS: Zodiac PCM words Exact and Fast alike, frame rate, saved mid-stream, PWM volume, D25 amplifier, host stage");
}
