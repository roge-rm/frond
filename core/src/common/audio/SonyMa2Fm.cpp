#include "SonyMa2Fm.h"

#include <algorithm>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
// The Zodiac builds a separate copy of ymfm in the ARM core.
#define ymfm sony_ma2_ymfm
#include "../../uarm/uarm/ymfm/ymfm_opl.cpp"
#include "../../uarm/uarm/ymfm/ymfm_adpcm.cpp"
#include "../../uarm/uarm/ymfm/ymfm_pcm.cpp"
#undef ymfm

namespace ymfm = sony_ma2_ymfm;
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

constexpr unsigned voices = 16;
constexpr uint32_t voiceMask = (1u << voices) - 1;
constexpr uint32_t sampleRate = 49716;

class Ma2Registers : public ymfm::opl3_registers {
public:
    bool FourOperators(unsigned channel) const { return algorithms[channel & ~4u] >= 2; }

    void operator_map(operator_mapping& map) const {
        for (unsigned ch = 0; ch < CHANNELS; ++ch) {
            if (ch >= voices || ((ch & 4) && FourOperators(ch))) {
                map.chan[ch] = operator_list();
                continue;
            }
            // A group of four voices starts at slots 0, 8, 16 or 24. Pairing adds the following
            // group's slots to the first voice, in the same order Pa1Lib fills them.
            const unsigned first = (ch / 4) * 8 + ch % 4;
            map.chan[ch] = FourOperators(ch) ? operator_list(first, first + 4, first + 8, first + 12)
                                            : operator_list(first, first + 4);
        }
    }

    uint32_t ch_algorithm(uint32_t offset) const {
        const unsigned algorithm = algorithms[(offset >> 8) * 9 + (offset & 15)];
        // Diagram order in Yamaha's MA-2 Authoring Tool manual §3.3.1.6. ymfm implements
        // those four topologies as OPL3 connections. Selectors 6 and 7 have no published diagram.
        static constexpr unsigned routing[8] = {0, 1, 8, 10, 9, 11, 9, 11};
        return routing[algorithm];
    }

    void save_restore(ymfm::ymfm_saved_state& state) {
        ymfm::opl3_registers::save_restore(state);
        state.save_restore(algorithms);
    }

    uint8_t algorithms[CHANNELS]{};
};

class Ma2Engine : public ymfm::fm_engine_base<Ma2Registers> {
public:
    explicit Ma2Engine(ymfm::ymfm_interface& intf) : fm_engine_base(intf) {
        reset();
        m_active_channels = 0;
    }

    void SetAlgorithm(unsigned ch, uint8_t value) {
        const unsigned first = ch & ~4u;
        const bool wasFour = regs().FourOperators(first);
        regs().algorithms[ch] = value & 7;
        write(0xc0 + Ma2Registers::channel_offset(ch), 0x10 | ((value >> 3) & 7) << 1);
        if (wasFour == regs().FourOperators(first)) return;

        // Key writes are immediate, even when the next synthesis sample has not run yet.
        assign_operators();
        for (unsigned voice : {first, first + 4}) {
            const auto reg = uint16_t(0xb0 + Ma2Registers::channel_offset(voice));
            write(reg, regs().read(reg));
        }
    }

    void State(ymfm::ymfm_saved_state& state) {
        const uint32_t modified = m_modified_channels;
        save_restore(state);
        if (state.saving()) m_modified_channels = modified;
        state.save_restore(m_active_channels);
        state.save_restore(m_modified_channels);
        state.save_restore(m_prepare_count);
        if (state.saving()) return;

        // The mapping changes immediately with the algorithm register. The next clock
        // refreshes dirty caches, so only clean caches need to be rebuilt while loading.
        assign_operators();
        if (m_modified_channels) return;
        for (const auto& ch : m_channel)
            for (unsigned n = 0; n < 4; ++n)
                if (auto* op = ch->debug_operator(n))
                    m_regs.cache_operator_data(op->choffs(), op->opoffs(), op->debug_cache());
    }
};

}  // namespace

struct SonyMa2Fm::Core {
    ymfm::ymfm_interface intf;
    Ma2Engine engine{intf};
    uint8_t fm[0xa0]{};
    uint8_t level[voices], velocity[voices], pan[voices];
    uint8_t master{0};
    uint32_t clock{0}, phase{0};
    int32_t last[2]{}, current[2]{};

    Core() {
        engine.write(0x105, 1);
        for (unsigned ch = 0; ch < voices; ++ch) {
            level[ch] = 24;
            velocity[ch] = 31;
            pan[ch] = 64;
            engine.SetAlgorithm(ch, 0);
        }
    }

    void State(ymfm::ymfm_saved_state& state) {
        engine.State(state);
        state.save_restore(fm);
        state.save_restore(level);
        state.save_restore(velocity);
        state.save_restore(pan);
        state.save_restore(master);
        state.save_restore(clock);
        state.save_restore(phase);
        state.save_restore(last);
        state.save_restore(current);
    }

    void SyncOperator(unsigned slot) {
        const uint8_t* r = fm + slot;
        const uint8_t g0 = r[0], g1 = r[0x20], g2 = r[0x40], g3 = r[0x60], g4 = r[0x80];
        const unsigned op = Ma2Registers::operator_offset(slot);
        engine.write(0x20 + op, (g4 & 8) << 4 | (g0 & 8) << 3 | (g0 & 4) << 3 | (g0 & 1) << 4 | g0 >> 4);
        engine.write(0x40 + op, (g3 & 3) << 6 | g3 >> 2);
        engine.write(0x60 + op, (g2 & 0xf0) | (g1 & 0x0f));
        engine.write(0x80 + op, (g2 & 0x0f) << 4 | g1 >> 4);
        engine.write(0xe0 + op, g4 & 7);
    }

    void Sample() {
        engine.clock(voiceMask);
        last[0] = current[0];
        last[1] = current[1];
        int32_t left = 0, right = 0;
        for (unsigned ch = 0; ch < voices; ++ch) {
            if ((ch & 4) && engine.regs().FourOperators(ch)) continue;
            Ma2Engine::output_data out;
            engine.output(out.clear(), 0, 32767, 1u << ch);
            const int32_t sample = (out.data[0] * level[ch] * velocity[ch]) >> 10;
            left += sample * (pan[ch] <= 64 ? 64 : 127 - pan[ch]) / 64;
            right += sample * (pan[ch] >= 64 ? 64 : pan[ch]) / 64;
        }
        current[0] = (left * master) >> 5;
        current[1] = (right * master) >> 5;
    }
};

SonyMa2Fm::SonyMa2Fm() : core(std::make_unique<Core>()) {}
SonyMa2Fm::~SonyMa2Fm() = default;

void SonyMa2Fm::Reset() { core = std::make_unique<Core>(); }

void SonyMa2Fm::Write(unsigned page, unsigned index, uint8_t data) {
    const unsigned ch = index & 15;
    if (page == 1) {
        if (index == 0x10) core->master = data & 31;
        if (index >= 0xe0 && index < 0xf0) core->velocity[ch] = data & 31;
        return;
    }
    if (page != 0 || index < 0x10 || index > 0xff) return;
    const auto offset = Ma2Registers::channel_offset(ch);
    if (index < 0xb0) {
        core->fm[index - 0x10] = data;
        core->SyncOperator((index - 0x10) % 0x20);
    } else if (index < 0xc0) {
        core->engine.write(0xa0 + offset, data);
    } else if (index < 0xd0) {
        core->engine.write(0xb0 + offset, data & 0x3f);
    } else if (index < 0xe0) {
        core->engine.SetAlgorithm(ch, data);
    } else if (index < 0xf0) {
        core->level[ch] = data & 31;
    } else {
        core->pan[ch] = data & 127;
    }
}

void SonyMa2Fm::Advance(uint64_t cycles, uint32_t clock) {
    if (!cycles || !clock) return;
    if (clock != core->clock) {
        core->phase = core->clock ? uint64_t(core->phase) * clock / core->clock : 0;
        core->clock = clock;
    }
    while (cycles) {
        const uint64_t span = std::min(cycles, uint64_t(1) << 28);
        const uint64_t position = core->phase + span * sampleRate;
        core->phase = position % clock;
        for (uint64_t count = position / clock; count; --count) core->Sample();
        cycles -= span;
    }
}

void SonyMa2Fm::Output(int32_t& left, int32_t& right) const {
    if (!core->clock) return;
    const auto interpolate = [&](unsigned ch) {
        return core->last[ch] + int32_t(int64_t(core->current[ch] - core->last[ch]) *
                                       core->phase / core->clock);
    };
    left += interpolate(0);
    right += interpolate(1);
}

std::vector<uint8_t> SonyMa2Fm::Save() const {
    std::vector<uint8_t> data;
    ymfm::ymfm_saved_state state(data, true);
    core->State(state);
    return data;
}

size_t SonyMa2Fm::StateSize() {
    static const size_t size = SonyMa2Fm().Save().size();
    return size;
}

bool SonyMa2Fm::Load(const std::vector<uint8_t>& data) {
    // ymfm's OPL3 state is little endian. Validate fields it uses as indexes and bounds
    // before loading, then check the mixer fields in a temporary engine.
    constexpr size_t regs = 11, algorithms = regs + 9 + Ma2Registers::REGISTERS;
    constexpr size_t channels = algorithms + Ma2Engine::CHANNELS;
    constexpr size_t operators = channels + 6 * Ma2Engine::CHANNELS;
    constexpr size_t extra = operators + 10 * Ma2Engine::OPERATORS;
    constexpr size_t end = extra + 12 + 0xa0 + 3 * voices + 1 + 24;
    if (data.size() != end || StateSize() != end) return false;
    const auto u16 = [&](size_t at) { return uint32_t(data[at] | data[at + 1] << 8); };
    const auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
    if ((u32(0) & 3) || data[5] != Ma2Registers::DEFAULT_PRESCALE || data[7] || data[8] || data[9] ||
        u16(regs) >= 210 * 64 || (data[regs + 9 + 0xbd] & 32))
        return false;
    for (unsigned ch = 0; ch < Ma2Engine::CHANNELS; ++ch)
        if (data[algorithms + ch] > 7) return false;
    for (unsigned op = 0; op < Ma2Engine::OPERATORS; ++op) {
        const size_t at = operators + 10 * op;
        if (u16(at + 4) > 0x3ff || data[at + 6] < ymfm::EG_ATTACK || data[at + 6] > ymfm::EG_RELEASE ||
            data[at + 7] > 1 || data[at + 8] > 1 || data[at + 9] > 7)
            return false;
    }
    const uint32_t active = u32(extra), modified = u32(extra + 4);
    if ((active & ~voiceMask) || modified > Ma2Engine::ALL_CHANNELS || u32(extra + 8) > 4096)
        return false;

    auto copy = data;
    auto restored = std::make_unique<Core>();
    ymfm::ymfm_saved_state state(copy, false);
    restored->State(state);
    if (restored->phase >= std::max(restored->clock, 1u) || restored->master > 31) return false;
    for (unsigned ch = 0; ch < voices; ++ch)
        if (restored->level[ch] > 31 || restored->velocity[ch] > 31 || restored->pan[ch] > 127)
            return false;
    for (unsigned ch = 0; ch < 2; ++ch)
        if (restored->last[ch] < -0x80000 || restored->last[ch] > 0x80000 ||
            restored->current[ch] < -0x80000 || restored->current[ch] > 0x80000)
            return false;

    core = std::move(restored);
    return true;
}
