#ifndef FROND_WIFI_ROUTER_H
#define FROND_WIFI_ROUTER_H

// libslirp behind an emulated Wi-Fi card: whole Ethernet II frames in and out, no FCS. slirp's
// DHCP server gives the guest 10.0.2.15, the host is 10.0.2.2 and the DNS proxy 10.0.2.3.
// Answers the Network HotSync wakeup to 10.0.2.2 itself, like SlirpIp. Single-threaded; time
// is guest nanoseconds. Built into libcommon-net.a so only the cores link libslirp.

#ifndef __EMSCRIPTEN__
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace frondNet {

// The virtual network's addresses, host byte order. The default is the 10.0.2.x layout every Frond network uses.
struct RouterLayout {
    uint32_t network{0x0a000200}, netmask{0xffffff00}, host{0x0a000202}, dhcpStart{0x0a00020f}, dns{0x0a000203};
    bool dhcp{true};  // slirp's DHCP server for the guest
};

class WifiRouter {
   public:
    WifiRouter();
    explicit WifiRouter(const RouterLayout& layout);
    ~WifiRouter();
    bool Valid() const;
    // One Ethernet frame from the guest, 14 to 1518 bytes.
    void Input(const uint8_t*, size_t, uint64_t now);
    // Runs timers and host sockets.
    void Poll(uint64_t now);
    // The next Ethernet frame for the guest, if there is one.
    bool Receive(std::vector<uint8_t>&);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace frondNet

#endif
#endif
