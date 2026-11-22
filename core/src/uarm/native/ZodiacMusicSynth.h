#ifndef FROND_ZODIAC_MUSIC_SYNTH_H
#define FROND_ZODIAC_MUSIC_SYNTH_H

// The Zodiac's music hooks played through TinySoundFont: takes the hooks' events (zodiac_music.h), sequences them
// (zodiac_music_player.h) and mixes the SoundFont's sound into the host's 44100 Hz stereo stream.
// Threads: the emulation thread only pushes the hooks' events. A helper thread (pump) takes them, parses files and
// hands cues to the audio thread through a second ring; the audio thread (mix) sequences and renders, and sends
// every Held it is done with back through a third ring for the helper to free. mix doesn't allocate, free or lock.
// The music's clock is the frames the audio device plays, held only while the emulator is paused (hold, from the
// run loop). Guest time maps onto those frames through an offset set at the first cue; when a cue drifts from it by
// more than maxDriftNs either way (Uncapped, a slow host) the offset moves to it. On a forward jump the songs'
// skipped note-ons are dropped and only their state (programs, controllers, note-offs) goes out.
// The helper always drains the hooks' ring: while the audio side is behind, events wait in its own queue, where
// note-ons and note-offs may be dropped (notes off on the channel then follow) and every other event is kept.
// One file defines TSF_IMPLEMENTATION before including this.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "../../../../vendor/tinysoundfont/tsf.h"
#include "zodiac_music_player.h"

class ZodiacMusicSynth {
   public:
    static constexpr int rate = 44100;
    // Frames rendered between cue checks, about 1.5 ms.
    static constexpr size_t step = 64;
    static constexpr uint64_t maxDriftNs = 250000000;
    static constexpr int maxVoices = 64;
    // Under the guest's own sound so the two don't clip together often.
    static constexpr float gainDb = -6.0f;
    // Cues in flight to the audio thread, and Helds out at once (so the return ring can't fill).
    static constexpr size_t maxCues = 1024, maxHeld = 1024;
    // Song events a period may send; the rest go in the next.
    static constexpr size_t maxSongEvents = 512;
    // Events the helper keeps waiting; past this, notes are dropped.
    static constexpr size_t maxWaiting = 16384;
    static constexpr auto pumpEvery = std::chrono::milliseconds(2);

    ZodiacMusicSynth()
        : cues_(maxCues), returns_(maxHeld),
          player_([this](zodiac_music::Held* h) {
              // Never full: at most maxHeld Helds exist.
              returns_.push(std::move(h));
          }),
          sink_([this](uint64_t, const uint8_t* b, size_t n, bool escape) {
              if (!escape) apply(b, n);
          }) {}
    ~ZodiacMusicSynth() {
        stop();
        // No mix runs now: free what the audio side still holds.
        player_.releaseAll();
        for (zodiac_music::Cue* c; (c = cues_.front()); cues_.pop()) prep_.release(c->held);
        drainReturns();
        if (font_) tsf_close(font_);
    }
    ZodiacMusicSynth(const ZodiacMusicSynth&) = delete;
    ZodiacMusicSynth& operator=(const ZodiacMusicSynth&) = delete;

    // Loads a SoundFont 2 from memory. False when TinySoundFont can't read it. Call before any mix.
    bool load(const void* data, int size) {
        if (font_) tsf_close(font_);
        font_ = tsf_load_memory(data, size);
        if (!font_) return false;
        tsf_set_output(font_, TSF_STEREO_INTERLEAVED, rate, gainDb);
        tsf_set_max_voices(font_, maxVoices);
        // All 16 channels up front, so the audio thread doesn't grow them; 10 is the drum kit.
        for (int c = 15; c >= 0; --c) tsf_channel_set_presetnumber(font_, c, 0, c == 9);
        return true;
    }
    bool loadFile(const std::string& path) {
        std::FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) return false;
        std::vector<uint8_t> data;
        uint8_t chunk[65536];
        for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, file)) > 0;) data.insert(data.end(), chunk, chunk + n);
        std::fclose(file);
        return !data.empty() && data.size() < 0x7fffffff && load(data.data(), int(data.size()));
    }

    // The hooks to take events from (enabled); they must outlive the synth. Before start and any pump.
    void attach(zodiac_music::Hooks* hooks) { hooks_ = hooks; }
    // Called on the helper thread with each event taken, before the synth's own use of it (a MIDI out's offer).
    // Before start.
    void tap(std::function<void(const zodiac_music::Event&)> f) { tap_ = std::move(f); }

    // The helper's work, once: frees what the audio thread sent back, then moves the hooks' events to it as cues.
    // Only the helper thread calls this (or a test, with no helper running). True when it moved anything.
    bool pump() {
        drainReturns();
        if (!hooks_) return false;
        bool moved = false;
        for (zodiac_music::Event e; hooks_->take(e);) {
            if (tap_) tap_(e);
            wait(std::move(e));
        }
        while (!waiting_.empty() && !cues_.full() && prep_.outstanding() < maxHeld) {
            cues_.push(prep_.prepare(std::move(waiting_.front())));
            waiting_.pop_front();
            moved = true;
        }
        return moved;
    }
    // Runs pump on a helper thread until stop.
    void start() {
        if (helper_.joinable()) return;
        quit_.store(false);
        helper_ = std::thread([this] {
            while (!quit_.load(std::memory_order_relaxed)) {
                pump();
                std::this_thread::sleep_for(pumpEvery);
            }
        });
    }
    void stop() {
        if (!helper_.joinable()) return;
        quit_.store(true);
        helper_.join();
    }

    // Run loop: true while the emulator is paused (in the background, a paused replay). mix then renders nothing
    // and the song clock stands still.
    void hold(bool held) { held_.store(held, std::memory_order_relaxed); }

    // Audio thread. Mixes frames of music into stereo (interleaved left and right), saturating: frames is the whole
    // period the device plays, guest audio or not.
    void mix(int16_t* stereo, size_t frames) {
        if (!font_ || held_.load(std::memory_order_relaxed)) return;
        // Bounded work: at most one ring's worth of cues and maxSongEvents song events a period.
        size_t cueBudget = maxCues, eventBudget = maxSongEvents;
        for (size_t at = 0; at < frames;) {
            const size_t n = frames - at < step ? frames - at : step;
            const uint64_t hostNs = frame_ * 1000000000ull / rate;
            zodiac_music::Cue* c = cues_.front();
            if (!anchored_ && c) {
                offsetNs_ = int64_t(c->timeNs) - int64_t(hostNs);
                anchored_ = true;
            }
            uint64_t now = guestNs(hostNs);
            for (; c && cueBudget; c = cues_.front(), --cueBudget) {
                if (c->timeNs > now + maxDriftNs) {
                    // Ahead: the span skipped gives only its state.
                    offsetNs_ = int64_t(c->timeNs) - int64_t(hostNs);
                    now = c->timeNs;
                    player_.skipBefore(now);
                } else if (c->timeNs + maxDriftNs < now) {
                    offsetNs_ = int64_t(c->timeNs) - int64_t(hostNs);
                    now = c->timeNs;
                    player_.skipBefore(0);
                } else if (c->timeNs > now) {
                    break;
                }
                // The songs up to the cue first; out of budget, the cue waits for the next period.
                if (!player_.advance(c->timeNs, sink_, &eventBudget)) break;
                const zodiac_music::Cue cue = *c;
                cues_.pop();
                player_.feed(cue, sink_);
            }
            player_.advance(now, sink_, &eventBudget);
            tsf_render_short(font_, stereo + at * 2, int(n), 1);
            at += n;
            frame_ += n;
        }
    }

    // One MIDI message to the SoundFont: notes, controllers, programs, pitch bend and the Universal SysEx master
    // volume; anything else is ignored.
    void apply(const uint8_t* b, size_t n) {
        if (!n) return;
        const int c = b[0] & 15;
        switch (b[0] & 0xf0) {
            case 0x80:
                if (n >= 2) tsf_channel_note_off(font_, c, b[1]);
                break;
            case 0x90:
                if (n < 3) break;
                if (b[2]) tsf_channel_note_on(font_, c, b[1], b[2] / 127.0f);
                else tsf_channel_note_off(font_, c, b[1]);
                break;
            case 0xb0:
                if (n >= 3) tsf_channel_midi_control(font_, c, b[1], b[2]);
                break;
            case 0xc0:
                if (n >= 2) tsf_channel_set_presetnumber(font_, c, b[1], c == 9);
                break;
            case 0xe0:
                if (n >= 3) tsf_channel_set_pitchwheel(font_, c, b[1] | (b[2] << 7));
                break;
            case 0xf0:
                if (n == 8 && b[0] == 0xf0 && b[1] == 0x7f && b[3] == 0x04 && b[4] == 0x01)
                    tsf_set_volume(font_, float(b[5] | (b[6] << 7)) / 16383.0f * dbToGain(gainDb));
                break;
        }
    }

    uint64_t framesMixed() const { return frame_; }
    // Helper side: files refused and Helds not yet freed.
    uint64_t refused() const { return prep_.refused(); }
    size_t outstanding() const { return prep_.outstanding(); }
    size_t waiting() const { return waiting_.size(); }
    uint64_t notesDropped() const { return notesDropped_; }
    // Audio side.
    size_t playing() const { return player_.playing(); }
    size_t cuesWaiting() const { return cues_.size(); }

   private:
    static float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
    uint64_t guestNs(uint64_t hostNs) const {
        const int64_t t = int64_t(hostNs) + offsetNs_;
        return t < 0 ? 0 : uint64_t(t);
    }
    // Helper side: queues e for the audio thread. Past maxWaiting a note-on or note-off is dropped; a dropped
    // note-off's channel gets notes off once there is room again.
    void wait(zodiac_music::Event&& e) {
        using zodiac_music::Kind;
        if (e.kind == Kind::reset) {
            // Nothing before a reset matters; its own all notes off covers the channels.
            waiting_.clear();
            notesOffDue_ = 0;
        }
        const uint8_t type = e.bytes[0] & 0xf0;
        if (e.kind == Kind::message && (type == 0x80 || type == 0x90) && waiting_.size() >= maxWaiting) {
            ++notesDropped_;
            if (type == 0x80 || !e.bytes[2]) notesOffDue_ |= uint16_t(1u << (e.bytes[0] & 15));
            return;
        }
        if (notesOffDue_ && waiting_.size() < maxWaiting) {
            for (unsigned ch = 0; ch < 16; ++ch) {
                if (!(notesOffDue_ & (1u << ch))) continue;
                zodiac_music::Event off;
                off.timeNs = e.timeNs;
                off.kind = Kind::message;
                off.bytes[0] = uint8_t(0xb0 | ch);
                off.bytes[1] = 123;
                off.length = 3;
                waiting_.push_back(std::move(off));
            }
            notesOffDue_ = 0;
        }
        waiting_.push_back(std::move(e));
    }
    void drainReturns() {
        for (zodiac_music::Held** h; (h = returns_.front()); returns_.pop()) prep_.release(*h);
    }

    tsf* font_ = nullptr;
    zodiac_music::Hooks* hooks_ = nullptr;
    std::function<void(const zodiac_music::Event&)> tap_;
    // Helper side.
    zodiac_music::Preparer prep_;
    std::deque<zodiac_music::Event> waiting_;
    uint16_t notesOffDue_ = 0;
    uint64_t notesDropped_ = 0;
    std::thread helper_;
    std::atomic<bool> quit_{false};
    // Between the two: cues to the audio thread, Helds back.
    zodiac_music::SpscRing<zodiac_music::Cue> cues_;
    zodiac_music::SpscRing<zodiac_music::Held*> returns_;
    // Audio side.
    zodiac_music::Player player_;
    const zodiac_music::Sink sink_;
    std::atomic<bool> held_{false};
    uint64_t frame_ = 0;
    int64_t offsetNs_ = 0;
    bool anchored_ = false;
};

#endif
