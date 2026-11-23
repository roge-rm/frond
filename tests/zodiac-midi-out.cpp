// The Zodiac's MIDI out engine (native/ZodiacMidiOut.h) against a fake port and clock: real-time messages and a
// song's messages at the host times they're due and never written early, a stop with all notes off, a hold
// turning notes off and standing the clock still, lookahead for a port that schedules by timestamp, a forward
// jump dropping skipped note-ons, a late event moving the clock back, a slow guest by the guest audio's clock, its
// stamps in order through callback bursts, a resume waiting for a new period, the clock's two threads, audio off
// on the host clock, a queue clear after a hold, and
// the hooks, engine thread and a
// steady clock together, with notes off at the end. Built with ASan and UBSan, and with TSan.
#include "ZodiacMidiOut.h"
#include "ZodiacRtMidi.h"

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
    // Stamped in the past by the old offset, so at the last stamp (the first note's): stamps never go back.
    assert(allOff(s, 4, 10000 * ms));
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

// With a delay, the event that moves the clock, forward or back, waits for the delay too.
void driftDelayed() {
    Log log;
    fakeNow = 10000 * ms;
    auto out = make(log);
    out->setDelay(50 * ms);
    out->offer(message(100 * ms, 0x90, 0x3c, 0x64));
    out->work();
    runTo(*out, 10050 * ms);
    assert(log.copy().size() == 1);
    runTo(*out, 10100 * ms);
    // 5 s ahead: the offset moves, the message goes out 50 ms on.
    out->offer(message(5000 * ms, 0x91, 0x40, 0x7f));
    out->work();
    assert(log.copy().size() == 1 && out->nextWakeNs() == 10150 * ms);
    runTo(*out, 10149 * ms);
    assert(log.copy().size() == 1);
    runTo(*out, 10150 * ms);
    auto s = log.copy();
    assert(s.size() == 2 && s[1].at == 10150 * ms && s[1].hostNs == 10150 * ms && s[1].b == (Bytes{0x91, 0x40, 0x7f}));
    // 2 s on, the guest only 100 ms further: back to it, again after the delay.
    fakeNow = 12150 * ms;
    out->offer(message(5100 * ms, 0x81, 0x40, 0));
    out->work();
    assert(log.copy().size() == 2);
    runTo(*out, 12199 * ms);
    assert(log.copy().size() == 2);
    runTo(*out, 12200 * ms);
    s = log.copy();
    assert(s.size() == 3 && s[2].at == 12200 * ms && s[2].hostNs == 12200 * ms);
}

// On the guest audio's clock: a guest at half real speed (every other period an underrun), its song's messages go
// out when the guest audio of their time plays, by the periods the clock published, not at real speed from the
// first event. synthLag takes the external synth's own lag off the timestamps.
void guestPaced() {
    for (const uint64_t lagNs : {uint64_t(0), 20 * ms}) {
        Log log;
        fakeNow = 10000 * ms;
        GuestAudioClock clock;
        auto out = make(log);
        out->setReference(&clock);
        out->setSynthLag(lagNs);
        constexpr size_t period = 512;
        const uint64_t periodNs = GuestAudioClock::framesNs(period);
        uint64_t guestNs = 0, nextCallback = fakeNow.load();
        // Host ns when each full period's guest audio plays, with its guest ns.
        std::vector<std::pair<uint64_t, uint64_t>> plays;
        bool full = true, offered = false;
        while (fakeNow.load() < 11500 * ms) {
            if (fakeNow.load() >= nextCallback) {
                AudioQueueSpan span;
                span.known = true;
                span.startNs = span.endNs = guestNs;
                if (full) {
                    span.frames = period;
                    span.endNs = guestNs + periodNs;
                    plays.push_back({nextCallback + periodNs, guestNs});
                    guestNs = span.endNs;
                }
                clock.run(span, period, nextCallback);
                full = !full;
                nextCallback += periodNs;
            }
            if (!offered && guestNs >= 50 * ms) {
                out->offer(play(100 * ms, 7, body));
                offered = true;
            }
            fakeNow += ms;
            out->work();
        }
        // When the guest audio of guest ns g plays.
        const auto playsAt = [&](uint64_t g) {
            for (const auto& p : plays)
                if (g < p.second + periodNs) return p.first + (g - p.second);
            return uint64_t(0);
        };
        const auto s = log.copy();
        assert(s.size() == 5);
        const uint64_t due[] = {100 * ms, 350 * ms, 350 * ms, 600 * ms, 600 * ms};
        for (size_t i = 0; i < s.size(); ++i) {
            const int64_t off = int64_t(s[i].hostNs + lagNs) - int64_t(playsAt(due[i]));
            // Within a period (the clock runs on over each underrun and stands to wait); sent the lag ahead, its
            // time is a guess past the last period at real speed, so up to the lag early with this slow a guest.
            assert(off > -int64_t(periodNs + lagNs + 2 * ms) && off < int64_t(periodNs + 2 * ms));
            // Never written after it's due, nor much before (the engine's 5 ms wake).
            assert(s[i].at <= s[i].hostNs + ms && s[i].at + 6 * ms >= s[i].hostNs);
        }
        // Half speed: the 250 ms between the notes take about 500 ms.
        assert(s[1].hostNs - s[0].hostNs > 450 * ms && s[1].hostNs - s[0].hostNs < 550 * ms);
    }
}

// Timestamps never go back, though the map moves under them: callbacks in bursts (two 1 ms apart, then a gap)
// move each published period's host time, and with a lookahead messages go out by the older period.
void stampsInOrder() {
    Log log;
    fakeNow = 10000 * ms;
    GuestAudioClock clock;
    auto out = make(log, 30 * ms);
    out->setReference(&clock);
    constexpr size_t period = 512;
    const uint64_t periodNs = GuestAudioClock::framesNs(period);
    uint64_t guestNs = 0, nextCallback = fakeNow.load(), pair = 0;
    for (uint32_t i = 0; i < 200; ++i) out->offer(message(100 * ms + i * ms, 0x90, uint8_t(i & 0x7f), 0x40));
    while (fakeNow.load() < 10600 * ms) {
        if (fakeNow.load() >= nextCallback) {
            AudioQueueSpan span;
            span.known = true;
            span.frames = period;
            span.startNs = guestNs;
            span.endNs = guestNs += periodNs;
            clock.noteNewest(guestNs + 100 * ms);
            clock.run(span, period, nextCallback);
            nextCallback += ++pair % 2 ? ms : 2 * periodNs - ms;
        }
        fakeNow += ms;
        out->work();
    }
    const auto s = log.copy();
    assert(s.size() == 200);
    for (size_t i = 1; i < s.size(); ++i) assert(s[i].hostNs >= s[i - 1].hostNs);
}

// A hold: the device plays on with the clock standing, nothing published. On resume the old period isn't timed
// by: the next message waits for a new one, so it isn't stamped in the past or sent at once.
void resumed() {
    Log log;
    fakeNow = 10000 * ms;
    GuestAudioClock clock;
    auto out = make(log);
    out->setReference(&clock);
    constexpr size_t period = 512;
    const uint64_t periodNs = GuestAudioClock::framesNs(period);
    uint64_t guestNs = 0, nextCallback = fakeNow.load(), holdNs = 0, resumeNs = 0;
    out->offer(message(100 * ms, 0x90, 0x3c, 0x40));
    out->offer(message(250 * ms, 0x90, 0x3e, 0x40));
    while (fakeNow.load() < 11800 * ms) {
        // Held from guest 245 ms for 1 s.
        if (!holdNs && guestNs >= 245 * ms) {
            holdNs = fakeNow.load();
            clock.hold(true);
            out->hold(true);
        }
        if (holdNs && !resumeNs && fakeNow.load() >= holdNs + 1000 * ms) {
            resumeNs = fakeNow.load();
            clock.hold(false);
            out->hold(false);
        }
        if (fakeNow.load() >= nextCallback) {
            AudioQueueSpan span;
            span.known = true;
            span.frames = period;
            span.startNs = guestNs;
            span.endNs = guestNs + periodNs;
            clock.noteNewest(guestNs + 100 * ms);
            if (!clock.held()) guestNs = span.endNs;
            clock.run(span, period, nextCallback);
            nextCallback += periodNs;
        }
        fakeNow += ms;
        out->work();
    }
    const auto s = log.copy();
    assert(resumeNs && s.size() == 2 + 32);
    // The note, all notes off at the hold, then the second note after the resume, in a period published since.
    assert(s[33].b == (Bytes{0x90, 0x3e, 0x40}) && s[33].at >= resumeNs && s[33].hostNs >= resumeNs + periodNs);
}

// The audio callback's side and the engine's thread at once, on the steady clock, for TSan: the seqlock's end and
// read.
void referenceThreads() {
    Log log;
    GuestAudioClock clock;
    {
        ZodiacMidiOut out(std::make_unique<FakePort>(log, ZodiacMidiOut::steadyNs));
        out.setReference(&clock);
        out.start();
        for (uint32_t i = 0; i < 20; ++i) out.offer(message(20 * ms + i * 5 * ms, 0x90, uint8_t(60 + i), 0x40));
        std::atomic<bool> done{false};
        std::thread audio([&] {
            constexpr size_t period = 256;
            uint64_t guestNs = 0;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!done.load() && std::chrono::steady_clock::now() < deadline) {
                AudioQueueSpan span;
                span.known = true;
                span.frames = period;
                span.startNs = guestNs;
                span.endNs = guestNs += GuestAudioClock::framesNs(period);
                clock.noteNewest(guestNs + 100 * ms);
                clock.run(span, period, GuestAudioClock::steadyNs());
                std::this_thread::sleep_for(std::chrono::microseconds(GuestAudioClock::framesNs(period) / 1000));
            }
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (log.copy().size() < 20 && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        done = true;
        audio.join();
        out.stop();
        const auto s = log.copy();
        assert(s.size() >= 20);
        for (uint32_t i = 0; i < 20; ++i) assert(s[i].b == (Bytes{0x90, uint8_t(60 + i), 0x40}));
        for (size_t i = 1; i < 20; ++i) assert(s[i].hostNs >= s[i - 1].hostNs);
    }
}

// Audio off: the callback stops and the reference goes (setReference(nullptr)); the music carries on by the
// host clock from the guest time it reached, and goes back to the reference when audio is on again.
void audioOff() {
    Log log;
    fakeNow = 10000 * ms;
    GuestAudioClock clock;
    auto out = make(log);
    out->setReference(&clock);
    constexpr size_t period = 512;
    const uint64_t periodNs = GuestAudioClock::framesNs(period);
    uint64_t guestNs = 0, nextCallback = fakeNow.load();
    for (const uint64_t t : {100 * ms, 300 * ms, 500 * ms}) out->offer(message(t, 0x90, uint8_t(t / ms / 10), 0x40));
    bool on = true;
    while (fakeNow.load() < 11000 * ms) {
        if (on && guestNs >= 200 * ms) {
            on = false;
            out->setReference(nullptr);
        }
        if (on && fakeNow.load() >= nextCallback) {
            AudioQueueSpan span;
            span.known = true;
            span.frames = period;
            span.startNs = guestNs;
            span.endNs = guestNs += periodNs;
            clock.noteNewest(guestNs + 100 * ms);
            clock.run(span, period, nextCallback);
            nextCallback += periodNs;
        }
        fakeNow += ms;
        out->work();
    }
    const auto s = log.copy();
    assert(s.size() == 3);
    assert(s[2].hostNs - s[1].hostNs > 198 * ms && s[2].hostNs - s[1].hostNs < 202 * ms);
    assert(s[1].hostNs - s[0].hostNs > 190 * ms && s[1].hostNs - s[0].hostNs < 215 * ms);
    assert(onTime(s, 0, ms + periodNs));
}

// A queue clear (no marks: spans not known) after a hold: the next guest audio, 1 s ahead, is a jump, not eased
// to as the resume would have been.
void clearAfterHold() {
    GuestAudioClock clock;
    constexpr size_t period = 512;
    const uint64_t periodNs = GuestAudioClock::framesNs(period);
    uint64_t guestNs = 0, hostNs = 10000 * ms;
    const auto full = [&](uint64_t startNs) {
        AudioQueueSpan span;
        span.known = true;
        span.frames = period;
        span.startNs = startNs;
        span.endNs = startNs + periodNs;
        clock.run(span, period, hostNs += periodNs);
    };
    for (int i = 0; i < 20; ++i, guestNs += periodNs) full(guestNs);
    clock.hold(true);
    full(guestNs);
    clock.hold(false);
    clock.run(AudioQueueSpan{}, period, hostNs += periodNs);
    full(guestNs + 1000 * ms);
    GuestAudioClock::Reference r;
    const bool read = clock.read(r);
    assert(read && r.jumps == 1 && !r.back && r.jumpNs == guestNs + 1000 * ms);
    (void)read;
}

// Port names without the numbers that change with plug order.
void portKeys() {
    assert(zodiacMidiPortKey("Midi Through:Midi Through Port-0 14:0") == "midi through:midi through port-0");
    assert(zodiacMidiPortKey("FLUID Synth (4321):Synth input port (4321:0) 128:0") == "fluid synth:synth input port");
    assert(zodiacMidiPortKey("FLUID Synth (987):Synth input port (987:0) 129:0") ==
           zodiacMidiPortKey("FLUID Synth (4321):Synth input port (4321:0) 128:0"));
    assert(zodiacMidiPortKey("Roland SC-88 (2)") == "roland sc-88");
    assert(zodiacMidiPortKey("Port (A)") == "port (a)");
    assert(zodiacMidiPortKey("IAC Driver Bus 1") == "iac driver bus 1");
    assert(zodiacMidiPortKey("Bus 12:") == "bus 12:");
    assert(zodiacMidiPortKey("") == "");
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
    driftDelayed();
    guestPaced();
    stampsInOrder();
    resumed();
    referenceThreads();
    audioOff();
    clearAfterHold();
    portKeys();
    sleeps();
    threads();
    std::puts("PASS: Zodiac MIDI out (event stream, song times, never early, stop, hold, lookahead, forward jump, late event, delay, "
              "drift after the delay, guest audio clock, stamps in order, resume, clock threads, audio off, a clear after a hold, port names, "
              "sleeping thread, hooks and engine thread, notes off at the end)");
    return 0;
}
