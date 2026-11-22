#ifndef FROND_PALM_PRISM_FRAME_H
#define FROND_PALM_PRISM_FRAME_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace palmPrismFrame {

inline constexpr std::array<uint8_t, 6> snap{0xaa, 0xaa, 3, 0, 0, 0};
inline constexpr unsigned descriptorSize = 60;

inline std::vector<uint8_t> Receive(const std::vector<uint8_t>& frame,
                                    const std::array<uint8_t, 6>& bssid) {
    if (frame.size() < 14 || frame.size() > 1514) return {};

    std::vector<uint8_t> rx(descriptorSize, 0);
    const unsigned payload = frame.size() - 6;
    rx[1] = 0x20;                        // RFC 1042 SNAP
    rx[6] = 27; rx[7] = 80; rx[8] = 110; // Silence, signal and 11 Mbit/s
    rx[14] = 0x08; rx[15] = 0x02;        // Data, FromDS
    std::copy_n(frame.begin(), 6, rx.begin() + 18);
    std::copy(bssid.begin(), bssid.end(), rx.begin() + 24);
    std::copy_n(frame.begin() + 6, 6, rx.begin() + 30);
    rx[44] = payload;
    rx[45] = payload >> 8;
    std::copy_n(frame.begin(), 12, rx.begin() + 46);
    rx[58] = payload >> 8;
    rx[59] = payload;
    rx.insert(rx.end(), snap.begin(), snap.end());
    rx.insert(rx.end(), frame.begin() + 12, frame.end());
    if (rx.size() & 1) rx.push_back(0);
    return rx;
}

inline std::vector<uint8_t> Transmit(const std::vector<uint8_t>& bytes,
                                     const std::array<uint8_t, 6>& mac) {
    if (bytes.size() < descriptorSize + 8) return {};
    const unsigned payload = bytes[44] | (unsigned(bytes[45]) << 8);
    if (payload < 8 || payload > 1508 || payload > bytes.size() - descriptorSize ||
        !std::equal(snap.begin(), snap.end(), bytes.begin() + descriptorSize)) return {};

    // With ToDS set, address3 holds the Ethernet destination.
    std::vector<uint8_t> frame(bytes.begin() + 30, bytes.begin() + 36);
    frame.insert(frame.end(), mac.begin(), mac.end());
    frame.insert(frame.end(), bytes.begin() + descriptorSize + snap.size(),
                 bytes.begin() + descriptorSize + payload);
    return frame;
}

}  // namespace palmPrismFrame

#endif
