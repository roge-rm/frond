#include "WifiRouter.h"
#ifndef __EMSCRIPTEN__
#include <arpa/inet.h>
#include <libslirp.h>
#include <poll.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <set>
#include <vector>

#include "NetSyncWakeup.h"

namespace frondNet {

namespace {

// Frames slirp may queue for the guest before they're dropped; TCP resends what's lost.
constexpr size_t queueLimit = 64;

// The MAC slirp uses for 10.0.2.2.
const uint8_t hostMac[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};

// The wakeup answer for an IPv4 frame, sent back to the guest's MAC, or an empty frame.
std::vector<uint8_t> WakeupFrame(const uint8_t* frame, size_t size) {
    if (size < 14 + 20 || frame[12] != 0x08 || frame[13] != 0x00 || (frame[14] >> 4) != 4) return {};
    // Leave out Ethernet padding past the IP total length.
    const size_t total = (size_t(frame[16]) << 8) | frame[17];
    const auto answer = NetSyncWakeupAnswer(frame + 14, std::min(total, size - 14));
    if (answer.empty()) return {};
    std::vector<uint8_t> out(14 + answer.size());
    std::copy(frame + 6, frame + 12, out.begin());
    // From the MAC the guest sent to, unless that was a group address.
    const uint8_t* source = (frame[0] & 1) ? hostMac : frame;
    std::copy(source, source + 6, out.begin() + 6);
    out[12] = 0x08;
    out[13] = 0x00;
    std::copy(answer.begin(), answer.end(), out.begin() + 14);
    return out;
}

}  // namespace

struct WifiRouter::Impl {
    struct Timer {
        SlirpTimerCb callback;
        void* context;
        int64_t deadline{-1};
    };
    uint64_t now{};
    Slirp* slirp{};
    SlirpCb callbacks{};
    std::deque<std::vector<uint8_t>> incoming;
    std::vector<pollfd> descriptors;
    std::set<Timer*> timers;
    Impl() {
        callbacks.send_packet = [](const void* bytes, size_t size, void* context) -> slirp_ssize_t {
            auto& self = *static_cast<Impl*>(context);
            if (size >= 14 && size <= 1518 && self.incoming.size() < queueLimit)
                self.incoming.emplace_back(static_cast<const uint8_t*>(bytes),
                                           static_cast<const uint8_t*>(bytes) + size);
            return size;
        };
        callbacks.guest_error = [](const char* message, void*) {
            fprintf(stderr, "Wi-Fi router: %s\n", message);
        };
        callbacks.clock_get_ns = [](void* context) -> int64_t {
            return static_cast<Impl*>(context)->now;
        };
        callbacks.timer_new = [](SlirpTimerCb callback, void* cbcontext, void* context) -> void* {
            auto& self = *static_cast<Impl*>(context);
            auto* timer = new Timer{callback, cbcontext};
            self.timers.insert(timer);
            return timer;
        };
        callbacks.timer_free = [](void* timer, void* context) {
            auto& self = *static_cast<Impl*>(context);
            self.timers.erase(static_cast<Timer*>(timer));
            delete static_cast<Timer*>(timer);
        };
        callbacks.timer_mod = [](void* timer, int64_t time, void*) {
            static_cast<Timer*>(timer)->deadline = time;
        };
        callbacks.register_poll_fd = [](int, void*) {};
        callbacks.unregister_poll_fd = [](int, void*) {};
        callbacks.notify = [](void*) {};
        SlirpConfig config{};
        config.version = 4;
        config.in_enabled = true;
        config.vhostname = "frond";
        config.vnetwork.s_addr = htonl(0x0a000200);
        config.vnetmask.s_addr = htonl(0xffffff00);
        config.vhost.s_addr = htonl(hostAddress);
        config.vdhcp_start.s_addr = htonl(guestAddress);
        config.vnameserver.s_addr = htonl(dnsAddress);
        config.if_mtu = 1500;
        config.if_mru = 1500;
        slirp = slirp_new(&config, &callbacks, this);
    }
    ~Impl() {
        if (slirp) slirp_cleanup(slirp);
        for (auto* timer : timers) delete timer;
    }
    static int Add(int fd, int events, void* context) {
        auto& self = *static_cast<Impl*>(context);
        short flags = 0;
        if (events & SLIRP_POLL_IN) flags |= POLLIN;
        if (events & SLIRP_POLL_OUT) flags |= POLLOUT;
        if (events & SLIRP_POLL_PRI) flags |= POLLPRI;
        self.descriptors.push_back({fd, flags, 0});
        return self.descriptors.size() - 1;
    }
    static int Events(int index, void* context) {
        auto& self = *static_cast<Impl*>(context);
        if (index < 0 || size_t(index) >= self.descriptors.size()) return SLIRP_POLL_ERR;
        const auto flags = self.descriptors[index].revents;
        int events = 0;
        if (flags & POLLIN) events |= SLIRP_POLL_IN;
        if (flags & POLLOUT) events |= SLIRP_POLL_OUT;
        if (flags & POLLPRI) events |= SLIRP_POLL_PRI;
        if (flags & (POLLERR | POLLNVAL)) events |= SLIRP_POLL_ERR;
        if (flags & POLLHUP) events |= SLIRP_POLL_HUP;
        return events;
    }
};
WifiRouter::WifiRouter() : impl(std::make_unique<Impl>()) {}
WifiRouter::~WifiRouter() = default;
bool WifiRouter::Valid() const { return impl->slirp; }
void WifiRouter::Input(const uint8_t* data, size_t size, uint64_t now) {
    if (!Valid() || size < 14 || size > 1518) return;
    impl->now = now;
    if (auto answer = WakeupFrame(data, size); !answer.empty()) {
        if (impl->incoming.size() < queueLimit) impl->incoming.push_back(std::move(answer));
        return;
    }
    slirp_input(impl->slirp, data, size);
}
void WifiRouter::Poll(uint64_t now) {
    if (!Valid()) return;
    impl->now = now;
    std::vector<Impl::Timer*> due;
    for (auto* timer : impl->timers)
        if (timer->deadline >= 0 && uint64_t(timer->deadline) <= now / 1000000)
            due.push_back(timer);
    for (auto* timer : due)
        if (impl->timers.count(timer)) {
            timer->deadline = -1;
            timer->callback(timer->context);
        }
    impl->descriptors.clear();
    uint32_t timeout = 0;
    slirp_pollfds_fill(impl->slirp, &timeout, Impl::Add, impl.get());
    const int status = poll(impl->descriptors.data(), impl->descriptors.size(), 0);
    slirp_pollfds_poll(impl->slirp, status < 0, Impl::Events, impl.get());
}
bool WifiRouter::Receive(std::vector<uint8_t>& bytes) {
    if (impl->incoming.empty()) return false;
    bytes = std::move(impl->incoming.front());
    impl->incoming.pop_front();
    return true;
}

}  // namespace frondNet
#endif
