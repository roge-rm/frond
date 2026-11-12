#include "HostNetwork.h"
#ifndef __EMSCRIPTEN__
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace {
constexpr uint16_t param = 0x1204, resources = 0x1206, closed = 0x1214, badSocket = 0x1208,
                   timeout = 0x1212, wouldBlock = 0x122f, unsupported = 0x120e;
using Clock = std::chrono::steady_clock;
uint16_t palmError(int error) {
    switch (error) {
        case 0:
            return 0;
        case EAGAIN:
        case EINPROGRESS:
        case EALREADY:
            return wouldBlock;
        case ETIMEDOUT:
            return timeout;
        case ECONNRESET:
        case EPIPE:
            return closed;
        case ECONNREFUSED:
        case EHOSTUNREACH:
            return 0x122d;
        case ENETUNREACH:
        case ENETDOWN:
            return 0x120c;
        case ENOTCONN:
            return 0x120b;
        case EADDRINUSE:
            return 0x120f;
        case EMFILE:
        case ENFILE:
        case ENOBUFS:
            return resources;
        case ENOMEM:
            return 0x1207;
        case EMSGSIZE:
            return 0x120a;
        case EOPNOTSUPP:
        case EPROTONOSUPPORT:
            return unsupported;
        case EBADF:
            return badSocket;
        default:
            return param;
    }
}
sockaddr_in endpoint(uint32_t address, uint16_t port) {
    sockaddr_in result{};
    result.sin_family = AF_INET;
    result.sin_addr.s_addr = htonl(address);
    result.sin_port = htons(port);
    return result;
}
struct DnsJob {
    explicit DnsJob(std::string name) : name(std::move(name)) {}
    std::string name;
    uint32_t address{};
    int error{};
    std::atomic<bool> ready{false}, cancelled{false};
};
// One bounded resolver queue for the process. Cancellation never joins a libc
// resolver call on the emulation thread. Jobs retain no CPU/guest/model pointers.
class Resolver {
    struct State {
        std::mutex mutex;
        std::condition_variable wake;
        std::deque<std::shared_ptr<DnsJob>> queue;
        bool stop = false;
    };
    std::shared_ptr<State> state = std::make_shared<State>();
    std::thread worker;

   public:
    Resolver()
        : worker([s = state] {
              for (;;) {
                  std::shared_ptr<DnsJob> job;
                  {
                      std::unique_lock lock(s->mutex);
                      s->wake.wait(lock, [&] { return s->stop || !s->queue.empty(); });
                      if (s->stop) return;
                      job = s->queue.front();
                      s->queue.pop_front();
                  }
                  if (job->cancelled.load()) continue;
                  addrinfo hints{}, *answers = nullptr;
                  hints.ai_family = AF_INET;
                  hints.ai_socktype = SOCK_STREAM;
                  job->error = getaddrinfo(job->name.c_str(), nullptr, &hints, &answers);
                  if (!job->error && answers)
                      job->address =
                          ntohl(reinterpret_cast<sockaddr_in*>(answers->ai_addr)->sin_addr.s_addr);
                  else if (!job->error)
                      job->error = EAI_NONAME;
                  if (answers) freeaddrinfo(answers);
                  job->ready.store(true, std::memory_order_release);
              }
          }) {}
    ~Resolver() {
        {
            std::lock_guard lock(state->mutex);
            state->stop = true;
        }
        state->wake.notify_one();
        worker.detach();
    }
    std::shared_ptr<DnsJob> Submit(const std::string& name) {
        auto job = std::make_shared<DnsJob>(name);
        {
            std::lock_guard lock(state->mutex);
            for (auto it = state->queue.begin(); it != state->queue.end();)
                if ((*it)->cancelled.load())
                    it = state->queue.erase(it);
                else
                    ++it;
            if (state->queue.size() >= 16) return {};
            state->queue.push_back(job);
        }
        state->wake.notify_one();
        return job;
    }
};
Resolver& resolver() {
    static Resolver worker;
    return worker;
}
}  // namespace
struct HostNetwork::Impl {
    struct Socket {
        int fd = -1, connectError = 0, pendingError = 0;
        unsigned type = 1;
        bool connecting = false, connected = false, nonblocking = false;
        uint32_t sendTicks = 0, receiveTicks = 0, connectAddress = 0;
        uint16_t connectPort = 0;
    };
    struct Pending {
        Clock::time_point started = Clock::now();
        bool delaying = false;
        std::shared_ptr<DnsJob> dns;
    };
    std::array<Socket, 16> sockets;
    std::map<Key, Pending> pending;
    Result Wait(const Key& key, int32_t ticks, bool nonblocking = false) {
        if (nonblocking || ticks == 0) return {-1, wouldBlock};
        if (ticks < -1) return {-1, param};
        if (!pending.count(key) && pending.size() >= 32) return {-1, resources};
        auto& wait = pending[key];
        if (!wait.delaying) {
            wait.started = Clock::now();
            wait.delaying = true;
        }
        if (ticks > 0 &&
            Clock::now() - wait.started >= std::chrono::milliseconds(int64_t(ticks) * 10))
            return {-1, timeout};
        return {0, 0, true};
    }
    static int32_t Limit(int32_t ticks, uint32_t option) {
        return option && (ticks < 0 || uint32_t(ticks) > option) ? int32_t(option) : ticks;
    }
    Socket* Find(unsigned index) {
        return index < sockets.size() && sockets[index].fd >= 0 ? &sockets[index] : nullptr;
    }
};
HostNetwork::HostNetwork() : impl(std::make_unique<Impl>()) {}
HostNetwork::~HostNetwork() { Reset(); }
void HostNetwork::Reset() {
    for (unsigned i = 0; i < 16; i++) Close(i);
    for (auto& [key, p] : impl->pending)
        if (p.dns) p.dns->cancelled = true;
    impl->pending.clear();
}
void HostNetwork::Complete(const Key& key) {
    auto it = impl->pending.find(key);
    if (it != impl->pending.end()) {
        if (it->second.dns) it->second.dns->cancelled = true;
        impl->pending.erase(it);
    }
}
HostNetwork::Result HostNetwork::Open(unsigned index, unsigned type, int protocol) {
    if (index >= 16 || (type != 1 && type != 2) || (protocol && protocol != (type == 1 ? 6 : 17)))
        return {-1, param};
    Close(index);
    auto& s = impl->sockets[index];
    s.fd =
        socket(AF_INET, (type == 1 ? SOCK_STREAM : SOCK_DGRAM) | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    s.type = type;
    return s.fd < 0 ? Result{-1, palmError(errno)} : Result{int32_t(index + 4)};
}
HostNetwork::Result HostNetwork::Close(unsigned index) {
    if (index >= 16) return {-1, badSocket};
    auto& s = impl->sockets[index];
    if (s.fd >= 0) close(s.fd);
    s = Impl::Socket{};
    return {0};
}
HostNetwork::Result HostNetwork::Connect(unsigned index, uint32_t address, uint16_t port,
                                         int32_t ticks, const Key& key) {
    auto* s = impl->Find(index);
    if (!s) return {-1, closed};
    if (s->connected) return {-1, 0x1213};
    if (s->connectError) return {-1, palmError(s->connectError)};
    if (s->connecting && (s->connectAddress != address || s->connectPort != port))
        return {-1, 0x1209};
    if (!s->connecting) {
        s->connectAddress = address;
        s->connectPort = port;
        auto target = endpoint(address, port);
        if (connect(s->fd, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0) {
            s->connected = true;
            return {0};
        }
        if (errno != EINPROGRESS && errno != EALREADY && errno != EWOULDBLOCK) {
            s->connectError = s->pendingError = errno;
            return {-1, palmError(errno)};
        }
        s->connecting = true;
    }
    pollfd p{s->fd, POLLOUT, 0};
    if (poll(&p, 1, 0) > 0 && p.revents) {
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) error = errno;
        s->connecting = false;
        if (error) {
            s->connectError = s->pendingError = error;
            return {-1, palmError(error)};
        }
        s->connected = true;
        return {0};
    }
    auto result = impl->Wait(key, ticks, s->nonblocking);
    if (result.error == timeout) {
        Close(index);
    }
    return result;
}
HostNetwork::Result HostNetwork::Bind(unsigned index, uint32_t address, uint16_t port) {
    auto* s = impl->Find(index);
    if (!s) return {-1, closed};
    auto local = endpoint(address, port);
    return bind(s->fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0
               ? Result{-1, palmError(errno)}
               : Result{0};
}
HostNetwork::Result HostNetwork::Send(unsigned index, const void* bytes, size_t size,
                                      unsigned flags, bool peer, uint32_t address, uint16_t port,
                                      int32_t ticks, const Key& key) {
    auto* s = impl->Find(index);
    if (!s) return {-1, closed};
    if (flags & ~5u) return {-1, 0x1260};
    int hostFlags = MSG_DONTWAIT | MSG_NOSIGNAL | ((flags & 1) ? MSG_OOB : 0) |
                    ((flags & 4) ? MSG_DONTROUTE : 0);
    auto target = endpoint(address, port);
    auto n =
        sendto(s->fd, bytes, size, hostFlags, peer ? reinterpret_cast<sockaddr*>(&target) : nullptr,
               peer ? sizeof(target) : 0);
    if (n >= 0) return {int32_t(n)};
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return impl->Wait(key, Impl::Limit(ticks, s->sendTicks), s->nonblocking);
    return {-1, palmError(errno)};
}
HostNetwork::Result HostNetwork::Receive(unsigned index, void* bytes, size_t size, unsigned flags,
                                         uint32_t& address, uint16_t& port, int32_t ticks,
                                         const Key& key) {
    auto* s = impl->Find(index);
    if (!s) return {-1, closed};
    if (flags & ~3u) return {-1, 0x1260};
    sockaddr_in from{};
    socklen_t length = sizeof(from);
    auto n = recvfrom(s->fd, bytes, size,
                      MSG_DONTWAIT | ((flags & 1) ? MSG_OOB : 0) | ((flags & 2) ? MSG_PEEK : 0),
                      reinterpret_cast<sockaddr*>(&from), &length);
    if (n >= 0) {
        if (from.sin_family != AF_INET) {
            length = sizeof(from);
            if (getpeername(s->fd, reinterpret_cast<sockaddr*>(&from), &length) < 0 &&
                s->connected)
                // Linux may forget the peer after both FINs while unread TCP
                // bytes remain. Preserve the endpoint for those bytes and EOF.
                from = endpoint(s->connectAddress, s->connectPort);
        }
        address = ntohl(from.sin_addr.s_addr);
        port = ntohs(from.sin_port);
        return {int32_t(n)};
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return impl->Wait(key, Impl::Limit(ticks, s->receiveTicks), s->nonblocking);
    return {-1, palmError(errno)};
}
HostNetwork::Result HostNetwork::Select(unsigned width, uint32_t masks[3], int32_t ticks,
                                        const Key& key) {
    if (width > 32) return {-1, param};
    const auto inRange = width == 32 ? UINT32_MAX : (uint32_t(1) << width) - 1;
    if (((masks[0] | masks[1] | masks[2]) & inRange) & ~uint32_t(0x000ffff0))
        return {-1, badSocket};
    std::array<pollfd, 16> p{};
    uint32_t ready[3]{};
    unsigned n = 0;
    for (unsigned i = 0; i < 16; i++) {
        const uint32_t bit = 1u << (i + 4);
        if (i + 4 >= width) continue;
        const bool wanted = (masks[0] | masks[1] | masks[2]) & bit;
        if (!wanted) continue;
        auto* s = impl->Find(i);
        if (!s) return {-1, closed};
        p[i].fd = s->fd;
        p[i].events = ((masks[0] & bit) ? POLLIN : 0) | ((masks[1] & bit) ? POLLOUT : 0) |
                      ((masks[2] & bit) ? POLLPRI : 0);
    }
    // Unused entries must not poll descriptor zero.
    for (unsigned i = 0; i < 16; i++)
        if (!p[i].events) p[i].fd = -1;
    if (poll(p.data(), p.size(), 0) < 0 && errno != EINTR) return {-1, palmError(errno)};
    for (unsigned i = 0; i < 16; i++) {
        const uint32_t bit = 1u << (i + 4);
        const auto events = p[i].revents;
        for (unsigned j = 0; j < 3; j++) {
            const auto wanted = j == 0   ? (POLLIN | POLLHUP | POLLERR)
                                : j == 1 ? (POLLOUT | POLLHUP | POLLERR)
                                         : POLLPRI;
            if ((masks[j] & bit) && (events & wanted)) {
                ready[j] |= bit;
                ++n;
            }
        }
    }
    if (!n && ticks) {
        auto result = impl->Wait(key, ticks);
        if (result.waiting) return result;
        if (result.error != timeout) return result;
    }
    for (unsigned j = 0; j < 3; j++) masks[j] = ready[j];
    return {int32_t(n)};
}
HostNetwork::Result HostNetwork::Resolve(const std::string& name, uint32_t& address, int32_t ticks,
                                         const Key& key) {
    in_addr numeric{};
    if (inet_pton(AF_INET, name.c_str(), &numeric) == 1) {
        address = ntohl(numeric.s_addr);
        return {0};
    }
    if (name.empty() || name.size() > 255) return {-1, 0x1236};
    if (!impl->pending.count(key) && impl->pending.size() >= 32) return {-1, resources};
    auto& wait = impl->pending[key];
    if (wait.dns && wait.dns->name != name) {
        wait.dns->cancelled = true;
        wait = Impl::Pending{};
    }
    if (!wait.dns) wait.dns = resolver().Submit(name);
    if (!wait.dns) return {-1, resources};
    if (wait.dns->ready.load(std::memory_order_acquire)) {
        if (wait.dns->error) return {-1, uint16_t(wait.dns->error == EAI_AGAIN ? 0x123a : 0x123e)};
        address = wait.dns->address;
        return {0};
    }
    auto result = impl->Wait(key, ticks);
    if (result.error == timeout) result.error = 0x123a;
    return result;
}
HostNetwork::Result HostNetwork::Shutdown(unsigned index, unsigned direction) {
    auto* s = impl->Find(index);
    if (!s) return {-1, closed};
    if (direction > 2) return {-1, param};
    return shutdown(s->fd, direction) < 0 ? Result{-1, palmError(errno)} : Result{0};
}
HostNetwork::Result HostNetwork::Address(unsigned index, bool peer, uint32_t& address,
                                         uint16_t& port) {
    auto* s = impl->Find(index);
    if (!s) return {-1, closed};
    sockaddr_in a{};
    socklen_t length = sizeof(a);
    int status = peer ? getpeername(s->fd, reinterpret_cast<sockaddr*>(&a), &length)
                      : getsockname(s->fd, reinterpret_cast<sockaddr*>(&a), &length);
    if (status < 0) return {-1, palmError(errno)};
    address = ntohl(a.sin_addr.s_addr);
    port = ntohs(a.sin_port);
    return {0};
}
HostNetwork::Result HostNetwork::SetOption(unsigned index, uint16_t level, uint16_t option,
                                           uint32_t value) {
    auto* s = impl->Find(index);
    if (!s) return {-1, closed};
    if (level == 0xffff && option == 0x2000) {
        s->nonblocking = value;
        return {0};
    }
    if (level == 0xffff && (option == 0x1005 || option == 0x1006)) {
        if (value > INT32_MAX) return {-1, param};
        (option == 0x1005 ? s->sendTicks : s->receiveTicks) = value;
        return {0};
    }
    int hostLevel = SOL_SOCKET, hostOption = 0;
    if (level == 0xffff) {
        switch (option) {
            case 4:
                hostOption = SO_REUSEADDR;
                break;
            case 8:
                hostOption = SO_KEEPALIVE;
                break;
            case 0x20:
                hostOption = SO_BROADCAST;
                break;
            case 0x1001:
                hostOption = SO_SNDBUF;
                break;
            case 0x1002:
                hostOption = SO_RCVBUF;
                break;
            default:
                return {-1, unsupported};
        }
    } else if (level == 6 && option == 1) {
        hostLevel = IPPROTO_TCP;
        hostOption = TCP_NODELAY;
    } else
        return {-1, unsupported};
    const int setting = value;
    return setsockopt(s->fd, hostLevel, hostOption, &setting, sizeof(setting)) < 0
               ? Result{-1, palmError(errno)}
               : Result{0};
}
HostNetwork::Result HostNetwork::GetOption(unsigned index, uint16_t level, uint16_t option,
                                           uint32_t& value) {
    auto* s = impl->Find(index);
    if (!s) return {-1, closed};
    if (level == 0xffff && option == 0x2000) {
        value = s->nonblocking;
        return {0};
    }
    if (level == 0xffff && option == 0x1008) {
        value = s->type;
        return {0};
    }
    if (level == 0xffff && (option == 0x1005 || option == 0x1006)) {
        value = option == 0x1005 ? s->sendTicks : s->receiveTicks;
        return {0};
    }
    int hostLevel = SOL_SOCKET, hostOption = 0;
    if (level == 0xffff) {
        switch (option) {
            case 4:
                hostOption = SO_REUSEADDR;
                break;
            case 8:
                hostOption = SO_KEEPALIVE;
                break;
            case 0x20:
                hostOption = SO_BROADCAST;
                break;
            case 0x1001:
                hostOption = SO_SNDBUF;
                break;
            case 0x1002:
                hostOption = SO_RCVBUF;
                break;
            case 0x1007:
                hostOption = SO_ERROR;
                break;
            default:
                return {-1, unsupported};
        }
    } else if (level == 6 && option == 1) {
        hostLevel = IPPROTO_TCP;
        hostOption = TCP_NODELAY;
    } else
        return {-1, unsupported};
    if (hostOption == SO_ERROR) {
        // Querying SO_ERROR consumes Linux's pending connect error. Retain the
        // connect result so a later retry cannot misreport a refused socket as
        // successfully connected after the application reads the error option.
        if (s->connecting) {
            pollfd p{s->fd, POLLOUT, 0};
            if (poll(&p, 1, 0) <= 0 || !p.revents) {
                value = 0;
                return {0};
            }
            int error = 0;
            socklen_t size = sizeof(error);
            if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) error = errno;
            s->connectError = s->pendingError = error;
            s->connecting = false;
            s->connected = !error;
        }
        if (s->pendingError) {
            value = palmError(s->pendingError);
            s->pendingError = 0;
            return {0};
        }
    }
    int setting = 0;
    socklen_t size = sizeof(setting);
    if (getsockopt(s->fd, hostLevel, hostOption, &setting, &size) < 0)
        return {-1, palmError(errno)};
    value = hostOption == SO_ERROR ? palmError(setting) : setting;
    return {0};
}
#endif
