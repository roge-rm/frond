// Firmware-free Zire 72 microphone: the WM9712L ADC through the real SoC, its guest-time
// pacing, gain, ALC, saved state and the source lifecycle (checkpoint, load, reset, sleep).
// Also checks the E2 shares the codec without an ADC.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "soc_PXA.h"
#include "audio_input.h"
#include "mem.h"

namespace {
struct Source {
    unsigned pulls{}, disarms{}, fences{};
    bool fenced{};
    double amplitude{200};
    uint32_t rate{};
    AudioInputSource Hooks() {
        return {this,
                [](void *p, AudioInputRoute route, uint32_t n, uint32_t d) -> int16_t {
                    auto &s = *static_cast<Source *>(p);
                    assert(!s.fenced && route == AudioInputRoute::internalMic && d == 1);
                    s.rate = n;
                    const double v = s.amplitude * std::sin(2 * M_PI * 1000.0 * s.pulls / n);
                    ++s.pulls;
                    return int16_t(std::lround(v));
                },
                [](void *p, bool f) { auto &s = *static_cast<Source *>(p); s.fenced = f; ++s.fences; },
                // Disarm resets the source, fence included, as SdlMicrophone does.
                [](void *p) { auto &s = *static_cast<Source *>(p); ++s.disarms; s.fenced = false; }};
    }
};

struct Machine {
    std::vector<uint8_t> rom, nand;
    SocPXA soc;
    // The E2 and T|X need their NAND (34603008 and 132 MiB, erased).
    explicit Machine(DeviceType5 type = deviceTypeZire72)
        : rom(type == deviceTypeTX ? 16u << 20 : 4096),
          nand(type == deviceTypeE2 ? 34603008 : type == deviceTypeTX ? 132u << 20 : 0, 0xff),
          soc(type, (type == deviceTypeE2 ? 16u : 32u) << 20, rom.data(), uint32_t(rom.size()),
              nand.empty() ? nullptr : nand.data(), nand.size(), -1, type == deviceTypeE2 ? 0 : 2) {}
    void wr(uint32_t a, uint32_t v) { assert(memAccess(cpuGetMem(soc.GetCpu()), a, 4, true, &v)); }
    uint32_t rd(uint32_t a) { uint32_t v = 0; assert(memAccess(cpuGetMem(soc.GetCpu()), a, 4, false, &v)); return v; }
    void loop() {
        wr(0xa0003000, 0xeafffffe);
        cpuInvalidateCodeCache(soc.GetCpu());
        cpuSetReg(soc.GetCpu(), ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC | ARM_SR_I | ARM_SR_F);
        cpuSetReg(soc.GetCpu(), 15, 0xa0003000);
    }
    void run(unsigned ms = 10) { soc.Run(uint64_t(ms) * 100000, 100000000); }
    // Primary codec registers sit at 0x40500200 + 2 * register.
    void codec(unsigned reg, uint16_t value) { wr(0x40500200 + 2 * reg, value); }
    // The Zire's Voice Memo settings, from a trace of the real ROM, with ALC off and unity gain.
    void configure(uint16_t recGain = 0x5757, uint16_t micVol = 0x6800, uint16_t recSel = 0x0000) {
        loop();
        codec(0x0e, micVol); codec(0x1a, recSel); codec(0x1c, recGain);
        codec(0x24, 0x0376); codec(0x26, 0x4000); codec(0x2a, 0x0001); codec(0x32, 8000);
    }
    std::vector<uint8_t> save() {
        assert(soc.Save());
        auto d = soc.GetSavestate();
        return {(uint8_t *)d.data, (uint8_t *)d.data + d.size};
    }
    void sleep() {
        wr(0xa0003000, 0xee070e10);
        cpuInvalidateCodeCache(soc.GetCpu());
        cpuSetReg(soc.GetCpu(), 15, 0xa0003000);
        cpuSetReg(soc.GetCpu(), 0, 3);
        cpuCyclePure(soc.GetCpu());
        assert(soc.PowerSleeping());
    }
    // Runs ms milliseconds a millisecond at a time and reads every frame the ADC pushed out of PCDR.
    std::vector<uint32_t> capture(Source &s, unsigned ms) {
        std::vector<uint32_t> frames;
        for (unsigned i = 0; i < 16; ++i) rd(0x40500040);  // drop what the FIFO already holds
        for (unsigned i = 0; i < ms; ++i) {
            const unsigned before = s.pulls;
            run(1);
            const unsigned n = s.pulls - before;
            assert(n <= 16);
            for (unsigned j = 0; j < n; ++j) frames.push_back(rd(0x40500040));
        }
        return frames;
    }
};

double rms(const std::vector<uint32_t> &frames, bool right, size_t skip = 0) {
    double sum = 0;
    for (size_t i = skip; i < frames.size(); ++i) {
        const double v = int16_t(right ? frames[i] >> 16 : frames[i] & 0xffff);
        sum += v * v;
    }
    return std::sqrt(sum / double(frames.size() - skip));
}

int16_t peak(const std::vector<uint32_t> &frames, size_t skip = 0) {
    int16_t p = 0;
    for (size_t i = skip; i < frames.size(); ++i) p = std::max<int16_t>(p, int16_t(std::abs(int16_t(frames[i] & 0xffff))));
    return p;
}

// A chunk's words (version first) by chunk type.
uint32_t *wmChunk(std::vector<uint8_t> &state, uint32_t type, uint32_t *size = nullptr) {
    auto *words = reinterpret_cast<uint32_t *>(state.data());
    unsigned offset = 1 + 2 * words[0];
    for (unsigned i = 0; i < words[0]; ++i) {
        if (words[1 + 2 * i] == type) {
            if (size) *size = words[2 + 2 * i];
            return words + offset;
        }
        offset += words[2 + 2 * i];
    }
    return nullptr;
}
}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // Off: no demand, no pulls, and the old one zero per PCM period.
    {
        Machine m;
        m.loop();
        Source s;
        auto h = s.Hooks();
        m.soc.SetMicrophoneSource(&h);
        assert(!m.soc.MicrophoneDemand().active);
        m.run(20);
        assert(!s.pulls && m.rd(0x40500040) == 0);
    }

    Machine m;
    m.configure();
    Source s;
    auto h = s.Hooks();
    m.soc.SetMicrophoneSource(&h);
    auto demand = m.soc.MicrophoneDemand();
    assert(demand.active && !demand.fenced && demand.route == AudioInputRoute::internalMic);
    assert(demand.rateNumerator == 8000 && demand.rateDenominator == 1);

    // Guest-time pacing: 8000 frames a second whatever the PCM period.
    m.run(5);
    unsigned start = s.pulls;
    m.run(500);
    assert(s.pulls - start >= 3999 && s.pulls - start <= 4001);
    m.codec(0x32, 11025);
    m.run(5);
    start = s.pulls;
    m.run(1000);
    assert(s.pulls - start >= 11024 && s.pulls - start <= 11026 && s.rate == 11025);
    m.codec(0x32, 8000);
    m.run(5);

    // Unity gain passes the source through on both channels; half the input gives half the RMS.
    auto full = m.capture(s, 200);
    const double fullRms = rms(full, false);
    assert(std::fabs(fullRms - 200 / std::sqrt(2.0)) < 2 && std::fabs(rms(full, true) - fullRms) < 0.01);
    s.amplitude = 100;
    const double halfRms = rms(m.capture(s, 200), false);
    printf("Zire 72 ADC RMS %.2f / %.2f = %.4f\n", fullRms, halfRms, fullRms / halfRms);
    assert(std::fabs(fullRms / halfRms - 2) < 0.01);

    // Mic boost (0x0e bit 7) and record boost (0x1a bit 14) are 20 dB each; PGA steps 0.75 dB.
    m.configure(0x5757, 0x6880, 0x0000);
    double boosted = rms(m.capture(s, 100), false);
    assert(std::fabs(boosted / halfRms - 10) < 0.1);
    m.configure(0x5757, 0x6800, 0x4000);
    boosted = rms(m.capture(s, 100), false);
    assert(std::fabs(boosted / halfRms - 10) < 0.1);
    m.configure(0x5f5f, 0x6800, 0x0000);  // +6 dB in 0.75 dB steps
    assert(std::fabs(rms(m.capture(s, 100), false) / halfRms - std::pow(10.0, 6.0 / 20)) < 0.02);
    m.configure(0x0404, 0x6800, 0x0000);  // +6 dB in 1.5 dB steps
    assert(std::fabs(rms(m.capture(s, 100), false) / halfRms - std::pow(10.0, 6.0 / 20)) < 0.02);
    // Mute, and other record sources read silence.
    m.configure(0xd757, 0x6800, 0x0000);
    auto muted = m.capture(s, 50);
    assert(!muted.empty() && rms(muted, false) == 0 && rms(muted, true) == 0);
    m.configure(0x5757, 0x6800, 0x0400);
    auto line = m.capture(s, 50);
    assert(rms(line, false) == 0 && std::fabs(rms(line, true) - halfRms) < 1);
    m.configure(0x5757, 0x6800, 0x0404);
    assert(!m.soc.MicrophoneDemand().active);
    start = s.pulls;
    m.run(20);
    assert(s.pulls == start);

    // ALC as Voice Memo sets it: two input levels settle near the same output level.
    for (double level : {200.0, 100.0}) {
        m.configure(0x7f7f, 0x6880, 0x7000);
        m.codec(0x60, 0xf032);
        m.codec(0x62, 0xfb9f);
        s.amplitude = level;
        auto alc = m.capture(s, 1500);
        const int16_t settled = peak(alc, alc.size() - 4000);
        printf("Zire 72 ALC input %.0f peak %d\n", level, settled);
        assert(settled > 24000 && settled < 32767);
    }
    // Left-only ALC starts from the left PGA (0 dB here, the right is +30 dB). The left records
    // line input first so the ALC is idle, then switches to the mic. The decay is slow enough
    // that the gain doesn't move in the capture.
    m.configure(0x577f, 0x6800, 0x0400);
    m.codec(0x60, 0xf0f0);
    m.codec(0x62, 0xb800);
    s.amplitude = 200;
    m.run(5);
    m.codec(0x1a, 0x0000);
    {
        const int16_t left = peak(m.capture(s, 20));
        assert(left > 190 && left < 215);
    }
    m.codec(0x62, 0x0000);

    // Save and load mid-stream: the same frames follow.
    m.configure();
    s.amplitude = 200;
    m.run(3);
    auto saved = m.save();
    const unsigned savedPulls = s.pulls;
    auto after = m.capture(s, 20);
    assert(m.soc.Load(saved.size(), saved.data()));
    assert(s.disarms == 1);  // A load disarms the source.
    m.soc.SetMicrophoneSource(&h);
    s.pulls = savedPulls;
    assert(m.capture(s, 20) == after);

    // Version 0 (no ADC state) still loads: the registers come back and the ADC carries on.
    {
        auto old = saved;
        uint32_t *chunk = wmChunk(old, 0x0140);
        assert(chunk && chunk[0] == 1);
        chunk[0] = 0;
        assert(m.soc.Load(old.size(), old.data()));
        m.soc.SetMicrophoneSource(&h);
        assert(m.soc.MicrophoneDemand().active);
        m.run(5);
        assert(std::fabs(rms(m.capture(s, 100), false) - 200 / std::sqrt(2.0)) < 2);
        // A bad ADC field is refused and the machine carries on as it was. The ALC gain is the
        // fourth word from the end of the chunk.
        auto bad = saved;
        uint32_t size = 0;
        chunk = wmChunk(bad, 0x0140, &size);
        chunk[size - 4] = 200;
        const auto live = m.save();
        assert(!m.soc.Load(bad.size(), bad.data()));
        assert(m.save() == live);
    }

    // Lifecycle, as on the TG50: fenced in checkpoints, disarmed by load, reset and sleep.
    m.soc.SetMicrophoneSource(&h);
    const unsigned disarms = s.disarms;
    {
        SoC::InputCheckpoint checkpoint(m.soc);
        assert(s.fenced && m.soc.MicrophoneDemand().fenced);
        const unsigned count = s.pulls;
        m.run(20);
        assert(s.pulls == count);
        assert(checkpoint.Restore(saved.size(), saved.data()));
    }
    assert(!s.fenced && s.disarms == disarms);
    m.run(5);
    m.soc.Reset();
    assert(s.disarms == disarms + 1);
    m.configure();
    m.soc.SetMicrophoneSource(&h);
    m.sleep();
    assert(s.disarms == disarms + 2);
    m.soc.SetMicrophoneSource(&h);
    assert(s.disarms == disarms + 3);  // No arming an asleep guest.

    // The E2 and T|X have the same codec and no ADC model: no demand, no source, and version 0
    // state, 10 words (the ADC fields) shorter than the Zire's version 1 chunk.
    uint32_t zireSize = 0;
    {
        auto state = m.save();
        const uint32_t *chunk = wmChunk(state, 0x0140, &zireSize);
        assert(chunk && chunk[0] == 1);
    }
    for (DeviceType5 type : {deviceTypeE2, deviceTypeTX}) {
        Machine other(type);
        other.configure();
        Source es;
        auto eh = es.Hooks();
        other.soc.SetMicrophoneSource(&eh);
        assert(!other.soc.MicrophoneDemand().active);
        other.run(20);
        assert(!es.pulls && !es.disarms && other.rd(0x40500040) == 0);
        auto state = other.save();
        uint32_t size = 0;
        const uint32_t *chunk = wmChunk(state, 0x0140, &size);
        assert(chunk && chunk[0] == 0 && size == zireSize - 10);
    }
    printf("Zire 72 microphone ADC checks passed\n");
}
