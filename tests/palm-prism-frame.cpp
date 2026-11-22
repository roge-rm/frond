#include <cassert>

#include "PalmPrismFrame.h"

int main() {
    constexpr std::array<uint8_t, 6> destination{2, 3, 5, 7, 11, 13};
    constexpr std::array<uint8_t, 6> source{2, 17, 19, 23, 29, 31};
    constexpr std::array<uint8_t, 6> bssid{2, 0, 0, 0, 0, 2};

    for (unsigned size : {14u, 15u, 1513u, 1514u}) {
        std::vector<uint8_t> ethernet(size, 0x9a);
        std::copy(destination.begin(), destination.end(), ethernet.begin());
        std::copy(source.begin(), source.end(), ethernet.begin() + 6);
        ethernet[12] = 8;
        ethernet[13] = 6;

        auto rx = palmPrismFrame::Receive(ethernet, bssid);
        const unsigned payload = size - 6;
        assert(rx.size() == 60 + payload + (payload & 1));
        assert(rx[1] == 0x20 && rx[14] == 0x08 && rx[15] == 0x02);
        assert(std::equal(destination.begin(), destination.end(), rx.begin() + 18));
        assert(std::equal(bssid.begin(), bssid.end(), rx.begin() + 24));
        assert(std::equal(source.begin(), source.end(), rx.begin() + 30));
        assert((rx[44] | (unsigned(rx[45]) << 8)) == payload);
        assert(((unsigned(rx[58]) << 8) | rx[59]) == payload);
        assert(std::equal(palmPrismFrame::snap.begin(), palmPrismFrame::snap.end(), rx.begin() + 60));
        assert(std::equal(ethernet.begin() + 12, ethernet.end(), rx.begin() + 66));
        if (payload & 1) assert(rx.back() == 0);

        // The driver's transmit descriptor puts the destination in address3.
        auto tx = rx;
        tx[15] = 0x01;
        std::copy(destination.begin(), destination.end(), tx.begin() + 30);
        assert(palmPrismFrame::Transmit(tx, source) == ethernet);
        tx[60] ^= 1;
        assert(palmPrismFrame::Transmit(tx, source).empty());
        tx[60] ^= 1;
        tx.resize(60 + payload - 1);
        assert(palmPrismFrame::Transmit(tx, source).empty());
    }

    assert(palmPrismFrame::Receive(std::vector<uint8_t>(13), bssid).empty());
    assert(palmPrismFrame::Receive(std::vector<uint8_t>(1515), bssid).empty());
    assert(palmPrismFrame::Transmit(std::vector<uint8_t>(67), source).empty());
}
