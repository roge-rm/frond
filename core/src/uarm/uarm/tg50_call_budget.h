#ifndef FROND_TG50_CALL_BUDGET_H
#define FROND_TG50_CALL_BUDGET_H

#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace Tg50Pace {
class Budget {
 public:
    using Clock = std::chrono::steady_clock;
    explicit Budget(uint64_t guestTime, Clock::time_point now = Clock::now())
        : started(now), lastProgress(now), startedGuest(guestTime) {}

    void Check(uint64_t guestTime, Clock::time_point now = Clock::now()) const {
        constexpr uint64_t bytesPerSecond = 96 * 1024;
        const uint64_t extraSeconds = plannedBytes / bytesPerSecond + (plannedBytes % bytesPerSecond != 0);
        Require(now >= started && now - started < std::chrono::seconds(30 + extraSeconds),
                "TG50 transaction host deadline exceeded");
        Require(now >= lastProgress && now - lastProgress < std::chrono::seconds(30),
                "TG50 transaction made no verified progress for 30 seconds");
        Require(guestTime >= startedGuest && guestTime - startedGuest < 20000000000ull + plannedBytes * 2560ull,
                "TG50 transaction guest deadline exceeded");
        Require(cycles < CycleLimit(), "TG50 transaction cycle limit exceeded");
    }
    void BeginFileTransfer(uint32_t bytes, uint64_t guestTime, Clock::time_point now = Clock::now()) {
        Check(guestTime, now);
        Require(!fileDeclared && bytes <= 16 * 1024 * 1024, "TG50 invalid file-transfer byte plan");
        fileDeclared = true;
        plannedBytes = bytes;
    }
    void FileProgress(uint32_t bytes, uint64_t guestTime, Clock::time_point now = Clock::now()) {
        Check(guestTime, now);
        Require(fileDeclared && bytes && bytes <= plannedBytes - completedBytes, "TG50 invalid file-transfer progress");
        completedBytes += bytes;
        lastProgress = now;
    }
    void RequireFileComplete() const {
        Require(!fileDeclared || completedBytes == plannedBytes, "TG50 file transfer is incomplete");
    }
    void EnterCall(uint64_t guestTime) {
        Check(guestTime);
        Require(++calls <= 100000, "TG50 transaction call limit exceeded");
    }
    void EnterCallback(uint64_t guestTime) {
        Check(guestTime);
        Require(++callbacks <= 65536, "TG50 transaction callback limit exceeded");
    }
    void Charge(uint64_t completedCycles, uint64_t guestTime) {
        Require(completedCycles <= CycleLimit() - cycles,
                "TG50 transaction cycle limit exceeded");
        cycles += completedCycles;
        Check(guestTime);
    }
    uint64_t Slice(uint64_t guestTime) const {
        Check(guestTime);
        const uint64_t remaining = CycleLimit() - cycles;
        return remaining < 1000000 ? remaining : 1000000;
    }

 private:
    // Extra work is bounded by declared IO bytes; model accounting uses 10 ns per cycle.
    uint64_t CycleLimit() const { return 1000000000ull + plannedBytes * 256ull; }
    static void Require(bool ok, const char* why) {
        if (!ok) throw std::runtime_error(why);
    }
    Clock::time_point started, lastProgress;
    bool fileDeclared{false};
    uint64_t plannedBytes{0}, completedBytes{0};
    uint64_t startedGuest, cycles{0};
    uint32_t calls{0}, callbacks{0};
};
}
#endif
