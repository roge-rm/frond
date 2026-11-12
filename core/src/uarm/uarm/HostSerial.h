#ifndef FROND_HOST_SERIAL_H
#define FROND_HOST_SERIAL_H
#ifndef __EMSCRIPTEN__
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "SerialHostRoute.h"
struct PxaUart;

// Connects the TX FFUART to a host PTY, a TCP listener on 127.0.0.1 or a Unix socket listener. A worker
// thread owns the descriptors and moves bytes through two bounded queues; it never
// touches the UART. Every method runs on the emulator thread. Nothing here is
// session state: a load or reset detaches, and Service attaches again once the UART
// is idle.
class HostSerial {
   public:
    enum class Kind { off, pty, tcp, unixSocket };
    struct Config {
        Kind kind{Kind::off};
        uint16_t port{0};  // TCP only; 0 picks a free port
        std::string path;   // Unix socket only: absolute, in an existing 0700 folder you own
    };
    struct Counters {
        uint64_t received{}, sent{}, rxDiscarded{}, txDropped{};
    };
    // Each queue holds this many bytes. The worker stops reading the host once
    // the receive queue is full; CTS drops while the send queue is half full.
    static constexpr size_t queueBytes = 4096;

    static bool Parse(const std::string& text, Config& config);  // off, pty, tcp:PORT, unix:PATH
    static std::string Format(const Config& config);

    HostSerial();
    ~HostSerial();
    HostSerial(const HostSerial&) = delete;
    HostSerial& operator=(const HostSerial&) = delete;

    // Opens the endpoint and starts the worker. Off is accepted and opens nothing.
    bool Start(PxaUart* uart, const Config& config, std::string& error, SerialHostRoute route = {});
    // Detaches from the UART, stops the worker and closes every descriptor.
    void Stop();
    // Call between emulation slices: attaches when the UART is idle, updates
    // modem inputs, drops received bytes while the guest receiver is off and
    // wakes the UART when bytes are waiting.
    void Service();

    Config GetConfig() const;
    bool Attached() const;
    bool Connected() const;
    std::string Endpoint() const;  // /dev/pts/N, 127.0.0.1:PORT or the socket path; empty when off
    std::string Describe() const;  // off, or kind, endpoint and peer state
    Counters GetCounters() const;
    size_t PendingReceive() const;
    size_t PendingSend() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// Byte pipe to a host PTY, a TCP listener on 127.0.0.1 or a Unix socket, the same endpoints and
// worker as HostSerial, for a device model other than the UART (the USB serial
// bridge). Every method runs on the emulator thread.
class HostByteLink {
   public:
    HostByteLink();
    ~HostByteLink();
    HostByteLink(const HostByteLink&) = delete;
    HostByteLink& operator=(const HostByteLink&) = delete;

    bool Open(const HostSerial::Config& config, std::string& error, bool scopedReceive = false);  // off opens nothing
    // Scoped links isolate input by peer and receive window. Closing/changing a
    // window drops its queued input; the worker samples new bytes in that window.
    void SetReceiveWindow(bool enabled, uint64_t tag);
    void Close();
    bool IsOpen() const;
    bool Connected() const;
    HostSerial::Config GetConfig() const;
    std::string Endpoint() const;
    // Host to guest: takes up to max bytes.
    size_t Receive(uint8_t* out, size_t max);
    // Guest to host: room left in the send queue, 0 without a peer.
    size_t SendSpace() const;
    // Queues up to SendSpace() bytes and returns how many were taken.
    size_t Send(const uint8_t* data, size_t size);
    size_t SendForPeer(const uint8_t* data,size_t size,uint64_t generation);
    uint64_t PeerGeneration() const;
    // Drops both queues.
    void Clear();
    size_t PendingReceive() const;
    size_t PendingSend() const;
    HostSerial::Counters GetCounters() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
#endif
#endif
