// Seed helper for targets that read their input through FuzzedDataProvider.
#pragma once

#include <cstdint>
#include <vector>

// Builds input for targets that read it through FuzzedDataProvider: bytes are taken from the front,
// numbers from the back (the first number from the very end, most significant byte first).
struct Fdp {
    using Bytes = std::vector<uint8_t>;
    Bytes front, back;
    Fdp& Data(const Bytes& b) { return front.insert(front.end(), b.begin(), b.end()), *this; }
    // A value for ConsumeIntegralInRange(min, max), or ConsumeIntegral<T> with min 0 and max the type's.
    Fdp& Int(uint64_t value, uint64_t min, uint64_t max) {
        Bytes b;
        for (uint64_t range = max - min, v = value - min; range; range >>= 8, v >>= 8) b.push_back(uint8_t(v));
        back.insert(back.begin(), b.begin(), b.end());
        return *this;
    }
    Fdp& Bool(bool v) { return Int(v, 0, 255); }
    Bytes Build() const {
        Bytes all = front;
        all.insert(all.end(), back.begin(), back.end());
        return all;
    }
};

