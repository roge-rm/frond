// The Zodiac speed line (ZodiacSpeed.h): the FM engine's sounding share over a window, "FM off" when the engine made
// no samples, and nothing about FM when the counts aren't given (the rx1950) or went back (a state load).
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <string>

#include "ZodiacSpeed.h"

namespace {
bool has(const std::string& line, const char* text) { return line.find(text) != std::string::npos; }
}  // namespace

int main() {
    ZodiacSpeed z(5000000);
    z.Fm(1000, 0);
    assert(!z.Sample(0, 0, 0));
    // 5 s, 248580 samples (49716 a second), a quarter of them sounding.
    z.Fm(1000 + 248580, 62145);
    bool sampled = z.Sample(5000000, 1000, 5 * 32768);
    assert(sampled && has(z.Line(), "; FM sounding 25.0% of 49.7k samples/s"));
    // No samples in the window: the chip is off.
    sampled = z.Sample(10000000, 2000, 10 * 32768);
    assert(sampled && has(z.Line(), "; FM off"));
    // Counts that went back leave FM out of the line.
    z.Fm(10, 5);
    sampled = z.Sample(15000000, 3000, 15 * 32768);
    assert(sampled && !has(z.Line(), "FM"));
    // The rx1950 never gives them.
    ZodiacSpeed rx(5000000, "rx1950 speed");
    rx.Sample(0, 0, 0);
    sampled = rx.Sample(5000000, 1000, 5 * 32768);
    assert(sampled && !has(rx.Line(), "FM"));
    std::puts("PASS: zodiac speed line gives the FM sounding share, FM off, and nothing without counts");
    return 0;
}
