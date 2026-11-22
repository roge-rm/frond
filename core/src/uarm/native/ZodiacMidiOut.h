#ifndef FROND_ZODIAC_MIDI_OUT_H
#define FROND_ZODIAC_MIDI_OUT_H

// The Zodiac's music hooks sent to a MIDI output port: takes the hooks' events (zodiac_music.h), sequences them
// (zodiac_music_player.h) and writes each message to a Port with the host time it is due.
// Threads: the emulation thread only pushes the hooks' events. The engine's own thread (start) takes them, either
// from the hooks (attach) or from the SoundFont synth's helper (offer), sequences and writes to the port, so the
// port's writes never happen on the emulation thread or the audio callback.
// Clock: host time from the clock function, held while the emulator is paused (hold). Guest time maps onto it
// through an offset set at the first event; when an event drifts from it by more than maxDriftNs either way the
// offset moves to it, and on a forward jump the songs' skipped note-ons are dropped. A hold, stop and the end
// send all notes off.
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "zodiac_music_player.h"

class ZodiacMidiOut {
   public:
    // A MIDI output. send gets one complete message (a sysex with its F0 and F7, or an SMF F7 escape's raw bytes)
    // and the host time, in the clock's ns, it is due; it is at most lookahead past the time of the call.
    // Only the engine's thread calls it.
    class Port {
       public:
        virtual ~Port() = default;
        virtual void send(uint64_t hostNs, const uint8_t* bytes, size_t size) = 0;
    };
    using Clock = std::function<uint64_t()>;

    static constexpr uint64_t maxDriftNs = 250000000;
    static constexpr auto workEvery = std::chrono::milliseconds(1);

    static uint64_t steadyNs() {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // lookaheadNs: how far ahead of the clock messages go out, for a port that schedules by timestamp.
    explicit ZodiacMidiOut(std::unique_ptr<Port> port, uint64_t lookaheadNs = 0, Clock clock = steadyNs)
        : port_(std::move(port)), clock_(std::move(clock)), lookaheadNs_(lookaheadNs),
          player_([this](zodiac_music::Held* h) { prep_.release(h); }),
          sink_([this](uint64_t timeNs, const uint8_t* b, size_t n, bool) { port_->send(hostNs(timeNs), b, n); }) {}
    ~ZodiacMidiOut() {
        stop();
        if (!quiet_) zodiac_music::Player::allNotesOff(lastGuestNs_, sink_);
        player_.releaseAll();
        for (auto& c : cues_) prep_.release(c.held);
    }
    ZodiacMidiOut(const ZodiacMidiOut&) = delete;
    ZodiacMidiOut& operator=(const ZodiacMidiOut&) = delete;

    // Takes events straight from the hooks too; they must outlive this. Before start. Without it, only offer
    // feeds it.
    void attach(zodiac_music::Hooks* hooks) { hooks_ = hooks; }
    // Any thread but the emulation thread: a copy of one of the hooks' events, in the order caught.
    void offer(const zodiac_music::Event& e) {
        std::lock_guard<std::mutex> lock(inboxLock_);
        inbox_.push_back(e);
    }

    // Run loop: true while the emulator is paused. The clock stands still and sounding notes are turned off.
    void hold(bool held) {
        std::lock_guard<std::mutex> lock(clockLock_);
        if (held == held_) return;
        const uint64_t now = clock_();
        if (held) heldSince_ = now;
        else heldNs_ += now - heldSince_;
        held_ = held;
    }

    // One pass at the clock's time: takes what arrived, sends what is due. The engine's thread calls this; a test
    // may call it with no thread running.
    void work() {
        uint64_t music;
        bool held;
        {
            std::lock_guard<std::mutex> lock(clockLock_);
            held = held_;
            music = (held ? heldSince_ : clock_()) - heldNs_;
        }
        take();
        if (held) {
            if (!quiet_) {
                zodiac_music::Player::allNotesOff(lastGuestNs_, sink_);
                quiet_ = true;
            }
            return;
        }
        music += lookaheadNs_;
        if (!anchored_ && !cues_.empty()) {
            offsetNs_ = int64_t(cues_.front().timeNs) - int64_t(music);
            anchored_ = true;
        }
        uint64_t now = guestNs(music);
        while (!cues_.empty()) {
            const zodiac_music::Cue& c = cues_.front();
            if (c.timeNs > now + maxDriftNs) {
                offsetNs_ = int64_t(c.timeNs) - int64_t(music);
                now = c.timeNs;
                player_.skipBefore(now);
            } else if (c.timeNs + maxDriftNs < now) {
                offsetNs_ = int64_t(c.timeNs) - int64_t(music);
                now = c.timeNs;
                player_.skipBefore(0);
            } else if (c.timeNs > now) {
                break;
            }
            const zodiac_music::Cue cue = c;
            cues_.pop_front();
            quiet_ = false;
            player_.feed(cue, sink_);
        }
        player_.advance(now, sink_);
        // A song playing on after a hold may sound again.
        if (player_.playing()) quiet_ = false;
        lastGuestNs_ = now;
    }

    void start() {
        if (thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(quitLock_);
            quit_ = false;
        }
        thread_ = std::thread([this] {
            std::unique_lock<std::mutex> lock(quitLock_);
            while (!quit_) {
                lock.unlock();
                work();
                lock.lock();
                quitWake_.wait_for(lock, workEvery, [this] { return quit_; });
            }
        });
    }
    void stop() {
        if (!thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(quitLock_);
            quit_ = true;
        }
        quitWake_.notify_all();
        thread_.join();
    }

    // The engine's side, or with no thread running.
    size_t playing() const { return player_.playing(); }
    size_t cuesWaiting() const { return cues_.size(); }
    uint64_t refused() const { return prep_.refused(); }
    size_t outstanding() const { return prep_.outstanding(); }

   private:
    // What offer gave first: a synth that was dropped before the hooks were attached may have handed some over.
    void take() {
        std::deque<zodiac_music::Event> got;
        {
            std::lock_guard<std::mutex> lock(inboxLock_);
            got.swap(inbox_);
        }
        for (auto& e : got) cue(std::move(e));
        if (hooks_)
            for (zodiac_music::Event e; hooks_->take(e);) cue(std::move(e));
    }
    void cue(zodiac_music::Event&& e) {
        if (e.kind == zodiac_music::Kind::reset) {
            // Nothing waiting before a reset matters.
            for (auto& c : cues_) prep_.release(c.held);
            cues_.clear();
        }
        cues_.push_back(prep_.prepare(std::move(e)));
    }
    uint64_t guestNs(uint64_t music) const {
        const int64_t t = int64_t(music) + offsetNs_;
        return t < 0 ? 0 : uint64_t(t);
    }
    // Guest ns to the clock's: back through the offset, plus the time held so far.
    uint64_t hostNs(uint64_t guest) const {
        const int64_t music = int64_t(guest) - offsetNs_;
        std::lock_guard<std::mutex> lock(clockLock_);
        const int64_t t = music + int64_t(heldNs_);
        return t < 0 ? 0 : uint64_t(t);
    }

    const std::unique_ptr<Port> port_;
    const Clock clock_;
    const uint64_t lookaheadNs_;
    zodiac_music::Hooks* hooks_ = nullptr;
    std::mutex inboxLock_;
    std::deque<zodiac_music::Event> inbox_;
    mutable std::mutex clockLock_;
    bool held_ = false;
    uint64_t heldSince_ = 0, heldNs_ = 0;
    // The engine's thread.
    zodiac_music::Preparer prep_;
    std::deque<zodiac_music::Cue> cues_;
    zodiac_music::Player player_;
    const zodiac_music::Sink sink_;
    int64_t offsetNs_ = 0;
    bool anchored_ = false, quiet_ = true;
    uint64_t lastGuestNs_ = 0;
    std::thread thread_;
    std::mutex quitLock_;
    std::condition_variable quitWake_;
    bool quit_ = false;
};

#endif
