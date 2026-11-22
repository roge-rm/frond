#ifndef FROND_ZODIAC_MUSIC_SYNTH_H
#define FROND_ZODIAC_MUSIC_SYNTH_H

// The Zodiac's music hooks played through TinySoundFont: takes the hooks' events (zodiac_music.h), sequences them
// (zodiac_music_player.h) and mixes the SoundFont's sound into the host's 44100 Hz stereo stream.
// Threads: the emulation thread only pushes the hooks' events. A helper thread (pump) takes them, parses files and
// hands cues to the audio thread through a second ring; the audio thread (mix) sequences and renders, and sends
// every Held it is done with back through a third ring for the helper to free. mix doesn't allocate, free or lock.
// The music's clock is the guest audio actually played: mix is given only the frames taken from the guest's queue,
// so a pause or an underrun holds the songs where they are. Guest time maps onto those frames through an offset set
// at the first cue; when a cue drifts from it by more than maxDriftNs either way (Uncapped, dropped guest audio) the
// offset moves to it. One file defines TSF_IMPLEMENTATION before including this.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

    // The helper's work, once: frees what the audio thread sent back, then moves the hooks' events to it as cues.
    // Only the helper thread calls this (or a test, with no helper running). True when it moved anything.
    bool pump() {
        drainReturns();
        if (!hooks_) return false;
        bool moved = false;
        zodiac_music::Event e;
        while (!cues_.full() && prep_.outstanding() < maxHeld && hooks_->take(e)) {
            cues_.push(prep_.prepare(std::move(e)));
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

    // Audio thread. Mixes frames of music into stereo (interleaved left and right), saturating: frames is how many
    // frames of guest audio were played into it this period.
    void mix(int16_t* stereo, size_t frames) {
        if (!font_) return;
        size_t budget = maxCues;  // bounded work: at most one ring's worth of cues a period
        for (size_t at = 0; at < frames;) {
            const size_t n = frames - at < step ? frames - at : step;
            const uint64_t hostNs = frame_ * 1000000000ull / rate;
            zodiac_music::Cue* c = cues_.front();
            if (!anchored_ && c) {
                offsetNs_ = int64_t(c->timeNs) - int64_t(hostNs);
                anchored_ = true;
            }
            uint64_t now = guestNs(hostNs);
            for (; c && budget; c = cues_.front(), --budget) {
                if (c->timeNs > now + maxDriftNs || c->timeNs + maxDriftNs < now) {
                    offsetNs_ = int64_t(c->timeNs) - int64_t(hostNs);
                    now = c->timeNs;
                } else if (c->timeNs > now) {
                    break;
                }
                const zodiac_music::Cue cue = *c;
                cues_.pop();
                player_.feed(cue, sink_);
            }
            player_.advance(now, sink_);
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
    // Audio side.
    size_t playing() const { return player_.playing(); }
    size_t cuesWaiting() const { return cues_.size(); }

   private:
    static float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
    uint64_t guestNs(uint64_t hostNs) const {
        const int64_t t = int64_t(hostNs) + offsetNs_;
        return t < 0 ? 0 : uint64_t(t);
    }
    void drainReturns() {
        for (zodiac_music::Held** h; (h = returns_.front()); returns_.pop()) prep_.release(*h);
    }

    tsf* font_ = nullptr;
    zodiac_music::Hooks* hooks_ = nullptr;
    // Helper side.
    zodiac_music::Preparer prep_;
    std::thread helper_;
    std::atomic<bool> quit_{false};
    // Between the two: cues to the audio thread, Helds back.
    zodiac_music::SpscRing<zodiac_music::Cue> cues_;
    zodiac_music::SpscRing<zodiac_music::Held*> returns_;
    // Audio side.
    zodiac_music::Player player_;
    const zodiac_music::Sink sink_;
    uint64_t frame_ = 0;
    int64_t offsetNs_ = 0;
    bool anchored_ = false;
};

#endif
