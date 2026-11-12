// The Zodiac's PCM sound out: SSI1's words through the machine's PCM output in the same order and values in
// Exact and Fast, at the frame rate imx1MachineAudio gives, carried on through a state saved mid-stream; the
// PWM volume and D25 amplifier line as a gain; the host stage's slot pairing, averaging, scaling and gain
// ramp; and ZodiacPcmSink, what SocIMX1 runs: the same host frames from Exact and Fast through stream start,
// a volume change and the amplifier going off and on, its gating on PCM output and suspend, and its restart.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <numeric>
#include <initializer_list>
#include <utility>
#include <vector>
#include "CPU.h"
#include "audio_queue.h"
#include "imx1_boot_probe.h"
#include "zodiac_audio.h"

// A guest that fills 64 bytes of SRAM with a pattern, plays them over and over through DMA channel 10 into
// SSI1 (the ROM's set-up, in repeat), sets the PWM to the ROM's volume level 3 and port D's D23 to D27 as the
// ROM's audio module leaves them, with D25 as given, then spins. A scripted guest plays a constant 0x4000 in
// both slots and, after the set-up, waits, turns the volume up to level 6, waits, turns the amplifier off,
// waits and turns it on again.
struct Guest {
    static constexpr int16_t constant = 0x4000;
    static constexpr uint32_t waitLoops = 150000;
    std::vector<uint32_t> code;
    std::vector<std::pair<size_t, uint32_t>> literals;
    explicit Guest(bool d25, bool scripted = false) {
        constexpr uint32_t ssi = 0x218000, dma = 0x209000, ch = 0x209300, pwm = 0x208000, portD = 0x21c300;
        for (uint32_t i = 0; i < 16; ++i)
            store(0x300000 + i * 4, scripted ? 0x40004000u : 0x01000100u * (i + 1) + 0x80008000u * (i & 1));
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
        if (scripted) {
            wait(); store(pwm + 4, 0x80);
            wait(); store(portD + 0x1c, (1u << 24) | (1u << 27));
            wait(); store(portD + 0x1c, (1u << 24) | (1u << 27) | (1u << 25));
        }
        code.push_back(0xeafffffe);  // b .
        for (const auto& literal : literals) {
            code[literal.first] |= uint32_t((code.size() - literal.first - 2) * 4);
            code.push_back(literal.second);
        }
    }
    void load(unsigned reg, uint32_t value) { literals.emplace_back(code.size(), value); code.push_back(0xe59f0000 | reg << 12); }
    void store(uint32_t address, uint32_t value) { load(0, address); load(1, value); code.push_back(0xe5801000); }
    void wait() {
        load(2, waitLoops);
        code.push_back(0xe2522001);  // subs r2, r2, #1
        code.push_back(0x1afffffd);  // bne the subs
    }
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

// Fast is the machine's Fast engine, as SocIMX1's Fast setting turns on.
static Imx1Machine* create(const Guest& g, bool fast) {
    Imx1ProbeOptions o{};
    o.bootstrapPeripherals = true;
    auto* m = imx1MachineCreate(g.rom(), g.size(), Imx1ProbeEntry::physicalFlash, false, o);
    assert(m);
    if (fast) assert(imx1MachineSetFastEngine(m, true) && imx1MachineFastEngine(m));
    return m;
}
static void run(Imx1Machine* m, uint32_t instructions, uint32_t slice = 5000) {
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
            // No PWM before the guest sets it up; the duty written counts from the write, before the output
            // takes it up at its next period boundary.
            assert(!imx1MachineAudio(m).pwmKnown && !zodiacAudioGain(imx1MachineAudio(m)));
            run(m, 50000);
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

using Frames = std::vector<uint32_t>;
static void collect(void* context, uint32_t frame) { static_cast<Frames*>(context)->push_back(frame); }
static int16_t left(uint32_t f) { return int16_t(uint16_t(f)); }
static int16_t right(uint32_t f) { return int16_t(uint16_t(f >> 16)); }
// Lets a gain ramp finish on silent frames, which are dropped. An even count keeps the half and double rates
// on a whole output frame.
static void settle(ZodiacAudioOutput& o) {
    Frames dropped;
    for (uint32_t i = 0; i < 2 * ZodiacAudioOutput::rampFrames; ++i) { o.word(0, 0, collect, &dropped); o.word(0, 1, collect, &dropped); }
}

static void hostStage() {
    Frames out;
    ZodiacAudioOutput o;
    o.setGain(65536);
    // No rate yet: nothing comes out.
    o.word(100, 0, collect, &out); o.word(200, 1, collect, &out);
    assert(out.empty());
    // At the host's rate each frame comes out as it is; slot 0 is left. A slot 1 word with no slot 0 before
    // it is dropped.
    o.setRate(44100, 1); settle(o);
    o.word(7, 1, collect, &out);
    o.word(1000, 0, collect, &out); o.word(uint16_t(-2000), 1, collect, &out);
    assert(out.size() == 1 && left(out[0]) == 1000 && right(out[0]) == -2000);
    // Twice the host's rate (88200 as 176400/2): each output is the average of two frames.
    out.clear();
    o.setRate(176400, 2); settle(o);
    for (int16_t v : {100, 300, -50, -150}) { o.word(uint16_t(v), 0, collect, &out); o.word(uint16_t(-v), 1, collect, &out); }
    assert(out.size() == 2 && left(out[0]) == 200 && right(out[0]) == -200 && left(out[1]) == -100 && right(out[1]) == 100);
    // Half the host's rate: each frame twice. The gain scales and clamps.
    out.clear();
    o.setRate(22050, 1); o.setGain(32768); settle(o);
    o.word(1000, 0, collect, &out); o.word(uint16_t(-1000), 1, collect, &out);
    assert(out.size() == 2 && left(out[1]) == 500 && right(out[1]) == -500);
    out.clear();
    o.setGain(4 << 16); settle(o);
    o.word(20000, 0, collect, &out); o.word(uint16_t(-20000), 1, collect, &out);
    assert(out.size() == 2 && left(out[0]) == 32767 && right(out[0]) == -32768);
    // The gain is 16.16: just under 1 takes full scale down one step.
    out.clear();
    o.setGain(65535); settle(o);
    o.word(32767, 0, collect, &out); o.word(uint16_t(-32767), 1, collect, &out);
    assert(out.size() == 2 && left(out[0]) == 32766 && right(out[0]) == -32766);
    // A restart drops a held slot 0 word.
    out.clear();
    o.setGain(65536);
    o.word(5, 0, collect, &out); o.restart(); o.word(6, 1, collect, &out);
    assert(out.empty());
    // The Zodiac's rate (the System PLL at 96 MHz-odd over PERCLK3 / 17 and 128): 44117.6 frames a
    // second make 44100 a second, give or take one.
    o.setRate(2126249984, 48195); settle(o);
    for (int i = 0; i < 44118; ++i) { o.word(1, 0, collect, &out); o.word(1, 1, collect, &out); }
    assert(out.size() >= 44099 && out.size() <= 44101);
    // The first one may take in part of the last silent frame.
    for (size_t i = 1; i < out.size(); ++i) assert(left(out[i]) == 1 && right(out[i]) == 1);
}

// Gain changes ramp over rampFrames input frames, up and down, and a restart fades in from silence.
static void gainRamp() {
    constexpr int16_t v = 10000;
    constexpr uint32_t n = ZodiacAudioOutput::rampFrames;
    ZodiacAudioOutput o;
    o.setRate(44100, 1);
    auto play = [&](uint32_t frames) {
        Frames out;
        for (uint32_t i = 0; i < frames; ++i) { o.word(uint16_t(v), 0, collect, &out); o.word(uint16_t(-v), 1, collect, &out); }
        for (auto f : out) assert(right(f) == -left(f));
        return out;
    };
    // Each step at most a ramp's share of the change, plus one for rounding, and the end on the target.
    auto ramps = [&](const Frames& out, int from, int to) {
        const int most = (to > from ? to - from : from - to) / int(n) + 2;
        int last = from;
        for (size_t i = 0; i < out.size(); ++i) {
            const int now = left(out[i]);
            assert((to > from ? now >= last : now <= last) && (now > last ? now - last : last - now) <= most);
            last = now;
        }
        // Halfway there halfway through, give or take 2% of the change (the steps round toward zero), and on
        // the target from the last ramp frame.
        const int middle = left(out[n / 2]) - (from + to) / 2, change = to > from ? to - from : from - to;
        assert((middle < 0 ? -middle : middle) <= change / 50 + most && left(out[n - 1]) == to && left(out.back()) == to);
    };
    // The amplifier on: from silence up to full.
    o.setGain(65536);
    ramps(play(n + 10), 0, v);
    // A volume step down to a quarter.
    o.setGain(16384);
    ramps(play(n + 10), v, v / 4);
    // The amplifier off.
    o.setGain(0);
    ramps(play(n + 10), v / 4, 0);
    // On again, then a restart (a state load) fades in afresh from silence to the gain it had.
    o.setGain(65536);
    play(n);
    o.restart();
    ramps(play(n + 10), 0, v);
    // A new rate starts the stream afresh too.
    o.setRate(88200, 1);
    const auto doubled = play(2 * n + 10);
    assert(left(doubled[0]) < v / 100 && left(doubled.back()) == v);
}

// ZodiacPcmSink gates the push on PCM output being on and not suspended, but makes the frames all the same,
// and its restart clears the queue.
static void sinkGating() {
    auto* queue = audioQueueCreate(1 << 16);
    ZodiacPcmSink a, b;
    a.queue = queue;
    b.enabled = true;
    Frames ungated;
    for (auto* s : {&a, &b}) { s->sound.setRate(44100, 1); s->sound.setGain(65536); }
    // b, with no queue, keeps every frame; a pushes only while on and not suspended.
    unsigned frame = 0;
    auto play = [&](unsigned frames) {
        for (unsigned i = 0; i < frames; ++i, ++frame) {
            const uint16_t l = uint16_t(frame * 7), r = uint16_t(frame * 13);
            ZodiacPcmSink::word(&a, l, 0); ZodiacPcmSink::word(&a, r, 1);
            b.sound.word(l, 0, collect, &ungated); b.sound.word(r, 1, collect, &ungated);
        }
    };
    auto pending = [&] { return audioQueuePendingSamples(queue); };
    play(100);
    assert(pending() == 0);
    a.enabled = true; a.suspended = true;
    play(100);
    assert(pending() == 0);
    a.suspended = false;
    play(1500);
    assert(pending() == 1500);
    // What comes out is what an ungated stream has at that point, the gain ramp included.
    Frames got(1500);
    assert(audioQueuePopChunk(queue, got.data(), got.size()) == 1500);
    assert(Frames(ungated.begin() + 200, ungated.end()) == got);
    a.enabled = false;
    play(10);
    assert(pending() == 0);
    // A restart (state load, reset) empties the queue.
    a.enabled = true;
    play(10);
    assert(pending() == 10);
    a.restart();
    assert(pending() == 0);
}

// The scripted guest through ZodiacPcmSink as SocIMX1 wires it, Exact and Fast in different slice sizes.
static void sinkStream() {
    const Guest guest(true, true);
    Frames frames[2];
    for (unsigned fast = 0; fast < 2; ++fast) {
        struct Tap {
            ZodiacPcmSink sink;
            uint64_t pairs = 0;
            bool held = false;
            static void hook(void* context, uint16_t word, unsigned slot) {
                auto& t = *static_cast<Tap*>(context);
                if (slot == 0) t.held = true;
                else if (t.held) { ++t.pairs; t.held = false; }
                ZodiacPcmSink::word(&t.sink, word, slot);
            }
        } tap;
        auto* queue = audioQueueCreate(1 << 20);
        auto* m = create(guest, fast);
        tap.sink.machine = m;
        tap.sink.queue = queue;
        tap.sink.enabled = true;
        imx1MachineSetPcmOutput(m, Tap::hook, &tap);
        run(m, 1500000, fast ? 7919 : 5000);
        const auto audio = imx1MachineAudio(m);
        assert(audio.rateNumerator && audio.rateDenominator);
        frames[fast].resize(audioQueuePendingSamples(queue));
        audioQueuePopChunk(queue, frames[fast].data(), frames[fast].size());
        // Every frame from the first one SSI1 sends comes out: none is lost while the rate is unknown.
        const uint64_t common = std::gcd(audio.rateNumerator, audio.rateDenominator);
        const uint64_t expected = tap.pairs * 44100 * (audio.rateDenominator / common) / (audio.rateNumerator / common);
        fprintf(stderr, "%s: %llu pairs, %zu frames\n", fast ? "fast" : "exact", (unsigned long long)tap.pairs, frames[fast].size());
        assert(frames[fast].size() == expected && expected > 0);
        imx1MachineStop(m);
        }
    assert(frames[0] == frames[1]);
    // The levels held for a while: level 3, level 6, silence with the amplifier off and level 6 again.
    const auto level = [](uint32_t high) { return int(int64_t(Guest::constant) * ((uint64_t(high) << 16) / 0x82) / 65536); };
    std::vector<int> plateaus;
    const auto& f = frames[0];
    for (size_t i = 0, run = 0; i < f.size(); ++i) {
        // The priming zeros are an odd number of words after the first, unpaired slot 1, so one frame is half
        // zero.
        assert(left(f[i]) == right(f[i]) || i < 16);
        run = i && f[i] == f[i - 1] ? run + 1 : 1;
        if (run == 200 && (plateaus.empty() || plateaus.back() != left(f[i]))) plateaus.push_back(left(f[i]));
    }
    for (int p : plateaus) fprintf(stderr, "plateau %d\n", p);
    assert((plateaus == std::vector<int>{level(0x33), level(0x80), 0, level(0x80)}));
    // Each change ramps: after the priming zeros, no step bigger than a ramp's share of the largest change for
    // each of the up to two input frames (this guest runs SSI1 at about 64 kHz) an output frame takes in.
    int most = 0;
    for (size_t i = 16; i < f.size(); ++i) {
        const int step = left(f[i]) - left(f[i - 1]);
        most = std::max(most, step < 0 ? -step : step);
    }
    fprintf(stderr, "largest step %d\n", most);
    assert(most <= 2 * level(0x80) / int(ZodiacAudioOutput::rampFrames) + 2);
}

int main() {
    unsetenv("FROND_JIT"); cpuSetJitSync(true); setenv("FROND_JIT_HOT", "1", 1);
    hostStage();
    gainRamp();
    sinkGating();
    volumeAndAmplifier();
    sinkStream();
    machineStream();
    std::puts("PASS: Zodiac PCM words Exact and Fast alike, frame rate, saved mid-stream, PWM volume, D25 amplifier, host stage, gain ramp, the sink's gating, restart and Exact == Fast host frames");
}
