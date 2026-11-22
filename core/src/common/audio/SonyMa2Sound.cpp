#include "SonyMa2Sound.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#include "savestate/Chunk.h"
#include "savestate/ChunkProbe.h"

namespace {
constexpr unsigned sequenceRefillLevel = 18;
}

void SonyMa2Sound::Write(uint64_t now, uint64_t clock, uint8_t data) {
    AdvanceAdpcm(now, clock);
    AdvanceFm(now, clock);
    LogTrace(now, 'w', page, index, data);
    if (index == 0x0f) {
        page = data & 1;
        return;
    }

    const auto previous = registers[page][index];
    registers[page][index] = data;
    WriteFm(now, data);
    if (page == 0 && index == 0x00 && sequenceBytes < 96) {
        ++sequenceBytes;
        if (sequenceStatus == SequenceStatus::fifoLevel && sequenceBytes >= sequenceRefillLevel) sequencePending = false;
    }
    if (page == 0 && index == 0x05 && adpcmFifo.size() < 384) adpcmFifo.push_back(data);
    if (page == 1 && index == 0x01 && ((previous ^ data) & 4)) {
        adpcmDeadline = (data & 4) ? now : 0;
        if (!(data & 4)) {
            adpcmSampleHeld = false;
            OutputSample(now, 0, false);
        }
    }
    if (page == 0 && index == 0x06) {
        if ((data & 0x11) != 0x11) {
            sequenceDeadline = 0;
            sequencePending = false;
        } else if ((previous & 0x11) != 0x11) {
            // Pa1Lib advances SMF time by 20 ms for each 01 FF 00 timer packet.
            sequencePeriod = std::max<uint64_t>(1, clock / 50);
            sequenceDeadline = now + sequencePeriod;
        }
    }
    if (page == 1 && index == 0x04 && (data & 0x80)) ResetAdpcm(now);
    if (page == 1 && index == 0x04 && (data & 0x60)) {
        sequenceBytes = 0;
        sequencePending = false;
    }
    UpdateIrq(now);
}

uint8_t SonyMa2Sound::Read(uint64_t now, uint64_t clock) {
    AdvanceAdpcm(now, clock);
    auto value = registers[page][index];
    if (index == 0x0e && (page == 1 || sequenceStatus == SequenceStatus::fifoLevel))
        value = 3;
    if (page == 0 && index == 0x08) value = sequencePending ? 1 : 0;
    if (page == 0 && index == 0x0a) value = AdpcmStatus();
    LogTrace(now, 'r', page, index, value);
    if (page == 0 && index == 0x08 && sequenceStatus == SequenceStatus::readToClear) {
        sequencePending = false;
        UpdateIrq(now);
    }
    return value;
}

void SonyMa2Sound::Save(Chunk& chunk) { SaveState(chunk); }
void SonyMa2Sound::Save(ChunkProbe& chunk) { SaveState(chunk); }

template <typename T>
void SonyMa2Sound::SaveState(T& chunk) {
    chunk.Put32(1);
    chunk.PutBuffer(registers, sizeof(registers));
    chunk.Put32(page);
    chunk.Put32(index);
    chunk.Put32(sequenceBytes);
    chunk.Put64(sequenceDeadline);
    chunk.Put64(sequencePeriod);
    chunk.PutBool(sequencePending);

    std::array<uint8_t, 384> fifo{};
    std::copy(adpcmFifo.begin(), adpcmFifo.end(), fifo.begin());
    chunk.Put32(adpcmFifo.size());
    chunk.PutBuffer(fifo.data(), fifo.size());
    chunk.Put64(adpcmDeadline);
    chunk.Put32(uint32_t(adpcmLevel));
    chunk.Put32(adpcmStep);
    chunk.Put8(adpcmByte);
    chunk.PutBool(adpcmSecondNibble);
    chunk.PutBool(adpcmSampleHeld);
    chunk.Put16(uint16_t(adpcmSample));

    chunk.Put64(fmDeadline);
    chunk.Put64(fmCycle);
    chunk.Put16(uint16_t(fmSample));
    if constexpr (T::isProbe) {
        chunk.PutBuffer(nullptr, SonyMa2Fm::StateSize());
    } else {
        auto engine = fm.Save();
        chunk.PutBuffer(engine.data(), engine.size());
    }
}

bool SonyMa2Sound::Load(Chunk& chunk, uint64_t now) {
    if (chunk.Get32() != 1) return false;
    chunk.GetBuffer(registers, sizeof(registers));
    page = chunk.Get32();
    index = chunk.Get32();
    sequenceBytes = chunk.Get32();
    sequenceDeadline = chunk.Get64();
    sequencePeriod = chunk.Get64();
    sequencePending = chunk.GetBool();

    std::array<uint8_t, 384> fifo;
    const auto count = chunk.Get32();
    chunk.GetBuffer(fifo.data(), fifo.size());
    adpcmDeadline = chunk.Get64();
    adpcmLevel = int32_t(chunk.Get32());
    adpcmStep = chunk.Get32();
    adpcmByte = chunk.Get8();
    adpcmSecondNibble = chunk.GetBool();
    adpcmSampleHeld = chunk.GetBool();
    adpcmSample = int16_t(chunk.Get16());

    fmDeadline = chunk.Get64();
    fmCycle = chunk.Get64();
    fmSample = int16_t(chunk.Get16());
    std::vector<uint8_t> engine(SonyMa2Fm::StateSize());
    chunk.GetBuffer(engine.data(), engine.size());

    const auto near = [now](uint64_t cycle) {
        constexpr uint64_t maxDistance = 1ull << 27;
        return !cycle || (now < std::numeric_limits<uint64_t>::max() - 2 * maxDistance &&
                          (cycle > now ? cycle - now : now - cycle) <= maxDistance);
    };
    const bool sequenceRunning = (registers[0][0x06] & 0x11) == 0x11;
    const bool invalidFifoIrq = sequenceStatus == SequenceStatus::fifoLevel &&
                                (bool(sequenceDeadline) != sequenceRunning ||
                                 (sequencePending && (!sequenceRunning || sequenceBytes >= sequenceRefillLevel)));
    if (!chunk.IsAtEnd() || page > 1 || index > 255 || sequenceBytes > 96 || invalidFifoIrq ||
        count > fifo.size() || adpcmLevel < -32768 || adpcmLevel > 32767 || adpcmStep < 127 ||
        adpcmStep > 24576 || (sequenceDeadline && !sequencePeriod) || sequencePeriod > (1ull << 27) ||
        !near(sequenceDeadline) || !near(adpcmDeadline) || !near(fmDeadline) ||
        (fmDeadline && (fmCycle > fmDeadline || !near(fmCycle) || fmDeadline - fmCycle > 65536)) ||
        !fm.Load(engine)) {
        return false;
    }
    adpcmFifo.assign(fifo.begin(), fifo.begin() + count);
    irqAsserted = sequencePending && (registers[1][0x01] & 1);
    if (onInterrupt) onInterrupt(irqAsserted, true);
    return true;
}

void SonyMa2Sound::SetTrace(FILE* output, uint64_t now) {
    trace = output;
    LogTrace(now, 's', page, index, 0);
}

void SonyMa2Sound::RestoreOutput(uint64_t cycles) {
    if (!onSample) return;
    onSample(cycles, adpcmSample, false);
    onSample(cycles, fmSample, true);
}

void SonyMa2Sound::OutputSample(uint64_t cycles, int sample, bool isFm) {
    const int speaker = registers[1][0x25];
    auto& current = isFm ? fmSample : adpcmSample;
    // Yamaha's 5-bit analogue gain codes give mute, then -30 to 0 dB in 1 dB steps.
    constexpr unsigned gain[] = {
        0, 1036, 1163, 1305, 1464, 1642, 1843, 2068,
        2320, 2603, 2920, 3277, 3677, 4125, 4629, 5193,
        5827, 6538, 7336, 8231, 9235, 10362, 11627, 13045,
        14637, 16423, 18427, 20675, 23198, 26029, 29205, 32768,
    };
    const int level = (speaker & 0x80) ? speaker & 31 : 0;
    const int eq = registers[1][0x22] & 31;
    const auto scaled = int64_t(sample) * gain[level] * gain[eq] / (1ll << 30);
    current = int16_t(std::clamp<int64_t>(scaled, -32768, 32767));
    if (onSample) onSample(cycles, current, isFm);
}

void SonyMa2Sound::Advance(uint64_t cycles, uint64_t clock) {
    AdvanceAdpcm(cycles, clock);
    AdvanceFm(cycles, clock);
    if (!sequenceDeadline || cycles < sequenceDeadline) return;
    const auto elapsed = 1 + (cycles - sequenceDeadline) / sequencePeriod;
    const auto retired = std::min<uint64_t>(elapsed, sequenceBytes / 3);
    sequenceBytes -= unsigned(retired) * 3;
    sequenceDeadline += elapsed * sequencePeriod;
    if (retired && sequenceBytes < sequenceRefillLevel) sequencePending = true;
    UpdateIrq(cycles);
}

uint8_t SonyMa2Sound::AdpcmStatus() const {
    const bool room = adpcmFifo.size() <= 384 - 96;
    const bool finished = adpcmFifo.empty() && !adpcmSecondNibble && !adpcmSampleHeld;
    return (room ? 1 : 0) | (finished ? 4 : 0);
}

void SonyMa2Sound::ResetAdpcm(uint64_t cycles) {
    adpcmFifo.clear();
    adpcmDeadline = 0;
    adpcmSecondNibble = adpcmSampleHeld = false;
    adpcmLevel = 0;
    adpcmStep = 127;
    OutputSample(cycles, 0, false);
}

void SonyMa2Sound::AdvanceAdpcm(uint64_t cycles, uint64_t clock) {
    if (!adpcmDeadline || cycles < adpcmDeadline) return;
    const int rate = (registers[1][0x01] & 8) ? 8000 : 4000;
    const auto period = std::max<uint64_t>(1, clock / rate);
    while (adpcmDeadline <= cycles) {
        if (!adpcmSecondNibble && adpcmFifo.empty()) {
            adpcmSampleHeld = false;
            OutputSample(adpcmDeadline, 0, false);
            adpcmDeadline = cycles + period;
            break;
        }
        if (!adpcmSecondNibble) {
            adpcmByte = adpcmFifo.front();
            adpcmFifo.pop_front();
        }
        const unsigned nibble = adpcmSecondNibble ? adpcmByte >> 4 : adpcmByte & 15;
        adpcmSecondNibble = !adpcmSecondNibble;
        adpcmSampleHeld = true;

        // Yamaha 4-bit ADPCM, as in ymfm's ADPCM-B decoder.
        constexpr int stepScale[] = {57, 57, 57, 57, 77, 102, 128, 153};
        const int delta = ((2 * int(nibble & 7) + 1) * adpcmStep) / 8;
        adpcmLevel = std::clamp(adpcmLevel + ((nibble & 8) ? -delta : delta), -32768, 32767);
        adpcmStep = std::clamp(adpcmStep * stepScale[nibble & 7] / 64, 127, 24576);
        const int sample = adpcmLevel * (registers[1][0x21] & 31) * (registers[1][0x10] & 31) / (31 * 31);
        OutputSample(adpcmDeadline, sample, false);
        adpcmDeadline += period;
    }
}

// The Yamaha voice and channel fields match the sampler already used by the Zodiac.
void SonyMa2Sound::WriteFm(uint64_t cycles, uint8_t data) {
    if (page == 0 && index == 0x06) {
        if ((data & 0x11) == 0x11) {
            if (!fmDeadline) fmCycle = fmDeadline = cycles;
        } else {
            fmDeadline = 0;
            OutputSample(cycles, 0, true);
        }
        return;
    }
    if (index < 0x10) return;
    if (page == 1 && (index == 0x30 || index == 0x31 || index == 0x38)) return;

    fm.Write(page, index, data);
}

void SonyMa2Sound::AdvanceFm(uint64_t cycles, uint64_t clock) {
    if (!fmDeadline || cycles < fmDeadline) return;
    const uint64_t period = std::max<uint64_t>(1, clock / 44100);
    while (fmDeadline <= cycles) {
        int32_t left = 0, right = 0;
        fm.Advance(fmDeadline - fmCycle, clock);
        fm.Output(left, right);
        const int sample = ((left + right) / 2) * (registers[1][0x20] & 31) / 31;
        OutputSample(fmDeadline, sample, true);
        fmCycle = fmDeadline;
        fmDeadline += period;
    }
}

void SonyMa2Sound::UpdateIrq(uint64_t cycles) {
    const bool asserted = sequencePending && (registers[1][0x01] & 1);
    if (irqAsserted == asserted) return;
    irqAsserted = asserted;
    LogTrace(cycles, 'i', 0, 0x08, asserted);
    if (onInterrupt) onInterrupt(asserted, false);
}

void SonyMa2Sound::Reset(uint64_t cycles, bool restoring) {
    memset(registers, 0, sizeof(registers));
    page = index = sequenceBytes = 0;
    ResetAdpcm(cycles);
    fm.Reset();
    fmDeadline = fmCycle = 0;
    OutputSample(cycles, 0, true);
    sequenceDeadline = sequencePeriod = 0;
    sequencePending = false;
    if (restoring) {
        irqAsserted = false;
        if (onInterrupt) onInterrupt(false, true);
    } else {
        UpdateIrq(cycles);
    }
    LogTrace(cycles, 's', 0, 0, 0);
}

void SonyMa2Sound::LogTrace(uint64_t cycles, char operation, unsigned bank, unsigned reg, unsigned value) {
    if (!trace) return;
    fprintf(trace, "%llu,%c,%u,%02x,%02x\n", static_cast<unsigned long long>(cycles),
            operation, bank, reg, value);
}
