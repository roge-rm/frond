// Host-side SMF reader for the Zodiac music hooks (zodiac_smf.h): lengths, merge order, tempo and SMPTE times,
// running status, sysex and meta payloads, and refusal of bad files. Built with ASan and UBSan, and fed mutated
// files, so a guest's bad bytes can't read past the copy.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "zodiac_smf.h"

using namespace zodiac_smf;
using Bytes = std::vector<uint8_t>;

namespace {
void put32(Bytes& b, uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s)); }
Bytes header(uint16_t format, uint16_t tracks, uint16_t division) {
    Bytes b = {'M', 'T', 'h', 'd'};
    put32(b, 6);
    for (uint16_t v : {format, tracks, division}) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
    return b;
}
void track(Bytes& b, const Bytes& body) {
    b.insert(b.end(), {'M', 'T', 'r', 'k'});
    put32(b, uint32_t(body.size()));
    b.insert(b.end(), body.begin(), body.end());
}
const Bytes endOfTrack = {0x00, 0xff, 0x2f, 0x00};
Bytes cat(Bytes a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); return a; }

Error parse(const Bytes& b, Song& s, size_t limit = maxEvents) { return parseSmf(b.data(), b.size(), s, limit); }

void format0() {
    // 96 ticks per quarter at the default 120 bpm: 96 ticks are 500 ms. Running status for the second note on,
    // a program change (one data byte) after it, and a VLQ delta of 0x81 0x00 (128 ticks).
    Bytes f = header(0, 1, 96);
    track(f, cat({0x00, 0x90, 60, 100,  0x60, 64, 90,  0x00, 0xc0, 5,  0x81, 0x00, 0x80, 60, 0}, endOfTrack));
    Song s;
    assert(parse(f, s) == Error::none);
    assert(s.format == 0 && s.tracks == 1 && s.division == 96 && s.events.size() == 5);
    assert(s.events[0].status == 0x90 && s.events[0].data1 == 60 && s.events[0].data2 == 100 && s.events[0].timeUs == 0);
    assert(s.events[1].status == 0x90 && s.events[1].data1 == 64 && s.events[1].data2 == 90 && s.events[1].timeUs == 500000);
    assert(s.events[2].status == 0xc0 && s.events[2].data1 == 5 && s.events[2].timeUs == 500000);
    assert(s.events[3].status == 0x80 && s.events[3].timeUs == 500000 + 128 * 500000 / 96);
    assert(s.events[4].status == 0xff && s.events[4].data1 == 0x2f && s.lengthUs == s.events[3].timeUs);
    assert(smfLength(f.data(), f.size()) == f.size());
}

void format1Tempo() {
    // Tempo track: 250000 us per quarter from 0, 1000000 from tick 192. Notes at 96, 192 and 288 ticks.
    Bytes f = header(1, 2, 96);
    track(f, cat({0x00, 0xff, 0x51, 3, 0x03, 0xd0, 0x90,  0x81, 0x40, 0xff, 0x51, 3, 0x0f, 0x42, 0x40}, endOfTrack));
    track(f, cat({0x60, 0x91, 60, 1,  0x60, 0x81, 60, 0,  0x60, 0x91, 62, 1}, endOfTrack));
    Song s;
    assert(parse(f, s) == Error::none);
    std::vector<uint64_t> notes;
    for (const auto& e : s.events) if (e.status < 0xf0) notes.push_back(e.timeUs);
    assert((notes == std::vector<uint64_t>{250000, 500000, 1500000}));
    // At tick 192 the tempo change (track 0) comes before the note off (track 1).
    size_t tempoAt = 0, offAt = 0;
    for (size_t i = 0; i < s.events.size(); ++i) {
        if (s.events[i].status == 0xff && s.events[i].data1 == 0x51 && s.events[i].timeUs == 500000) tempoAt = i;
        if (s.events[i].status == 0x81) offAt = i;
    }
    assert(tempoAt && tempoAt < offAt);
    assert(s.events[tempoAt].length == 3 && f[s.events[tempoAt].offset] == 0x0f);
}

void smpte() {
    // -25 fps, 40 ticks a frame: 1000 ticks a second, tempo ignored.
    Bytes f = header(0, 1, uint16_t(uint8_t(-25)) << 8 | 40);
    track(f, cat({0x00, 0xff, 0x51, 3, 0x00, 0x00, 0x01,  0x8b, 0x5c, 0x90, 60, 1}, endOfTrack));
    Song s;
    assert(parse(f, s) == Error::none);
    assert(s.events[1].status == 0x90 && s.events[1].timeUs == 1500000);
    Bytes bad = header(0, 1, uint16_t(uint8_t(-26)) << 8 | 40);
    track(bad, endOfTrack);
    assert(parse(bad, s) == Error::badHeader);
}

void sysexAndLength() {
    Bytes f = header(0, 1, 96);
    // An unknown chunk before the track is skipped and counted in the length.
    f.insert(f.end(), {'X', 'F', 'I', 'H', 0, 0, 0, 2, 0xaa, 0xbb});
    track(f, cat({0x00, 0xf0, 3, 0x7e, 0x09, 0xf7,  0x00, 0xf7, 1, 0xf8}, endOfTrack));
    const size_t real = f.size();
    Bytes padded = cat(f, {0x12, 0x34, 0x56});
    assert(smfLength(padded.data(), padded.size()) == real);
    for (size_t cut = 0; cut < real; ++cut) assert(smfLength(f.data(), cut) == 0);
    Song s;
    assert(parse(padded, s) == Error::none && s.events.size() == 3);
    assert(s.events[0].status == 0xf0 && s.events[0].length == 3 && padded[s.events[0].offset] == 0x7e);
    assert(s.events[1].status == 0xf7 && s.events[1].length == 1 && padded[s.events[1].offset] == 0xf8);
    Bytes notMidi = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    assert(smfLength(notMidi.data(), notMidi.size()) == 0 && parse(notMidi, s) == Error::notSmf);
}

void refused() {
    Song s;
    auto one = [](const Bytes& body) { Bytes f = header(0, 1, 96); track(f, body); return f; };
    assert(parse(one({0x00, 0x40, 0x40}), s) == Error::badEvent);                       // data byte, no running status
    // Data bytes with the top bit set are kept after a status (games ship program and controller values of 0x80
    // and 0xff). Under running status such a byte is the next status.
    assert(parse(one(cat({0x00, 0xb0, 7, 0xff,  0x00, 0xc9, 0xff}, endOfTrack)), s) == Error::none);
    assert(s.events[0].data2 == 0xff && s.events[1].status == 0xc9 && s.events[1].data1 == 0xff);
    assert(parse(one(cat({0x00, 0x90, 60, 1,  0x00, 61, 0x81}, endOfTrack)), s) == Error::none);
    assert(s.events[1].status == 0x90 && s.events[1].data1 == 61 && s.events[1].data2 == 0x81);
    assert(parse(one(cat({0x00, 0x90, 60, 1,  0x00, 0x81, 1, 0}, endOfTrack)), s) == Error::none);
    assert(s.events[1].status == 0x81 && s.events[1].data1 == 1 && s.events.size() == 3);
    assert(parse(one({0x00, 0x90, 60, 1}), s) == Error::badTrack);                      // no end of track
    assert(parse(one({0x80, 0x80, 0x80, 0x80, 0x00, 0x90, 60, 1}), s) == Error::badTrack);  // 5-byte delta
    assert(parse(one(cat({0x00, 0xff, 0x51, 2, 1, 2}, endOfTrack)), s) == Error::badEvent);   // short tempo
    assert(parse(one(cat({0x00, 0xff, 0x51, 3, 0, 0, 0}, endOfTrack)), s) == Error::badEvent); // tempo 0
    assert(parse(one(cat({0x00, 0xf0, 9, 1}, endOfTrack)), s) == Error::badTrack);      // sysex past the track
    assert(parse(one(cat({0x00, 0xf8}, endOfTrack)), s) == Error::badEvent);            // real-time byte
    Bytes two = header(0, 2, 96); track(two, endOfTrack); track(two, endOfTrack);
    assert(parse(two, s) == Error::badHeader);                                         // format 0, 2 tracks
    Bytes zeroDivision = header(0, 1, 0); track(zeroDivision, endOfTrack);
    assert(parse(zeroDivision, s) == Error::badHeader);
    Bytes missing = header(1, 2, 96); track(missing, endOfTrack);
    assert(parse(missing, s) == Error::truncated && s.events.empty());                  // second track missing
    assert(parse(one(cat({0x00, 0x90, 60, 1, 0x00, 0x80, 60, 0}, endOfTrack)), s, 2) == Error::tooManyEvents);
    // 0x0fffffff ticks at 1 tick per quarter of 16.7 s each: far past 24 hours.
    Bytes slow = header(0, 1, 1);
    track(slow, cat({0x00, 0xff, 0x51, 3, 0xff, 0xff, 0xff,  0xff, 0xff, 0xff, 0x7f, 0x90, 60, 1}, endOfTrack));
    assert(parse(slow, s) == Error::tooLong);
}

void format2() {
    // Two separate songs: the second starts where the first ended.
    Bytes f = header(2, 2, 96);
    track(f, cat({0x60, 0x90, 60, 1}, endOfTrack));
    track(f, cat({0x60, 0x90, 62, 1}, endOfTrack));
    Song s;
    assert(parse(f, s) == Error::none);
    std::vector<uint64_t> notes;
    for (const auto& e : s.events) if (e.status == 0x90) notes.push_back(e.timeUs);
    assert((notes == std::vector<uint64_t>{500000, 1000000}));
}

void mutated() {
    Bytes f = header(1, 2, 96);
    track(f, cat({0x00, 0xff, 0x51, 3, 0x07, 0xa1, 0x20, 0x00, 0xf0, 2, 0x01, 0xf7}, endOfTrack));
    track(f, cat({0x10, 0x90, 60, 100, 0x10, 62, 100, 0x10, 0xb0, 7, 90, 0x10, 0xe0, 0, 64, 0x10, 0x80, 60, 0}, endOfTrack));
    std::mt19937 rng(1234);
    size_t parsed = 0;
    for (int i = 0; i < 20000; ++i) {
        Bytes m = f;
        const int changes = 1 + int(rng() % 4);
        for (int c = 0; c < changes; ++c) m[rng() % m.size()] = uint8_t(rng());
        if (rng() % 4 == 0) m.resize(rng() % m.size());
        Song s;
        const size_t n = smfLength(m.data(), m.size());
        assert(n <= m.size());
        if (parse(m, s) == Error::none) {
            ++parsed;
            for (size_t k = 1; k < s.events.size(); ++k) assert(s.events[k - 1].timeUs <= s.events[k].timeUs);
            for (const auto& e : s.events) assert(uint64_t(e.offset) + e.length <= m.size());
        } else {
            assert(s.events.empty());
        }
    }
    assert(parsed > 100);  // some mutations still parse, so the checks above ran
}
}  // namespace

int main() {
    format0(); format1Tempo(); smpte(); sysexAndLength(); refused(); format2(); mutated();
    std::puts("PASS: Zodiac SMF reader (length, merge, tempo, SMPTE, sysex, refusals, mutations)");
}
