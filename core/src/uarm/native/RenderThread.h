#ifndef FROND_RENDER_THREAD_H
#define FROND_RENDER_THREAD_H

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "DeviceControls.h"
#include "Rotation.h"
#include "WindowLayout.h"

// Decides whether a draw is due, the same way for the one-thread renderer and the
// guest thread: a new frame, a forced redraw, a change of LCD power, pressed
// buttons or the stick, or one of the few presents after the last change.
class DrawGate {
   public:
    bool Pass(bool frame, bool forceRedraw, DeviceKeyMask buttons, bool lcdEnabled, DeviceStick stick = {}) {
        forceRedraw = forceRedraw || pressedButtons != buttons || shownStick != stick;
        pressedButtons = buttons;
        shownStick = stick;
        const bool wasLcdEnabled = lcd;
        lcd = lcdEnabled;
        if (!frame && !forceRedraw && lcd == wasLcdEnabled && flushPipelineCounter++ >= FLUSH_PIPELINE_FRAMES)
            return false;
        if (frame || forceRedraw || lcd != wasLcdEnabled) flushPipelineCounter = 0;
        return true;
    }

   private:
    static constexpr uint32_t FLUSH_PIPELINE_FRAMES = 3;
    DeviceKeyMask pressedButtons{0};
    DeviceStick shownStick{};
    bool lcd{true};
    uint32_t flushPipelineCounter{0};
};

// What the render thread draws next. Draws the guest thread published since the last
// take are merged: the newest pixels, LCD state and buttons, and every row any of them
// changed.
struct DrawState {
    const uint32_t* pixels{nullptr};  // whole LCD frame, or null when no new frame came
    uint32_t firstDirtyLine{0}, lastDirtyLine{0};
    bool lcdEnabled{true};
    DeviceKeyMask buttons{0};
    DeviceStick stick{};
    bool redraw{false};  // the guest asked for a redraw, e.g. after an expose or a new layout
};

// What the SDL thread has to act on, gathered from SDL events by RenderLink's filter.
struct RenderNotices {
    bool resized{false};     // the window changed size
    bool redraw{false};      // the window needs drawing again
    bool redrawAll{false};   // the textures were lost: upload the whole frame again
    bool lost{false};        // the renderer was lost: make a new one
    bool pump{false};        // a window or app event came: run SDL's event pump soon
    bool foreground{false};  // the app came back from the background

    RenderNotices& operator|=(const RenderNotices& other) {
        resized = resized || other.resized;
        redraw = redraw || other.redraw;
        redrawAll = redrawAll || other.redrawAll;
        lost = lost || other.lost;
        pump = pump || other.pump;
        foreground = foreground || other.foreground;
        return *this;
    }
};

// Hands the guest thread's work over to the SDL thread and back. The SDL thread, the one
// that made the window, owns the window, renderer, SDL events and audio device calls. The
// guest thread owns the SoC and everything that touches it.
//
// LCD frames go through three buffers: the guest thread copies into its own back buffer,
// then swaps it with the middle one under the lock; the SDL thread swaps the middle one
// with its front buffer when it draws. Neither side waits for the other and the newest
// frame wins.
class RenderLink {
   public:
    struct Input {
        bool layout{false};  // a new window layout instead of an event
        SDL_Event event{};
        WindowLayout windowLayout{};
        std::optional<Rotation> rotation;  // the rotation this layout is for, when it changed
        // Runs on the guest thread before the layout applies, e.g. to change the input view.
        std::function<void()> before;
    };

    RenderLink(int width, int height) : width(width), height(height), renderThread(std::this_thread::get_id()) {
        for (auto& buffer : buffers) buffer.assign(size_t(width) * height, 0);
        // Each buffer takes the whole frame the first time it is filled.
        for (auto& stale : staleLines) stale = {0, uint32_t(height - 1)};
    }

    std::function<void(const SDL_Event&)> beforeEvent;

    // SDL thread: a new layout, queued behind the events so far.
    void PushLayout(const WindowLayout& layout, std::optional<Rotation> rotation = std::nullopt,
                    std::function<void()> before = {}) {
        std::lock_guard<std::mutex> lock(mutex);
        inputs.push_back(Input{true, {}, layout, rotation, std::move(before)});
    }

    // SDL thread: installs Filter and hands SDL's queue over to the guest thread, with no
    // event lost or doubled. SDL_SetEventFilter empties SDL's queue and the Android UI
    // thread can push at any time, so a watch first holds each new event and marks SDL's
    // copy of it, the older queued events go over, then the filter goes in and the held
    // events follow. SDL checks for watches before it takes its lock, so a push already
    // under way can miss the watch; the 1 ms wait lets it reach the queue first.
    void InstallFilter() {
        SDL_AddEventWatch(Hold, this);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        SDL_FilterEvents(Queued, this);
        SDL_SetEventFilter(Filter, this);
        SDL_DelEventWatch(Hold, this);
        std::lock_guard<std::mutex> lock(mutex);
        ReleaseHeld();
    }
    // SDL thread: hands over anything SDL queued without the filter and empties its queue.
    void DrainQueue() { SDL_FilterEvents(Queued, this); }

    // SDL event filter, installed by InstallFilter. SDL runs it on the thread that pushes
    // the event: Android's UI thread for touches, the SDL thread inside its event pump
    // otherwise. Every event goes straight to the guest thread's queue and is dropped from
    // SDL's own. Input doesn't wake the SDL thread; window, app and render events do, so
    // it can lay out, redraw or pump.
    static int SDLCALL Filter(void* userdata, SDL_Event* event) {
        auto& link = *static_cast<RenderLink*>(userdata);
        if (link.beforeEvent) link.beforeEvent(*event);
        std::lock_guard<std::mutex> lock(link.mutex);
        link.ReleaseHeld();
        link.Take(*event);
        return 0;
    }
    // SDL thread: what the filter noticed since the last call, and whether the app is in
    // the background.
    RenderNotices TakeNotices(bool& inBackground) {
        std::lock_guard<std::mutex> lock(mutex);
        RenderNotices taken = notices;
        notices = {};
        inBackground = background;
        return taken;
    }

    // Guest thread: everything pushed since the last call.
    void TakeInputs(std::vector<Input>& out) {
        out.clear();
        std::lock_guard<std::mutex> lock(mutex);
        out.swap(inputs);
    }

    // Guest thread: one draw. frame is the SoC's whole LCD frame or null.
    void Publish(const uint32_t* frame, uint32_t firstDirtyLine, uint32_t lastDirtyLine, bool lcdEnabled,
                 DeviceKeyMask buttons, bool redraw, DeviceStick stick = {}) {
        if (frame) {
            lastDirtyLine = std::min<uint32_t>(lastDirtyLine, height - 1);
            // The back buffer last held the frame from a few draws ago: bring the rows
            // changed since then up to date as well.
            auto& stale = staleLines[back];
            const uint32_t first = std::min(firstDirtyLine, stale.first);
            const uint32_t last = stale.first <= stale.last ? std::max(lastDirtyLine, stale.last) : lastDirtyLine;
            if (first <= last)
                memcpy(buffers[back].data() + size_t(first) * width, frame + size_t(first) * width,
                       size_t(last - first + 1) * width * 4);
            stale = {UINT32_MAX, 0};
            for (int i = 0; i < 3; ++i) {
                if (i == back) continue;
                staleLines[i].first = std::min(staleLines[i].first, firstDirtyLine);
                staleLines[i].last = std::max(staleLines[i].last, lastDirtyLine);
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (frame) {
                std::swap(back, middle);
                if (!pendingFrame) {
                    pending.firstDirtyLine = firstDirtyLine;
                    pending.lastDirtyLine = lastDirtyLine;
                } else {
                    pending.firstDirtyLine = std::min(pending.firstDirtyLine, firstDirtyLine);
                    pending.lastDirtyLine = std::max(pending.lastDirtyLine, lastDirtyLine);
                }
                pendingFrame = true;
            }
            pending.lcdEnabled = lcdEnabled;
            pending.buttons = buttons;
            pending.stick = stick;
            pending.redraw = pending.redraw || redraw;
            pendingDraw = true;
            woken = true;
        }
        wake.notify_one();
    }

    // SDL thread: the merged draw since the last take, if any. The pixels stay valid
    // until the next take.
    bool TakeDraw(DrawState& out) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!pendingDraw) return false;
        out = pending;
        out.pixels = nullptr;
        pending.redraw = false;
        if (pendingFrame) {
            std::swap(front, middle);
            out.pixels = buffers[front].data();
            hasFront = true;
        }
        pendingDraw = pendingFrame = false;
        return true;
    }
    // SDL thread: the last frame it took, for a redraw after the renderer changes.
    const uint32_t* FrontFrame() const { return hasFront ? buffers[front].data() : nullptr; }

    // Guest thread: requests for the SDL thread.
    void RequestRotation(Rotation rotation) {
        std::lock_guard<std::mutex> lock(mutex);
        rotationRequest = rotation;
        woken = true;
        wake.notify_one();
    }
    // Any thread: wakes the SDL thread for a request kept outside the link.
    void Wake() {
        std::lock_guard<std::mutex> lock(mutex);
        woken = true;
        wake.notify_one();
    }
    // Doesn't wake the SDL thread; it sets the title on its next wake.
    void SetTitle(const std::string& title) {
        std::lock_guard<std::mutex> lock(mutex);
        titleRequest = title;
    }
    std::optional<Rotation> TakeRotation() {
        std::lock_guard<std::mutex> lock(mutex);
        auto rotation = rotationRequest;
        rotationRequest.reset();
        return rotation;
    }
    std::optional<std::string> TakeTitle() {
        std::lock_guard<std::mutex> lock(mutex);
        auto title = std::move(titleRequest);
        titleRequest.reset();
        return title;
    }

    // Runs work on the SDL thread and waits for it; runs it at once when called there.
    // Used for replay snapshots and audio device calls, which belong to the SDL thread.
    void Call(const std::function<void()>& work) {
        if (std::this_thread::get_id() == renderThread) {
            work();
            return;
        }
        std::unique_lock<std::mutex> lock(mutex);
        call = &work;
        callDone = false;
        woken = true;
        wake.notify_one();
        done.wait(lock, [&] { return callDone || stopped; });
        call = nullptr;
    }
    // SDL thread: runs a waiting call, if any.
    void RunCall() {
        const std::function<void()>* work;
        {
            std::lock_guard<std::mutex> lock(mutex);
            work = callDone ? nullptr : call;
        }
        if (!work) return;
        (*work)();
        {
            std::lock_guard<std::mutex> lock(mutex);
            callDone = true;
        }
        done.notify_all();
    }

    // SDL thread: waits until the guest thread or the filter has something, or the time
    // is up. Returns false when the time ran out.
    bool Wait(int ms) {
        std::unique_lock<std::mutex> lock(mutex);
        const bool wasWoken = wake.wait_for(lock, std::chrono::milliseconds(ms), [&] { return woken; });
        woken = false;
        return wasWoken;
    }

    // Guest thread: it has left its loop. SDL thread: it stops; the guest thread leaves
    // its loop and any waiting call returns.
    void Finish() {
        std::lock_guard<std::mutex> lock(mutex);
        finished = true;
        woken = true;
        wake.notify_one();
    }
    bool Finished() {
        std::lock_guard<std::mutex> lock(mutex);
        return finished;
    }
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopped = true;
        }
        done.notify_all();
    }
    bool Stopped() {
        std::lock_guard<std::mutex> lock(mutex);
        return stopped;
    }

   private:
    struct Lines { uint32_t first, last; };

    // Marks SDL's copy of an event the watch already holds.
    static constexpr uint32_t heldStamp = 0xffffffff;

    // Watch used while InstallFilter runs: holds each new event, in order.
    static int SDLCALL Hold(void* userdata, SDL_Event* event) {
        auto& link = *static_cast<RenderLink*>(userdata);
        std::lock_guard<std::mutex> lock(link.mutex);
        link.held.push_back(*event);
        event->common.timestamp = heldStamp;
        return 1;
    }
    // For SDL_FilterEvents: takes each queued event the watch doesn't hold and removes all.
    static int SDLCALL Queued(void* userdata, SDL_Event* event) {
        auto& link = *static_cast<RenderLink*>(userdata);
        if (event->common.timestamp == heldStamp) return 0;
        std::lock_guard<std::mutex> lock(link.mutex);
        link.Take(*event);
        return 0;
    }
    // With the mutex held: the events the watch held, behind the ones taken so far.
    void ReleaseHeld() {
        for (const auto& event : held) Take(event);
        held.clear();
    }
    // With the mutex held: queues an event for the guest thread and notes what the SDL
    // thread has to act on.
    void Take(const SDL_Event& event) {
        inputs.push_back(Input{false, event, {}, std::nullopt, {}});
        auto& n = notices;
        switch (event.type) {
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) n.resized = true;
                if (event.window.event == SDL_WINDOWEVENT_EXPOSED) n.redraw = true;
                break;
            case SDL_APP_DIDENTERBACKGROUND:
                background = true;
                break;
            case SDL_APP_DIDENTERFOREGROUND:
                background = false;
                n.foreground = true;
                break;
            case SDL_RENDER_TARGETS_RESET:
                n.redrawAll = true;
                break;
            case SDL_RENDER_DEVICE_RESET:
                n.redrawAll = true;
                n.lost = true;
                break;
            case SDL_QUIT:
            case SDL_APP_TERMINATING:
            case SDL_APP_LOWMEMORY:
            case SDL_APP_WILLENTERBACKGROUND:
            case SDL_APP_WILLENTERFOREGROUND:
                break;
            default:
                return;
        }
        n.pump = true;
        woken = true;
        wake.notify_one();
    }

    const int width, height;
    const std::thread::id renderThread;

    std::mutex mutex;
    std::condition_variable wake, done;
    bool woken{false};

    // Frames. back and staleLines belong to the guest thread; middle, front and pending
    // are shared under the mutex.
    std::vector<uint32_t> buffers[3];
    int back{0}, middle{1}, front{2};
    bool hasFront{false};
    Lines staleLines[3];
    DrawState pending;
    bool pendingDraw{false}, pendingFrame{false};

    std::vector<Input> inputs;
    std::vector<SDL_Event> held;  // events the watch held while InstallFilter ran
    RenderNotices notices;
    bool background{false};
    std::optional<Rotation> rotationRequest;
    std::optional<std::string> titleRequest;
    const std::function<void()>* call{nullptr};
    bool callDone{true};
    bool finished{false}, stopped{false};
};

#endif
