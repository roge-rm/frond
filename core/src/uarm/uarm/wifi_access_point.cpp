#include "wifi_access_point.h"

#include <algorithm>
#include <cstring>

// Locally administered, "FrAP".
const uint8_t WifiAccessPoint::bssid[6] = {0x02, 0x46, 0x72, 0x41, 0x50, 0x01};
const char WifiAccessPoint::ssid[] = "Frond";

namespace {
    const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    // 1 and 2 Mbit/s basic, 5.5 and 11.
    const uint8_t rates[] = {0x82, 0x84, 0x0b, 0x16};
    const uint8_t snap[6] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00};
    // Frame control byte 0: type and subtype.
    enum : uint8_t {
        fcAssocRequest = 0x00, fcAssocResponse = 0x10, fcReassocRequest = 0x20, fcReassocResponse = 0x30,
        fcProbeRequest = 0x40, fcProbeResponse = 0x50, fcBeacon = 0x80, fcDisassoc = 0xa0, fcAuth = 0xb0,
        fcDeauth = 0xc0, fcData = 0x08,
    };
    // Frame control byte 1.
    constexpr uint8_t toDs = 0x01, fromDs = 0x02, protectedFrame = 0x40;
    void put16(std::vector<uint8_t>& b, uint16_t v) {
        b.push_back(uint8_t(v));
        b.push_back(uint8_t(v >> 8));
    }
}

void WifiAccessPoint::reset() {
    station = idle;
    std::memset(stationMac, 0, 6);
    sequence = 0;
    nextBeacon = 0;
    queue.clear();
}

std::vector<uint8_t> WifiAccessPoint::header(uint8_t type, const uint8_t* to, const uint8_t* from, uint8_t flags) {
    std::vector<uint8_t> f = {type, flags, 0, 0};
    f.insert(f.end(), to, to + 6);
    // Management frames and frames from the DS: address 2 is the BSSID.
    f.insert(f.end(), bssid, bssid + 6);
    f.insert(f.end(), from, from + 6);
    put16(f, uint16_t(sequence++ << 4));
    return f;
}

std::vector<uint8_t> WifiAccessPoint::beaconFrame(uint8_t type, const uint8_t* to, uint64_t now) {
    auto f = header(type, to, bssid, 0);
    const uint64_t tsf = now / 1000;
    for (int i = 0; i < 8; ++i) f.push_back(uint8_t(tsf >> (8 * i)));
    put16(f, 100);
    // ESS, short preamble.
    put16(f, 0x0021);
    const size_t ssidLength = std::strlen(ssid);
    f.push_back(0);
    f.push_back(uint8_t(ssidLength));
    f.insert(f.end(), ssid, ssid + ssidLength);
    f.push_back(1);
    f.push_back(sizeof rates);
    f.insert(f.end(), rates, rates + sizeof rates);
    f.push_back(3);
    f.push_back(1);
    f.push_back(apChannel);
    if (type == fcBeacon) {
        // TIM: DTIM count 0, period 1, nothing buffered.
        const uint8_t tim[] = {5, 4, 0, 1, 0, 0};
        f.insert(f.end(), tim, tim + sizeof tim);
    }
    return f;
}

void WifiAccessPoint::send(std::vector<uint8_t> frame, uint64_t at) {
    if (queue.size() >= 64) return;
    queue.push_back({at, std::move(frame)});
}

void WifiAccessPoint::tuned(unsigned channel, uint64_t now) {
    if (channel == apChannel && toStation) {
        auto f = beaconFrame(fcBeacon, broadcast, now);
        toStation(f.data(), f.size(), apChannel, now);
        ++beacons;
    }
}

void WifiAccessPoint::stationFrame(const uint8_t* f, size_t size, unsigned channel, uint64_t now) {
    if (channel != apChannel || size < 24) return;
    const uint8_t type = f[0] & 0xfc, flags = f[1];
    const uint8_t* addr1 = f + 4;
    const uint8_t* source = f + 10;
    const bool forUs = !std::memcmp(addr1, bssid, 6);
    const uint64_t at = now + replyNs;
    switch (type) {
        case fcProbeRequest: {
            // Answer a wildcard SSID or ours.
            const uint8_t* ie = f + 24;
            const size_t left = size - 24;
            bool match = true;
            if (left >= 2 && ie[0] == 0 && ie[1] && size_t(ie[1]) + 2 <= left)
                match = ie[1] == std::strlen(ssid) && !std::memcmp(ie + 2, ssid, ie[1]);
            if (match) send(beaconFrame(fcProbeResponse, source, at), at);
            return;
        }
        case fcAuth: {
            if (!forUs || size < 30) return;
            const uint16_t algorithm = uint16_t(f[24] | f[25] << 8), seq = uint16_t(f[26] | f[27] << 8);
            if (seq != 1) return;
            auto r = header(fcAuth, source, bssid, 0);
            put16(r, algorithm);
            put16(r, 2);
            // Open system only: shared key gets "algorithm not supported".
            put16(r, algorithm == 0 ? 0 : 13);
            if (algorithm == 0) {
                std::memcpy(stationMac, source, 6);
                station = authenticatedState;
            }
            send(std::move(r), at);
            return;
        }
        case fcAssocRequest:
        case fcReassocRequest: {
            if (!forUs) return;
            const bool known = station != idle && !std::memcmp(stationMac, source, 6);
            auto r = header(type == fcAssocRequest ? fcAssocResponse : fcReassocResponse, source, bssid, 0);
            put16(r, 0x0021);
            // Status 0, or 9 "not authenticated".
            put16(r, known ? 0 : 9);
            put16(r, 0xc001);
            r.push_back(1);
            r.push_back(sizeof rates);
            r.insert(r.end(), rates, rates + sizeof rates);
            if (known) station = associatedState;
            send(std::move(r), at);
            return;
        }
        case fcDeauth:
        case fcDisassoc:
            if (forUs && !std::memcmp(stationMac, source, 6)) station = type == fcDeauth ? idle : authenticatedState;
            return;
        case fcData: {
            // Data to the DS from the associated station: 802.11 + LLC/SNAP to Ethernet II.
            if (!(flags & toDs) || (flags & fromDs) || !forUs || !associated() || std::memcmp(stationMac, source, 6))
                return;
            if (flags & protectedFrame) return;
            if (size < 24 + 8 || std::memcmp(f + 24, snap, 6)) return;
            std::vector<uint8_t> e(f + 16, f + 22);
            e.insert(e.end(), source, source + 6);
            e.insert(e.end(), f + 30, f + size);
            ++dataUp;
            if (ethernetOut && e.size() >= 14 && e.size() <= 1518) ethernetOut(e.data(), e.size());
            return;
        }
        default:
            return;
    }
}

void WifiAccessPoint::ethernetIn(const uint8_t* e, size_t size, uint64_t now) {
    if (size < 14 || !associated()) return;
    // Unicast only to the station; group frames go to it too.
    if (!(e[0] & 1) && std::memcmp(e, stationMac, 6)) return;
    auto f = header(fcData, e, e + 6, fromDs);
    f.insert(f.end(), snap, snap + 6);
    f.insert(f.end(), e + 12, e + size);
    ++dataDown;
    send(std::move(f), now);
}

void WifiAccessPoint::advance(uint64_t now, bool listening) {
    if (listening && now >= nextBeacon) {
        if (nextBeacon) tuned(apChannel, now);
        nextBeacon = (now / beaconNs + 1) * beaconNs;
    }
    while (!queue.empty() && queue.front().at <= now) {
        auto f = std::move(queue.front().frame);
        queue.pop_front();
        if (toStation) toStation(f.data(), f.size(), apChannel, now);
    }
}

uint64_t WifiAccessPoint::nextEvent(bool listening) const {
    uint64_t at = UINT64_MAX;
    if (listening) at = nextBeacon;
    if (!queue.empty()) at = std::min(at, queue.front().at);
    return at;
}
