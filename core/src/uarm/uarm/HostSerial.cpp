#ifndef __EMSCRIPTEN__
#include "HostSerial.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "UnixListener.h"
#ifndef FROND_BYTE_LINK_ONLY
#include "pxa_UART.h"
#endif

// Tests only: runs on the worker after a host write, before the sent bytes leave the queue.
void (*hostSerialAfterWriteHook)() = nullptr;

namespace {
// Queues shared by the emulator thread and the worker. Only the worker touches
// descriptors other than the eventfd; only the emulator thread touches the UART.
struct Shared {
    std::mutex mutex;
    std::deque<uint8_t> rx, tx;  // host to guest, guest to host
    std::atomic<size_t> rxSize{0}, txSize{0};
    std::atomic<bool> connected{false}, stop{false};
    std::atomic<uint64_t> received{0}, sent{0}, rxDiscarded{0}, txDropped{0};
    uint64_t txGeneration{0};  // bumped under mutex whenever tx is cleared
    bool scopedReceive{false}; // immutable after worker start; legacy serial/USB unchanged
    bool receiveEnabled{false}; // mutex-protected, for scoped links only
    uint64_t receiveTag{0}, receiveGeneration{0};
    std::atomic<uint64_t> peerGeneration{0};
    int wake{-1};

    // Caller holds mutex. Bytes the worker copied out before this are not erased later.
    void ClearTx() {
        tx.clear();
        txSize = 0;
        ++txGeneration;
    }
    void ClearReceive() { // caller holds mutex
        rxDiscarded += rx.size();rx.clear();rxSize=0;++receiveGeneration;
    }
    void Wake() {
        const uint64_t one = 1;
        (void)!write(wake, &one, sizeof(one));
    }
    void Disconnect() {
        std::lock_guard lock(mutex);
        connected = false;
        ClearTx();
        if (scopedReceive) {ClearReceive();++peerGeneration;}
    }
};

void closeFd(int& fd) {
    if (fd >= 0) close(fd);
    fd = -1;
}

bool nonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 &&
           fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
}

// Moves bytes between one descriptor and the queues. Returns false on EOF, hangup
// or error. A hangup while the receive queue is full ends the connection at once;
// bytes the host sent that are still unread in the kernel are lost.
bool pump(Shared& s, int fd, short revents, bool socket) {
    const bool hangup = revents & (POLLHUP | POLLERR | POLLRDHUP);
    if (revents & POLLIN || hangup) {
        uint8_t buffer[1024];
        uint64_t generation=0;
        if (s.scopedReceive) {std::lock_guard lock(s.mutex);generation=s.receiveGeneration;}
        const size_t space = HostSerial::queueBytes - std::min(HostSerial::queueBytes, s.rxSize.load());
        if (!space) {
            if (hangup) return false;
        } else {
            const ssize_t got = read(fd, buffer, std::min(space, sizeof(buffer)));
            if (got == 0) return false;
            if (got < 0 && errno != EAGAIN && errno != EINTR) return false;  // EIO: PTY hung up
            if (got > 0) {
                std::lock_guard lock(s.mutex);
                if (s.scopedReceive && (!s.receiveEnabled || generation!=s.receiveGeneration))
                    s.rxDiscarded += got;
                else {
                    s.rx.insert(s.rx.end(), buffer, buffer + got);
                    s.rxSize = s.rx.size();
                }
                // Publish the count after its queue/discard result is visible.
                s.received += got;
            }
        }
    }
    if (s.txSize.load()) {
        uint8_t buffer[1024];
        size_t count;
        uint64_t generation;
        {
            std::lock_guard lock(s.mutex);
            count = std::min(s.tx.size(), sizeof(buffer));
            std::copy(s.tx.begin(), s.tx.begin() + count, buffer);
            generation = s.txGeneration;
        }
        const ssize_t done = socket ? send(fd, buffer, count, MSG_NOSIGNAL) : write(fd, buffer, count);
        if (done < 0 && errno != EAGAIN && errno != EINTR) return false;
        if (done > 0 && hostSerialAfterWriteHook) hostSerialAfterWriteHook();
        if (done > 0) {
            std::lock_guard lock(s.mutex);
            // A reset or load may have cleared the queue and the guest refilled it.
            if (generation == s.txGeneration) {
                s.tx.erase(s.tx.begin(), s.tx.begin() + std::min<size_t>(done, s.tx.size()));
                s.txSize = s.tx.size();
            }
            s.sent += done;
        }
    }
    return true;
}

// A PTY master reports POLLHUP from when the last slave descriptor closes until a
// program opens it again. It does not report the first open, so a new PTY counts
// as connected.
void ptyWorker(std::shared_ptr<Shared> s, int master) {
    bool hung = false;
    while (!s->stop) {
        pollfd fds[2] = {{s->wake, POLLIN, 0}, {master, 0, 0}};
        if (!hung) {
            if (s->rxSize < HostSerial::queueBytes) fds[1].events |= POLLIN;
            if (s->txSize) fds[1].events |= POLLOUT;
        }
        // POLLHUP stays raised while nobody has the slave open, so check for a new
        // open on a short timer instead of polling the master.
        if (poll(fds, hung ? 1 : 2, hung ? 50 : -1) < 0 && errno != EINTR) break;
        if (fds[0].revents & POLLIN) {
            uint64_t value;
            (void)!read(s->wake, &value, sizeof(value));
        }
        if (hung) {
            pollfd probe{master, POLLIN, 0};
            if (poll(&probe, 1, 0) >= 0 && !(probe.revents & POLLHUP)) {
                hung = false;
                s->connected = true;
            }
            continue;
        }
        if (fds[1].revents && !pump(*s, master, fds[1].revents, false)) {
            hung = true;
            tcflush(master, TCIFLUSH);  // what the old peer wrote is not for the next one
            s->Disconnect();
        }
    }
}

// TCP or Unix socket listener. One client at a time; a second client is accepted and closed at once.
void tcpWorker(std::shared_ptr<Shared> s, int listener) {
    int client = -1;
    while (!s->stop) {
        pollfd fds[3] = {{s->wake, POLLIN, 0}, {listener, POLLIN, 0}, {client, 0, 0}};
        if (client >= 0) {
            fds[2].events = POLLRDHUP;  // peer close, seen even while not reading
            if (s->rxSize < HostSerial::queueBytes) fds[2].events |= POLLIN;
            if (s->txSize) fds[2].events |= POLLOUT;
        }
        if (poll(fds, client >= 0 ? 3 : 2, -1) < 0 && errno != EINTR) break;
        if (fds[0].revents & POLLIN) {
            uint64_t value;
            (void)!read(s->wake, &value, sizeof(value));
        }
        if (fds[1].revents & POLLIN) {
            const int accepted = accept(listener, nullptr, nullptr);
            if (accepted >= 0 && client < 0 && nonblocking(accepted)) {
                const int on = 1;
                setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
                client = accepted;
                {
                    std::lock_guard lock(s->mutex);
                    s->ClearTx();
                    if (s->scopedReceive) {s->ClearReceive();++s->peerGeneration;}
                }
                s->connected = true;
            } else if (accepted >= 0) {
                close(accepted);
            }
        }
        if (client >= 0 && fds[2].revents && !pump(*s, client, fds[2].revents, true)) {
            closeFd(client);
            s->Disconnect();
        }
    }
    closeFd(client);
}
}  // namespace

#ifndef FROND_BYTE_LINK_ONLY
struct HostSerial::Impl {
    Config config;
    PxaUart* uart{nullptr};
    SerialHostRoute route{};
    bool restart{}, window{};
    std::string error;
    uint64_t receiveTag{};
    std::shared_ptr<Shared> shared;
    std::thread worker;
    int fd{-1};  // PTY master or listener, owned by the worker while it runs
    ino_t socketInode{0};  // Unix socket file made by openEndpoint
    std::string endpoint;
    bool attached{false};
    int lastInputs{-1};
    bool ctsOpen{true};
    uint32_t disableCount{0};
    uint64_t consumed{0};         // bytes taken from rx by the guest or dropped
    uint64_t receivedAtService{0};  // shared->received at the previous Service

    static uint_fast16_t Read(void* context) {
        auto& self = *static_cast<Impl*>(context);
        if (!self.shared) return UART_CHAR_NONE;
        auto& s = *self.shared;
        // The peer honours RTS, like a host port with hardware flow control.
        if (!s.rxSize.load(std::memory_order_relaxed) ||
            !(pxaUartGetModemOutputs(self.uart) & PXA_UART_RTS))
            return UART_CHAR_NONE;
        uint8_t byte;
        size_t left;
        {
            std::lock_guard lock(s.mutex);
            if (s.rx.empty()) return UART_CHAR_NONE;
            byte = s.rx.front();
            s.rx.pop_front();
            left = s.rxSize = s.rx.size();
            ++self.consumed;
        }
        if (left == queueBytes / 2) s.Wake();
        return byte;
    }
    static void Write(uint_fast16_t chr, void* context) {
        auto* shared = static_cast<Impl*>(context)->shared.get();
        if (!shared) return;
        auto& s = *shared;
        bool wasEmpty = false;
        if (s.connected) {
            std::lock_guard lock(s.mutex);
            if (s.tx.size() < queueBytes) {
                wasEmpty = s.tx.empty();
                s.tx.push_back(uint8_t(chr));
                s.txSize = s.tx.size();
            } else {
                ++s.txDropped;
            }
        } else {
            ++s.txDropped;  // no peer on the line
        }
        if (wasEmpty) s.Wake();
    }
    static bool Active(void* context) {
        const auto* shared = static_cast<Impl*>(context)->shared.get();
        return shared && shared->rxSize.load(std::memory_order_relaxed) != 0;
    }
    void CloseEndpoint() {
        if (!shared) return;
        shared->stop=true;shared->Wake();
        if(worker.joinable())worker.join();
        closeFd(fd);closeFd(shared->wake);shared.reset();
        if(config.kind==Kind::unixSocket)frondUnix::Remove(config.path,socketInode);
        socketInode=0;window=false;lastInputs=-1;endpoint.clear();
    }
    static void Replaced(void* context) {
        auto& self=*static_cast<Impl*>(context);
        pxaUartSetModemInputs(self.uart,0);
        self.CloseEndpoint();self.restart=true;
    }
    static void ModeChanged(void* context) {
        auto& self=*static_cast<Impl*>(context);
        if(!self.shared || self.route.paused(self.route.context))return;
        const bool enabled=self.route.enabled(self.route.context) && pxaUartReceiverEnabled(self.uart);
        const auto state=pxaUartIrState(self.uart);
        const uint64_t tag=(uint64_t(state.baud)<<8)|(state.lcr&0x3f);
        auto& s=*self.shared;
        std::lock_guard lock(s.mutex);
        if(self.window!=enabled || (enabled && tag!=self.receiveTag)) {
            s.receiveEnabled=enabled;s.receiveTag=tag;s.ClearReceive();s.ClearTx();
            self.window=enabled;self.receiveTag=tag;
        }
        if(!enabled && self.lastInputs!=0 && pxaUartSetModemInputs(self.uart,0)) self.lastInputs=0;
    }
    static void Detached(void* context) {
        auto& self = *static_cast<Impl*>(context);
        self.attached = false;
        self.lastInputs = -1;
        if (self.shared) self.DropQueues();
    }
    void DropQueues() {
        DropReceived(SIZE_MAX);
        std::lock_guard lock(shared->mutex);
        shared->ClearTx();
    }
    // Drops up to count bytes from the front of the receive queue.
    void DropReceived(size_t count) {
        bool wasFull;
        {
            std::lock_guard lock(shared->mutex);
            wasFull = shared->rx.size() >= queueBytes / 2;
            count = std::min(count, shared->rx.size());
            shared->rx.erase(shared->rx.begin(), shared->rx.begin() + count);
            shared->rxSize = shared->rx.size();
            shared->rxDiscarded += count;
            consumed += count;
        }
        if (wasFull && count) shared->Wake();
    }
};

#endif

bool HostSerial::Parse(const std::string& text, Config& config) {
    if (text == "off") {
        config = {};
        return true;
    }
    if (text == "pty") {
        config = {Kind::pty, 0};
        return true;
    }
    if (text.rfind("unix:", 0) == 0) {
        if (text.size() < 6 || text.find('\0') != std::string::npos) return false;
        config = {Kind::unixSocket, 0, text.substr(5)};
        return true;
    }
    if (text.rfind("tcp:", 0) != 0 || text.size() < 5 || text.size() > 9) return false;
    unsigned port = 0;
    for (size_t i = 4; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        port = port * 10 + unsigned(text[i] - '0');
    }
    if (port > 65535) return false;
    config = {Kind::tcp, uint16_t(port)};
    return true;
}

std::string HostSerial::Format(const Config& config) {
    switch (config.kind) {
        case Kind::pty:
            return "pty";
        case Kind::tcp:
            return "tcp:" + std::to_string(config.port);
        case Kind::unixSocket:
            return "unix:" + config.path;
        default:
            return "off";
    }
}

#ifndef FROND_BYTE_LINK_ONLY
HostSerial::HostSerial() : impl(std::make_unique<Impl>()) {}
HostSerial::~HostSerial() { Stop(); }
#endif

namespace {
// Opens a PTY master, a 127.0.0.1 listener or a Unix socket listener and the worker's eventfd.
bool openEndpoint(const HostSerial::Config& config, Shared& shared, int& fd, std::string& endpoint,
                  uint16_t& port, ino_t& inode, std::string& error) {
    shared.wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (shared.wake < 0) {
        error = std::string("eventfd: ") + strerror(errno);
        return false;
    }
    fd = -1;
    port = config.port;
    inode = 0;
    if (config.kind == HostSerial::Kind::unixSocket) {
        fd = frondUnix::Listen(config.path, error, inode);
        if (fd < 0) {
            closeFd(shared.wake);
            return false;
        }
        endpoint = config.path;
    } else if (config.kind == HostSerial::Kind::pty) {
        fd = posix_openpt(O_RDWR | O_NOCTTY);
        const char* name = fd >= 0 && grantpt(fd) == 0 && unlockpt(fd) == 0 ? ptsname(fd) : nullptr;
        termios raw{};
        if (!name || !nonblocking(fd) || tcgetattr(fd, &raw) != 0) {
            error = std::string("Could not open a PTY: ") + strerror(errno);
            closeFd(fd);
            closeFd(shared.wake);
            return false;
        }
        cfmakeraw(&raw);
        tcsetattr(fd, TCSANOW, &raw);
        endpoint = name;
        shared.connected = true;
    } else {
        fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        const int on = 1;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(config.port);
        socklen_t length = sizeof(address);
        if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0 ||
            bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(fd, 1) != 0 ||
            getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
            error = "Could not listen on 127.0.0.1:" + std::to_string(config.port) + ": " + strerror(errno);
            closeFd(fd);
            closeFd(shared.wake);
            return false;
        }
        port = ntohs(address.sin_port);
        endpoint = "127.0.0.1:" + std::to_string(port);
    }
    return true;
}
}  // namespace

#ifndef FROND_BYTE_LINK_ONLY
bool HostSerial::Start(PxaUart* uart, const Config& config, std::string& error, SerialHostRoute route) {
    Stop();impl->error.clear();
    if (config.kind == Kind::off) return true;
    if (!uart) {
        error = "This device has no serial port.";
        return false;
    }
    impl->route=route;
    if(route.context) {
        SerialHostHooks hooks{impl.get(),Impl::Read,Impl::Write,Impl::Active,Impl::Replaced,Impl::ModeChanged};
        if(!route.setHooks || !route.enabled || !route.paused || !route.available || !route.setHooks(route.context,&hooks)) {
            impl->route={};error="Serial UART is busy; detach the infrared accessory first.";return false;
        }
    }
    auto shared = std::make_shared<Shared>();
    shared->scopedReceive=route.context!=nullptr;
    int fd = -1;
    std::string endpoint;
    uint16_t port = 0;
    ino_t inode = 0;
    if (!openEndpoint(config, *shared, fd, endpoint, port, inode, error)) {
        if(route.context)route.setHooks(route.context,nullptr);
        impl->route={};return false;
    }
    impl->config = {config.kind, port, config.path};
    impl->socketInode = inode;
    impl->uart = uart;
    impl->shared = shared;
    impl->fd = fd;
    impl->endpoint = endpoint;
    impl->attached = route.context!=nullptr;
    impl->restart=false;impl->window=false;
    impl->lastInputs = -1;
    impl->ctsOpen = true;
    impl->consumed = impl->receivedAtService = 0;
    impl->worker = config.kind == Kind::pty ? std::thread(ptyWorker, shared, fd) : std::thread(tcpWorker, shared, fd);
    return true;
}

void HostSerial::Stop() {
    if(impl->route.context) {
        impl->route.setHooks(impl->route.context,nullptr);
        pxaUartSetModemInputs(impl->uart,0);
    } else if(impl->shared && impl->attached) pxaUartDetachExternal(impl->uart);
    impl->CloseEndpoint();
    impl->route={};impl->restart=false;impl->uart=nullptr;impl->attached=false;
    impl->config={};impl->endpoint.clear();impl->error.clear();
}

void HostSerial::Service() {
    if(impl->route.context) {
        if(impl->route.paused(impl->route.context))return;
        if(impl->restart && !impl->route.available(impl->route.context)) {
            Stop();impl->error="Restored accessory owns the serial UART.";return;
        }
        if(impl->restart) {
            auto shared=std::make_shared<Shared>();shared->scopedReceive=true;
            uint16_t port=0;std::string error;
            if(!openEndpoint(impl->config,*shared,impl->fd,impl->endpoint,port,impl->socketInode,error)) {
                Stop();impl->error=error;return;
            }
            impl->shared=shared;impl->restart=false;
            impl->worker=impl->config.kind==Kind::pty?std::thread(ptyWorker,shared,impl->fd):std::thread(tcpWorker,shared,impl->fd);
        }
        Impl::ModeChanged(impl.get());
        if(!impl->route.enabled(impl->route.context))return;
    }
    auto* s = impl->shared.get();
    if (!s) return;
    if (!impl->attached) {
        if (!pxaUartAttachExternal(impl->uart, Impl::Read, Impl::Write, Impl::Active, nullptr,
                                   Impl::Detached, impl.get()))
            return;  // busy, frozen or in loopback; try again next time
        impl->attached = true;
        impl->lastInputs = -1;
        impl->disableCount = pxaUartDisableCount(impl->uart);
        impl->receivedAtService = s->received;
    }
    // A closed port has no receiver: bytes from the host are lost, as on a wire.
    // After a close and reopen since the last call, drop the bytes that were
    // already waiting then; later ones may have come after the reopen.
    const auto disabled = pxaUartDisableCount(impl->uart);
    const uint64_t waiting = impl->receivedAtService;
    impl->receivedAtService = s->received;
    if (!impl->route.context && !pxaUartReceiverEnabled(impl->uart))
        impl->DropReceived(SIZE_MAX);
    else if (!impl->route.context && disabled != impl->disableCount && waiting > impl->consumed)
        impl->DropReceived(size_t(waiting - impl->consumed));
    impl->disableCount = disabled;
    const size_t pending = s->txSize;
    if (pending >= queueBytes / 2) impl->ctsOpen = false;
    if (pending <= queueBytes / 4) impl->ctsOpen = true;
    const int inputs = s->connected ? PXA_UART_DSR | PXA_UART_DCD | (impl->ctsOpen ? PXA_UART_CTS : 0) : 0;
    if (inputs != impl->lastInputs && pxaUartSetModemInputs(impl->uart, uint8_t(inputs)))
        impl->lastInputs = inputs;
    if (s->rxSize) pxaUartKick(impl->uart);
}

HostSerial::Config HostSerial::GetConfig() const { return impl->config; }
bool HostSerial::Attached() const { return impl->attached; }
bool HostSerial::Connected() const { return impl->shared && impl->shared->connected; }
std::string HostSerial::Endpoint() const { return impl->endpoint; }

std::string HostSerial::Describe() const {
    if (!impl->shared) return impl->restart ? Format(impl->config)+" suspended" :
        impl->error.empty()?"off":"off error: "+impl->error;
    return std::string(impl->config.kind == Kind::pty ? "pty " : impl->config.kind == Kind::unixSocket ? "unix " : "tcp ") +
           impl->endpoint +
           (Connected() ? " connected" : " waiting");
}

HostSerial::Counters HostSerial::GetCounters() const {
    if (!impl->shared) return {};
    auto& s = *impl->shared;
    return {s.received, s.sent, s.rxDiscarded, s.txDropped};
}
size_t HostSerial::PendingReceive() const { return impl->shared ? impl->shared->rxSize.load() : 0; }
size_t HostSerial::PendingSend() const { return impl->shared ? impl->shared->txSize.load() : 0; }

#endif

struct HostByteLink::Impl {
    HostSerial::Config config;
    std::shared_ptr<Shared> shared;
    std::thread worker;
    int fd{-1};
    ino_t socketInode{0};
    std::string endpoint;
};

HostByteLink::HostByteLink() : impl(std::make_unique<Impl>()) {}
HostByteLink::~HostByteLink() { Close(); }

bool HostByteLink::Open(const HostSerial::Config& config, std::string& error, bool scopedReceive) {
    Close();
    if (config.kind == HostSerial::Kind::off) return true;
    auto shared = std::make_shared<Shared>();
    shared->scopedReceive=scopedReceive;
    int fd = -1;
    std::string endpoint;
    uint16_t port = 0;
    ino_t inode = 0;
    if (!openEndpoint(config, *shared, fd, endpoint, port, inode, error)) return false;
    impl->config = {config.kind, port, config.path};
    impl->socketInode = inode;
    impl->shared = shared;
    impl->fd = fd;
    impl->endpoint = endpoint;
    impl->worker = config.kind == HostSerial::Kind::pty ? std::thread(ptyWorker, shared, fd)
                                                       : std::thread(tcpWorker, shared, fd);
    return true;
}

void HostByteLink::SetReceiveWindow(bool enabled,uint64_t tag) {
    auto* s=impl->shared.get();if (!s || !s->scopedReceive) return;
    {
        std::lock_guard lock(s->mutex);
        if (s->receiveEnabled==enabled && (!enabled || s->receiveTag==tag)) return;
        s->receiveEnabled=enabled;s->receiveTag=tag;s->ClearReceive();
    }
    s->Wake();
}

void HostByteLink::Close() {
    if (!impl->shared) return;
    impl->shared->stop = true;
    impl->shared->Wake();
    if (impl->worker.joinable()) impl->worker.join();
    closeFd(impl->fd);
    close(impl->shared->wake);
    impl->shared.reset();
    if (impl->config.kind == HostSerial::Kind::unixSocket) frondUnix::Remove(impl->config.path, impl->socketInode);
    impl->socketInode = 0;
    impl->config = {};
    impl->endpoint.clear();
}

bool HostByteLink::IsOpen() const { return bool(impl->shared); }
bool HostByteLink::Connected() const { return impl->shared && impl->shared->connected; }
HostSerial::Config HostByteLink::GetConfig() const { return impl->config; }
std::string HostByteLink::Endpoint() const { return impl->endpoint; }

size_t HostByteLink::Receive(uint8_t* out, size_t max) {
    auto* s = impl->shared.get();
    if (!s || !s->rxSize.load(std::memory_order_relaxed)) return 0;
    size_t count, left;
    {
        std::lock_guard lock(s->mutex);
        count = std::min(max, s->rx.size());
        std::copy(s->rx.begin(), s->rx.begin() + count, out);
        s->rx.erase(s->rx.begin(), s->rx.begin() + count);
        left = s->rxSize = s->rx.size();
    }
    // The worker stopped reading the host while the queue was full.
    if (count && left + count >= HostSerial::queueBytes / 2 && left < HostSerial::queueBytes / 2) s->Wake();
    return count;
}

size_t HostByteLink::SendSpace() const {
    auto* s = impl->shared.get();
    if (!s || !s->connected) return 0;
    return HostSerial::queueBytes - std::min(HostSerial::queueBytes, s->txSize.load());
}

size_t HostByteLink::Send(const uint8_t* data, size_t size) {
    return SendForPeer(data,size,UINT64_MAX);
}
uint64_t HostByteLink::PeerGeneration() const {return impl->shared?impl->shared->peerGeneration.load():0;}
size_t HostByteLink::SendForPeer(const uint8_t* data, size_t size,uint64_t generation) {
    auto* s = impl->shared.get();
    if (!s || !s->connected) return 0;
    bool wasEmpty;
    size_t count;
    {
        std::lock_guard lock(s->mutex);
        if (!s->connected || (generation!=UINT64_MAX && generation!=s->peerGeneration)) return 0;
        count = std::min(size, HostSerial::queueBytes - std::min(HostSerial::queueBytes, s->tx.size()));
        wasEmpty = s->tx.empty();
        s->tx.insert(s->tx.end(), data, data + count);
        s->txSize = s->tx.size();
    }
    if (wasEmpty && count) s->Wake();
    return count;
}

void HostByteLink::Clear() {
    auto* s = impl->shared.get();
    if (!s) return;
    bool wasFull;
    {
        std::lock_guard lock(s->mutex);
        wasFull = s->rx.size() >= HostSerial::queueBytes / 2;
        s->rxDiscarded += s->rx.size();
        s->rx.clear();
        s->rxSize = 0;
        s->ClearTx();
    }
    if (wasFull) s->Wake();
}

size_t HostByteLink::PendingReceive() const { return impl->shared ? impl->shared->rxSize.load() : 0; }
size_t HostByteLink::PendingSend() const { return impl->shared ? impl->shared->txSize.load() : 0; }
HostSerial::Counters HostByteLink::GetCounters() const {
    if (!impl->shared) return {};
    auto& s = *impl->shared;
    return {s.received, s.sent, s.rxDiscarded, s.txDropped};
}
#endif
