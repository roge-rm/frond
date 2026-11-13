#ifndef FROND_INPUT_REPLAY_H
#define FROND_INPUT_REPLAY_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "keys.h"

class SoC;
class SdlRenderer;

class InputReplay {
   public:
    bool Load(const std::string& path, int width, int height, std::string& error);
    // Delivers due events. Snapshot failures are recorded in SnapshotResults, not returned.
    void Dispatch(uint64_t elapsedMs, SoC& soc, SdlRenderer& renderer);
    // Same, with snapshots saved by save(path, blank), which works like
    // SdlRenderer::SaveFramebuffer.
    void Dispatch(uint64_t elapsedMs, SoC& soc, const std::function<bool(const char*, bool&)>& save);
    uint64_t LastEventMs() const { return events.empty() ? 0 : events.back().at; }
    bool Finished() const { return next == events.size() && !paused; }
    // A pause event holds the main loop's guest time from the slice it lands in until Continue,
    // so a host command (a save) lands within a slice of the pause's time. Host commands that
    // run the guest themselves (guest-ready, install, card commands) still run it.
    bool Paused() const { return paused; }
    // Ends a pause and returns the replay time it was at, in ms, for the caller to restart the
    // replay clock from.
    uint64_t Continue() { paused = false; return pausedAtMs; }
    // One entry per snapshot taken so far, in order: "ok", "failed", or "lcd-off" or
    // "no-frame" for a black frame saved with the LCD off or before its first frame.
    const std::vector<std::string>& SnapshotResults() const { return snapshots; }

   private:
    enum class Kind { penDown, penUp, keyDown, keyUp, keyboardDown, keyboardUp, keyboardAttach, snapshot, stick, pause };
    struct Event {
        uint64_t at;
        Kind kind;
        int x{0}, y{0};
        KeyId key{keyInvalid};
        std::string path;
    };
    std::vector<Event> events;
    std::vector<std::string> snapshots;
    size_t next{0};
    bool paused{false};
    uint64_t pausedAtMs{0};
};

#endif
