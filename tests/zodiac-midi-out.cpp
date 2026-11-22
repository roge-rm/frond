// The Zodiac's MIDI out engine (native/ZodiacMidiOut.h) against a fake port and clock: real-time messages and a
// song's messages at the host times they're due and never written early, a stop with all notes off, a hold
// turning notes off and standing the clock still, lookahead for a port that schedules by timestamp, a forward
// jump dropping skipped note-ons, a late event moving the clock back, and the hooks, engine thread and a
// steady clock together, with notes off at the end. Built with ASan and UBSan, and with TSan.
#include "ZodiacMidiOut.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

using namespace zodiac_music;
using Bytes = std::vector<uint8_t>;

namespace {
constexpr uint64_t ms = 1000000;

struct Sent {
    uint64_t at;      // the clock when send was called
    uint64_t hostNs;  // when it's due
    Bytes b;
};
struct Log {
    std::mutex lock;
    std::vector<Sent> sent;
    std::vector<Sent> copy() {
        std::lock_guard<std::mutex> l(lock);
        return sent;
    }
};
std::atomic<uint64_t> fakeNow{0};

class FakePort : public ZodiacMidiOut::Port {
   public:
    FakePort(Log& log, ZodiacMidiOut::Clock clock) : log_(log), clock_(std::move(clock)) {}
    void send(uint64_t hostNs, const uint8_t* bytes, size_t size) override {
        std::lock_guard<std::mutex> l(log_.lock);
        log_.sent.push_back({clock_(), hostNs, Bytes(bytes, bytes + size)});
    }

   private:
    Log& log_;
    const ZodiacMidiOut::Clock clock_;
};

uint64_t fakeClock() { return fakeNow.load(); }

std::unique_ptr<ZodiacMidiOut> make(Log& log, uint64_t lookaheadNs = 0) {
    return std::make_unique<ZodiacMidiOut>(std::make_unique<FakePort>(log, fakeClock), lookaheadNs, fakeClock);
}

std::shared_ptr<const Smf> smf(const Bytes& body) {
    auto s = std::make_shared<Smf>();
    s->bytes = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96};
    s->bytes.insert(s->bytes.end(), {'M', 'T', 'r', 'k', 0, 0, 0, uint8_t(body.size())});
    s->bytes.insert(s->bytes.end(), body.begin(), body.end());
    return s;
}
Event ev(uint64_t t, Kind k, uint32_t handle = 0) {
    Event e;
    e.timeNs = t;
    e.kind = k;
    e.handle = handle;
    return e;
}
Event play(uint64_t t, uint32_t handle, const Bytes& body) {
    Event e = ev(t, Kind::play, handle);
    e.smf = smf(body);
    return e;
}
Event message(uint64_t t, uint8_t a, uint8_t b, uint8_t c) {
    Event e = ev(t, Kind::message, 1);
    e.bytes[0] = a, e.bytes[1] = b, e.bytes[2] = c;
    e.length = 3;
    return e;
}

// Tempo 250000 with 96 ticks a quarter: 96 ticks are 250 ms. Note on 60 at 0, note on 62 and CC 7 at 250 ms,
// note offs at 500 ms, end at 750 ms.
const Bytes body = {0x00, 0xff, 0x51, 0x03, 0x03, 0xd0, 0x90,
                    0x00, 0x90, 0x3c, 0x64,
                    0x60, 0x90, 0x3e, 0x64,
                    0x00, 0xb0, 0x07, 0x50,
                    0x60, 0x80, 0x3c, 0x00,
                    0x00, 0x80, 0x3e, 0x00,
                    0x60, 0xff, 0x2f, 0x00};

bool allOff(const std::vector<Sent>& s, size_t from, uint64_t hostNs) {
    if (s.size() < from + 32) return false;
    for (unsigned c = 0; c < 16; ++c) {
        const auto &a = s[from + c * 2], &b = s[from + c * 2 + 1];
        if (a.hostNs != hostNs || b.hostNs != hostNs || a.b != Bytes{uint8_t(0xb0 | c), 123, 0} ||
            b.b != Bytes{uint8_t(0xb0 | c), 120, 0})
            return false;
    }
    return true;
}
// Never written before it's due less the lookahead, nor late by more than a step.
bool onTime(const std::vector<Sent>& s, uint64_t lookaheadNs, uint64_t stepNs) {
    for (const auto& x : s)
        if (x.at + lookaheadNs < x.hostNs || x.at + lookaheadNs > x.hostNs + stepNs) return false;
    return true;
}
// Steps the fake clock 1 ms at a time to t, a pass each step.
void runTo(ZodiacMidiOut& out, uint64_t t) {
    while (fakeNow.load() < t) {
        fakeNow += ms;
        out.work();
    }
}

void stream() {
    Log log;
    fakeNow = 10000 * ms;
    {
        auto out = make(log);
        // The first event sets the clock: guest 100 ms is host 10 s.
        out->offer(message(100 * ms, 0xc0, 0x05, 0));
        out->work();
        auto s = log.copy();
        assert(s.size() == 1 && s[0].hostNs == 10000 * ms && s[0].b == (Bytes{0xc0, 0x05, 0}));
        out->offer(play(200 * ms, 7, body));
        runTo(*out, 10099 * ms);
        assert(log.copy().size() == 1);
        runTo(*out, 10100 * ms);
        s = log.copy();
        assert(s.size() == 2 && s[1].hostNs == 10100 * ms && s[1].b == (Bytes{0x90, 0x3c, 0x64}));
        runTo(*out, 10349 * ms);
        assert(log.copy().size() == 2);
        runTo(*out, 10350 * ms);
        s = log.copy();
        assert(s.size() == 4 && s[2].b == (Bytes{0x90, 0x3e, 0x64}) && s[3].b == (Bytes{0xb0, 0x07, 0x50}));
        assert(s[2].hostNs == 10350 * ms && s[3].hostNs == 10350 * ms);
        // The note offs, the end with notes off at 10.85 s (the song stops by itself before the stop at 10.9 s,
        // which sends notes off again). Events come about when the guest makes them: one far ahead would move the
        // clock.
        runTo(*out, 10800 * ms);
        out->offer(ev(1000 * ms, Kind::stop, 7));
        runTo(*out, 10950 * ms);
        s = log.copy();
        assert(s.size() == 6 + 32 + 32 && s[4].hostNs == 10600 * ms && s[5].b == (Bytes{0x80, 0x3e, 0}));
        assert(allOff(s, 6, 10850 * ms) && allOff(s, 38, 10900 * ms));
        assert(onTime(s, 0, ms));
        assert(!out->playing() && !out->outstanding() && !out->cuesWaiting());
    }
    // The end sends notes off once more.
    const auto s = log.copy();
    assert(s.size() == 70 + 32 && allOff(s, 70, 10950 * ms));
}

void hold() {
    Log log;
    fakeNow = 10000 * ms;
    {
        auto out = make(log);
        out->offer(play(100 * ms, 7, body));
        out->work();
        runTo(*out, 10100 * ms);
        assert(log.copy().size() == 1);
        // Paused from 10.2 s to 15 s: notes off once, then nothing.
        runTo(*out, 10200 * ms);
        out->hold(true);
        runTo(*out, 15000 * ms);
        auto s = log.copy();
        assert(s.size() == 33 && allOff(s, 1, 10200 * ms));
        out->hold(false);
        // The song's 250 ms events come 4.8 s later than they would have.
        runTo(*out, 15049 * ms);
        assert(log.copy().size() == 33);
        runTo(*out, 15050 * ms);
        s = log.copy();
        assert(s.size() == 35 && s[33].hostNs == 15050 * ms && s[33].b == (Bytes{0x90, 0x3e, 0x64}));
        assert(onTime(s, 0, ms));
    }
    // Ended while playing: notes off.
    const auto s = log.copy();
    assert(s.size() == 35 + 32 && allOff(s, 35, 15050 * ms));
}

void lookahead() {
    Log log;
    fakeNow = 10000 * ms;
    auto out = make(log, 20 * ms);
    out->offer(play(100 * ms, 7, body));
    out->work();
    auto s = log.copy();
    // Sent at once, due 20 ms on.
    assert(s.size() == 1 && s[0].at == 10000 * ms && s[0].hostNs == 10020 * ms);
    runTo(*out, 10249 * ms);
    assert(log.copy().size() == 1);
    runTo(*out, 10250 * ms);
    s = log.copy();
    assert(s.size() == 3 && s[1].hostNs == 10270 * ms && s[1].at == 10250 * ms);
    assert(onTime(s, 20 * ms, ms));
}

void jumpForward() {
    Log log;
    fakeNow = 10000 * ms;
    auto out = make(log);
    out->offer(play(100 * ms, 7, body));
    out->work();
    runTo(*out, 10100 * ms);
    // The guest ran 5 s ahead (Uncapped): the song's skipped span keeps its state but drops its note-ons.
    out->offer(message(5000 * ms, 0x91, 0x40, 0x7f));
    out->work();
    const auto s = log.copy();
    assert(s.size() == 1 + 3 + 32 + 1);
    assert(s[1].b == (Bytes{0xb0, 0x07, 0x50}) && s[2].b == (Bytes{0x80, 0x3c, 0}) && s[3].b == (Bytes{0x80, 0x3e, 0}));
    assert(allOff(s, 4, 10100 * ms - 5000 * ms + 850 * ms));
    for (const auto& x : s) assert(x.b != (Bytes{0x90, 0x3e, 0x64}));
    assert(s.back().b == (Bytes{0x91, 0x40, 0x7f}) && s.back().hostNs == 10100 * ms);
}

void lateEvent() {
    Log log;
    fakeNow = 10000 * ms;
    auto out = make(log);
    out->offer(message(100 * ms, 0x90, 0x3c, 0x64));
    out->work();
    // 2 s on, the guest is only 200 ms further (running slow): the clock goes back to it.
    fakeNow = 12000 * ms;
    out->offer(message(300 * ms, 0x80, 0x3c, 0));
    out->work();
    out->offer(message(310 * ms, 0x90, 0x3e, 0x64));
    runTo(*out, 12009 * ms);
    assert(log.copy().size() == 2);
    runTo(*out, 12010 * ms);
    const auto s = log.copy();
    assert(s.size() == 3 && s[1].hostNs == 12000 * ms && s[2].hostNs == 12010 * ms);
}

// Messages go out the delay after the guest made them; nextWakeNs says when the next is due.
void delayed() {
    Log log;
    fakeNow = 10000 * ms;
    auto out = make(log);
    out->setDelay(50 * ms);
    assert(out->delay() == 50 * ms);
    out->offer(play(100 * ms, 7, body));
    out->work();
    // Anchored at 10 s, the song's first note due 50 ms on.
    assert(log.copy().empty() && out->nextWakeNs() == 10050 * ms);
    runTo(*out, 10049 * ms);
    assert(log.copy().empty());
    runTo(*out, 10050 * ms);
    auto s = log.copy();
    assert(s.size() == 1 && s[0].hostNs == 10050 * ms && s[0].b == (Bytes{0x90, 0x3c, 0x64}));
    assert(out->nextWakeNs() == 10300 * ms);
    runTo(*out, 10300 * ms);
    s = log.copy();
    assert(s.size() == 3 && s[1].hostNs == 10300 * ms);
    assert(onTime(s, 0, ms));
    // Held: nothing to wake for.
    out->hold(true);
    assert(out->nextWakeNs() == UINT64_MAX);
    out->hold(false);
    // Clamped under the drift limit.
    out->setDelay(10000 * ms);
    assert(out->delay() == ZodiacMidiOut::maxDelayNs);
}

// The engine's thread sleeps with nothing due, and wakes for each message of a song on time.
void sleeps() {
    Log log;
    {
        ZodiacMidiOut out(std::make_unique<FakePort>(log, ZodiacMidiOut::steadyNs));
        out.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const uint64_t idle = out.passes();
        fprintf(stderr, "idle: %llu passes in 300 ms\n", (unsigned long long)idle);
        assert(idle <= 2);
        out.offer(play(100 * ms, 7, body));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (log.copy().size() < 5 + 32) {
            if (std::chrono::steady_clock::now() >= deadline) {
                fprintf(stderr, "stuck: %zu sent, %llu passes\n", log.copy().size(), (unsigned long long)out.passes());
                abort();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const uint64_t song = out.passes() - idle;
        fprintf(stderr, "song: %llu passes for 750 ms\n", (unsigned long long)song);
        assert(song < 40);
        const auto s = log.copy();
        // Never early, and late by less than 50 ms (sanitizers slow it).
        for (const auto& x : s) assert(x.at >= x.hostNs && x.at < x.hostNs + 50 * ms);
        out.stop();
    }
}

// The hooks on this thread, the engine on its own with a steady clock, as in the app.
void threads() {
    constexpr uint32_t noteOnPc = 0x1000;
    Library lib;
    for (auto& pc : lib.pcs) pc = 0xfffffff0;
    lib.pcs[unsigned(Call::noteOn)] = noteOnPc;
    Hooks hooks;
    hooks.enable();
    Log log;
    {
        ZodiacMidiOut out(std::make_unique<FakePort>(log, ZodiacMidiOut::steadyNs));
        out.attach(&hooks);
        out.start();
        const auto read = [](uint32_t, void*, uint32_t) { return false; };
        for (uint32_t i = 0; i < 50; ++i) {
            const uint32_t r[16] = {1, 0, 60 + i % 12, 100};
            const bool hit = hooks.hit(lib, noteOnPc, r, i * ms, read);
            assert(hit);
            (void)hit;
            out.poke();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (log.copy().size() < 50 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        out.stop();
        const auto s = log.copy();
        assert(s.size() == 50);
        for (uint32_t i = 0; i < 50; ++i) assert(s[i].b == (Bytes{0x90, uint8_t(60 + i % 12), 100}));
        // Written by the engine's thread when due, not before.
        for (const auto& x : s) assert(x.at >= x.hostNs);
    }
    const auto s = log.copy();
    assert(s.size() == 50 + 32 && !hooks.pending() && !hooks.stats().dropped);
}
}  // namespace

int main() {
    stream();
    hold();
    lookahead();
    jumpForward();
    lateEvent();
    delayed();
    sleeps();
    threads();
    std::puts("PASS: Zodiac MIDI out (event stream, song times, never early, stop, hold, lookahead, forward jump, late event, delay, "
              "sleeping thread, hooks and engine thread, notes off at the end)");
    return 0;
}
