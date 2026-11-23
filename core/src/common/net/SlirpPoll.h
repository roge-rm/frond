#ifndef FROND_SLIRP_POLL_H
#define FROND_SLIRP_POLL_H

#include <libslirp.h>
#include <poll.h>

#include <vector>

namespace frondNet {

// Windows socket handles are wider than int. libslirp 4.9 can pass them through.
#if defined(_WIN32) && SLIRP_CHECK_VERSION(4, 9, 0)
using SlirpPollSocket = slirp_os_socket;

inline void ConfigureSlirpPoll(SlirpConfig& config, SlirpCb& callbacks) {
    config.version = 6;
    callbacks.register_poll_socket = [](SlirpPollSocket, void*) {};
    callbacks.unregister_poll_socket = [](SlirpPollSocket, void*) {};
}

inline void FillSlirpPoll(Slirp* slirp, SlirpAddPollSocketCb add, void* context) {
    uint32_t timeout = 0;
    slirp_pollfds_fill_socket(slirp, &timeout, add, context);
}
#else
using SlirpPollSocket = int;

inline void ConfigureSlirpPoll(SlirpConfig& config, SlirpCb& callbacks) {
    config.version = 4;
    callbacks.register_poll_fd = [](SlirpPollSocket, void*) {};
    callbacks.unregister_poll_fd = [](SlirpPollSocket, void*) {};
}

inline void FillSlirpPoll(Slirp* slirp, SlirpAddPollCb add, void* context) {
    uint32_t timeout = 0;
    slirp_pollfds_fill(slirp, &timeout, add, context);
}
#endif

// Winsock uses POLLRDBAND for out-of-band data and rejects POLLPRI.
#ifdef _WIN32
constexpr short slirpPollRead = POLLRDNORM;
constexpr short slirpPollPriority = POLLRDBAND;
#else
constexpr short slirpPollRead = POLLIN;
constexpr short slirpPollPriority = POLLPRI;
#endif

inline int AddSlirpPollSocket(std::vector<pollfd>& descriptors, SlirpPollSocket fd, int events) {
    short flags = 0;
    if (events & SLIRP_POLL_IN) flags |= slirpPollRead;
    if (events & SLIRP_POLL_OUT) flags |= POLLOUT;
    if (events & SLIRP_POLL_PRI) flags |= slirpPollPriority;
    descriptors.push_back({static_cast<decltype(pollfd::fd)>(fd), flags, 0});
    return int(descriptors.size() - 1);
}

inline int SlirpPollEvents(const std::vector<pollfd>& descriptors, int index) {
    if (index < 0 || size_t(index) >= descriptors.size()) return SLIRP_POLL_ERR;
    const auto flags = descriptors[size_t(index)].revents;
    int events = 0;
    if (flags & slirpPollRead) events |= SLIRP_POLL_IN;
    if (flags & POLLOUT) events |= SLIRP_POLL_OUT;
    if (flags & slirpPollPriority) events |= SLIRP_POLL_PRI;
    if (flags & (POLLERR | POLLNVAL)) events |= SLIRP_POLL_ERR;
    if (flags & POLLHUP) events |= SLIRP_POLL_HUP;
    return events;
}

inline void PollSlirp(Slirp* slirp, std::vector<pollfd>& descriptors,
                     int (*add)(SlirpPollSocket, int, void*), SlirpGetREventsCb events, void* context) {
    descriptors.clear();
    FillSlirpPoll(slirp, add, context);
    // WSAPoll requires at least one socket. slirp still has timers to service.
    const bool failed = !descriptors.empty() && poll(descriptors.data(), descriptors.size(), 0) < 0;
    slirp_pollfds_poll(slirp, failed, events, context);
}

}  // namespace frondNet

#endif
