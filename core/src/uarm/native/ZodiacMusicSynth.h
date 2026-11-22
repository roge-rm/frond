#ifndef FROND_ZODIAC_MUSIC_SYNTH_H
#define FROND_ZODIAC_MUSIC_SYNTH_H

// The Zodiac's music hooks played through TinySoundFont: takes the hooks' events (zodiac_music.h), sequences them
// (zodiac_music_player.h) and mixes the SoundFont's sound into the host's 44100 Hz stereo stream. Everything here
// runs on the host audio callback's thread, the hooks' only consumer; the emulation thread only pushes events.
// Guest time maps onto output frames through an offset set at the first event. When the guest drifts from the host
// clock by more than maxDriftNs either way (a pause, Uncapped), the offset moves to the event that drifted, so
// songs follow guest time between those points. One file defines TSF_IMPLEMENTATION before including this.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../../../../vendor/tinysoundfont/tsf.h"
#include "zodiac_music_player.h"

class ZodiacMusicSynth {
   public:
    static constexpr int rate = 44100;
    // Frames rendered between event checks, about 1.5 ms.
    static constexpr size_t step = 64;
    static constexpr uint64_t maxDriftNs = 250000000;
    static constexpr int maxVoices = 64;
    // Under the guest's own sound so the two don't clip together often.
    static constexpr float gainDb = -6.0f;

    ZodiacMusicSynth() : sink_([this](uint64_t, const uint8_t* b, size_t n) { apply(b, n); }) {
        waiting_.reserve(zodiac_music::maxEvents);
    }
    ~ZodiacMusicSynth() {
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

    // The hooks to take events from; they must outlive the synth's use.
    void attach(zodiac_music::Hooks* hooks) { hooks_ = hooks; }

    // Mixes frames of music into stereo (interleaved left and right), saturating.
    void mix(int16_t* stereo, size_t frames) {
        if (!font_ || !hooks_) return;
        zodiac_music::Event e;
        while (waiting_.size() < zodiac_music::maxEvents && hooks_->take(e)) waiting_.push_back(std::move(e));
        for (size_t at = 0; at < frames;) {
            const size_t n = frames - at < step ? frames - at : step;
            const uint64_t hostNs = frame_ * 1000000000ull / rate;
            if (!anchored_ && next_ < waiting_.size()) {
                offsetNs_ = int64_t(waiting_[next_].timeNs) - int64_t(hostNs);
                anchored_ = true;
            }
            uint64_t now = guestNs(hostNs);
            while (next_ < waiting_.size()) {
                auto& w = waiting_[next_];
                if (w.timeNs > now + maxDriftNs || w.timeNs + maxDriftNs < now) {
                    offsetNs_ = int64_t(w.timeNs) - int64_t(hostNs);
                    now = w.timeNs;
                } else if (w.timeNs > now) {
                    break;
                }
                player_.feed(w, sink_);
                w = zodiac_music::Event();
                ++next_;
            }
            player_.advance(now, sink_);
            tsf_render_short(font_, stereo + at * 2, int(n), 1);
            at += n;
            frame_ += n;
        }
        if (next_ == waiting_.size()) {
            waiting_.clear();
            next_ = 0;
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
    uint64_t refused() const { return player_.refused(); }
    size_t playing() const { return player_.playing(); }

   private:
    static float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }
    uint64_t guestNs(uint64_t hostNs) const {
        const int64_t t = int64_t(hostNs) + offsetNs_;
        return t < 0 ? 0 : uint64_t(t);
    }

    tsf* font_ = nullptr;
    zodiac_music::Hooks* hooks_ = nullptr;
    zodiac_music::Player player_;
    const zodiac_music::Sink sink_;
    std::vector<zodiac_music::Event> waiting_;
    size_t next_ = 0;
    uint64_t frame_ = 0;
    int64_t offsetNs_ = 0;
    bool anchored_ = false;
};

#endif
