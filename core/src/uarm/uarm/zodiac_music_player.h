#ifndef FROND_ZODIAC_MUSIC_PLAYER_H
#define FROND_ZODIAC_MUSIC_PLAYER_H

// The host sequencer for the Zodiac's music hooks (zodiac_music.h): plays each SMF the guest played from its Play
// to its Stop, Close or end, in guest time, and passes the real-time calls on, as MIDI messages to a sink with the
// guest time each is due. Master volume goes out as a Universal SysEx master volume message. Data bytes over 0x7f
// are clamped to 0x7f. Stop, Close, a reset or load and a song's end send all notes off (CC 123 and 120) on all
// 16 channels. Host only, never saved; nothing here touches the machine. It runs on the consumer's thread (the
// host audio side), never the emulation thread: it parses each file at its first Play and keeps the last few.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "zodiac_music.h"

namespace zodiac_music {

// time: guest ns when the message is due; bytes: one complete message (a sysex with its F0 and F7).
using Sink = std::function<void(uint64_t timeNs, const uint8_t* bytes, size_t size)>;

class Player {
   public:
    // Takes the next event from the hooks, in the order caught: the songs play up to its time first.
    void feed(const Event& e, const Sink& sink) {
        advance(e.timeNs, sink);
        switch (e.kind) {
            case Kind::play: {
                // A Play of a handle already playing starts it over.
                end(e.handle, e.timeNs, sink, false);
                auto song = parsed(e.smf);
                if (song) songs_.push_back({e.handle, std::move(song), e.timeNs, 0});
                break;
            }
            case Kind::stop:
            case Kind::close: end(e.handle, e.timeNs, sink, true); break;
            case Kind::reset:
                songs_.clear();
                parsed_.clear();
                allNotesOff(e.timeNs, sink);
                break;
            case Kind::message: {
                uint8_t b[3] = {e.bytes[0], clamp(e.bytes[1]), clamp(e.bytes[2])};
                sink(e.timeNs, b, e.length);
                break;
            }
            case Kind::sysEx:
                if (e.data.size() >= 2 && e.data.front() == 0xf0 && e.data.back() == 0xf7)
                    sink(e.timeNs, e.data.data(), e.data.size());
                break;
            case Kind::masterVolume: {
                const uint32_t v = (e.value > 127 ? 127 : e.value) * 16383 / 127;
                const uint8_t b[8] = {0xf0, 0x7f, 0x7f, 0x04, 0x01, uint8_t(v & 0x7f), uint8_t(v >> 7), 0xf7};
                sink(e.timeNs, b, sizeof b);
                break;
            }
        }
    }

    // Sends every song message due by now, in time order (songs started earlier first at the same time).
    void advance(uint64_t nowNs, const Sink& sink) {
        for (;;) {
            Song* next = nullptr;
            uint64_t at = 0;
            for (auto& s : songs_) {
                if (s.next >= s.smf->song.events.size()) continue;
                const uint64_t t = s.startNs + s.smf->song.events[s.next].timeUs * 1000;
                if (t <= nowNs && (!next || t < at)) next = &s, at = t;
            }
            if (!next) break;
            send(*next, at, sink);
        }
        // Songs played through end by themselves.
        for (size_t i = songs_.size(); i-- > 0;) {
            auto& s = songs_[i];
            if (s.next < s.smf->song.events.size()) continue;
            const uint64_t endNs = s.startNs + s.smf->song.lengthUs * 1000;
            if (endNs > nowNs) continue;
            songs_.erase(songs_.begin() + long(i));
            allNotesOff(endNs, sink);
        }
    }

    size_t playing() const { return songs_.size(); }
    // Files that didn't parse.
    uint64_t refused() const { return refused_; }

    static uint8_t clamp(uint8_t v) { return v > 0x7f ? 0x7f : v; }

    static void allNotesOff(uint64_t timeNs, const Sink& sink) {
        for (uint8_t c = 0; c < 16; ++c) {
            const uint8_t notes[3] = {uint8_t(0xb0 | c), 123, 0}, sound[3] = {uint8_t(0xb0 | c), 120, 0};
            sink(timeNs, notes, 3);
            sink(timeNs, sound, 3);
        }
    }

   private:
    struct Parsed {
        std::shared_ptr<const Smf> smf;
        zodiac_smf::Song song;
    };
    struct Song {
        uint32_t handle;
        std::shared_ptr<const Parsed> smf;
        uint64_t startNs;
        size_t next;
    };
    static constexpr size_t keepParsed = 16;
    std::vector<Song> songs_;
    std::vector<std::shared_ptr<const Parsed>> parsed_;
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

    void end(uint32_t handle, uint64_t timeNs, const Sink& sink, bool notesOff) {
        bool found = false;
        for (size_t i = songs_.size(); i-- > 0;)
            if (songs_[i].handle == handle) { songs_.erase(songs_.begin() + long(i)); found = true; }
        if (found || notesOff) allNotesOff(timeNs, sink);
    }

    void send(Song& s, uint64_t timeNs, const Sink& sink) {
        const auto& e = s.smf->song.events[s.next++];
        const auto& bytes = s.smf->smf->bytes;
        if (e.status == 0xff) return;  // meta: tempo is in the times already
        if (e.status == 0xf0 || e.status == 0xf7) {
            if (size_t(e.offset) + e.length > bytes.size()) return;
            std::vector<uint8_t> m;
            if (e.status == 0xf0) m.push_back(0xf0);
            m.insert(m.end(), bytes.begin() + e.offset, bytes.begin() + e.offset + e.length);
            if (!m.empty()) sink(timeNs, m.data(), m.size());
            return;
        }
        const uint8_t type = e.status & 0xf0;
        const uint8_t b[3] = {e.status, clamp(e.data1), clamp(e.data2)};
        sink(timeNs, b, type == 0xc0 || type == 0xd0 ? 2 : 3);
    }
};

}  // namespace zodiac_music

#endif
