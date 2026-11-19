#ifndef FROND_WIFI_ACCESS_POINT_H
#define FROND_WIFI_ACCESS_POINT_H

// An 802.11b access point for an emulated Wi-Fi chip that sends and takes 802.11 frames (the rx1950's ACX100):
// open network "Frond" on channel 6 with beacons every 100 TU, probe responses, open system authentication and
// association, and a bridge between the station's data frames and Ethernet II frames (ethernetOut and
// ethernetIn, which the owner connects to Frond's host network). No WEP or WPA. Frames to the station wait
// replyNs so they come after the frame that asked for them. Time is guest nanoseconds.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

class WifiAccessPoint {
   public:
    static constexpr unsigned apChannel = 6;
    static constexpr uint64_t beaconNs = 102400000, replyNs = 300000;
    static const uint8_t bssid[6];
    static const char ssid[];

    void reset();
    // A frame the station sent on a channel.
    void stationFrame(const uint8_t* frame, size_t size, unsigned channel, uint64_t now);
    // The station's receiver tuned to a channel during a scan: a beacon goes out at once on ours.
    void tuned(unsigned channel, uint64_t now);
    // Sends what's due by now. nextEvent is when the next frame is due while the station listens (listening
    // says whether it does; beacons stop while it doesn't).
    void advance(uint64_t now, bool listening);
    uint64_t nextEvent(bool listening) const;
    // Frames to the station: the 802.11 frame and the channel; false when the station didn't take it.
    std::function<bool(const uint8_t* frame, size_t size, unsigned channel, uint64_t now)> toStation;
    // Ethernet II frames between the associated station and the network.
    std::function<void(const uint8_t* frame, size_t size)> ethernetOut;
    void ethernetIn(const uint8_t* frame, size_t size, uint64_t now);
    bool associated() const { return station == associatedState; }
    const uint8_t* stationAddress() const { return stationMac; }

    uint64_t beacons = 0, dataUp = 0, dataDown = 0;

    template <class A> void state(A& a) {
        a.Do32(station).DoBuffer(stationMac, 6).Do16(sequence).Do64(nextBeacon).Do64(beacons).Do64(dataUp).Do64(dataDown);
        uint32_t count = uint32_t(queue.size());
        a.Do32(count);
        a.Require(count <= 64, "access point queue");
        queue.resize(count <= 64 ? count : 0);
        for (auto& q : queue) a.Do64(q.at).DoVector(q.frame, 2400);
    }

   private:
    enum : uint32_t { idle, authenticatedState, associatedState };
    uint32_t station = idle;
    uint8_t stationMac[6] = {};
    uint16_t sequence = 0;
    uint64_t nextBeacon = 0;
    struct Pending {
        uint64_t at;
        std::vector<uint8_t> frame;
    };
    std::deque<Pending> queue;
    std::vector<uint8_t> header(uint8_t type, const uint8_t* to, const uint8_t* from, uint8_t flags);
    std::vector<uint8_t> beaconFrame(uint8_t type, const uint8_t* to, uint64_t now);
    void send(std::vector<uint8_t> frame, uint64_t at);
};

#endif
