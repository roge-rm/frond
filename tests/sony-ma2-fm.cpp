#include "SonyMa2Fm.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <vector>

namespace {

void Note(SonyMa2Fm& fm, unsigned ch, unsigned algorithm) {
    constexpr uint8_t tone[] = {0x14, 0x42, 0xf3, 0x16, 0xa0};
    const unsigned slot = (ch / 4) * 8 + ch % 4;
    fm.Write(1, 0x10, 31);
    fm.Write(0, 0xd0 + ch, 0x10 | algorithm);
    for (unsigned op = 0; op < 4; ++op)
        for (unsigned group = 0; group < 5; ++group)
            fm.Write(0, 0x10 + slot + 4 * op + 0x20 * group, tone[group] + (group == 0 ? 16 * op : 0));
    fm.Write(0, 0xb0 + ch, 0x44);
    fm.Write(0, 0xc0 + ch, 0x32);
}

std::vector<int32_t> Render(SonyMa2Fm& fm) {
    std::vector<int32_t> samples;
    for (unsigned i = 0; i < 512; ++i) {
        fm.Advance(751, 33161216);
        int32_t left = 0, right = 0;
        fm.Output(left, right);
        assert(left == right);
        samples.push_back(left);
    }
    return samples;
}

}  // namespace

int main() {
    constexpr unsigned voices[] = {0, 1, 2, 3, 8, 9, 10, 11};
    std::array<std::vector<int32_t>, 4> routes;
    for (unsigned algorithm = 2; algorithm <= 5; ++algorithm) {
        for (unsigned ch : voices) {
            SonyMa2Fm fm;
            Note(fm, ch, algorithm);

            SonyMa2Fm restored;
            // Save after the topology and key writes, before the first sound clock.
            assert(restored.Load(fm.Save()));
            const auto first = Render(fm);
            assert(first == Render(restored));
            assert(std::any_of(first.begin(), first.end(), [](auto sample) { return sample != 0; }));
            if (!ch) routes[algorithm - 2] = first;
            assert(routes[algorithm - 2] == first);

            // Save again with a running envelope, feedback and interpolation position.
            const auto saved = fm.Save();
            assert(saved.size() == SonyMa2Fm::StateSize());
            assert(restored.Load(saved));
            assert(Render(fm) == Render(restored));
            fm.Write(0, 0xc0 + ch, 0x12);
            restored.Write(0, 0xc0 + ch, 0x12);
            assert(Render(fm) == Render(restored));
        }
    }
    for (unsigned a = 0; a < routes.size(); ++a)
        for (unsigned b = a + 1; b < routes.size(); ++b)
            assert(routes[a] != routes[b]);

    SonyMa2Fm fresh;
    assert(fresh.Load(SonyMa2Fm().Save()));
    auto truncated = fresh.Save();
    truncated.pop_back();
    assert(!fresh.Load(truncated));
}
