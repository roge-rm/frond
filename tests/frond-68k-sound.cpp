// The frond-68k piezo: tone changes at guest cycles rendered as a square wave at the right
// frequency, start and length, with silence between and across a restart.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "Sound.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

// Rising edges and where the sound starts and ends, in samples.
struct Tone {
    int edges{0}, first{-1}, last{-1};
};

Tone measure(const std::vector<int16_t>& s, size_t from, size_t to) {
    Tone tone;
    for (size_t i = std::max<size_t>(from, 1); i < to && i < s.size(); ++i)
        if (s[i] - s[i - 1] > 1000) {
            ++tone.edges;
            if (tone.first < 0) tone.first = int(i);
            tone.last = int(i);
        }
    return tone;
}

}  // namespace

int main() {
    constexpr double clock = 16580608;
    Piezo68k piezo;
    std::vector<int16_t> out;
    auto at = [&](double seconds) { return uint64_t(seconds * clock); };
    piezo.Change(at(0.1), 440, 0.04);
    piezo.Change(at(0.4), -1, -1);
    piezo.Change(at(0.5), 2500, 0.07);
    piezo.Change(at(0.6), -1, -1);
    // Rendered in uneven slices like the runner's.
    for (double t = 0.013; t < 0.8; t += 0.017) piezo.Render(at(t), clock, &out);
    piezo.Render(at(0.8), clock, &out);
    const int rate = Piezo68k::kRate;
    check(std::abs(int(out.size()) - int(0.8 * rate)) <= 1, "sample count follows guest time");
    const Tone a = measure(out, 0, size_t(0.45 * rate)), b = measure(out, size_t(0.45 * rate), out.size());
    check(std::abs(a.first - int(0.1 * rate)) <= 2, "440 Hz starts at its cycle");
    check(std::abs(a.edges - 132) <= 1, "440 Hz for 300 ms");
    check(std::abs(b.edges - 250) <= 1, "2500 Hz for 100 ms");
    check(std::abs(b.last - int(0.6 * rate)) <= 20, "2500 Hz stops at its cycle");
    bool quiet = true;
    for (size_t i = size_t(0.401 * rate); i < size_t(0.499 * rate); ++i) quiet = quiet && out[i] == 0;
    check(quiet, "silence between tones");
    int peak = 0;
    for (auto v : out) peak = std::max(peak, std::abs(int(v)));
    check(peak > 10000 && peak < 32767, "audible level without clipping");

    // A restart drops pending changes and leaves silence.
    piezo.Change(at(0.9), 1000, 0.5);
    piezo.Restart(at(0.85));
    out.clear();
    piezo.Render(at(1.0), clock, &out);
    check(measure(out, 0, out.size()).edges == 0 && !piezo.Playing(), "restart silences");
    // Skipping without output still applies changes.
    piezo.Change(at(1.1), 1000, 0.5);
    piezo.Render(at(1.2), clock, nullptr);
    check(piezo.Playing(), "skipped render applies a tone change");
    if (failures) return 1;
    std::puts("frond-68k sound: ok");
    return 0;
}
