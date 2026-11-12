#ifndef FROND_IMX1_REFERENCE_TIME_H
#define FROND_IMX1_REFERENCE_TIME_H

#include <cassert>
#include <cstdint>
#include <limits>
#include <numeric>

// std::gcd, answering at once when the two are equal or one divides the other, as the clock denominators
// nearly always do.
inline uint64_t imx1Gcd(uint64_t a, uint64_t b) {
    if (a == b) return a;
    if (a && b % a == 0) return a;
    if (b && a % b == 0) return b;
    return std::gcd(a, b);
}
// Smallest count of n/d steps after which floor(phase/den + steps*n/d) reaches target.
// When the product doesn't fit 128 bits it returns a smaller count, never a larger one. At least 1.
inline uint64_t imx1StepsUntil(uint64_t phase, uint64_t den, uint64_t n, uint64_t d, uint64_t target) {
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
// Smallest count of steps of stepClocks source clocks after which a divider with this phase has made cycles cycles.
inline uint64_t imx1StepsUntilDivided(uint64_t cycles, uint64_t divider, uint64_t phase, uint64_t stepClocks) {
    if (!cycles || !divider || !stepClocks) return 1;
    const __uint128_t need = __uint128_t(cycles) * divider;
    if (need <= phase) return 1;
    const __uint128_t steps = (need - phase + stepClocks - 1) / stepClocks;
    return steps > UINT64_MAX ? UINT64_MAX : uint64_t(steps);
}

// A deadline can have an unrelated denominator; its exact difference needs 128 bits.
struct Imx1ReferenceInterval {
    uint64_t ticks = 0;
    __uint128_t numerator = 0, denominator = 1;
    bool positive() const { return ticks || numerator; }
};

// Exact CLK32 periods. Nanoseconds are a view; the proper fraction is retained.
struct Imx1ReferenceTime {
    uint64_t ticks = 0, numerator = 0, denominator = 1;
    bool valid() const { return denominator && numerator < denominator; }
    bool supports(uint64_t d) const {
        return valid() && d && d / imx1Gcd(d, denominator) <= UINT64_MAX / denominator;
    }
    bool add(uint64_t n, uint64_t d) {
        if (!valid() || !supports(d)) return false;
        const auto common = denominator * (d / imx1Gcd(d, denominator));
        const __uint128_t fraction = __uint128_t(numerator) * (common / denominator) +
                                     __uint128_t(n % d) * (common / d);
        const __uint128_t whole = __uint128_t(ticks) + n / d + fraction / common;
        if (whole > UINT64_MAX) return false;
        ticks = uint64_t(whole); numerator = uint64_t(fraction % common); denominator = common;
        return true;
    }
    int compare(const Imx1ReferenceTime& other) const {
        if (ticks != other.ticks) return ticks < other.ticks ? -1 : 1;
        const auto a = __uint128_t(numerator) * other.denominator;
        const auto b = __uint128_t(other.numerator) * denominator;
        return a < b ? -1 : a > b ? 1 : 0;
    }
    // Requires this >= earlier and a representable common denominator.
    Imx1ReferenceTime since(const Imx1ReferenceTime& earlier) const {
        assert(valid() && earlier.valid() && supports(earlier.denominator) && compare(earlier) >= 0);
        const auto common = denominator * (earlier.denominator / imx1Gcd(denominator, earlier.denominator));
        const auto a = numerator * (common / denominator), b = earlier.numerator * (common / earlier.denominator);
        Imx1ReferenceTime delta{ticks - earlier.ticks - (a < b), a < b ? common - (b - a) : a - b, common};
        const auto divisor = imx1Gcd(delta.numerator, delta.denominator);
        delta.numerator /= divisor; delta.denominator /= divisor;
        return delta;
    }
    Imx1ReferenceInterval intervalSince(const Imx1ReferenceTime& earlier) const {
        assert(valid() && earlier.valid() && compare(earlier) >= 0);
        const auto common = __uint128_t(denominator) * (earlier.denominator / imx1Gcd(denominator, earlier.denominator));
        const auto a = __uint128_t(numerator) * (common / denominator);
        const auto b = __uint128_t(earlier.numerator) * (common / earlier.denominator);
        return {ticks - earlier.ticks - (a < b), a < b ? common - (b - a) : a - b, common};
    }
    bool nanoseconds(uint64_t& value) const {
        if (!valid()) return false;
        // Split before multiplying by 1e9 so the full uint64 tick range fits 128 bits.
        const __uint128_t ns = __uint128_t(ticks / 32768) * 1000000000 +
            ((__uint128_t(ticks % 32768) * denominator + numerator) * 1000000000) /
            (__uint128_t(32768) * denominator);
        if (ns > UINT64_MAX) return false;
        value = uint64_t(ns); return true;
    }
};
#endif
