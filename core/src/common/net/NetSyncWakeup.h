#ifndef FROND_NET_SYNC_WAKEUP_H
#define FROND_NET_SYNC_WAKEUP_H

// Palm Network HotSync first sends a UDP wakeup (0xfade, type 1, then the PC's address, mask
// and name) to port 14237 of its Primary PC and connects to TCP 14238 only once the PC sends
// it back as type 2. Answering in the router lets pilot-link's net: listener, which has no UDP
// side, take the sync. Shared by SlirpIp and WifiRouter.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "IpNetwork.h"

namespace frondNet {

constexpr uint16_t netSyncWakeupPort = 14237;

namespace netSyncDetail {

inline void Put16(uint8_t* at, uint16_t value) {
    at[0] = uint8_t(value >> 8);
    at[1] = uint8_t(value);
}
inline void Put32(uint8_t* at, uint32_t value) {
    Put16(at, uint16_t(value >> 16));
    Put16(at + 2, uint16_t(value));
}
inline uint16_t Get16(const uint8_t* at) { return uint16_t((at[0] << 8) | at[1]); }
inline uint32_t Get32(const uint8_t* at) { return (uint32_t(Get16(at)) << 16) | Get16(at + 2); }

inline uint16_t Checksum(const uint8_t* data, size_t size, uint32_t sum = 0) {
    for (size_t i = 0; i + 1 < size; i += 2) sum += Get16(data + i);
    if (size & 1) sum += uint32_t(data[size - 1]) << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return uint16_t(~sum);
}

}  // namespace netSyncDetail

// The answer to a wakeup sent to 10.0.2.2, as an IPv4 packet, or an empty packet when the
// IPv4 packet given isn't one.
inline std::vector<uint8_t> NetSyncWakeupAnswer(const uint8_t* packet, size_t size) {
    using namespace netSyncDetail;
    if (size < 20) return {};
    const size_t header = size_t(packet[0] & 0x0f) * 4;
    if (packet[9] != 17 || header < 20 || size < header + 8 + 4 || Get32(packet + 16) != hostAddress ||
        (Get16(packet + 6) & 0x3fff) != 0 || Get16(packet + header + 2) != netSyncWakeupPort)
        return {};
    const uint8_t* udp = packet + header;
    const size_t length = std::min<size_t>(Get16(udp + 4), size - header);
    if (length < 8 + 4 || Get16(udp + 8) != 0xfade || udp[10] != 1) return {};
    const size_t payload = length - 8;
    std::vector<uint8_t> out(20 + 8 + payload, 0);
    out[0] = 0x45;
    Put16(&out[2], uint16_t(out.size()));
    out[8] = 64;
    out[9] = 17;
    Put32(&out[12], hostAddress);
    std::copy(packet + 12, packet + 16, out.begin() + 16);
    Put16(&out[10], Checksum(out.data(), 20));
    uint8_t* reply = &out[20];
    Put16(reply, netSyncWakeupPort);
    std::copy(udp, udp + 2, reply + 2);
    Put16(reply + 4, uint16_t(8 + payload));
    std::copy(udp + 8, udp + length, reply + 8);
    reply[10] = 2;
    // The UDP checksum covers a pseudo-header: both addresses, the protocol and the length.
    uint32_t pseudo = 17 + uint32_t(8 + payload);
    for (size_t i = 12; i < 20; i += 2) pseudo += Get16(&out[i]);
    uint16_t sum = Checksum(reply, 8 + payload, pseudo);
    Put16(reply + 6, sum ? sum : 0xffff);
    return out;
}

}  // namespace frondNet

#endif
