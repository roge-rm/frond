// imx1StepsUntil's 64-bit and divided-denominator branches against the plain 128-bit count, on random clock-like
// values and on edge cases: products that just fit or overflow 64 bits, a phase just under the target, n or d of 1.
#include "imx1_reference_time.h"

#include <cstdio>
#include <cstdlib>
#include <random>

namespace {

// The count from the 128-bit sums alone, as imx1StepsUntil works it out when nothing smaller fits.
uint64_t reference(uint64_t phase, uint64_t den, uint64_t n, uint64_t d, uint64_t target) {
    if (!target || !n || !d || !den) return 1;
    const __uint128_t max = ~__uint128_t(0);
    const __uint128_t whole = __uint128_t(target) * den;
    if (whole <= phase) return 1;
    const __uint128_t need = whole - phase;
    __uint128_t steps;
    if (need > max / d) steps = __uint128_t(target - 1) * d / n + 1;
    else {
        const __uint128_t divisor = __uint128_t(den) * n;
        steps = need * d / divisor + (need * d % divisor != 0);
    }
    return steps > UINT64_MAX ? UINT64_MAX : steps ? uint64_t(steps) : 1;
}

uint64_t checked = 0, divided = 0;

void check(uint64_t phase, uint64_t den, uint64_t n, uint64_t d, uint64_t target) {
    ++checked;
    if (d && den % d == 0) ++divided;
    const uint64_t got = imx1StepsUntil(phase, den, n, d, target), want = reference(phase, den, n, d, target);
    if (got != want) {
        std::printf("FAIL: imx1StepsUntil(%llu, %llu, %llu, %llu, %llu) = %llu, want %llu\n",
                    (unsigned long long)phase, (unsigned long long)den, (unsigned long long)n, (unsigned long long)d,
                    (unsigned long long)target, (unsigned long long)got, (unsigned long long)want);
        std::exit(1);
    }
}

}  // namespace

int main() {
    std::mt19937_64 r(1);
    for (int i = 0; i < 2000000; ++i) {
        // Mostly d dividing den, as the clock denominators do, with some unrelated ones.
        const uint64_t d = r() % 3 == 0 ? (r() % 100000 + 1) * 64 : r() % (1ull << (r() % 40)) + 1;
        const uint64_t k = r() % (1ull << (r() % 34)) + 1;
        uint64_t den = d * k;
        if (den / k != d || den > (1ull << 62)) continue;
        if (r() % 8 == 0) den += r() % 7 + 1;
        const uint64_t n = r() % (1ull << (r() % 45)) + 1;
        const uint64_t phase = den > 1 ? r() % den : 0;
        const uint64_t target = r() % 8 == 0 ? UINT64_MAX - r() % 4 : r() % (1ull << (r() % 50)) + r() % 2;
        check(phase, den, n, d, target);
    }
    const unsigned long long big = UINT64_MAX;
    for (unsigned long long d : {1ull, 2ull, 3ull, 64ull, 1ull << 32, big}) {
        for (unsigned long long k : {1ull, 2ull, 7ull, 1ull << 31, 1ull << 32}) {
            const unsigned long long den = d * k;
            if (den / k != d) continue;
            for (unsigned long long n : {1ull, 3ull, 1ull << 31, 1ull << 33, big})
                for (unsigned long long target : {1ull, 2ull, big / den, big / den + 1, big / d, big / d + 1, big - 1, big})
                    for (unsigned long long phase : {0ull, 1ull, den / 2, den - 1, den, big}) check(phase, den, n, d, target);
        }
    }
    check(0, 0, 1, 1, 5);
    check(0, 1, 0, 1, 5);
    check(0, 1, 1, 0, 5);
    check(0, 1, 1, 1, 0);
    std::printf("PASS: imx1StepsUntil matches the 128-bit count in %llu cases (%llu with d dividing den)\n",
                (unsigned long long)checked, (unsigned long long)divided);
    return 0;
}
