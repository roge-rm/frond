#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Single owner/thread, one raw job. No session persistence or external-output replay.
class RawPrintJob {
 public:
    enum class State { idle, collecting, publishing, published, rejected, uncertain, aborted, overflow, invalid, allocationFailed };
    enum class Publication { rejected, published, uncertain };
    using Output = Publication (*)(void*, const uint8_t*, size_t);

    explicit RawPrintJob(size_t limit) : limit_(limit) {}
    RawPrintJob(const RawPrintJob&) = delete;
    RawPrintJob& operator=(const RawPrintJob&) = delete;
    State state() const { return state_; }
    size_t size() const { return bytes_.size(); }
    size_t limit() const { return limit_; }
    bool begin() {
        if (state_ == State::collecting || state_ == State::publishing) return false;
        bytes_.clear(); state_ = State::collecting; return true;
    }
    bool append(const uint8_t* data, size_t count) {
        if (state_ != State::collecting) return false;
        if (count > limit_ - bytes_.size()) return fail(State::overflow);
        if (count && !data) return fail(State::invalid);
        if (!count) return true;
        try { bytes_.insert(bytes_.end(), data, data + count); }
        catch (...) { return fail(State::allocationFailed); }
        return true;
    }
    bool abort() {
        if (state_ != State::collecting) return false;
        return fail(State::aborted), true;
    }
    State finish(Output output, void* context) {
        if (state_ != State::collecting) return state_;
        if (!output) { fail(State::rejected); return state_; }
        state_ = State::publishing;
        Publication result = Publication::uncertain;
        try { result = output(context, bytes_.data(), bytes_.size()); }
        catch (...) {} // Output may already have escaped before the exception.
        state_ = result == Publication::published ? State::published :
                 result == Publication::rejected ? State::rejected : State::uncertain;
        bytes_.clear();
        return state_;
    }
 private:
    bool fail(State reason) { bytes_.clear(); state_ = reason; return false; }
    const size_t limit_;
    std::vector<uint8_t> bytes_;
    State state_ = State::idle;
};
