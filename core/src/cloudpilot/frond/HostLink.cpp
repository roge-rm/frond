#include "HostLink.h"

#include "UnixListener.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace {

bool nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 && fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

void closeFd(int& fd) {
    if (fd >= 0) close(fd);
    fd = -1;
}

}  // namespace

bool HostLink68k::Parse(const std::string& text, Kind& kind, uint16_t& port, std::string* path) {
    port = 0;
    if (text.rfind("unix:", 0) == 0) {
        if (!path || text.size() < 6 || text.find('\0') != std::string::npos) return false;
        kind = Kind::unixSocket;
        *path = text.substr(5);
        return true;
    }
    if (text == "off" || text == "pty") {
        kind = text == "off" ? Kind::off : Kind::pty;
        return true;
    }
    if (text.rfind("tcp:", 0) != 0 || text.size() < 5 || text.size() > 9) return false;
    unsigned value = 0;
    for (size_t i = 4; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + unsigned(text[i] - '0');
    }
    if (value > 65535) return false;
    kind = Kind::tcp;
    port = uint16_t(value);
    return true;
}

bool HostLink68k::Open(Kind newKind, uint16_t newPort, std::string& error, const std::string& newPath) {
    Close();
    if (newKind == Kind::off) return true;
    if (newKind == Kind::unixSocket) {
        fd = frondUnix::Listen(newPath, error, socketInode);
        if (fd < 0) return false;
        path = newPath;
        endpoint = newPath;
    } else if (newKind == Kind::pty) {
        fd = posix_openpt(O_RDWR | O_NOCTTY);
        const char* name = fd >= 0 && grantpt(fd) == 0 && unlockpt(fd) == 0 ? ptsname(fd) : nullptr;
        termios raw{};
        if (!name || !nonblocking(fd) || tcgetattr(fd, &raw) != 0) {
            error = std::string("could not open a PTY: ") + strerror(errno);
            closeFd(fd);
            return false;
        }
        cfmakeraw(&raw);
        tcsetattr(fd, TCSANOW, &raw);
        endpoint = name;
        // A new PTY has no slave open yet; the first open isn't reported, so count it as there.
        hung = false;
    } else {
        fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        const int on = 1;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(newPort);
        socklen_t length = sizeof(address);
        if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0 ||
            bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(fd, 1) != 0 ||
            getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            error = "could not listen on 127.0.0.1:" + std::to_string(newPort) + ": " + strerror(errno);
            closeFd(fd);
            return false;
        }
        port = ntohs(address.sin_port);
        endpoint = "127.0.0.1:" + std::to_string(port);
    }
    kind = newKind;
    return true;
}

void HostLink68k::SetAsleep(bool value) {
    if (kind != Kind::unixSocket || value == asleep) return;
    asleep = value;
    if (asleep) {
        closeFd(client);
        closeFd(fd);
        frondUnix::Remove(path, socketInode);
        socketInode = 0;
        tx.clear();
        return;
    }
    // On failure it stays without a listener, as a closed port.
    std::string error;
    fd = frondUnix::Listen(path, error, socketInode);
}

void HostLink68k::Close() {
    closeFd(client);
    closeFd(fd);
    if (kind == Kind::unixSocket) frondUnix::Remove(path, socketInode);
    path.clear();
    socketInode = 0;
    kind = Kind::off;
    endpoint.clear();
    DropReceived();
    tx.clear();
    hung = false;
    asleep = false;
}

bool HostLink68k::Connected() const {
    return kind == Kind::pty ? !hung : kind == Kind::off ? false : client >= 0;
}

std::string HostLink68k::Describe() const {
    if (kind == Kind::off) return "off";
    return std::string(kind == Kind::pty ? "pty " : kind == Kind::unixSocket ? "unix " : "tcp ") + endpoint + (Connected() ? " connected" : " waiting");
}

std::string HostLink68k::Mode() const {
    return kind == Kind::off ? "off" : kind == Kind::pty ? "pty" : kind == Kind::unixSocket ? "unix:" + path : "tcp:" + std::to_string(port);
}

// What the peer sent before it went is still the guest's, like its last acknowledgement.
void HostLink68k::Disconnect() {
    tx.clear();
    if (kind != Kind::pty) closeFd(client);
    else hung = true;
}

// Moves bytes both ways; false when the peer went away.
bool HostLink68k::Pump(int io) {
    uint8_t buffer[4096];
    while (rx.size() < kQueueBytes) {
        const ssize_t got = read(io, buffer, std::min(sizeof(buffer), kQueueBytes - rx.size()));
        if (got > 0) {
            rx.insert(rx.end(), buffer, buffer + got);
            received += size_t(got);
            continue;
        }
        if (got == 0) return false;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return false;  // EIO: the PTY's last slave closed
    }
    while (!tx.empty()) {
        const size_t count = std::min(tx.size(), sizeof(buffer));
        std::copy(tx.begin(), tx.begin() + count, buffer);
        const ssize_t done = write(io, buffer, count);
        if (done > 0) {
            tx.erase(tx.begin(), tx.begin() + done);
            sent += size_t(done);
            continue;
        }
        if (done < 0 && errno == EINTR) continue;
        if (done < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        return false;
    }
    return true;
}

void HostLink68k::Poll() {
    if (kind == Kind::off) return;
    if (kind != Kind::pty) {
        while (true) {
            const int accepted = accept(fd, nullptr, nullptr);
            if (accepted < 0) break;
            if (client >= 0 || !nonblocking(accepted)) {
                close(accepted);
                continue;
            }
            if (kind == Kind::tcp) {
                const int on = 1;
                setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
            }
            client = accepted;
            DropReceived();
        }
        if (client >= 0 && !Pump(client)) Disconnect();
        return;
    }
    // A PTY master reports POLLHUP from when the last slave closes until a program opens it again.
    if (hung) {
        pollfd probe{fd, POLLIN, 0};
        if (poll(&probe, 1, 0) < 0 || (probe.revents & POLLHUP)) return;
        hung = false;
    }
    if (!Pump(fd)) {
        Disconnect();
        tcflush(fd, TCIFLUSH);
    }
}

size_t HostLink68k::Take(uint8_t* out, size_t max) {
    const size_t count = std::min(max, rx.size());
    std::copy(rx.begin(), rx.begin() + count, out);
    rx.erase(rx.begin(), rx.begin() + count);
    scanned -= std::min(scanned, count);
    return count;
}

size_t HostLink68k::CountThrough(uint8_t value) const {
    if (value != scanValue) {
        scanValue = value;
        scanned = 0;
    }
    const auto at = std::find(rx.begin() + std::min(scanned, rx.size()), rx.end(), value);
    scanned = size_t(at - rx.begin());
    return at == rx.end() ? rx.size() : scanned + 1;
}

void HostLink68k::Send(const uint8_t* data, size_t size) {
    if (!Connected()) return;
    const size_t room = kQueueBytes - std::min(kQueueBytes, tx.size());
    tx.insert(tx.end(), data, data + std::min(size, room));
}
