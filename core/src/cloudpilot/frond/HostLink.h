#ifndef FROND_68K_HOST_LINK_H
#define FROND_68K_HOST_LINK_H

// A byte pipe to a host PTY, a TCP listener on 127.0.0.1 or a Unix socket listener (one client at a time), for the
// DragonBall UART's serial and infrared sides. Non-blocking, polled from the main loop; all
// methods run on the emulator thread.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <sys/types.h>

class HostLink68k {
   public:
    enum class Kind { off, pty, tcp, unixSocket };
    // Bytes held each way. Reading from the host stops while the receive side is full; bytes
    // the guest sends with no peer, or past this, are dropped.
    static constexpr size_t kQueueBytes = 65536;

    // off, pty, tcp:PORT (0 picks a free port) or unix:PATH. Without a path to fill, unix:PATH is refused.
    static bool Parse(const std::string& text, Kind& kind, uint16_t& port, std::string* path = nullptr);

    HostLink68k() = default;
    ~HostLink68k() { Close(); }
    HostLink68k(const HostLink68k&) = delete;
    HostLink68k& operator=(const HostLink68k&) = delete;

    // Off closes and opens nothing. A Unix socket needs an absolute path in an existing 0700
    // folder owned by the user; a leftover socket file there is replaced, anything else refused.
    // The socket file is removed on Close.
    bool Open(Kind kind, uint16_t port, std::string& error, const std::string& path = std::string());
    void Close();
    // Accepts a client, notices a PTY peer coming and going, moves bytes both ways.
    void Poll();
    // A Unix socket closes while the Palm sleeps and listens again on waking, as an ARM core's
    // port does, so a peer sees it asleep. Bytes already received stay the guest's.
    void SetAsleep(bool asleep);

    Kind GetKind() const { return kind; }
    bool Connected() const;
    // off, or "pty /dev/pts/N" / "tcp 127.0.0.1:PORT" / "unix PATH" and "connected" or "waiting".
    std::string Describe() const;
    // The mode as --serial takes it: off, pty, tcp:PORT or unix:PATH.
    std::string Mode() const;

    size_t Available() const { return rx.size(); }
    size_t Take(uint8_t* out, size_t max);
    // Received bytes up to and including the first byte equal to value, else all of them.
    size_t CountThrough(uint8_t value) const;
    void Send(const uint8_t* data, size_t size);
    void DropReceived() {
        rx.clear();
        scanned = 0;
    }

    uint64_t Received() const { return received; }
    uint64_t Sent() const { return sent; }

   private:
    void Disconnect();
    bool Pump(int fd);

    Kind kind{Kind::off};
    uint16_t port{0};
    int fd{-1};      // PTY master or listener
    int client{-1};  // TCP or Unix client
    std::string path;  // Unix socket path
    ino_t socketInode{0};
    bool hung{false};  // PTY with no slave open
    bool asleep{false};
    std::string endpoint;
    std::deque<uint8_t> rx, tx;
    // Leading bytes of rx that CountThrough has already found free of scanValue.
    mutable size_t scanned{0};
    mutable uint8_t scanValue{0};
    uint64_t received{0}, sent{0};
};

#endif
