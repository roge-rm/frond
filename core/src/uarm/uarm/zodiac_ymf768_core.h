#ifndef FROND_ZODIAC_YMF768_CORE_H
#define FROND_ZODIAC_YMF768_CORE_H

// Imx1Ymf768's FM engine: ymfm's OPL3 (Aaron Giles, BSD-3-Clause, ymfm/LICENSE), commit
// 81aec25ccbb98f4873a255f7551ac4dadac59b4a of https://github.com/aaronsgiles/ymfm, the files unchanged but
// for an include guard in ymfm_fm.ipp. ymfm is built here and the methods are defined here, so include this in
// one file only (imx1_bootstrap.cpp, which every build and test with the Zodiac board compiles).
#include "zodiac_ymf768.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "ymfm/ymfm_opl.cpp"
#include "ymfm/ymfm_adpcm.cpp"
#include "ymfm/ymfm_pcm.cpp"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// The engine, with the bookkeeping ymfm's own save leaves out, so a loaded state carries on sample for sample.
// ymfm's save_restore marks every channel modified, which would make the next sample prepare them all; a save
// puts the mask back so it changes nothing. A load sets up what ymfm keeps outside its save: each channel's
// operators and each operator's cached register data, as the last prepare left them.
struct Imx1Ymf768Engine : ymfm::fm_engine_base<ymfm::opl3_registers> {
    using Base = ymfm::fm_engine_base<ymfm::opl3_registers>;
    using Operator = ymfm::fm_operator<ymfm::opl3_registers>;
    static constexpr unsigned noOperator = 0xff;
    explicit Imx1Ymf768Engine(ymfm::ymfm_interface& intf) : Base(intf) {}
    void saveRestore(ymfm::ymfm_saved_state& s) {
        const uint32_t modified = m_modified_channels;
        save_restore(s);
        if (s.saving()) m_modified_channels = modified;
        s.save_restore(m_active_channels);
        s.save_restore(m_modified_channels);
        s.save_restore(m_prepare_count);
        uint8_t map[CHANNELS][4];
        for (unsigned ch = 0; ch < CHANNELS; ++ch)
            for (unsigned i = 0; i < 4; ++i) map[ch][i] = operatorNumber(m_channel[ch]->debug_operator(i));
        s.save_restore(map);
        if (s.saving()) return;
        for (unsigned ch = 0; ch < CHANNELS; ++ch)
            for (unsigned i = 0; i < 4; ++i) m_channel[ch]->assign(i, map[ch][i] == noOperator ? nullptr : m_operator[map[ch][i]].get());
        for (unsigned ch = 0; ch < CHANNELS; ++ch)
            for (unsigned i = 0; i < 4; ++i)
                if (Operator* op = m_channel[ch]->debug_operator(i))
                    m_regs.cache_operator_data(op->choffs(), op->opoffs(), op->debug_cache());
    }
    unsigned operatorNumber(const Operator* op) const {
        for (unsigned n = 0; n < OPERATORS; ++n)
            if (m_operator[n].get() == op) return n;
        return noOperator;
    }
    uint8_t clockPrescale() const { return m_clock_prescale; }
};
struct Imx1Ymf768Core {
    ymfm::ymfm_interface intf;
    Imx1Ymf768Engine engine{intf};
};

namespace {
// YMF768 channel to OPL3 channel. Four-operator pairs 0-4, 1-5, 2-6, 8-12, 9-13 and 10-14 land on OPL3's
// pairs 0-3, 1-4, 2-5, 9-12, 10-13 and 11-14; 3, 7, 11 and 15 go to channels without a pair.
constexpr uint8_t ymfOplChannel[16] = {0, 1, 2, 6, 3, 4, 5, 7, 9, 10, 11, 15, 12, 13, 14, 16};
unsigned oplChannelOffset(unsigned m) { return (m / 9) * 0x100 + m % 9; }
unsigned oplOperatorOffset(unsigned m, unsigned n) {
    static constexpr uint8_t first[9] = {0, 1, 2, 8, 9, 10, 16, 17, 18};
    return (m / 9) * 0x100 + first[m % 9] + 3 * n;
}
}  // namespace

Imx1Ymf768::Imx1Ymf768() : core(new Imx1Ymf768Core) { reset(); }
Imx1Ymf768::~Imx1Ymf768() = default;
Imx1Ymf768::Imx1Ymf768(Imx1Ymf768&&) noexcept = default;
Imx1Ymf768& Imx1Ymf768::operator=(Imx1Ymf768&&) noexcept = default;
Imx1Ymf768::Imx1Ymf768(const Imx1Ymf768& other) : core(new Imx1Ymf768Core) { *this = other; }
Imx1Ymf768& Imx1Ymf768::operator=(const Imx1Ymf768& other) {
    if (this == &other) return *this;
    if (!core) core.reset(new Imx1Ymf768Core);
    std::memcpy(regs, other.regs, sizeof(regs));
    index = other.index; bank = other.bank; fifoQueued = other.fifoQueued; fifoPartial = other.fifoPartial;
    fifoWait = other.fifoWait; timerAcc = other.timerAcc; engineAcc = other.engineAcc;
    enginePeriod = other.enginePeriod; samples = other.samples;
    std::memcpy(last, other.last, sizeof(last));
    pendingRight = other.pendingRight;
    quietOutput = false;
    quietSamples = other.quietSamples;
    quietSkipping = other.quietSkipping;
    std::vector<uint8_t> engine;
    other.saveEngine(engine);
    loadEngine(engine);
    return *this;
}

void Imx1Ymf768::reset() {
    std::memset(regs, 0, sizeof(regs));
    index = 0; bank = 0; fifoQueued = 0; fifoPartial = 0; fifoWait = 0;
    timerAcc = 0; engineAcc = 0; enginePeriod = 0; samples = 0; pendingRight = 0;
    quietOutput = false; quietSamples = 0;
    for (auto& v : last) v = 0;
    core->engine.reset();
    engineWrite(0x105, 1);  // OPL3 mode: four operators and the eight waveforms
    // ASSUMPTION: channels start as the driver's own channel reset (200b63d8) leaves them, level 24, pan
    // centre, bank 1 level 31 and vibrato depth 1, since games that never send CC7 never write bank 1 e0.
    for (unsigned ch = 0; ch < 16; ++ch) {
        regs[0][0xe0 + ch] = 24; regs[0][0xf0 + ch] = 64;
        regs[1][0xe0 + ch] = 31; regs[1][0xc0 + ch] = 1;
    }
    for (unsigned ch = 0; ch < 16; ch += (ch & 3) == 3 ? 5 : 1) syncAlgorithm(ch);
}

void Imx1Ymf768::engineWrite(unsigned reg, uint8_t value) { core->engine.write(uint16_t(reg), value); }

void Imx1Ymf768::write(bool dataPort, uint8_t value) {
    if (!dataPort) { index = value; return; }
    // Any register can change which channels sample mixes, so the next quiet sample runs in full.
    quietOutput = false;
    if (index == 0x0f) { bank = value & 1; regs[0][0x0f] = regs[1][0x0f] = value; return; }
    if (bank == 0 && index == 0x00) { fifoByte(); return; }
    regs[bank][index] = value;
    if (bank != 0) return;
    if (index >= 0x10 && index < 0xb0) {
        // Operator slot s of group g: channel (s / 8) * 4 + s % 4, operator (s / 4) % 2 of that channel,
        // or of the four-operator pair it belongs to.
        static constexpr uint16_t group[5] = {0x20, 0x40, 0x60, 0x80, 0xe0};
        const unsigned g = (index - 0x10) / 0x20, s = (index - 0x10) % 0x20;
        const unsigned ch = (s / 8) * 4 + s % 4, n = (s / 4) % 2;
        // ASSUMPTION "YMF768 operator group 4": the low 3 bits are the OPL3 waveform; the high bits are unknown.
        engineWrite(group[g] + oplOperatorOffset(ymfOplChannel[ch], n), g == 4 ? value & 7 : value);
    } else if (index >= 0xb0 && index < 0xd0) {
        syncFrequency(index & 15);
        const unsigned ch = index & 15;
        if ((ch & 4) == 0 && emulatedPair(ch) && fourOp(ch)) syncFrequency(ch + 4);
    } else if (index >= 0xd0 && index < 0xe0) {
        syncAlgorithm(index & 11);
    }
}

// F-number and block (b0, c0 bits 0-4) and key on (c0 bit 5) of channel ch into OPL3's a0 and b0. The second
// channel of an emulated pair follows its first.
void Imx1Ymf768::syncFrequency(unsigned ch) {
    unsigned from = ch;
    if ((ch & 4) && emulatedPair(ch - 4) && fourOp(ch - 4)) from = ch - 4;
    // The second channel of an OPL3 pair keys nothing: ymfm hands its operators to the first only at the next
    // sample, and a key on in between would start them.
    const bool silent = (ch & 4) && !emulatedPair(ch - 4) && fourOp(ch - 4);
    const unsigned m = ymfOplChannel[ch];
    engineWrite(0xa0 + oplChannelOffset(m), regs[0][0xb0 + from]);
    engineWrite(0xb0 + oplChannelOffset(m), regs[0][0xc0 + from] & (silent ? 0x1f : 0x3f));
}

// Algorithm and feedback (d0 bits 0-2 and 3-5, ASSUMPTION "YMF768 algorithms as the MA-3's" and "feedback
// in d0 bits 3-5") of the pair first, first + 4 into OPL3's c0 connection bits and 104 four-operator bits.
void Imx1Ymf768::syncAlgorithm(unsigned first) {
    const unsigned second = first + 4, m1 = ymfOplChannel[first], m2 = ymfOplChannel[second];
    const uint8_t d1 = regs[0][0xd0 + first], d2 = regs[0][0xd0 + second];
    const unsigned alg = d1 & 7;
    const bool four = alg >= 2;
    unsigned c1 = alg == 1, c2 = (d2 & 7) == 1;
    if (four && !emulatedPair(first)) {
        // OPL3 four-operator connections: FM-FM 1-2-3-4 (MA-3 4), AM-FM 1 + 2-3-4 (6), FM-AM 1-2 + 3-4 (5),
        // AM-AM 1 + 2-3 + 4 (7). MA-3's 2 (all four added) and 3 have no OPL3 match: 2 runs as AM-AM, 3 as FM-FM.
        static constexpr uint8_t cnt[8][2] = {{0, 0}, {0, 0}, {1, 1}, {0, 0}, {0, 0}, {0, 1}, {1, 0}, {1, 1}};
        c1 = cnt[alg][0]; c2 = cnt[alg][1];
    } else if (four) {
        c1 = 0; c2 = 0;  // an emulated pair: 1-2 plus 3-4
    }
    // Output to OPL3's A only; this layer pans.
    engineWrite(0xc0 + oplChannelOffset(m1), uint8_t(0x10 | ((d1 >> 3) & 7) << 1 | c1));
    engineWrite(0xc0 + oplChannelOffset(m2), uint8_t(0x10 | (four ? 0 : ((d2 >> 3) & 7) << 1) | c2));
    if (!emulatedPair(first)) {
        static constexpr uint8_t pairs[6] = {0, 1, 2, 8, 9, 10};
        uint8_t fourBits = 0;
        for (unsigned i = 0; i < 6; ++i) fourBits |= fourOp(pairs[i]) ? 1u << i : 0;
        engineWrite(0x104, fourBits);
    }
    // The second first, so its key off can't reach operators the first has just keyed.
    syncFrequency(second);
    syncFrequency(first);
}

// One engine sample (OPL3's 49716 Hz), each channel through its levels and pan, mixed.
void Imx1Ymf768::sample(int32_t& left, int32_t& right) {
    auto& engine = core->engine;
    engine.clock(Imx1Ymf768Engine::ALL_CHANNELS);
    // Quiet: every operator's envelope is past ymfm's EG_QUIET (0x380, private in fm_operator), so each one
    // outputs 0 whatever its phase and the mix is 0. The engine still clocks every sample, so its state stays
    // as a full run's. The output calls are skipped from the second quiet sample after a register write on,
    // once the first has set the feedback input of every channel mixed to 0, so a key on gives the same
    // samples as a full run.
    bool quiet = true;
    for (unsigned n = 0; n < Imx1Ymf768Engine::OPERATORS && quiet; ++n)
        quiet = engine.debug_operator(n)->debug_eg_attenuation() > 0x380;
    if (quiet && quietOutput && quietSkipping) {
        left = right = 0;
        ++quietSamples;
        return;
    }
    quietOutput = quiet;
    int32_t l = 0, r = 0;
    for (unsigned ch = 0; ch < 16; ++ch) {
        const bool second = (ch & 4) != 0;
        if (second && fourOp(ch - 4)) continue;
        uint32_t mask = 1u << ymfOplChannel[ch];
        if (!second && emulatedPair(ch) && fourOp(ch)) mask |= 1u << ymfOplChannel[ch + 4];
        Imx1Ymf768Engine::output_data out;
        engine.output(out.clear(), 0, 32767, mask);
        if (!out.data[0]) continue;
        // Channel level (bank 0 e0, linear 0..31), bank 1 e0 (linear 0..31) and pan (bank 0 f0, 64 centre).
        const int32_t v = int32_t((int64_t(out.data[0]) * (regs[0][0xe0 + ch] & 31) * (regs[1][0xe0 + ch] & 31)) >> 10);
        const int32_t pan = regs[0][0xf0 + ch] & 127;
        l += v * (pan <= 64 ? 64 : 127 - pan) / 64;
        r += v * (pan >= 64 ? 64 : pan) / 64;
    }
    // Master level (bank 1 10, linear 0..31). ymfm's OPL3 channel peaks near 8192; at full levels a channel
    // peaks near 7440, as no mixing headroom is known.
    const int32_t master = regs[1][0x10] & 31;
    left = (l * master) >> 5;
    right = (r * master) >> 5;
}

// Engine samples at engineRate a guest second. A call makes at most one more sample than its own clocks
// give, so a loaded remainder can't make a long run of them.
void Imx1Ymf768::advanceEngine(uint64_t clocks, uint64_t numerator, uint64_t denominator) {
    const uint64_t period = 32768 * numerator;
    if (period != enginePeriod) {
        enginePeriod = period;
        engineAcc = std::min(engineAcc, period - 1);
    }
    while (clocks) {
        // 2^28 clocks x a denominator under 2^14 x engineRate stays under 2^59.
        const uint64_t chunk = std::min<uint64_t>(clocks, uint64_t(1) << 28);
        clocks -= chunk;
        const uint64_t add = chunk * denominator * engineRate;
        engineAcc += add;
        if (engineAcc < period) continue;
        const uint64_t due = std::min(engineAcc / period, add / period + 1);
        engineAcc %= period;
        for (uint64_t n = 0; n < due; ++n) {
            last[0] = last[2]; last[1] = last[3];
            sample(last[2], last[3]);
            ++samples;
        }
    }
}

void Imx1Ymf768::saveEngine(std::vector<uint8_t>& out) const {
    ymfm::ymfm_saved_state s(out, true);
    core->engine.saveRestore(s);
}
void Imx1Ymf768::loadEngine(const std::vector<uint8_t>& in) {
    auto copy = in;
    ymfm::ymfm_saved_state s(copy, false);
    core->engine.saveRestore(s);
}
size_t Imx1Ymf768::engineStateBytes() {
    static const size_t bytes = [] {
        Imx1Ymf768Core probe;
        std::vector<uint8_t> out;
        ymfm::ymfm_saved_state s(out, true);
        probe.engine.saveRestore(s);
        return out.size();
    }();
    return bytes;
}

// The values in a saved engine that ymfm uses as table indexes or loop bounds, checked before a load. The
// layout is ymfm_saved_state's (little endian, enums as a byte) in fm_engine_base::save_restore's order:
// 11 bytes of engine, the OPL3 registers (LFO counters, LFO AM, noise, 0x200 registers), 3 int16 a channel,
// 10 bytes an operator, then Imx1Ymf768Engine's masks, prepare count and operator map.
bool Imx1Ymf768::engineStateValid(const std::vector<uint8_t>& in) {
    using Engine = Imx1Ymf768Engine;
    constexpr size_t registers = 11, channels = registers + 9 + ymfm::opl3_registers::REGISTERS;
    constexpr size_t operators = channels + 6 * Engine::CHANNELS, extra = operators + 10 * Engine::OPERATORS;
    constexpr size_t map = extra + 12, end = map + 4 * Engine::CHANNELS;
    if (in.size() != end || engineStateBytes() != end) return false;
    auto u16 = [&](size_t at) { return uint32_t(in[at] | in[at + 1] << 8); };
    auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
    static const uint8_t prescale = Imx1Ymf768Core().engine.clockPrescale();
    // The envelope counter's low 2 bits stay 0 with OPL3's divider of 1; nothing here uses the timers.
    if ((u32(0) & 3) || in[5] != prescale || in[8] > 1 || in[9] > 1) return false;
    if (u16(registers) >= 210 * 64) return false;  // AM LFO steps
    for (unsigned n = 0; n < Engine::OPERATORS; ++n) {
        const size_t at = operators + 10 * n;
        // Attenuation is 10 bits; OPL3 envelopes run attack to release (no depress or reverb).
        if (u16(at + 4) > 0x3ff || in[at + 6] < ymfm::EG_ATTACK || in[at + 6] > ymfm::EG_RELEASE) return false;
        if (in[at + 7] > 1 || in[at + 8] > 1 || in[at + 9] > 7) return false;
    }
    // The operator map is OPL3's for some set of four-operator pairs (bits 0-5 of register 104), as a load may
    // come between a 104 write and the prepare that follows it. Active channels have operators.
    const uint8_t* got = &in[map];
    static constexpr uint8_t pairFirst[6] = {0, 1, 2, 9, 10, 11};
    uint32_t four = 0;
    for (unsigned i = 0; i < 6; ++i) four |= got[4 * pairFirst[i] + 2] != Engine::noOperator ? 1u << i : 0;
    uint32_t empty = 0;
    for (unsigned ch = 0; ch < Engine::CHANNELS; ++ch) {
        const unsigned half = ch / 9, c = ch % 9, base = 18 * half;
        const unsigned pair = half * 3 + c % 3;
        uint8_t want[4] = {Engine::noOperator, Engine::noOperator, Engine::noOperator, Engine::noOperator};
        const unsigned first = base + (c < 3 ? c : c < 6 ? c + 3 : c + 6);
        if (c < 3 && (four >> pair & 1)) { want[0] = first; want[1] = first + 3; want[2] = first + 6; want[3] = first + 9; }
        else if (c >= 3 && c < 6 && (four >> pair & 1)) empty |= 1u << ch;
        else { want[0] = first; want[1] = first + 3; }
        if (std::memcmp(got + 4 * ch, want, 4)) return false;
    }
    if (u32(extra) > Engine::ALL_CHANNELS || (u32(extra) & empty) || u32(extra + 4) > Engine::ALL_CHANNELS) return false;
    return true;
}

#endif
