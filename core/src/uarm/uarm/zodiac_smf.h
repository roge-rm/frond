#ifndef FROND_ZODIAC_SMF_H
#define FROND_ZODIAC_SMF_H

// Host-side Standard MIDI File reader for the Zodiac's music hooks: a game hands TwSmfOpen an SMF image in guest
// memory with no size, so smfLength finds its end from the chunk headers, and parseSmf turns it into one list of
// events in time order, in microseconds with the file's tempo changes applied. The bytes come from the guest, so
// every read is bounds-checked and a bad file gives an error, never a partial song. Nothing here touches the
// machine: it only reads a copy of the bytes.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace zodiac_smf {

enum class Error { none, truncated, notSmf, badHeader, badTrack, badEvent, tooLong, tooManyEvents };

struct Event {
    uint64_t timeUs = 0;
    uint16_t track = 0;
    uint8_t status = 0;  // 0x80-0xef channel message, 0xf0 or 0xf7 system exclusive, 0xff meta
    uint8_t data1 = 0;   // meta: the meta type; data bytes can have the top bit set (see parseSmf)
    uint8_t data2 = 0;
    uint32_t offset = 0;  // sysex and meta: where the payload starts in the file (after the length)
    uint32_t length = 0;  // sysex and meta: payload bytes
};

struct Song {
    uint16_t format = 0, tracks = 0, division = 0;
    uint64_t lengthUs = 0;  // time of the last event, end of track included
    std::vector<Event> events;
};

constexpr uint32_t defaultTempo = 500000;  // microseconds per quarter note, 120 bpm
constexpr uint64_t maxLengthUs = 24ull * 3600 * 1000000;
constexpr size_t maxEvents = 1u << 20;

inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

// Bytes from MThd through the last track chunk the header counts, or 0 when they don't fit in `available`.
// Chunks of other types between tracks are skipped and counted, as the spec asks readers to.
inline size_t smfLength(const uint8_t* data, size_t available) {
    if (available < 14 || be32(data) != 0x4d546864 || be32(data + 4) < 6) return 0;
    const uint16_t tracks = be16(data + 10);
    size_t at = 8 + size_t(be32(data + 4));
    if (at > available) return 0;
    for (uint32_t found = 0; found < tracks;) {
        if (available - at < 8) return 0;
        const uint32_t type = be32(data + at), size = be32(data + at + 4);
        if (size > available - at - 8) return 0;
        at += 8 + size_t(size);
        if (type == 0x4d54726b) ++found;
    }
    return at;
}

namespace detail {
struct Reader {
    const uint8_t* data;
    size_t at, end;
    bool byte(uint8_t& v) { if (at >= end) return false; v = data[at++]; return true; }
    // A variable-length quantity: at most 4 bytes, 28 bits.
    bool vlq(uint32_t& v) {
        v = 0;
        for (int i = 0; i < 4; ++i) {
            uint8_t b;
            if (!byte(b)) return false;
            v = v << 7 | (b & 0x7f);
            if (!(b & 0x80)) return true;
        }
        return false;
    }
};
struct Timed { uint64_t tick; uint32_t order; Event event; };
}  // namespace detail

inline Error parseSmf(const uint8_t* data, size_t size, Song& song, size_t eventLimit = maxEvents) {
    song = Song{};
    if (size < 14 || be32(data) != 0x4d546864) return size < 4 ? Error::truncated : Error::notSmf;
    const uint32_t headerSize = be32(data + 4);
    if (headerSize < 6 || headerSize > size - 8) return Error::badHeader;
    const uint16_t format = be16(data + 8), tracks = be16(data + 10), division = be16(data + 12);
    if (format > 2 || !tracks || (format == 0 && tracks != 1) || !(division & 0x7fff)) return Error::badHeader;
    // SMPTE division: frames per second (24, 25, 29 for 29.97 drop frame, 30) times ticks per frame.
    uint64_t smpteTicksPerSecond = 0;
    if (division & 0x8000) {
        const int fps = -int8_t(division >> 8);
        if (fps != 24 && fps != 25 && fps != 29 && fps != 30) return Error::badHeader;
        smpteTicksPerSecond = uint64_t(fps) * (division & 0xff);
        if (!smpteTicksPerSecond) return Error::badHeader;
    }
    std::vector<detail::Timed> timed;
    size_t at = 8 + size_t(headerSize);
    uint32_t order = 0;
    for (uint16_t track = 0; track < tracks;) {
        if (size - at < 8) return Error::truncated;
        const uint32_t type = be32(data + at), length = be32(data + at + 4);
        if (length > size - at - 8) return Error::truncated;
        detail::Reader r{data, at + 8, at + 8 + size_t(length)};
        at = r.end;
        if (type != 0x4d54726b) continue;
        uint64_t tick = 0;
        uint8_t running = 0;
        bool ended = false;
        while (!ended) {
            uint32_t delta;
            if (!r.vlq(delta)) return Error::badTrack;
            tick += delta;
            uint8_t first;
            if (!r.byte(first)) return Error::badTrack;
            Event e;
            e.track = track;
            if (first == 0xff) {
                uint8_t metaType;
                uint32_t len;
                if (!r.byte(metaType) || !r.vlq(len) || len > r.end - r.at) return Error::badTrack;
                e.status = 0xff; e.data1 = metaType; e.offset = uint32_t(r.at); e.length = len;
                r.at += len;
                running = 0;
                if (metaType == 0x2f) ended = true;
                if (metaType == 0x51 && len != 3) return Error::badEvent;
            } else if (first == 0xf0 || first == 0xf7) {
                uint32_t len;
                if (!r.vlq(len) || len > r.end - r.at) return Error::badTrack;
                e.status = first; e.offset = uint32_t(r.at); e.length = len;
                r.at += len;
                running = 0;
            } else if (first >= 0xf1) {
                return Error::badEvent;  // system common and real time bytes have no place in a file
            } else {
                // Data bytes after a status are taken as they are, top bit included: shipped games have program
                // and controller values of 0x80 and 0xff (Outpost, Interstellar Flames). Whoever plays the event
                // clamps them. Under running status a byte with the top bit is a new status, so only the second
                // data byte can have it there.
                uint8_t d1 = first;
                if (first & 0x80) {
                    running = first;
                    if (!r.byte(d1)) return Error::badTrack;
                } else if (!running) {
                    return Error::badEvent;
                }
                const uint8_t kind = running >> 4;
                uint8_t d2 = 0;
                if (kind != 0xc && kind != 0xd && !r.byte(d2)) return Error::badTrack;
                e.status = running; e.data1 = d1; e.data2 = d2;
            }
            if (timed.size() >= eventLimit) return Error::tooManyEvents;
            timed.push_back({tick, order++, e});
            if (!ended && r.at >= r.end) return Error::badTrack;  // no end of track
        }
        ++track;
    }
    // Format 1 tracks play together, so merge by tick; format 2 tracks are separate songs played one after another.
    if (format == 2) {
        uint64_t base = 0, last = 0;
        uint16_t trackNow = 0;
        for (auto& t : timed) {
            if (t.event.track != trackNow) { base = last; trackNow = t.event.track; }
            t.tick += base;
            last = t.tick;
        }
    }
    std::stable_sort(timed.begin(), timed.end(), [](const detail::Timed& a, const detail::Timed& b) { return a.tick < b.tick; });
    // Ticks to microseconds: one anchor per tempo change, so rounding doesn't build up.
    uint64_t anchorTick = 0, anchorUs = 0;
    uint32_t tempo = defaultTempo;
    const uint32_t ppq = division & 0x7fff;
    std::vector<Event> events;
    events.reserve(timed.size());
    for (const auto& t : timed) {
        const uint64_t dt = t.tick - anchorTick;
        uint64_t us;
        if (smpteTicksPerSecond) {
            const uint64_t q = dt / smpteTicksPerSecond, rem = dt % smpteTicksPerSecond;
            if (q > maxLengthUs / 1000000) return Error::tooLong;
            us = q * 1000000 + rem * 1000000 / smpteTicksPerSecond;
        } else {
            const uint64_t q = dt / ppq, rem = dt % ppq;
            if (q > maxLengthUs / tempo) return Error::tooLong;
            us = q * tempo + rem * tempo / ppq;
        }
        Event e = t.event;
        e.timeUs = anchorUs + us;
        if (e.timeUs > maxLengthUs) return Error::tooLong;
        if (!smpteTicksPerSecond && e.status == 0xff && e.data1 == 0x51) {
            anchorTick = t.tick; anchorUs = e.timeUs;
            tempo = uint32_t(data[e.offset]) << 16 | uint32_t(data[e.offset + 1]) << 8 | data[e.offset + 2];
            if (!tempo) return Error::badEvent;
        }
        events.push_back(e);
    }
    song.format = format; song.tracks = tracks; song.division = division;
    song.lengthUs = events.empty() ? 0 : events.back().timeUs;
    song.events = std::move(events);
    return Error::none;
}

}  // namespace zodiac_smf

#endif
