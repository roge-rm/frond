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
    B stalled(0, now); stalled.BeginFileTransfer(16 * 1024 * 1024, 0, now);
    rejected([&] { stalled.Check(0, now + seconds(30)); });
    B copying(0, now); copying.BeginFileTransfer(16 * 1024 * 1024, 0, now);
    for (unsigned i = 0; i < 512; ++i) copying.FileProgress(32768, 0, now + milliseconds(i * 300));
    copying.RequireFileComplete();
    rejected([&] { copying.FileProgress(1, 0, now + seconds(154)); });
    B fixedDeadline(0, now); fixedDeadline.BeginFileTransfer(16 * 1024 * 1024, 0, now);
    for (unsigned i = 1; i <= 6; ++i) fixedDeadline.FileProgress(1, 0, now + seconds(i * 29));
    fixedDeadline.Check(0, now + seconds(200));
    rejected([&] { fixedDeadline.Check(0, now + seconds(201)); });
    rejected([&] { fixedDeadline.RequireFileComplete(); });
    B invalidPlan(0, now);
    rejected([&] { invalidPlan.BeginFileTransfer(16 * 1024 * 1024 + 1, 0, now); });
    B once(0, now); once.BeginFileTransfer(1024, 0, now);
    rejected([&] { once.BeginFileTransfer(1024, 0, now); });
    rejected([&] { once.FileProgress(0, 0, now); });
    rejected([&] { once.FileProgress(1025, 0, now); });
    B empty(0, now); empty.BeginFileTransfer(0, 0, now); empty.RequireFileComplete();
    rejected([&] { empty.Check(0, now + seconds(30)); });
    B fileGuest(0); fileGuest.BeginFileTransfer(16 * 1024 * 1024, 0);
    fileGuest.Check(62949672959ull);
    rejected([&] { fileGuest.Check(62949672960ull); });
    B fileCycles(0); fileCycles.BeginFileTransfer(16 * 1024 * 1024, 0);
    fileCycles.Charge(1000000000ull, 0);
    rejected([&] { fileCycles.Charge(4294967296ull, 0); });
    puts("TG50 absolute host/guest/cycle/call/callback budgets passed");
}
