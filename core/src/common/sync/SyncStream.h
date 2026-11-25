#ifndef FROND_SYNC_STREAM_H
#define FROND_SYNC_STREAM_H

// The byte stream between the sync engine and a handheld's HotSync port. The engine runs on
// its own thread and blocks on it; the transport side (a UART queue, the USB bridge, a
// slirp socket, a file descriptor) never blocks.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

namespace frondSync {

class SyncStream {
   public:
    virtual ~SyncStream() = default;
    // Waits up to timeoutMs for a byte. Returns the count read, 0 on timeout, -1 once closed.
    virtual long Read(uint8_t* out, size_t max, int timeoutMs) = 0;
    // Queues all of data, waiting for room. False once closed.
    virtual bool Write(const uint8_t* data, size_t size) = 0;
    // USB transports can send the NetSync header in a separate bulk packet.
    virtual bool WriteWithHeader(const uint8_t* data, size_t size, size_t) { return Write(data, size); }
    // Drops bytes already received, as after a line error.
    virtual void Flush() {}
    // Ends every wait: Read returns -1 and Write false from now on. Any thread.
    virtual void Abort() = 0;
};

// Two queues with a condition variable, for an in-process peer: one side is the engine's
// SyncStream, the other is polled by the transport without blocking. Thread-safe.
class SyncPipe {
   public:
    static constexpr size_t queueBytes = 16384;

    // Transport side, never blocks.
    size_t HostToDevice(uint8_t* out, size_t max) {
        std::lock_guard lock(mutex);
        size_t n = 0;
        while (n < max && !toDevice.empty()) out[n++] = toDevice.front(), toDevice.pop_front();
        if (n) changed.notify_all();
        return n;
    }
    size_t PendingToDevice() const {
        std::lock_guard lock(mutex);
        return toDevice.size();
    }
    size_t DeviceToHost(const uint8_t* data, size_t size) {
        std::lock_guard lock(mutex);
        if (closed || inputFinished) return size;
        const size_t n = std::min(size, queueBytes - std::min(queueBytes, fromDevice.size()));
        fromDevice.insert(fromDevice.end(), data, data + n);
        if (n) changed.notify_all();
        return n;
    }
    size_t RoomFromDevice() const {
        std::lock_guard lock(mutex);
        return queueBytes - std::min(queueBytes, fromDevice.size());
    }
    // Drops both queues, as when the cable comes out.
    void Clear() {
        std::lock_guard lock(mutex);
        toDevice.clear();
        fromDevice.clear();
        ++generation;
        changed.notify_all();
    }
    void Close() {
        std::lock_guard lock(mutex);
        closed = true;
        changed.notify_all();
    }
    void FinishInput() {
        std::lock_guard lock(mutex);
        inputFinished = true;
        changed.notify_all();
    }
    bool Closed() const {
        std::lock_guard lock(mutex);
        return closed;
    }
    // Closed, or the device side finished sending.
    bool Ended() const {
        std::lock_guard lock(mutex);
        return closed || inputFinished;
    }

    // Engine side.
    class End final : public SyncStream {
       public:
        explicit End(std::shared_ptr<SyncPipe> pipe) : pipe(std::move(pipe)) {}
        long Read(uint8_t* out, size_t max, int timeoutMs) override {
            auto& p = *pipe;
            std::unique_lock lock(p.mutex);
            p.changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] { return p.closed || p.inputFinished || !p.fromDevice.empty(); });
            if (p.fromDevice.empty()) return p.closed || p.inputFinished ? -1 : 0;
            size_t n = 0;
            while (n < max && !p.fromDevice.empty()) out[n++] = p.fromDevice.front(), p.fromDevice.pop_front();
            return long(n);
        }
        bool Write(const uint8_t* data, size_t size) override {
            auto& p = *pipe;
            std::unique_lock lock(p.mutex);
            while (size) {
                p.changed.wait(lock, [&] { return p.closed || p.toDevice.size() < queueBytes; });
                if (p.closed) return false;
                const size_t n = std::min(size, queueBytes - p.toDevice.size());
                p.toDevice.insert(p.toDevice.end(), data, data + n);
                data += n;
                size -= n;
            }
            return true;
        }
        void Flush() override {
            std::lock_guard lock(pipe->mutex);
            pipe->fromDevice.clear();
        }
        void Abort() override { pipe->Close(); }

       private:
        std::shared_ptr<SyncPipe> pipe;
    };

   private:
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<uint8_t> toDevice, fromDevice;
    uint64_t generation{0};
    bool closed{false}, inputFinished{false};
};

}  // namespace frondSync

#endif
