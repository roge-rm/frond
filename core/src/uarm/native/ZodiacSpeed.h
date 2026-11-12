#pragma once

// Speed figures for the Zodiac, worked out on the guest thread once per window: instructions per
// second of real time and per second spent emulating, guest time against real time, frames handed
// to the SDL thread and drawn there, and the CPU time each of the two threads used. The SDL thread
// only adds to the draw counters.

#include <pthread.h>
#include <time.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

class ZodiacSpeed {
public:
    // Made on the SDL thread, which it reads the CPU time of. windowUsec is how long each window is.
    explicit ZodiacSpeed(uint64_t windowUsec) : windowUsec(windowUsec) {
        if (pthread_getcpuclockid(pthread_self(), &sdlClock) != 0) sdlClockValid = false;
    }

    // SDL thread: one Render call, with or without a new frame, and its length in us.
    void Drawn(bool frame, uint64_t usec) {
        draws.fetch_add(1, std::memory_order_relaxed);
        if (frame) frames.fetch_add(1, std::memory_order_relaxed);
        drawUsec.fetch_add(usec, std::memory_order_relaxed);
    }

    // Guest thread: time in the machine's run, and one frame rotated and handed over.
    void Emulated(uint64_t usec) { window.emulationUsec += usec; }
    void HandedOver(uint64_t usec) {
        window.handoffUsec += usec;
        ++window.published;
    }

    // Guest thread: whether Sample is due, the first time or once a window has passed.
    bool Due(uint64_t now) const { return !started || now - start.wall >= windowUsec; }

    // Guest thread, when Due: true after a whole window, with its figures in Line() and Ips().
    // retired is the machine's instruction count, guestTicks its 32768 Hz clock.
    bool Sample(uint64_t now, uint64_t retired, uint64_t guestTicks) {
        const uint64_t guestCpu = ThreadCpuUsec(CLOCK_THREAD_CPUTIME_ID, true);
        const uint64_t sdlCpu = ThreadCpuUsec(sdlClock, sdlClockValid);
        if (!started) {
            started = true;
            draws = frames = drawUsec = 0;
            Restart(now, retired, guestTicks, guestCpu, sdlCpu);
            return false;
        }
        const uint64_t wall = now - start.wall;
        const double seconds = wall / 1e6;
        const uint64_t instructions = retired - start.retired;
        const double guestSeconds = (guestTicks - start.guestTicks) / 32768.0;
        const uint64_t drawn = draws.exchange(0), drawnFrames = frames.exchange(0), drawTime = drawUsec.exchange(0);
        ips = uint64_t(instructions / seconds);
        char text[512];
        std::snprintf(text, sizeof text,
            "zodiac speed: %.1fM instr/s, %.1fM per emulating second, %.3fx real time; guest thread %.1f%% emulating, "
            "%.1f%% CPU; %.1f frames/s handed over (%.2f ms each), %.1f drawn (%.1f/s merged), %.1f renders/s "
            "(%.2f ms each), SDL thread %.1f%% CPU",
            instructions / seconds / 1e6,
            window.emulationUsec ? instructions / (window.emulationUsec / 1e6) / 1e6 : 0.0,
            guestSeconds / seconds,
            100.0 * window.emulationUsec / wall,
            100.0 * (guestCpu - start.guestCpu) / wall,
            window.published / seconds,
            window.published ? window.handoffUsec / 1e3 / window.published : 0.0,
            drawnFrames / seconds,
            window.published > drawnFrames ? (window.published - drawnFrames) / seconds : 0.0,
            drawn / seconds,
            drawn ? drawTime / 1e3 / drawn : 0.0,
            sdlClockValid ? 100.0 * (sdlCpu - start.sdlCpu) / wall : 0.0);
        line = text;
        Restart(now, retired, guestTicks, guestCpu, sdlCpu);
        return true;
    }

    const std::string& Line() const { return line; }
    // Instructions per second of real time in the last window, 0 before the first.
    uint64_t Ips() const { return ips; }

private:
    struct Window {
        uint64_t wall{0}, retired{0}, guestTicks{0}, guestCpu{0}, sdlCpu{0};
        uint64_t emulationUsec{0}, handoffUsec{0}, published{0};
    };

    void Restart(uint64_t now, uint64_t retired, uint64_t guestTicks, uint64_t guestCpu, uint64_t sdlCpu) {
        start = Window{now, retired, guestTicks, guestCpu, sdlCpu};
        window = Window{};
    }

    static uint64_t ThreadCpuUsec(clockid_t clock, bool valid) {
        timespec time{};
        if (!valid || clock_gettime(clock, &time) != 0) return 0;
        return uint64_t(time.tv_sec) * 1000000 + uint64_t(time.tv_nsec) / 1000;
    }

    const uint64_t windowUsec;
    clockid_t sdlClock{};
    bool sdlClockValid{true};
    bool started{false};
    Window start, window;
    std::string line;
    uint64_t ips{0};
    std::atomic<uint64_t> draws{0}, frames{0}, drawUsec{0};
};
