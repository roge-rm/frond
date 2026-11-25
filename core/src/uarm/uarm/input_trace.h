#ifndef FROND_INPUT_TRACE_H
#define FROND_INPUT_TRACE_H

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#ifdef __ANDROID__
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#endif

// Input latency trace, host side only. FROND_INPUT_TRACE=<file> (or "stderr") writes one line per stage:
//   IT <stage> <host us, steady clock> <guest us or -> <fields>
// Stages: host (the event reaches the SDL filter), take (the guest thread hands it to the SoC), apply (the
// machine's pin, pen or stick changes), read (the first guest read that sees it), gframe (the first guest
// frame whose pixels differ from the frame at apply), publish (that frame handed to the SDL thread),
// present (SDL_RenderPresent returned with it), adc (every stick sample the guest takes), change (every
// guest frame that differs from the one before, with the pixels changed). On Android also display (when a
// presented frame was queued, latched and shown, from EGL_ANDROID_get_frame_timestamps, in host us). One press, or the stick's return
// to rest, is followed at a time; a new apply starts over.
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
    bool penArmed = false, stickArmed = false, stickRest = false;
    uint32_t stickWant[2] = {0, 0};
    bool frameArmed = false, publishArmed = false, presentArmed = false;
    std::vector<uint32_t> baseline, previous;  // previous: the last guest frame, for change lines
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

// Forgets the clock if it is the one registered for this context.
inline void clearGuestClock(void* context) {
    if (!on) return;
    std::lock_guard<std::mutex> lock(state().mutex);
    if (state().guestContext != context) return;
    state().guestTicks = nullptr;
    state().guestContext = nullptr;
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
// The first step away from rest starts a follow, and so does the return to rest; other steps only log.
inline void applyStick(int x, int y, bool fromRest, const uint32_t* frame, size_t pixels) {
    if (!on) return;
    std::lock_guard<std::mutex> lock(state().mutex);
    State& s = state();
    lineLocked("apply", true, "stick %d %d", x, y);
    const bool toRest = x == 0 && y == 0;
    if (fromRest == toRest) return;
    s.stickArmed = true;
    s.stickRest = toRest;
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
    lineLocked("adc", true, "%u %u", first, second);
    if (!s.stickArmed || (first == 512 && second == 512) != s.stickRest) return;
    s.stickArmed = false;
    lineLocked("read", true, "stick-adc %u %u", first, second);
}

// Guest thread, inside a run: the display scanned a new frame.
inline void guestFrame(const uint32_t* pixels, size_t count) {
    if (!on) return;
    std::lock_guard<std::mutex> lock(state().mutex);
    State& s = state();
    if (s.previous.size() != count || !std::equal(pixels, pixels + count, s.previous.begin())) {
        size_t changed = 0;
        if (s.previous.size() == count) for (size_t j = 0; j < count; ++j) changed += pixels[j] != s.previous[j];
        s.previous.assign(pixels, pixels + count);
        lineLocked("change", true, "%zu", changed);
    }
    if (!s.frameArmed || count != s.baseline.size()) return;
    size_t i = 0;
    while (i < count && pixels[i] == s.baseline[i]) ++i;
    if (i == count) return;
    s.frameArmed = false;
    s.publishArmed = true;
    lineLocked("gframe", true, "pixel %llu", static_cast<unsigned long long>(i));
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
#ifdef __ANDROID__
// SDL thread, with SDL's EGL context current: the EGL frame timestamp calls, looked up once, and the
// followed frames whose display time isn't known yet.
struct FrameTimes {
    bool tried = false;
    EGLDisplay (*display)() = nullptr;
    EGLSurface (*surface)(EGLint) = nullptr;
    EGLBoolean (*attrib)(EGLDisplay, EGLSurface, EGLint, EGLint) = nullptr;
    PFNEGLGETNEXTFRAMEIDANDROIDPROC nextId = nullptr;
    PFNEGLGETFRAMETIMESTAMPSANDROIDPROC times = nullptr;
    EGLSurface timed = EGL_NO_SURFACE;  // the surface timestamps were turned on for
    std::vector<std::pair<EGLuint64KHR, uint64_t>> pending;  // frame id, host us when presented
};
inline FrameTimes& frameTimes() {
    static FrameTimes f;
    return f;
}
// Caller holds the mutex. followed: the frame just presented is a followed one.
inline void displayTimesLocked(bool followed) {
    FrameTimes& f = frameTimes();
    if (!f.tried) {
        f.tried = true;
        if (void* egl = dlopen("libEGL.so", RTLD_NOW | RTLD_NOLOAD)) {
            f.display = reinterpret_cast<EGLDisplay (*)()>(dlsym(egl, "eglGetCurrentDisplay"));
            f.surface = reinterpret_cast<EGLSurface (*)(EGLint)>(dlsym(egl, "eglGetCurrentSurface"));
            f.attrib = reinterpret_cast<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLint, EGLint)>(dlsym(egl, "eglSurfaceAttrib"));
            auto proc = reinterpret_cast<void* (*)(const char*)>(dlsym(egl, "eglGetProcAddress"));
            if (proc) {
                f.nextId = reinterpret_cast<PFNEGLGETNEXTFRAMEIDANDROIDPROC>(proc("eglGetNextFrameIdANDROID"));
                f.times = reinterpret_cast<PFNEGLGETFRAMETIMESTAMPSANDROIDPROC>(proc("eglGetFrameTimestampsANDROID"));
            }
        }
    }
    if (!f.display || !f.surface || !f.attrib || !f.nextId || !f.times) return;
    EGLDisplay display = f.display();
    EGLSurface surface = f.surface(EGL_DRAW);
    if (surface == EGL_NO_SURFACE) return;
    if (surface != f.timed) {
        // A new surface: timestamps start with its next frame.
        f.pending.clear();
        f.timed = f.attrib(display, surface, EGL_TIMESTAMPS_ANDROID, EGL_TRUE) ? surface : EGL_NO_SURFACE;
        return;
    }
    EGLuint64KHR next = 0;
    if (followed && f.nextId(display, surface, &next) && next > 0) f.pending.emplace_back(next - 1, hostUsec());
    static const EGLint names[] = {EGL_REQUESTED_PRESENT_TIME_ANDROID, EGL_RENDERING_COMPLETE_TIME_ANDROID,
                                   EGL_COMPOSITION_LATCH_TIME_ANDROID, EGL_DISPLAY_PRESENT_TIME_ANDROID};
    for (size_t i = 0; i < f.pending.size();) {
        EGLnsecsANDROID values[4] = {};
        const bool got = f.times(display, surface, f.pending[i].first, 4, names, values);
        const bool waiting = got && std::any_of(values, values + 4, [](EGLnsecsANDROID v) { return v == EGL_TIMESTAMP_PENDING_ANDROID; });
        if (waiting && hostUsec() - f.pending[i].second < 2000000) { ++i; continue; }
        lineLocked("display", false, "frame %llu presented=%llu queued=%lld done=%lld latch=%lld shown=%lld",
                   (unsigned long long)f.pending[i].first, (unsigned long long)f.pending[i].second,
                   got ? (long long)(values[0] / 1000) : -1ll, got ? (long long)(values[1] / 1000) : -1ll,
                   got ? (long long)(values[2] / 1000) : -1ll, got ? (long long)(values[3] / 1000) : -1ll);
        f.pending.erase(f.pending.begin() + long(i));
    }
}
#endif

// SDL thread: a frame drawn and presented.
inline void presented() {
    if (!on) return;
    std::lock_guard<std::mutex> lock(state().mutex);
    State& s = state();
    const bool followed = s.presentArmed;
    if (followed) {
        s.presentArmed = false;
        lineLocked("present", false, "frame");
    }
#ifdef __ANDROID__
    displayTimesLocked(followed);
#endif
}

// Any thread: a free-form note, e.g. slice bounds.
inline void note(const char* stage, bool guestThread, const char* text) {
    if (!on) return;
    std::lock_guard<std::mutex> lock(state().mutex);
    lineLocked(stage, guestThread, "%s", text);
}

}  // namespace inputTrace

#endif
