#ifndef FROND_INPUT_TRACE_H
#define FROND_INPUT_TRACE_H

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

// Input latency trace, host side only. FROND_INPUT_TRACE=<file> (or "stderr") writes one line per stage:
//   IT <stage> <host us, steady clock> <guest us or -> <fields>
// Stages: host (the event reaches the SDL filter), take (the guest thread hands it to the SoC), apply (the
// machine's pin, pen or stick changes), read (the first guest read that sees it), gframe (the first guest
// frame whose pixels differ from the frame at apply), publish (that frame handed to the SDL thread),
// present (SDL_RenderPresent returned with it). One press is followed at a time; a new apply starts over.
// Off unless the variable is set; every hook is one test of `on`. Nothing here changes the machine.
namespace inputTrace {

struct State {
    FILE* out = nullptr;
    std::mutex mutex;
    uint64_t (*guestTicks)(void*) = nullptr;  // CLK32 ticks
    void* guestContext = nullptr;
    // What the press being followed waits for next.
    bool readArmed = false;
    unsigned readPort = 0;
    uint32_t readMask = 0, readLevel = 0;
    bool penArmed = false, stickArmed = false;
    uint32_t stickWant[2] = {0, 0};
    bool frameArmed = false, publishArmed = false, presentArmed = false;
    std::vector<uint32_t> baseline;
};

inline State& state() {
    static State s;
    return s;
}

inline FILE* openFromEnvironment() {
    const char* path = std::getenv("FROND_INPUT_TRACE");
    if (!path || !*path) return nullptr;
    if (!std::strcmp(path, "stderr")) return stderr;
    FILE* f = std::fopen(path, "a");
    if (f) std::setvbuf(f, nullptr, _IOLBF, 0);
    return f;
}

inline const bool on = [] {
    FILE* f = openFromEnvironment();
    state().out = f;
    return f != nullptr;
}();

inline uint64_t hostUsec() {
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline void setGuestClock(uint64_t (*ticks)(void*), void* context) {
    if (!on) return;
    std::lock_guard<std::mutex> lock(state().mutex);
    state().guestTicks = ticks;
    state().guestContext = context;
}

// Caller holds the mutex.
// The guest clock is read only on the guest thread (guest true).
inline void lineLocked(const char* stage, bool guestThread, const char* format, ...) __attribute__((format(printf, 3, 4)));
inline void lineLocked(const char* stage, bool guestThread, const char* format, ...) {
    State& s = state();
    char guest[32] = "-";
    if (guestThread && s.guestTicks) std::snprintf(guest, sizeof guest, "%llu",
                                    (unsigned long long)(s.guestTicks(s.guestContext) * 1000000ull / 32768));
    std::fprintf(s.out, "IT %s %llu %s ", stage, (unsigned long long)hostUsec(), guest);
    va_list args;
    va_start(args, format);
    std::vfprintf(s.out, format, args);
    va_end(args);
    std::fputc('\n', s.out);
}

// SDL filter and guest thread: an event, with what it is.
inline void hostEvent(const char* stage, bool guestThread, const char* kind, int a, int b, uint32_t sdlMsec) {
    if (!on) return;
    std::lock_guard<std::mutex> lock(state().mutex);
    lineLocked(stage, guestThread, "%s %d %d sdl=%u", kind, a, b, sdlMsec);
}

// Guest thread, between runs: a press reached the machine. The frame shown then is the one to differ from.
inline void startFollow(const uint32_t* frame, size_t pixels) {
    State& s = state();
    s.frameArmed = frame != nullptr;
    s.publishArmed = s.presentArmed = false;
    if (frame) s.baseline.assign(frame, frame + pixels);
}
inline void applyControl(unsigned control, bool down, unsigned port, unsigned pin, const uint32_t* frame, size_t pixels) {
    if (!on) return;
    std::lock_guard<std::mutex> lock(state().mutex);
    State& s = state();
    lineLocked("apply", true, "control %u %d", control, int(down));
    if (!down) return;
    s.readArmed = true;
    s.readPort = port;
    s.readMask = 1u << pin;
    s.readLevel = 0;  // low while pressed
    s.penArmed = s.stickArmed = false;
    startFollow(frame, pixels);
}
inline void applyPen(bool down, int x, int y, const uint32_t* frame, size_t pixels) {
    if (!on) return;
    std::lock_guard<std::mutex> lock(state().mutex);
    State& s = state();
    lineLocked("apply", true, "pen %d %d %d", int(down), x, y);
    if (!down) return;
    s.penArmed = true;
    s.readArmed = s.stickArmed = false;
    startFollow(frame, pixels);
}
// The first step away from rest starts a follow; later steps only log.
inline void applyStick(int x, int y, bool fromRest, const uint32_t* frame, size_t pixels) {
    if (!on) return;
    std::lock_guard<std::mutex> lock(state().mutex);
    State& s = state();
    lineLocked("apply", true, "stick %d %d", x, y);
    if (!fromRest || (x == 0 && y == 0)) return;
    s.stickArmed = true;
    s.stickWant[0] = uint32_t(512 - x);
    s.stickWant[1] = uint32_t(512 + y);
    s.readArmed = s.penArmed = false;
    startFollow(frame, pixels);
}

// Guest thread, inside a run: a GPIO SSR read.
inline void gpioRead(unsigned port, uint32_t value) {
    if (!on) return;
    std::lock_guard<std::mutex> lock(state().mutex);
    State& s = state();
    if (!s.readArmed || port != s.readPort || (value & s.readMask) != s.readLevel) return;
    s.readArmed = false;
    lineLocked("read", true, "gpio %u %08x", port, value);
}
// Guest thread, inside a run: an ASP pen FIFO read.
inline void penFifoRead(uint32_t value) {
    if (!on) return;
    std::lock_guard<std::mutex> lock(state().mutex);
    State& s = state();
    if (!s.penArmed) return;
    s.penArmed = false;
    lineLocked("read", true, "pen-fifo %08x", value);
}
// Guest thread, inside a run: the graphics chip's ADC took the stick's two axes.
inline void stickSampled(uint32_t first, uint32_t second) {
    if (!on) return;
    std::lock_guard<std::mutex> lock(state().mutex);
    State& s = state();
    if (!s.stickArmed || (first == 512 && second == 512)) return;
    s.stickArmed = false;
    lineLocked("read", true, "stick-adc %u %u", first, second);
}

// Guest thread, inside a run: the display scanned a new frame.
inline void guestFrame(const uint32_t* pixels, size_t count) {
    if (!on) return;
    std::lock_guard<std::mutex> lock(state().mutex);
    State& s = state();
    if (!s.frameArmed || count != s.baseline.size()) return;
    size_t i = 0;
    while (i < count && pixels[i] == s.baseline[i]) ++i;
    if (i == count) return;
    s.frameArmed = false;
    s.publishArmed = true;
    lineLocked("gframe", true, "pixel %zu", i);
}

// Guest thread: a frame handed to the SDL thread.
inline void published() {
    if (!on) return;
    std::lock_guard<std::mutex> lock(state().mutex);
    State& s = state();
    if (!s.publishArmed) return;
    s.publishArmed = false;
    s.presentArmed = true;
    lineLocked("publish", true, "frame");
}
// SDL thread: a frame drawn and presented.
inline void presented() {
    if (!on) return;
    std::lock_guard<std::mutex> lock(state().mutex);
    State& s = state();
    if (!s.presentArmed) return;
    s.presentArmed = false;
    lineLocked("present", false, "frame");
}

// Any thread: a free-form note, e.g. slice bounds.
inline void note(const char* stage, bool guestThread, const char* text) {
    if (!on) return;
    std::lock_guard<std::mutex> lock(state().mutex);
    lineLocked(stage, guestThread, "%s", text);
}

}  // namespace inputTrace

#endif
