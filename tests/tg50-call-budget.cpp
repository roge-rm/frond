#include <cassert>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include "tg50_call_budget.h"

template<class F> static void rejected(F run) {
    bool threw = false;
    try { run(); } catch (const std::runtime_error&) { threw = true; }
    assert(threw);
}
int main() {
    using B = Tg50Pace::Budget;
    using namespace std::chrono;
    const auto now = B::Clock::now();
    B host(100, now);
    host.Check(100, now + seconds(29));
    rejected([&] { host.Check(100, now + seconds(30)); });
    rejected([&] { host.Check(100, now - seconds(1)); });
    B guest(100);
    guest.Check(20000000099ull);
    rejected([&] { guest.Check(20000000100ull); });
    rejected([&] { guest.Check(99); });
    B progress(0);
    // Callbacks and repeated execution slices share one transaction budget.
    for (unsigned i = 0; i < 999; ++i) {
        progress.EnterCall(0);
        progress.EnterCallback(0);
        progress.Charge(1000000, 0);
    }
    assert(progress.Slice(0) == 1000000);
    rejected([&] { progress.Charge(1000000, 0); });
    B overflow(0);
    rejected([&] { overflow.Charge(UINT64_MAX, 0); });
    B calls(0), callbacks(0);
    for (unsigned i = 0; i < 100000; ++i) calls.EnterCall(0);
    rejected([&] { calls.EnterCall(0); });
    for (unsigned i = 0; i < 65536; ++i) callbacks.EnterCallback(0);
    rejected([&] { callbacks.EnterCallback(0); });
    puts("TG50 absolute host/guest/cycle/call/callback budgets passed");
}
