// Frame helpers for the slirp targets: the input picks frames and whether their IPv4, TCP, UDP
// and ICMP checksums are put right, so slirp gets past its checksum checks.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

inline uint16_t FuzzGet16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline void FuzzSet16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }

inline uint16_t FuzzChecksum(const uint8_t* data, size_t size, uint32_t sum = 0) {
    for (size_t i = 0; i + 1 < size; i += 2) sum += FuzzGet16(data + i);
    if (size & 1) sum += uint32_t(data[size - 1]) << 8;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return uint16_t(~sum);
}

// Fixes the checksums of the IPv4 packet at ip (no Ethernet), as far as its own lengths allow.
inline void FuzzFixIpv4(uint8_t* ip, size_t size) {
    if (size < 20 || (ip[0] >> 4) != 4) return;
    const size_t header = (ip[0] & 15) * 4;
    if (header < 20 || header > size) return;
    ip[10] = ip[11] = 0;
    FuzzSet16(ip + 10, FuzzChecksum(ip, header));
    size_t total = FuzzGet16(ip + 2);
    if (total > size || total < header || (FuzzGet16(ip + 6) & 0x3fff)) return;
    uint8_t* body = ip + header;
    const size_t length = total - header;
    uint32_t pseudo = FuzzGet16(ip + 12) + FuzzGet16(ip + 14) + FuzzGet16(ip + 16) + FuzzGet16(ip + 18) + ip[9] + uint32_t(length);
    if (ip[9] == 6 && length >= 20) {
        body[16] = body[17] = 0;
        FuzzSet16(body + 16, FuzzChecksum(body, length, pseudo));
    } else if (ip[9] == 17 && length >= 8) {
        body[6] = body[7] = 0;
        uint16_t sum = FuzzChecksum(body, length, pseudo);
        FuzzSet16(body + 6, sum ? sum : 0xffff);
    } else if (ip[9] == 1 && length >= 4) {
        body[2] = body[3] = 0;
        FuzzSet16(body + 2, FuzzChecksum(body, length));
    }
}
