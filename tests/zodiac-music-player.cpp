// The Zodiac music player (zodiac_music_player.h): song messages at their guest times from Play, tempo applied,
// clamped data bytes, sysex from the file, Stop and Close with all notes off, a Play of a playing handle starting
// over, two songs merged in time order, a song's end, a reset, real-time messages and master volume, an F7 escape
// sent as raw bytes, the song limit, and every Held the preparer made handed back. Built with ASan and UBSan.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "zodiac_music_player.h"

using namespace zodiac_music;
using Bytes = std::vector<uint8_t>;

namespace {
struct Sent { uint64_t t; Bytes b; bool escape; };
std::vector<Sent> sent;
const Sink sink = [](uint64_t t, const uint8_t* b, size_t n, bool escape) { sent.push_back({t, Bytes(b, b + n), escape}); };

// The two stages on one thread: events through the preparer to the player, Helds back.
struct Stages {
    Preparer prep;
    Player p{[this](Held* h) { prep.release(h); }};
    void feed(Event e, const Sink& s = sink) { p.feed(prep.prepare(std::move(e)), s); }
    void advance(uint64_t t) { p.advance(t, sink); }
    size_t playing() const { return p.playing(); }
    // Nothing held but the songs still playing.
    bool settled() const { return prep.outstanding() == p.playing(); }
};

std::shared_ptr<const Smf> smf(const Bytes& body, uint16_t division = 96) {
    auto s = std::make_shared<Smf>();
    s->bytes = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, uint8_t(division >> 8), uint8_t(division)};
    s->bytes.insert(s->bytes.end(), {'M', 'T', 'r', 'k', 0, 0, 0, uint8_t(body.size())});
    s->bytes.insert(s->bytes.end(), body.begin(), body.end());
    return s;
}
Event ev(uint64_t t, Kind k, uint32_t handle = 0) { Event e; e.timeNs = t; e.kind = k; e.handle = handle; return e; }
Event play(uint64_t t, uint32_t handle, std::shared_ptr<const Smf> s) { Event e = ev(t, Kind::play, handle); e.smf = s; return e; }
bool allOff(size_t from, uint64_t t) {
    if (sent.size() < from + 32) return false;
    for (unsigned c = 0; c < 16; ++c) {
        const auto &a = sent[from + c * 2], &b = sent[from + c * 2 + 1];
        if (a.t != t || b.t != t || a.b != Bytes{uint8_t(0xb0 | c), 123, 0} || b.b != Bytes{uint8_t(0xb0 | c), 120, 0})
            return false;
    }
    return true;
}
constexpr uint64_t ms = 1000000;

// Tempo 250000 (240 bpm): 96 ticks are 250 ms. Note on 60 at 0, program 0xff (clamped) at 96, sysex at 96, note
// off with velocity 0x80 (clamped) at 192, end at 288.
const Bytes body = {0x00, 0xff, 0x51, 0x03, 0x03, 0xd0, 0x90,
                    0x00, 0x90, 0x3c, 0x64,
                    0x60, 0xc2, 0xff,
                    0x00, 0xf0, 0x03, 0x7e, 0x01, 0xf7,
                    0x60, 0x80, 0x3c, 0x80,
                    0x60, 0xff, 0x2f, 0x00};

void songPlays() {
    sent.clear();
    Stages p;
    p.feed(play(1000 * ms, 7, smf(body)));
    assert(sent.empty() && p.playing() == 1);
    p.advance(1000 * ms);
    assert(sent.size() == 1 && sent[0].t == 1000 * ms && sent[0].b == (Bytes{0x90, 0x3c, 0x64}));
    p.advance(1249 * ms);
    assert(sent.size() == 1);
    p.advance(1250 * ms);
    assert(sent.size() == 3 && sent[1].b == (Bytes{0xc2, 0x7f}) && sent[2].b == (Bytes{0xf0, 0x7e, 0x01, 0xf7}));
    assert(sent[1].t == 1250 * ms && sent[2].t == 1250 * ms);
    p.advance(1600 * ms);
    assert(sent.size() == 4 && sent[3].t == 1500 * ms && sent[3].b == (Bytes{0x80, 0x3c, 0x7f}));
    assert(p.playing() == 1);
    // The end at 1750 ms: notes off there, and the song is done.
    p.advance(2000 * ms);
    assert(sent.size() == 36 && allOff(4, 1750 * ms) && p.playing() == 0 && p.settled() && !p.prep.outstanding());
}

void stopAndRestart() {
    sent.clear();
    Stages p;
    const auto s = smf(body);
    p.feed(play(0, 7, s));
    // A message from the guest at 300 ms: the song's 250 ms messages go first.
    Event cc = ev(300 * ms, Kind::message, 0x1234);
    cc.bytes[0] = 0xb1; cc.bytes[1] = 7; cc.bytes[2] = 200; cc.length = 3;
    p.feed(cc);
    assert(sent.size() == 4 && sent[3].t == 300 * ms && sent[3].b == (Bytes{0xb1, 7, 0x7f}));
    // Play again: notes off, then from the top.
    p.feed(play(400 * ms, 7, s));
    assert(sent.size() == 36 && allOff(4, 400 * ms) && p.playing() == 1);
    p.advance(400 * ms);
    assert(sent.size() == 37 && sent[36].t == 400 * ms && sent[36].b == (Bytes{0x90, 0x3c, 0x64}));
    // Stop: notes off, nothing after.
    p.feed(ev(500 * ms, Kind::stop, 7));
    assert(sent.size() == 69 && allOff(37, 500 * ms) && p.playing() == 0);
    p.advance(5000 * ms);
    assert(sent.size() == 69);
    // Close of a song that is playing, and a reset with two playing.
    p.feed(play(6000 * ms, 7, s));
    p.feed(ev(6000 * ms, Kind::close, 7));
    assert(p.playing() == 0 && sent.size() == 70 + 32 && allOff(70, 6000 * ms));
    p.feed(play(7000 * ms, 7, s));
    p.feed(play(7000 * ms, 8, s));
    p.feed(ev(7100 * ms, Kind::reset));
    assert(p.playing() == 0 && allOff(sent.size() - 32, 7100 * ms) && !p.prep.outstanding());
}

void twoSongs() {
    sent.clear();
    Stages p;
    // b: note on 61 every 48 ticks (125 ms at the default 120 bpm with 192 ticks a quarter), three times.
    const Bytes other = {0x00, 0x91, 0x3d, 0x10, 0x30, 0x91, 0x3d, 0x11, 0x30, 0x91, 0x3d, 0x12, 0x00, 0xff, 0x2f, 0x00};
    p.feed(play(0, 1, smf(body)));
    p.feed(play(0, 2, smf(other, 192)));
    p.advance(260 * ms);
    // 0: a's note on, then b's; 125: b; 250: a's program change and sysex, then b's.
    assert(sent.size() >= 6);
    const uint64_t times[] = {0, 0, 125, 250, 250, 250};
    for (size_t i = 0; i < 6; ++i) assert(sent[i].t == times[i] * ms);
    assert(sent[0].b[0] == 0x90 && sent[1].b[0] == 0x91 && sent[2].b == (Bytes{0x91, 0x3d, 0x11}));
    assert(sent[3].b[0] == 0xc2 && sent[4].b[0] == 0xf0 && sent[5].b == (Bytes{0x91, 0x3d, 0x12}));
    // b ends at 250 ms with its notes off; a plays on.
    assert(p.playing() == 1 && sent.size() == 38 && allOff(6, 250 * ms));
    p.advance(261 * ms);
    assert(sent.size() == 38 && p.settled());
}

void volumeAndSysEx() {
    sent.clear();
    Stages p;
    Event v = ev(10, Kind::masterVolume);
    v.value = 127;
    p.feed(v);
    v.value = 63;
    p.feed(v);
    v.value = 400;
    p.feed(v);
    assert(sent.size() == 3);
    assert(sent[0].b == (Bytes{0xf0, 0x7f, 0x7f, 0x04, 0x01, 0x7f, 0x7f, 0xf7}));
    const uint32_t half = 63 * 16383 / 127;
    assert(sent[1].b == (Bytes{0xf0, 0x7f, 0x7f, 0x04, 0x01, uint8_t(half & 0x7f), uint8_t(half >> 7), 0xf7}));
    assert(sent[2].b == sent[0].b);
    Event s = ev(20, Kind::sysEx);
    s.data = {0xf0, 0x43, 0x10, 0xf7};
    p.feed(s);
    s.data = {0x43, 0x10};  // not a whole sysex: dropped
    p.feed(s);
    assert(sent.size() == 4 && sent[3].b == (Bytes{0xf0, 0x43, 0x10, 0xf7}));
    // A Play the hooks couldn't match carries no file and plays nothing; nor does one whose file doesn't parse.
    p.feed(ev(30, Kind::play, 3));
    assert(p.playing() == 0 && sent.size() == 4 && !p.prep.refused());
    auto broken = std::make_shared<Smf>(*smf(body));
    broken->bytes[8] = 9;  // format 9
    p.feed(play(40, 3, broken));
    assert(p.playing() == 0 && sent.size() == 4 && p.prep.refused() == 1 && !p.prep.outstanding());
}

void escapesAndLimits() {
    sent.clear();
    Stages p;
    // An F7 escape of 0x90 0x3c 0x64 at 0: raw bytes, flagged, never taken as a note on. A 5000-byte sysex at 0 is
    // over the limit and not sent; the end is at 0 too.
    Bytes body = {0x00, 0xf7, 0x03, 0x90, 0x3c, 0x64, 0x00, 0xf0, 0xa7, 0x08};
    body.insert(body.end(), 5000 - 1, 0x11);
    body.push_back(0xf7);
    body.insert(body.end(), {0x00, 0xff, 0x2f, 0x00});
    auto s = std::make_shared<Smf>();
    s->bytes = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96, 'M', 'T', 'r', 'k', 0, 0,
                uint8_t(body.size() >> 8), uint8_t(body.size())};
    s->bytes.insert(s->bytes.end(), body.begin(), body.end());
    p.feed(play(0, 1, s));
    p.advance(0);
    assert(sent.size() == 1 + 32 && sent[0].escape && sent[0].b == (Bytes{0x90, 0x3c, 0x64}) && allOff(1, 0));
    for (size_t i = 1; i < sent.size(); ++i) assert(!sent[i].escape);
    assert(p.playing() == 0 && !p.prep.outstanding());
    // One song more than the limit: the oldest ends, with notes off, and its Held comes back.
    sent.clear();
    const auto long1 = smf({0x00, 0x90, 0x3c, 0x64, 0x7f, 0xff, 0x2f, 0x00});
    for (uint32_t h = 1; h <= Player::maxSongs; ++h) p.feed(play(0, h, long1));
    p.advance(0);
    assert(p.playing() == Player::maxSongs && p.prep.outstanding() == Player::maxSongs && sent.size() == Player::maxSongs);
    sent.clear();
    p.feed(play(0, 100, long1));
    assert(p.playing() == Player::maxSongs && p.prep.outstanding() == Player::maxSongs && sent.size() == 32 && allOff(0, 0));
    p.feed(ev(10, Kind::reset));
    assert(p.playing() == 0 && !p.prep.outstanding());
    // A guest sysex's Held comes back at once, sent or not.
    Event x = ev(20, Kind::sysEx);
    x.data = {0xf0, 1, 0xf7};
    p.feed(x);
    x.data = {1, 2};
    p.feed(x);
    assert(!p.prep.outstanding());
}
}  // namespace

int main() {
    songPlays();
    stopAndRestart();
    twoSongs();
    volumeAndSysEx();
    escapesAndLimits();
    std::puts("PASS: Zodiac music player (times, clamping, sysex, stop, restart, merge, end, reset, volume, escapes, "
              "song limit, Helds returned)");
    return 0;
}
