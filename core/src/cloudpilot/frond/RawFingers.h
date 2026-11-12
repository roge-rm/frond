#pragma once

#include <SDL.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>

// SDL's renderer moves each finger point into the viewport set at the moment the touch comes
// in, which on Android is any time, mid-frame included. This filter runs before that and keeps
// the window points, in order, for PollWindow to take back.
class RawFingers {
   public:
    static void Install() { SDL_SetEventFilter(Filter, nullptr); }

    // The window point SDL had for this finger event, or the event's own if it wasn't seen.
    static void Take(const SDL_TouchFingerEvent& event, float& x, float& y) {
        x = event.x;
        y = event.y;
        std::lock_guard<std::mutex> lock(guard);
        for (auto it = points.begin(); it != points.end(); ++it) {
            if (it->type != event.type || it->time != event.timestamp || it->touch != event.touchId ||
                it->finger != event.fingerId)
                continue;
            x = it->x;
            y = it->y;
            // Earlier points for the same finger were for events SDL dropped.
            const auto touch = it->touch;
            const auto finger = it->finger;
            points.erase(std::remove_if(points.begin(), it + 1,
                                        [&](const Point& p) { return p.touch == touch && p.finger == finger; }),
                         it + 1);
            return;
        }
    }

   private:
    struct Point {
        uint32_t type, time;
        SDL_TouchID touch;
        SDL_FingerID finger;
        float x, y;
    };

    static int SDLCALL Filter(void*, SDL_Event* event) {
        if (event->type == SDL_FINGERDOWN || event->type == SDL_FINGERMOTION) {
            std::lock_guard<std::mutex> lock(guard);
            if (points.size() >= 256) points.pop_front();
            const auto& f = event->tfinger;
            points.push_back({f.type, f.timestamp, f.touchId, f.fingerId, f.x, f.y});
        }
        return 1;
    }

    static inline std::mutex guard;
    static inline std::deque<Point> points;
};
