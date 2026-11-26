// For targets that let libslirp open host sockets: stops unless the process is in a network
// namespace of its own (only a loopback interface, as `unshare -rn` gives), then brings loopback up
// so guest connections to the host address get refused instead of going nowhere.
#pragma once

#include <ifaddrs.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

inline void RequireIsolatedNetwork() {
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) {
        std::perror("getifaddrs");
        std::exit(2);
    }
    for (auto* a = list; a; a = a->ifa_next)
        if (!(a->ifa_flags & IFF_LOOPBACK)) {
            std::fprintf(stderr, "This target opens host sockets for guest traffic. Run it under unshare -rn (found %s).\n",
                         a->ifa_name);
            std::exit(2);
        }
    freeifaddrs(list);
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    ifreq request{};
    std::strcpy(request.ifr_name, "lo");
    if (fd >= 0 && ioctl(fd, SIOCGIFFLAGS, &request) == 0) {
        request.ifr_flags |= IFF_UP;
        ioctl(fd, SIOCSIFFLAGS, &request);
    }
    if (fd >= 0) close(fd);
}
