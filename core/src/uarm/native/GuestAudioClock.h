#ifndef FROND_GUEST_AUDIO_CLOCK_H
#define FROND_GUEST_AUDIO_CLOCK_H

// The Zodiac music's clock, on the audio callback: the guest time of the guest's own sound as the device plays it,
// so the SoundFont and the MIDI out keep with the guest's sound effects.
// It runs on the device's frames, 64 at a time, and steers to the guest time of the frames popped from the audio
// queue (AudioQueueSpan, from the queue's marks): up to a quarter faster while behind, down to standing while
// ahead. With no guest audio (an underrun, the SSI off) it runs on by the frames, so a song keeps its pace across
// the guest's sound starting and stopping, and over guest audio left from before a stop (a break in the span).
// More than maxDriftNs off it jumps (a load, an overflowing queue), except that guest audio coming back after a gap,
// ahead by the queue it no longer has, is steered to up to maxEaseNs.
// Each period's start and end are published for the MIDI out's thread with a host time: when that period reaches
// the speaker, taken as one period after the callback that renders it.
// Threads: begin, next and end on the audio callback only; hold and read from any thread. Nothing locks: a
// seqlock with release stores and acquire loads (no fences, which TSan can't follow).
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "audio_queue.h"

class GuestAudioClock {
   public:
    static constexpr uint64_t rate = 44100;
    static constexpr uint64_t maxDriftNs = 250000000;
    // Run on this long with no guest audio, the guest audio coming back up to maxEaseNs ahead is steered to.
    static constexpr uint64_t gapNs = 100000000, maxEaseNs = 2000000000;
    // A cue more than this past the guest's newest time (the queue's newest mark) is from before a load that set
    // the guest time back: due now. A cue is made at most a run loop pass after the mark before it.
    static constexpr uint64_t staleNs = maxDriftNs;
    enum class Jump { none, forward, back };

    // A published period: host ns when its first frame plays and the guest ns there, the same at its end. jumps
    // counts the jumps so far, jumpNs is where the last one went, back whether it went back. newestNs: the guest's
    // newest time then, 0 if not known.
    struct Reference {
        uint64_t hostNs = 0, guestNs = 0, endHostNs = 0, endGuestNs = 0, newestNs = 0;
        uint32_t jumps = 0;
        uint64_t jumpNs = 0;
        bool back = false;
    };

    static uint64_t steadyNs() {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    static uint64_t framesNs(uint64_t frames) { return frames * 1000000000ull / rate; }

    // The run loop: true while the emulator is paused. The clock stands; begin says so. Meanwhile the device plays
    // out the queue, so on resume the guest audio is ahead by it: steered to like after a gap, not jumped to.
    void hold(bool held) { held_.store(held, std::memory_order_relaxed); }
    bool held() const { return held_.load(std::memory_order_relaxed); }

    // Audio thread. A period of frames, rendered at host ns hostNs (steadyNs). False while held: then no next or
    // end for it.
    bool begin(uint64_t hostNs, size_t frames) {
        if (held()) {
            wasHeld_ = true;
            return false;
        }
        if (wasHeld_) {
            wasHeld_ = false;
            resumed_ = true;
        }
        periodHostNs_ = hostNs + framesNs(frames);
        periodFrames_ = frames;
        first_ = true;
        return true;
    }
    // Guest ns for the frames from at to at + n of the period, in order from at = 0. Unanchored (no span known
    // yet) it gives 0 and the caller keeps its own clock.
    uint64_t next(const AudioQueueSpan& span, size_t at, size_t n, Jump* jump = nullptr) {
        if (jump) *jump = Jump::none;
        // Frames before a break are older guest audio, left from before the guest's sound stopped: run on by the
        // frames over them.
        const bool broken = span.breakAt < span.frames;
        const bool known = span.known && at < span.frames && (!broken || at >= span.breakAt);
        uint64_t target = 0;
        if (known) {
            const size_t from = broken ? span.breakAt : 0;
            const uint64_t startNs = broken ? span.breakNs : span.startNs;
            target = startNs;
            if (span.endNs > startNs) target += (span.endNs - startNs) * (at - from) / (span.frames - from);
        }
        if (!anchored_) {
            if (!known) return 0;
            clockNs_ = target;
            anchored_ = true;
        } else if (known && !easing_ && (runOnNs_ >= gapNs || resumed_) && target > clockNs_) {
            // Guest audio back after a gap or a hold: it now plays with less queued before it, so it's ahead.
            // Steered to, not jumped to, so no note is lost.
            easing_ = true;
        }
        if (known) {
            runOnNs_ = 0;
            resumed_ = false;
            newestNs_ = span.newestNs;
        }
        else runOnNs_ += framesNs(n);
        // Done once caught up; further than maxEaseNs it jumps after all.
        if (known && easing_ && (target <= clockNs_ + framesNs(n) || target > clockNs_ + maxEaseNs)) easing_ = false;
        if (known && !easing_ && (target > clockNs_ + maxDriftNs || target + maxDriftNs < clockNs_)) {
            const bool back = target < clockNs_;
            clockNs_ = target;
            if (jump) *jump = back ? Jump::back : Jump::forward;
            jumpNs_ = target;
            jumpBack_ = back;
            ++jumps_;
        }
        const uint64_t now = clockNs_;
        const int64_t stepNs = int64_t(framesNs(n));
        int64_t advance = stepNs;
        if (known) {
            int64_t error = int64_t(target) - int64_t(now);
            if (error < -stepNs) error = -stepNs;
            if (error > stepNs / 4) error = stepNs / 4;
            advance += error;
        }
        clockNs_ = now + uint64_t(advance);
        if (first_) {
            periodGuestNs_ = now;
            first_ = false;
        }
        return now;
    }
    // Audio thread, after the period's last next: publishes it.
    void end() {
        if (!anchored_ || first_) return;
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_relaxed);
        hostNs_.store(periodHostNs_, std::memory_order_release);
        guestNs_.store(periodGuestNs_, std::memory_order_release);
        endHostNs_.store(periodHostNs_ + framesNs(periodFrames_), std::memory_order_release);
        endGuestNs_.store(clockNs_, std::memory_order_release);
        newestOut_.store(newestNs_, std::memory_order_release);
        jumpsOut_.store(jumps_, std::memory_order_release);
        jumpNsOut_.store(jumpNs_, std::memory_order_release);
        backOut_.store(jumpBack_, std::memory_order_release);
        seq_.store(s + 2, std::memory_order_release);
    }
    // Audio thread: steps a whole period with nothing else to do with it (MIDI out with no SoundFont).
    void run(const AudioQueueSpan& span, size_t frames, uint64_t hostNs) {
        if (!begin(hostNs, frames)) return;
        for (size_t at = 0; at < frames; at += step) next(span, at, frames - at < step ? frames - at : step);
        end();
    }
    static constexpr size_t step = 64;

    bool anchored() const { return anchored_; }
    // Audio thread: the guest's newest time from the last span, 0 before one.
    uint64_t newestNs() const { return newestNs_; }

    // Whether a cue of guest ns timeNs, ahead of the clock at nowNs, waits for its time: false when it's from
    // before a load (past the guest's newest time), or with that unknown further than maxAheadNs ahead.
    static constexpr uint64_t maxAheadNs = 2000000000;
    static bool waits(uint64_t timeNs, uint64_t nowNs, uint64_t newestNs) {
        if (timeNs <= nowNs) return false;
        return newestNs ? timeNs <= newestNs + staleNs : timeNs <= nowNs + maxAheadNs;
    }
    // A reference that ended more than a period before host ns t: none published since, as through a hold. Not
    // to time by until the next.
    static bool stale(const Reference& r, uint64_t t) { return r.endHostNs + (r.endHostNs - r.hostNs) < t; }

    // Any thread: the last period published. False before the first.
    bool read(Reference& r) const {
        for (;;) {
            const uint32_t s = seq_.load(std::memory_order_acquire);
            if (!s) return false;
            if (s & 1) continue;
            r.hostNs = hostNs_.load(std::memory_order_acquire);
            r.guestNs = guestNs_.load(std::memory_order_acquire);
            r.endHostNs = endHostNs_.load(std::memory_order_acquire);
            r.endGuestNs = endGuestNs_.load(std::memory_order_acquire);
            r.newestNs = newestOut_.load(std::memory_order_acquire);
            r.jumps = jumpsOut_.load(std::memory_order_acquire);
            r.jumpNs = jumpNsOut_.load(std::memory_order_acquire);
            r.back = backOut_.load(std::memory_order_acquire);
            if (seq_.load(std::memory_order_relaxed) == s) return true;
        }
    }

    // Guest ns playing at host ns t by a reference: through the period at its rate, at real speed before it, and
    // past its end at real speed for at most one more period (the next one is published by then unless the clock
    // stands, held or with no device).
    static uint64_t guestAt(const Reference& r, uint64_t t) {
        if (t <= r.hostNs) return r.guestNs > r.hostNs - t ? r.guestNs - (r.hostNs - t) : 0;
        if (t >= r.endHostNs) {
            const uint64_t past = t - r.endHostNs, period = r.endHostNs - r.hostNs;
            return r.endGuestNs + (past < period ? past : period);
        }
        return r.guestNs + (r.endGuestNs - r.guestNs) * (t - r.hostNs) / (r.endHostNs - r.hostNs);
    }
    // Host ns when guest ns g plays by a reference: the inverse of guestAt. At a period that stood, its end.
    static uint64_t hostAt(const Reference& r, uint64_t g) {
        if (g < r.guestNs) return r.hostNs > r.guestNs - g ? r.hostNs - (r.guestNs - g) : 0;
        if (g >= r.endGuestNs) return r.endHostNs + (g - r.endGuestNs);
        return r.hostNs + (r.endHostNs - r.hostNs) * (g - r.guestNs) / (r.endGuestNs - r.guestNs);
    }

   private:
    std::atomic<bool> held_{false};
    // Audio thread.
    bool anchored_ = false, first_ = true, jumpBack_ = false, easing_ = false, wasHeld_ = false, resumed_ = false;
    uint64_t newestNs_ = 0, runOnNs_ = 0, clockNs_ = 0, periodHostNs_ = 0, periodGuestNs_ = 0, jumpNs_ = 0;
    size_t periodFrames_ = 0;
    uint32_t jumps_ = 0;
    // Published, under seq_ (odd while writing).
    std::atomic<uint32_t> seq_{0};
    std::atomic<uint64_t> hostNs_{0}, guestNs_{0}, endHostNs_{0}, endGuestNs_{0}, newestOut_{0}, jumpNsOut_{0};
    std::atomic<uint32_t> jumpsOut_{0};
    std::atomic<bool> backOut_{false};
};

#endif
