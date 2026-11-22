#ifndef SonyMa2Sound_h
#define SonyMa2Sound_h

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>

#include "SonyMa2Fm.h"

class Chunk;
class ChunkProbe;

class SonyMa2Sound {
   public:
    enum class SequenceStatus { readToClear, fifoLevel };

    explicit SonyMa2Sound(SequenceStatus status = SequenceStatus::readToClear) : sequenceStatus(status) {}
    void Reset(uint64_t cycles, bool restoring = false);
    void SelectRegister(uint8_t value) { index = value; }
    void Write(uint64_t now, uint64_t clock, uint8_t data);
    uint8_t Read(uint64_t now, uint64_t clock);
    void Advance(uint64_t cycles, uint64_t clock);
    void Save(Chunk& chunk);
    void Save(ChunkProbe& chunk);
    bool Load(Chunk& chunk, uint64_t now);
    void SetTrace(FILE* output, uint64_t now);
    void RestoreOutput(uint64_t cycles);
    int MixedSample() const { return int(adpcmSample) + fmSample; }

    std::function<void(bool asserted, bool restoring)> onInterrupt;
    std::function<void(uint64_t, int16_t, bool)> onSample;

   private:
    template <typename T> void SaveState(T& chunk);
    void OutputSample(uint64_t cycles, int sample, bool isFm);
    void UpdateIrq(uint64_t cycles);
    void AdvanceAdpcm(uint64_t cycles, uint64_t clock);
    void ResetAdpcm(uint64_t cycles);
    uint8_t AdpcmStatus() const;
    void AdvanceFm(uint64_t cycles, uint64_t clock);
    void WriteFm(uint64_t cycles, uint8_t data);
    void LogTrace(uint64_t cycles, char operation, unsigned page, unsigned index, unsigned value);

    const SequenceStatus sequenceStatus;
    FILE* trace{nullptr};
    uint8_t registers[2][256]{};
    unsigned page{0}, index{0}, sequenceBytes{0};
    uint64_t sequenceDeadline{0}, sequencePeriod{0};
    bool sequencePending{false}, irqAsserted{false};

    std::deque<uint8_t> adpcmFifo;
    uint64_t adpcmDeadline{0};
    int adpcmLevel{0}, adpcmStep{127};
    uint8_t adpcmByte{0};
    bool adpcmSecondNibble{false}, adpcmSampleHeld{false};

    SonyMa2Fm fm;
    int16_t adpcmSample{0}, fmSample{0};
    uint64_t fmDeadline{0}, fmCycle{0};
};

#endif
