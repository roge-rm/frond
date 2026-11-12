#ifndef FROND_SLIRP_IP_H
#define FROND_SLIRP_IP_H

// libslirp behind an IP-only link such as PPP. libslirp wants Ethernet frames, so this wraps
// the guest's packets in Ethernet, answers slirp's ARP for the guest and strips Ethernet from
// what comes back. Same 10.0.2.x layout as the Wi-Fi router: the guest is 10.0.2.15, the host
// 10.0.2.2 and the DNS proxy 10.0.2.3. Single-threaded; time is guest nanoseconds.
// Built into libcommon-net.a so only the cores link libslirp.

#include <memory>

#include "IpNetwork.h"

namespace frondNet {

class SlirpIp : public IpNetwork {
   public:
    SlirpIp();
    ~SlirpIp() override;
    bool Valid() const;
    void Input(const uint8_t* packet, size_t size, uint64_t now) override;
    void Poll(uint64_t now) override;
    bool Receive(std::vector<uint8_t>& packet) override;
    // A fresh slirp: the old one's sockets close and its queue goes.
    void Reset() override;
    // Packets waiting for the guest.
    size_t Queued() const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace frondNet

#endif
