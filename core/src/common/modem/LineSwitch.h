#ifndef FROND_MODEM_LINE_SWITCH_H
#define FROND_MODEM_LINE_SWITCH_H

// Where a modem call goes. A number ending in the HotSync number reaches a PipeLine, raw bytes
// the core passes to a host link for pilot-link (Modem Sync); any other number reaches the
// data line (the PPP server). Single-threaded, on the modem's clock.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

#include "HayesModem.h"

namespace frondModem {

// HOTSYNC on a phone keypad.
inline constexpr const char* kHotSyncNumber = "4687962";

// A far end whose bytes the owner moves to and from somewhere else.
class PipeLine : public ModemLine {
   public:
    static constexpr size_t kQueueBytes = 65536;
    // Once the far end has gone, bytes still on their way to the guest are dropped if the modem
    // takes none of them for this long (in online command mode it takes none).
    static constexpr uint64_t kDrainStallNs = 1000000000;

    // Someone is there to answer; with nobody the number gives NO ANSWER.
    void SetReachable(bool on) { reachable = on; }
    bool Reachable() const { return reachable; }
    bool InCall() const { return inCall; }
    // The far end went away during the call: the modem reports NO CARRIER once the bytes it
    // sent before going have reached the guest, or after kDrainStallNs with none taken.
    void PeerGone() { carrier = false; }

    // Bytes the guest sent, for the owner to pass on.
    size_t TakeOut(uint8_t* data, size_t max) {
        const size_t n = std::min(max, out.size());
        std::copy_n(out.begin(), n, data);
        out.erase(out.begin(), out.begin() + long(n));
        return n;
    }
    size_t OutAvailable() const { return out.size(); }
    // Bytes for the guest, from the owner. Returns how many fit.
    size_t PutIn(const uint8_t* data, size_t size) {
        const size_t n = std::min(size, kQueueBytes - std::min(kQueueBytes, in.size()));
        in.insert(in.end(), data, data + n);
        return n;
    }
    size_t InRoom() const { return kQueueBytes - std::min(kQueueBytes, in.size()); }

    void Answer(uint64_t now) override {
        drainMark = now;
        inCall = true;
        carrier = true;
        in.clear();
        out.clear();
    }
    void HangUp() override {
        inCall = false;
        in.clear();
        out.clear();
    }
    void FromModem(const uint8_t* data, size_t size, uint64_t) override {
        const size_t n = std::min(size, kQueueBytes - std::min(kQueueBytes, out.size()));
        out.insert(out.end(), data, data + n);
    }
    size_t ToModem(uint8_t* data, size_t max, uint64_t now) override {
        const size_t n = std::min(max, in.size());
        std::copy_n(in.begin(), n, data);
        in.erase(in.begin(), in.begin() + long(n));
        if (n) drainMark = now;
        return n;
    }
    // Called by the modem before Carrier while a call is up.
    void Service(uint64_t now) override {
        if (carrier || in.empty()) drainMark = now;
        else if (now - drainMark >= kDrainStallNs) in.clear();
    }
    bool Carrier(uint64_t) const override { return carrier || !in.empty(); }

   private:
    bool reachable{false}, inCall{false}, carrier{true};
    uint64_t drainMark{0};
    std::deque<uint8_t> in, out;
};

// Picks the far end when a number is dialled and passes the call to it.
class LineSwitch : public ModemLine {
   public:
    LineSwitch(ModemLine& data, PipeLine& hotsync) : data(data), hotsync(hotsync) {}

    static bool IsHotSync(const std::string& digits) {
        const std::string number = kHotSyncNumber;
        return digits.size() >= number.size() && digits.compare(digits.size() - number.size(), number.size(), number) == 0;
    }
    bool HotSyncCall() const { return active == &hotsync; }

    bool Route(const std::string& digits) override {
        if (IsHotSync(digits)) {
            active = &hotsync;
            return hotsync.Reachable();
        }
        active = &data;
        return data.Route(digits);
    }
    void Answer(uint64_t now) override { active->Answer(now); }
    void HangUp() override { active->HangUp(); }
    void FromModem(const uint8_t* bytes, size_t size, uint64_t now) override { active->FromModem(bytes, size, now); }
    size_t ToModem(uint8_t* bytes, size_t max, uint64_t now) override { return active->ToModem(bytes, max, now); }
    bool Carrier(uint64_t now) const override { return active->Carrier(now); }
    void Service(uint64_t now) override { active->Service(now); }

   private:
    ModemLine& data;
    PipeLine& hotsync;
    ModemLine* active{&data};
};

}  // namespace frondModem

#endif
