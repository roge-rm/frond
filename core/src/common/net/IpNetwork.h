#ifndef FROND_IP_NETWORK_H
#define FROND_IP_NETWORK_H

// The far side of a guest's IP link: whole IPv4 packets in and out, on guest time in
// nanoseconds. SlirpIp is the real one; tests use their own.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace frondNet {

// The 10.0.2.x layout every Frond network uses: the guest, the host gateway and the DNS proxy.
constexpr uint32_t guestAddress = 0x0a00020f;
constexpr uint32_t hostAddress = 0x0a000202;
constexpr uint32_t dnsAddress = 0x0a000203;

class IpNetwork {
   public:
    virtual ~IpNetwork() = default;
    // One IPv4 packet from the guest.
    virtual void Input(const uint8_t* packet, size_t size, uint64_t now) = 0;
    // Runs timers and host sockets.
    virtual void Poll(uint64_t now) = 0;
    // The next IPv4 packet for the guest, if there is one.
    virtual bool Receive(std::vector<uint8_t>& packet) = 0;
    // The call is over: drops queued packets and the guest's connections, so a later call
    // starts clean.
    virtual void Reset() {}
};

}  // namespace frondNet

#endif
