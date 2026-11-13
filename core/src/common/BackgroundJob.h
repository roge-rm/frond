#ifndef FROND_BACKGROUND_JOB_H
#define FROND_BACKGROUND_JOB_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <thread>

// One job at a time on a thread of its own, started, polled and collected by one owner thread (the
// emulation thread for a session save). The job owns what it reads; the owner touches none of it
// until Finished() or Wait() says the job is done.
class BackgroundJob {
   public:
    BackgroundJob() = default;
    BackgroundJob(const BackgroundJob&) = delete;
    BackgroundJob& operator=(const BackgroundJob&) = delete;
    ~BackgroundJob() { Wait(); }

    // Waits for any job still running first.
    void Start(std::function<void()> work) {
        Wait();
        done.store(false, std::memory_order_relaxed);
        thread = std::thread([this, work = std::move(work)] {
            work();
            done.store(true, std::memory_order_release);
        });
    }
    // A job is started and not yet collected by Wait().
    bool Busy() const { return thread.joinable(); }
    // The job has returned; Wait() won't block.
    bool Finished() const { return Busy() && done.load(std::memory_order_acquire); }
    void Wait() {
        if (thread.joinable()) thread.join();
    }

   private:
    std::thread thread;
    std::atomic<bool> done{false};
};

// Save timing lines on stderr, with FROND_SAVE_TIMING set.
inline bool saveTimingWanted() {
    static const bool wanted = std::getenv("FROND_SAVE_TIMING") != nullptr;
    return wanted;
}

inline double saveTimingMs(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
}

#endif
