#ifndef FROND_ZODIAC_MIDI_OUT_H
#define FROND_ZODIAC_MIDI_OUT_H

// The Zodiac's music hooks sent to a MIDI output port: takes the hooks' events (zodiac_music.h), sequences them
// (zodiac_music_player.h) and writes each message to a Port with the host time it is due.
// Threads: the emulation thread only pushes the hooks' events. The engine's own thread (start) takes them, either
// from the hooks (attach) or from the SoundFont synth's helper (offer), sequences and writes to the port, so the
// port's writes never happen on the emulation thread or the audio callback.
// Clock: with a reference (setReference, the GuestAudioClock the audio callback runs), each message goes out when
// the guest sound playing reaches its guest time, less the synth's own lag (setSynthLag), so it keeps with the
// guest's sound through the queue's depth, underruns and a guest slower than real time. The reference's jumps
// drop the songs' skipped note-ons. With no reference yet (no audio device), host time from the clock function,
// held while the emulator is paused (hold), with guest time mapped onto it through an offset set at the first
// event and moved when an event drifts from it by more than maxDriftNs either way (a forward jump drops skipped
// note-ons); messages then go out delayNs after the guest made them (setDelay). Either way a hold, stop and the
// end send all notes off. Timestamps never go back.
// The thread sleeps until the next message is due, or until offer, hold, poke or stop wake it.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "GuestAudioClock.h"
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
        // True once the output can't take the music (it didn't open or went away). Any thread, without blocking.
        virtual bool failed() const { return false; }
    };
    using Clock = std::function<uint64_t()>;

    static constexpr uint64_t maxDriftNs = 250000000;
    // setDelay's limit, under maxDriftNs so a delayed event isn't taken as late.
    static constexpr uint64_t maxDelayNs = 200000000;
    // With the hooks attached and nothing due, the longest sleep: poke normally wakes it sooner.
    static constexpr auto idleWait = std::chrono::milliseconds(100);
    // With a reference, the longest sleep while something is due, so the thread follows the clock as it steers.
    static constexpr uint64_t referenceWaitNs = 5000000;

    static uint64_t steadyNs() {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // lookaheadNs: how far ahead of the clock messages go out, for a port that schedules by timestamp.
    explicit ZodiacMidiOut(std::unique_ptr<Port> port, uint64_t lookaheadNs = 0, Clock clock = steadyNs)
        : port_(std::move(port)), clock_(std::move(clock)), lookaheadNs_(lookaheadNs),
          player_([this](zodiac_music::Held* h) { prep_.release(h); }),
          sink_([this](uint64_t timeNs, const uint8_t* b, size_t n, bool) { port_->send(stamp(timeNs), b, n); }) {}
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
        {
            std::lock_guard<std::mutex> lock(inboxLock_);
            inbox_.push_back(e);
        }
        wake();
    }
    // The run loop, each pass: wakes the thread when the attached hooks have events waiting. One atomic read
    // otherwise.
    void poke() {
        if (hooks_ && hooks_->pending()) wake();
    }
    // The audio callback's clock to time by; it must outlive this. Before start.
    void setReference(const GuestAudioClock* clock) { reference_ = clock; }
    // With a reference: how much earlier than its sound messages go out, for the synth's own lag. Before start.
    void setSynthLag(uint64_t ns) { synthLagNs_ = ns; }

    // Without a reference: how long after the guest made them messages go out, in ns, up to maxDelayNs. Any
    // thread.
    void setDelay(uint64_t ns) { delayNs_.store(ns < maxDelayNs ? ns : maxDelayNs, std::memory_order_relaxed); }
    uint64_t delay() const { return delayNs_.load(std::memory_order_relaxed); }

    // Run loop: true while the emulator is paused. The clock stands still and sounding notes are turned off.
    void hold(bool held) {
        {
            std::lock_guard<std::mutex> lock(clockLock_);
            if (held == held_) return;
            const uint64_t now = clock_();
            if (held) heldSince_ = now;
            else heldNs_ += now - heldSince_;
            held_ = held;
        }
        wake();
    }

    // One pass at the clock's time: takes what arrived, sends what is due. The engine's thread calls this; a test
    // may call it with no thread running.
    void work() {
        passes_.fetch_add(1, std::memory_order_relaxed);
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
        if (reference_ && reference_->read(ref_)) {
            byReference();
            return;
        }
        byReference_ = false;
        music += lookaheadNs_;
        if (!anchored_ && !cues_.empty()) {
            offsetNs_ = int64_t(cues_.front().timeNs) - int64_t(music);
            anchored_ = true;
        }
        // now: the guest time arriving at this moment, which the drift checks use; due: now less the delay, what
        // goes out.
        passDelayNs_ = delay();
        uint64_t now = guestNs(music);
        uint64_t due = now > passDelayNs_ ? now - passDelayNs_ : 0;
        while (!cues_.empty()) {
            const zodiac_music::Cue& c = cues_.front();
            if (c.timeNs > now + maxDriftNs) {
                offsetNs_ = int64_t(c.timeNs) - int64_t(music);
                now = c.timeNs;
                due = now > passDelayNs_ ? now - passDelayNs_ : 0;
                player_.skipBefore(due);
                // The cue that moved the offset waits for the delay like any other.
                continue;
            }
            if (c.timeNs + maxDriftNs < now) {
                offsetNs_ = int64_t(c.timeNs) - int64_t(music);
                now = c.timeNs;
                due = now > passDelayNs_ ? now - passDelayNs_ : 0;
                player_.skipBefore(0);
                continue;
            }
            if (c.timeNs > due) {
                break;
            }
            const zodiac_music::Cue cue = c;
            cues_.pop_front();
            quiet_ = false;
            player_.feed(cue, sink_);
        }
        player_.advance(due, sink_);
        // A song playing on after a hold may sound again.
        if (player_.playing()) quiet_ = false;
        lastGuestNs_ = due;
    }

    // After work: the clock's time the next message is due, or UINT64_MAX with nothing waiting or while held.
    uint64_t nextWakeNs() const {
        {
            std::lock_guard<std::mutex> lock(clockLock_);
            if (held_) return UINT64_MAX;
        }
        uint64_t guest = player_.nextNs();
        if (!cues_.empty() && cues_.front().timeNs < guest) guest = cues_.front().timeNs;
        if (guest == UINT64_MAX) return UINT64_MAX;
        if (byReference_) {
            const uint64_t soonest = clock_() + referenceWaitNs;
            if (GuestAudioClock::stale(ref_, clock_())) return soonest;
            const uint64_t at = GuestAudioClock::hostAt(ref_, guest), early = synthLagNs_ + lookaheadNs_;
            const uint64_t t = at > early ? at - early : 0;
            return t < soonest ? t : soonest;
        }
        const int64_t t = int64_t(hostNs(guest)) + int64_t(passDelayNs_) - int64_t(lookaheadNs_);
        return t < 0 ? 0 : uint64_t(t);
    }

    void start() {
        if (thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(wakeLock_);
            quit_ = false;
        }
        thread_ = std::thread([this] {
            std::unique_lock<std::mutex> lock(wakeLock_);
            while (!quit_) {
                woken_ = false;
                lock.unlock();
                work();
                const uint64_t next = nextWakeNs();
                lock.lock();
                const auto woke = [this] { return quit_ || woken_; };
                if (next != UINT64_MAX) {
                    const uint64_t now = clock_();
                    if (next > now) wakeUp_.wait_for(lock, std::chrono::nanoseconds(next - now), woke);
                } else if (hooks_) {
                    wakeUp_.wait_for(lock, idleWait, woke);
                } else {
                    wakeUp_.wait(lock, woke);
                }
            }
        });
    }
    void stop() {
        if (!thread_.joinable()) return;
        {
            std::lock_guard<std::mutex> lock(wakeLock_);
            quit_ = true;
        }
        wakeUp_.notify_all();
        thread_.join();
    }
    // The port's failed, for the run loop. Any thread.
    bool portFailed() const { return port_->failed(); }
    // Passes the engine's thread has made, for tests.
    uint64_t passes() const { return passes_.load(std::memory_order_relaxed); }

    // The engine's side, or with no thread running.
    size_t playing() const { return player_.playing(); }
    size_t cuesWaiting() const { return cues_.size(); }
    uint64_t refused() const { return prep_.refused(); }
    size_t outstanding() const { return prep_.outstanding(); }

   private:
    // One pass by the reference: what is due is the guest sound playing at the clock's time, plus the lookahead
    // and the synth's lag.
    void byReference() {
        byReference_ = true;
        // None published for a while (a hold just ended): wait for the next period, not run on from the old one.
        if (GuestAudioClock::stale(ref_, clock_())) return;
        const uint64_t due = GuestAudioClock::guestAt(ref_, clock_() + lookaheadNs_ + synthLagNs_);
        if (ref_.jumps != jumpsSeen_) {
            jumpsSeen_ = ref_.jumps;
            player_.skipBefore(ref_.back ? 0 : ref_.jumpNs);
        }
        while (!cues_.empty()) {
            zodiac_music::Cue cue = cues_.front();
            if (GuestAudioClock::waits(cue.timeNs, due, ref_.newestNs)) break;
            // Late, or stale from before a load: due now, so a Play starts here with its whole song.
            if (cue.timeNs > due) cue.timeNs = due;
            cues_.pop_front();
            quiet_ = false;
            player_.feed(cue, sink_);
        }
        player_.advance(due, sink_);
        if (player_.playing()) quiet_ = false;
        lastGuestNs_ = due;
    }
    // The clock's ns a message of guest ns timeNs is due at the port. Never before the last one: the guest to
    // host map moves (callback jitter, steering, jumps) and a port that schedules by timestamp would reorder.
    uint64_t stamp(uint64_t timeNs) {
        uint64_t at;
        if (!byReference_) {
            at = hostNs(timeNs) + passDelayNs_;
        } else {
            at = GuestAudioClock::hostAt(ref_, timeNs);
            at = at > synthLagNs_ ? at - synthLagNs_ : 0;
        }
        if (at < lastStampNs_) at = lastStampNs_;
        lastStampNs_ = at;
        return at;
    }
    void wake() {
        {
            std::lock_guard<std::mutex> lock(wakeLock_);
            woken_ = true;
        }
        wakeUp_.notify_one();
    }
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
    uint64_t passDelayNs_ = 0;  // the delay this pass sends with
    const GuestAudioClock* reference_ = nullptr;
    GuestAudioClock::Reference ref_;  // this pass's
    bool byReference_ = false;
    uint32_t jumpsSeen_ = 0;
    uint64_t synthLagNs_ = 0, lastStampNs_ = 0;
    std::atomic<uint64_t> delayNs_{0};
    std::atomic<uint64_t> passes_{0};
    std::thread thread_;
    std::mutex wakeLock_;
    std::condition_variable wakeUp_;
    bool quit_ = false, woken_ = false;
};

#endif
