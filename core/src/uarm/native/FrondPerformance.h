#pragma once

// Asks Android to schedule the calling thread for steady work. API 33 and later get an ADPF
// hint session; older systems get a higher thread priority where the app may set one.
// Other platforms do nothing. Create it on the thread it is for.

#include <cstdint>
#include <string>

#ifdef __ANDROID__
#include <dlfcn.h>
#include <sys/resource.h>
#include <unistd.h>

#include <android/api-level.h>

#include <chrono>
#endif

class FrondPerformance {
public:
#ifdef __ANDROID__
    // targetUsec is the length of one slice of work.
    explicit FrondPerformance(uint64_t targetUsec) {
        if (android_get_device_api_level() >= 33 && StartSession(targetUsec)) {
            description_ = "performance hint session, target " + std::to_string(targetUsec) + " us";
            return;
        }
        // An app may lower its own threads' nice value; try the strongest level first.
        const pid_t tid = gettid();
        for (int level : {-8, -4, -2}) {
            if (setpriority(PRIO_PROCESS, tid, level) == 0) {
                description_ = "thread priority " + std::to_string(level);
                return;
            }
        }
        description_ = "none";
    }

    ~FrondPerformance() {
        if (session_ && close_) close_(session_);
    }

    FrondPerformance(const FrondPerformance&) = delete;
    FrondPerformance& operator=(const FrondPerformance&) = delete;

    // Call when a slice of work starts, and ReportWork when it ends.
    void BeginWork() { begin_ = std::chrono::steady_clock::now(); }

    // Adds the time since BeginWork to the work so far, and reports it once a full slice of wall
    // time has passed since the first pass, so passes cut short by input don't each count as a frame.
    // A changed slice length becomes the new target.
    void ReportWork(uint64_t targetUsec) {
        if (!session_) return;
        if (targetUsec != targetUsec_) {
            targetUsec_ = targetUsec;
            update_(session_, int64_t(targetUsec) * 1000);
        }
        const auto now = std::chrono::steady_clock::now();
        if (!windowOpen_) { windowOpen_ = true; windowStart_ = begin_; }
        spent_ += now - begin_;
        if (now - windowStart_ < std::chrono::microseconds(targetUsec)) return;
        report_(session_, std::chrono::duration_cast<std::chrono::nanoseconds>(spent_).count());
        windowOpen_ = false;
        spent_ = {};
    }
#else
    explicit FrondPerformance(uint64_t) {}
    void BeginWork() {}
    void ReportWork(uint64_t) {}
#endif

    // One line saying what took effect.
    const std::string& Description() const { return description_; }

private:
#ifdef __ANDROID__
    using GetManager = void* (*)();
    using CreateSession = void* (*)(void*, const int32_t*, size_t, int64_t);
    using UpdateTarget = int (*)(void*, int64_t);
    using ReportActual = int (*)(void*, int64_t);
    using CloseSession = void (*)(void*);

    bool StartSession(uint64_t targetUsec) {
        void* lib = dlopen("libandroid.so", RTLD_NOW);
        if (!lib) return false;
        auto getManager = reinterpret_cast<GetManager>(dlsym(lib, "APerformanceHint_getManager"));
        auto create = reinterpret_cast<CreateSession>(dlsym(lib, "APerformanceHint_createSession"));
        update_ = reinterpret_cast<UpdateTarget>(dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
        report_ = reinterpret_cast<ReportActual>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
        close_ = reinterpret_cast<CloseSession>(dlsym(lib, "APerformanceHint_closeSession"));
        if (!getManager || !create || !update_ || !report_ || !close_) return false;
        void* manager = getManager();
        if (!manager) return false;
        const int32_t tid = gettid();
        session_ = create(manager, &tid, 1, int64_t(targetUsec) * 1000);
        targetUsec_ = targetUsec;
        return session_ != nullptr;
    }

    void* session_ = nullptr;
    UpdateTarget update_ = nullptr;
    ReportActual report_ = nullptr;
    CloseSession close_ = nullptr;
    uint64_t targetUsec_ = 0;
    std::chrono::steady_clock::time_point begin_;
    std::chrono::steady_clock::time_point windowStart_;
    std::chrono::steady_clock::duration spent_{};
    bool windowOpen_ = false;
#endif
    std::string description_ = "none";
};
