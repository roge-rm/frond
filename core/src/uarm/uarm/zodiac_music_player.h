#ifndef FROND_ZODIAC_MUSIC_PLAYER_H
#define FROND_ZODIAC_MUSIC_PLAYER_H

// The host sequencer for the Zodiac's music hooks (zodiac_music.h): plays each SMF the guest played from its Play
// to its Stop, Close or end, in guest time, and passes the real-time calls on, as MIDI messages to a sink with the
// guest time each is due. Master volume goes out as a Universal SysEx master volume message. A data byte over 0x7f
// drops its message, as the ROM's DAL does (its handlers refuse a note, velocity, program or controller value over
// 127), except pitch bend, which it masks to 7 bits, a song's note-off velocity and the value of All Sound Off,
// Reset All Controllers and All Notes Off, which it doesn't read (sent as 0). Stop, Close, a reset or load and a song's end send all notes off (CC 123 and 120) on all
// 16 channels. Host only, never saved; nothing here touches the machine.
// Two stages, neither on the emulation thread:
// - Preparer, on a helper thread, takes the hooks' events, parses each file at its first Play (keeping the last
//   few) and turns every event into a Cue, plain data with at most one Held (a parsed song or a sysex's bytes).
// - Player, on the audio thread, sequences the cues. It never allocates or frees: each Held goes back through
//   release when the player is done with it, and the Preparer frees it.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "zodiac_music.h"

namespace zodiac_music {

// time: guest ns when the message is due; bytes: one complete message (a sysex with its F0 and F7). escape: an SMF
// F7 escape's bytes, sent as they are; they aren't a message on their own.
using Sink = std::function<void(uint64_t timeNs, const uint8_t* bytes, size_t size, bool escape)>;

struct Parsed {
    std::shared_ptr<const Smf> smf;
    zodiac_smf::Song song;
};

// What a cue carries that lives on the heap. Made and freed by the Preparer only.
struct Held {
    std::shared_ptr<const Parsed> song;  // play
    std::vector<uint8_t> data;           // sysEx
};

// An event ready for the player: Event without its heap parts. held is null for a play whose file didn't parse
// (it still ends the handle's song) and for every kind but play and sysEx.
struct Cue {
    uint64_t timeNs = 0;
    Kind kind = Kind::reset;
    uint32_t handle = 0, value = 0;
    uint8_t bytes[3]{};
    uint8_t length = 0;
    Held* held = nullptr;
};

using Release = std::function<void(Held*)>;

class Preparer {
   public:
    // Turns e into a cue; e's heap parts move into the cue's Held or are dropped here.
    Cue prepare(Event&& e) {
        Cue c;
        c.timeNs = e.timeNs;
        c.kind = e.kind;
        c.handle = e.handle;
        c.value = e.value;
        for (int i = 0; i < 3; ++i) c.bytes[i] = e.bytes[i];
        c.length = e.length;
        switch (e.kind) {
            case Kind::play:
                if (auto song = parsed(e.smf)) {
                    c.held = new Held;
                    c.held->song = std::move(song);
                    ++outstanding_;
                }
                break;
            case Kind::sysEx:
                c.held = new Held;
                c.held->data = std::move(e.data);
                ++outstanding_;
                break;
            case Kind::reset: parsed_.clear(); break;
            default: break;
        }
        e = Event();
        return c;
    }
    void release(Held* h) {
        if (!h) return;
        delete h;
        --outstanding_;
    }
    // Helds made and not yet released.
    size_t outstanding() const { return outstanding_; }
    // Files that didn't parse.
    uint64_t refused() const { return refused_; }

   private:
    static constexpr size_t keepParsed = 16;
    std::vector<std::shared_ptr<const Parsed>> parsed_;
    size_t outstanding_ = 0;
    uint64_t refused_ = 0;

    std::shared_ptr<const Parsed> parsed(const std::shared_ptr<const Smf>& smf) {
        if (!smf) return nullptr;
        for (const auto& p : parsed_)
            if (p->smf == smf) return p;
        auto p = std::make_shared<Parsed>();
        p->smf = smf;
        if (zodiac_smf::parseSmf(smf->bytes.data(), smf->bytes.size(), p->song) != zodiac_smf::Error::none) {
            ++refused_;
            return nullptr;
        }
        if (parsed_.size() >= keepParsed) parsed_.erase(parsed_.begin());
        parsed_.push_back(p);
        return p;
    }
};

class Player {
   public:
    // Songs playing at once; a Play past this ends the oldest.
    static constexpr size_t maxSongs = 16;
    // Sysex from a file longer than this isn't sent.
    static constexpr size_t maxFileSysEx = maxSysExBytes;

    explicit Player(Release release) : release_(std::move(release)) { songs_.reserve(maxSongs); }
    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;
    ~Player() { releaseAll(); }

    // Takes the next cue, in the order caught: the songs play up to its time first. Its Held goes back through
    // release, now or when its song ends.
    void feed(const Cue& c, const Sink& sink) {
        advance(c.timeNs, sink);
        switch (c.kind) {
            case Kind::play:
                // A Play of a handle already playing starts it over.
                end(c.handle, c.timeNs, sink, false);
                if (!c.held) break;
                if (!c.held->song) { release_(c.held); break; }
                if (songs_.size() >= maxSongs) {
                    release_(songs_.front().held);
                    songs_.erase(songs_.begin());
                    allNotesOff(c.timeNs, sink);
                }
                songs_.push_back({c.handle, c.held, c.held->song.get(), c.timeNs, 0});
                break;
            case Kind::stop:
            case Kind::close: end(c.handle, c.timeNs, sink, true); break;
            case Kind::reset:
                releaseAll();
                allNotesOff(c.timeNs, sink);
                break;
            case Kind::message: {
                uint8_t b[3] = {c.bytes[0], c.bytes[1], c.bytes[2]};
                if (fit(b, c.length, false)) sink(c.timeNs, b, c.length, false);
                break;
            }
            case Kind::sysEx:
                if (c.held) {
                    const auto& d = c.held->data;
                    if (d.size() >= 2 && d.front() == 0xf0 && d.back() == 0xf7) sink(c.timeNs, d.data(), d.size(), false);
                    release_(c.held);
                }
                break;
            case Kind::masterVolume: {
                const uint32_t v = (c.value > 127 ? 127 : c.value) * 16383 / 127;
                const uint8_t b[8] = {0xf0, 0x7f, 0x7f, 0x04, 0x01, uint8_t(v & 0x7f), uint8_t(v >> 7), 0xf7};
                sink(c.timeNs, b, sizeof b, false);
                break;
            }
        }
    }

    // Sends every song message due by now, in time order (songs started earlier first at the same time). budget,
    // when given, is how many song events it may take: each one counts it down, and it stops at 0 with the rest
    // left for the next call. False when it stopped there.
    bool advance(uint64_t nowNs, const Sink& sink, size_t* budget = nullptr) {
        for (;;) {
            Song* next = nullptr;
            uint64_t at = 0;
            for (auto& s : songs_) {
                if (s.next >= s.parsed->song.events.size()) continue;
                const uint64_t t = s.startNs + s.parsed->song.events[s.next].timeUs * 1000;
                if (t <= nowNs && (!next || t < at)) next = &s, at = t;
            }
            if (!next) break;
            if (budget) {
                if (!*budget) return false;
                --*budget;
            }
            send(*next, at, sink);
        }
        // Songs played through end by themselves.
        for (size_t i = songs_.size(); i-- > 0;) {
            auto& s = songs_[i];
            if (s.next < s.parsed->song.events.size()) continue;
            const uint64_t endNs = s.startNs + s.parsed->song.lengthUs * 1000;
            if (endNs > nowNs) continue;
            release_(s.held);
            songs_.erase(songs_.begin() + long(i));
            allNotesOff(endNs, sink);
        }
        return true;
    }

    // Song events due before timeNs that haven't gone out yet give only their state when they do: note-ons are
    // dropped, and programs, controllers, pitch bend, note-offs and sysex still go. For a clock that jumped
    // forward, so the span it skipped doesn't sound all at once. 0 plays everything again.
    void skipBefore(uint64_t timeNs) { skipBeforeNs_ = timeNs; }

    size_t playing() const { return songs_.size(); }
    // When advance next has something to do: the earliest song event or song end, or UINT64_MAX with no song.
    uint64_t nextNs() const {
        uint64_t at = UINT64_MAX;
        for (const auto& s : songs_) {
            const auto& events = s.parsed->song.events;
            const uint64_t t = s.startNs + (s.next < events.size() ? events[s.next].timeUs : s.parsed->song.lengthUs) * 1000;
            if (t < at) at = t;
        }
        return at;
    }
    // Hands back every song's Held; the player plays nothing after.
    void releaseAll() {
        for (auto& s : songs_) release_(s.held);
        songs_.clear();
    }

    // Data bytes over 0x7f as the DAL takes them (ROM 1.1, the SMF event dispatch at 200bf53c and its handlers):
    // false drops the message. length counts the status byte; song is false for the real-time calls, whose
    // note-off velocity the DAL checks.
    static bool fit(uint8_t (&b)[3], size_t length, bool song) {
        const uint8_t type = b[0] & 0xf0;
        if (type == 0xe0) {
            b[1] &= 0x7f;
            b[2] &= 0x7f;
            return true;
        }
        if (b[1] > 0x7f) return false;
        if (length < 3 || b[2] <= 0x7f) return true;
        if ((type == 0x80 && song) || (type == 0xb0 && (b[1] == 120 || b[1] == 121 || b[1] == 123))) {
            b[2] = 0;
            return true;
        }
        return false;
    }

    static void allNotesOff(uint64_t timeNs, const Sink& sink) {
        for (uint8_t c = 0; c < 16; ++c) {
            const uint8_t notes[3] = {uint8_t(0xb0 | c), 123, 0}, sound[3] = {uint8_t(0xb0 | c), 120, 0};
            sink(timeNs, notes, 3, false);
            sink(timeNs, sound, 3, false);
        }
    }

   private:
    struct Song {
        uint32_t handle;
        Held* held;
        const Parsed* parsed;
        uint64_t startNs;
        size_t next;
    };
    const Release release_;
    std::vector<Song> songs_;  // reserved for maxSongs, never grown
    uint64_t skipBeforeNs_ = 0;
    uint8_t sysEx_[maxFileSysEx + 1];

    void end(uint32_t handle, uint64_t timeNs, const Sink& sink, bool notesOff) {
        bool found = false;
        for (size_t i = songs_.size(); i-- > 0;)
            if (songs_[i].handle == handle) {
                release_(songs_[i].held);
                songs_.erase(songs_.begin() + long(i));
                found = true;
            }
        if (found || notesOff) allNotesOff(timeNs, sink);
    }

    void send(Song& s, uint64_t timeNs, const Sink& sink) {
        const auto& e = s.parsed->song.events[s.next++];
        const auto& bytes = s.parsed->smf->bytes;
        if (e.status == 0xff) return;  // meta: tempo is in the times already
        if (e.status == 0xf0 || e.status == 0xf7) {
            if (!e.length || size_t(e.offset) + e.length > bytes.size() || e.length > maxFileSysEx) return;
            if (e.status == 0xf7) {
                sink(timeNs, bytes.data() + e.offset, e.length, true);
                return;
            }
            sysEx_[0] = 0xf0;
            std::memcpy(sysEx_ + 1, bytes.data() + e.offset, e.length);
            sink(timeNs, sysEx_, e.length + 1, false);
            return;
        }
        const uint8_t type = e.status & 0xf0;
        if (type == 0x90 && e.data2 && timeNs < skipBeforeNs_) return;
        uint8_t b[3] = {e.status, e.data1, e.data2};
        const size_t length = type == 0xc0 || type == 0xd0 ? 2 : 3;
        if (fit(b, length, true)) sink(timeNs, b, length, false);
    }
};

}  // namespace zodiac_music

#endif
