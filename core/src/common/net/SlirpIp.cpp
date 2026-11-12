#include "SlirpIp.h"

#include "NetSyncWakeup.h"

#include <arpa/inet.h>
#include <libslirp.h>
#include <poll.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <set>

namespace frondNet {

namespace {

// The guest's made-up MAC and the one slirp uses for 10.0.2.2.
const uint8_t guestMac[6] = {0x02, 0x46, 0x72, 0x6f, 0x6e, 0x64};
const uint8_t hostMac[6] = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};

// Packets slirp may queue for the guest before they're dropped; TCP resends what's lost.
constexpr size_t queueLimit = 64;

void Put16(uint8_t* at, uint16_t value) {
    at[0] = uint8_t(value >> 8);
    at[1] = uint8_t(value);
}
void Put32(uint8_t* at, uint32_t value) {
    Put16(at, uint16_t(value >> 16));
    Put16(at + 2, uint16_t(value));
}
uint16_t Get16(const uint8_t* at) { return uint16_t((at[0] << 8) | at[1]); }
uint32_t Get32(const uint8_t* at) { return (uint32_t(Get16(at)) << 16) | Get16(at + 2); }

// An ARP packet in an Ethernet frame from the guest.
std::vector<uint8_t> Arp(uint16_t op, const uint8_t* targetMac, uint32_t targetIp) {
    std::vector<uint8_t> frame(42, 0);
    std::copy(targetMac, targetMac + 6, frame.begin());
    std::copy(guestMac, guestMac + 6, frame.begin() + 6);
    Put16(&frame[12], 0x0806);
    Put16(&frame[14], 1);
    Put16(&frame[16], 0x0800);
    frame[18] = 6;
    frame[19] = 4;
    Put16(&frame[20], op);
    std::copy(guestMac, guestMac + 6, frame.begin() + 22);
    Put32(&frame[28], guestAddress);
    if (op == 2) std::copy(targetMac, targetMac + 6, frame.begin() + 32);
    Put32(&frame[38], targetIp);
    return frame;
}

}  // namespace

struct SlirpIp::Impl {
    struct Timer {
        SlirpTimerCb callback;
        void* context;
        int64_t deadline{-1};
    };
    uint64_t now{};
    Slirp* slirp{};
    SlirpCb callbacks{};
    bool announced{false};
    std::deque<std::vector<uint8_t>> incoming;
    // ARP replies for slirp, fed in from Poll rather than from inside slirp's callback.
    std::deque<std::vector<uint8_t>> arpReplies;
    std::vector<pollfd> descriptors;
    std::set<Timer*> timers;

    Impl() {
        callbacks.send_packet = [](const void* bytes, size_t size, void* context) -> slirp_ssize_t {
            static_cast<Impl*>(context)->Output(static_cast<const uint8_t*>(bytes), size);
            return slirp_ssize_t(size);
        };
        callbacks.guest_error = [](const char* message, void*) {
            fprintf(stderr, "PPP network: %s\n", message);
        };
        callbacks.clock_get_ns = [](void* context) -> int64_t {
            return int64_t(static_cast<Impl*>(context)->now);
        };
        callbacks.timer_new = [](SlirpTimerCb callback, void* cbcontext, void* context) -> void* {
            auto& self = *static_cast<Impl*>(context);
            auto* timer = new Timer{callback, cbcontext};
            self.timers.insert(timer);
            return timer;
        };
        callbacks.timer_free = [](void* timer, void* context) {
            static_cast<Impl*>(context)->timers.erase(static_cast<Timer*>(timer));
            delete static_cast<Timer*>(timer);
        };
        // slirp gives timer deadlines in milliseconds.
        callbacks.timer_mod = [](void* timer, int64_t time, void*) {
            static_cast<Timer*>(timer)->deadline = time;
        };
        callbacks.register_poll_fd = [](int, void*) {};
        callbacks.unregister_poll_fd = [](int, void*) {};
        callbacks.notify = [](void*) {};
        SlirpConfig config{};
        config.version = 4;
        config.in_enabled = true;
        config.in6_enabled = false;
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

    // A frame from slirp: IPv4 goes to the guest without its Ethernet header, an ARP request
    // for the guest gets a reply, anything else is dropped.
    void Output(const uint8_t* frame, size_t size) {
        if (size < 14) return;
        const uint16_t type = Get16(frame + 12);
        if (type == 0x0800 && size >= 34) {
            // Trim any Ethernet padding to the IP total length.
            const size_t length = Get16(frame + 16);
            if (length >= 20 && 14 + length <= size && incoming.size() < queueLimit)
                incoming.emplace_back(frame + 14, frame + 14 + length);
        } else if (type == 0x0806 && size >= 42 && Get16(frame + 20) == 1 &&
                   Get32(frame + 38) == guestAddress && arpReplies.size() < 4) {
            arpReplies.push_back(Arp(2, frame + 22, Get32(frame + 28)));
        }
    }

    void Feed(const std::vector<uint8_t>& frame) { slirp_input(slirp, frame.data(), int(frame.size())); }

    static int Add(int fd, int events, void* context) {
        auto& self = *static_cast<Impl*>(context);
        short flags = 0;
        if (events & SLIRP_POLL_IN) flags |= POLLIN;
        if (events & SLIRP_POLL_OUT) flags |= POLLOUT;
        if (events & SLIRP_POLL_PRI) flags |= POLLPRI;
        self.descriptors.push_back({fd, flags, 0});
        return int(self.descriptors.size() - 1);
    }
    static int Events(int index, void* context) {
        auto& self = *static_cast<Impl*>(context);
        if (index < 0 || size_t(index) >= self.descriptors.size()) return SLIRP_POLL_ERR;
        const auto flags = self.descriptors[size_t(index)].revents;
        int events = 0;
        if (flags & POLLIN) events |= SLIRP_POLL_IN;
        if (flags & POLLOUT) events |= SLIRP_POLL_OUT;
        if (flags & POLLPRI) events |= SLIRP_POLL_PRI;
        if (flags & (POLLERR | POLLNVAL)) events |= SLIRP_POLL_ERR;
        if (flags & POLLHUP) events |= SLIRP_POLL_HUP;
        return events;
    }
};

SlirpIp::SlirpIp() : impl(std::make_unique<Impl>()) {}
SlirpIp::~SlirpIp() = default;

bool SlirpIp::Valid() const { return impl->slirp; }

void SlirpIp::Input(const uint8_t* packet, size_t size, uint64_t now) {
    if (!Valid() || size < 20 || size > 1500 || (packet[0] >> 4) != 4) return;
    impl->now = now;
    if (auto answer = NetSyncWakeupAnswer(packet, size); !answer.empty()) {
        if (impl->incoming.size() < queueLimit) impl->incoming.push_back(std::move(answer));
        return;
    }
    // An ARP request first puts the guest's MAC in slirp's table, so replies need no ARP round.
    if (!impl->announced) {
        impl->announced = true;
        impl->Feed(Arp(1, hostMac, hostAddress));
    }
    std::vector<uint8_t> frame(14 + size);
    std::copy(hostMac, hostMac + 6, frame.begin());
    std::copy(guestMac, guestMac + 6, frame.begin() + 6);
    Put16(&frame[12], 0x0800);
    std::copy(packet, packet + size, frame.begin() + 14);
    impl->Feed(frame);
}

void SlirpIp::Poll(uint64_t now) {
    if (!Valid()) return;
    impl->now = now;
    while (!impl->arpReplies.empty()) {
        const auto reply = std::move(impl->arpReplies.front());
        impl->arpReplies.pop_front();
        impl->Feed(reply);
    }
    std::vector<Impl::Timer*> due;
    for (auto* timer : impl->timers)
        if (timer->deadline >= 0 && uint64_t(timer->deadline) <= now / 1000000) due.push_back(timer);
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

void SlirpIp::Reset() {
    const uint64_t now = impl->now;
    impl.reset();
    impl = std::make_unique<Impl>();
    impl->now = now;
}

size_t SlirpIp::Queued() const { return impl->incoming.size(); }

bool SlirpIp::Receive(std::vector<uint8_t>& packet) {
    if (impl->incoming.empty()) return false;
    packet = std::move(impl->incoming.front());
    impl->incoming.pop_front();
    return true;
}

}  // namespace frondNet
