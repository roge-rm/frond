#ifndef SONY_MA2_FM_H
#define SONY_MA2_FM_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class SonyMa2Fm {
public:
    SonyMa2Fm();
    ~SonyMa2Fm();

    void Write(unsigned page, unsigned index, uint8_t data);
    void Advance(uint64_t cycles, uint32_t clock);
    void Output(int32_t& left, int32_t& right) const;

    std::vector<uint8_t> Save() const;
    bool Load(const std::vector<uint8_t>& data);
    static size_t StateSize();

private:
    struct Core;
    std::unique_ptr<Core> core;
};

#endif
