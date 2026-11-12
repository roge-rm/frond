#ifndef FROND_IR_IRLAP_H
#define FROND_IR_IRLAP_H

// An IrLAP secondary station (IrDA IrLAP 1.1) over SIR: it answers the guest's discovery, takes
// its SNRM with negotiated parameters, and then sends only when the guest polls it, in normal
// response mode. The guest is always the primary. Fed raw SIR bytes from the guest's IR
// transmitter and drained of raw SIR bytes for its receiver. Single-threaded; every time is
// guest nanoseconds.

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "Sir.h"

namespace frondIr {

// The layer above IrLAP (IrLMP).
class IrLapUser {
   public:
    virtual ~IrLapUser() = default;
    // The link is up: the UA to the guest's SNRM is queued.
    virtual void LinkUp(uint64_t now) = 0;
    // The link is gone, by DISC, a new SNRM, the watchdog or Drop.
    virtual void LinkDown(uint64_t now) = 0;
    // The information field of an I frame in sequence.
    virtual void Data(const uint8_t* data, size_t size, uint64_t now) = 0;
    // Called before each response so the layers above can queue what they have.
    virtual void Pump(uint64_t now) {}
};

class IrLap {
   public:
    struct Options {
        std::array<uint8_t, 4> address{{0x46, 0x72, 0x6f, 0x6e}};
        // Discovery service hints, two bytes (IrLMP 1.1 section 3.4.1).
        std::array<uint8_t, 2> hints{{0x90, 0x05}};
        std::string nickname = "Frond Phone";
        // Baud rates offered in the UA, as the IrLAP bit mask (bit 5 is 115200).
        uint8_t baudMask = 0x3f;
        // Answer a poll for a link we don't have with DM. The Dana keeps polling through DM answers
        // for ever, while silence lets its own disconnect time end the link.
        bool answerDm = true;
        // A link that has carried no print data (see LinkBusy) for this long ends without a word, 0
        // for never. A guest restored mid-print can hold a link and an LSAP it never sends on; a link
        // that has carried data stays up through polls.
        uint64_t idleNs = 0;
    };

    // Negotiated link parameters in use (IrLAP 1.1 section 6.6).
    struct Link {
        unsigned baud{9600};
        // What the guest takes from us: information bytes a frame and frames a turn.
        unsigned dataSize{64}, window{1};
        // The guest's turnaround needs, and the extra BOFs it wants at this baud.
        uint64_t minTurnNs{10000000}, maxTurnNs{500000000};
        unsigned extraBofs{10};
        // No frame from the guest for this long ends the link.
        uint64_t disconnectNs{3000000000ull};
    };

    enum class State { Ndm, Nrm };

    explicit IrLap(IrLapUser& user);
    IrLap(IrLapUser& user, Options options);

    // Raw SIR bytes from the guest.
    void Receive(const uint8_t* data, size_t size, uint64_t now);
    // Raw SIR bytes for the guest; a response is held until the guest's minimum turnaround time
    // after the frame that polled for it.
    size_t Read(uint8_t* data, size_t max, uint64_t now);
    size_t Available(uint64_t now) const;
    // When bytes queued for the guest can be read, or UINT64_MAX with none queued.
    uint64_t ReadyAt() const { return out.size() == outPos ? UINT64_MAX : holdUntil; }
    void Service(uint64_t now);
    // After a session load: back to NDM with no link and nothing queued.
    void Drop(uint64_t now);

    // Queues an I frame's information field; sent on a later poll. Never larger than DataSize().
    void Send(Bytes info);
    size_t Queued() const { return queue.size(); }
    // The layer above can't take more data: polls get RNR until it clears.
    void SetBusy(bool busy) { localBusy = busy; }

    // The layer above has taken real data on this link, so the idle time no longer ends it.
    void LinkBusy() { dataSeen = true; }

    State GetState() const { return state; }
    bool Connected() const { return state == State::Nrm; }
    const Link& GetLink() const { return link; }
    unsigned DataSize() const { return link.dataSize; }
    const std::array<uint8_t, 4>& Guest() const { return guest; }
    const std::string& GuestNickname() const { return guestNickname; }

    // Counters for tests and the transport's stats.
    struct Stats {
        unsigned discoveries{0}, xidReplies{0}, connects{0}, disconnects{0}, timeouts{0};
        unsigned framesIn{0}, framesOut{0}, outOfSequence{0}, rejects{0}, resent{0};
    };
    const Stats& GetStats() const { return stats; }
    unsigned BadFrames() const { return unwrapper.BadFrames(); }

   private:
    struct Sent {
        uint8_t ns;
        Bytes info;
    };

    void Frame(const Bytes& frame, uint64_t now);
    void Discovery(const Bytes& frame, uint64_t now);
    void Snrm(const Bytes& frame, uint64_t now);
    void Test(const Bytes& frame, uint8_t address, uint64_t now);
    void Nrm(const Bytes& frame, uint64_t now);
    void Acknowledge(uint8_t nr);
    void Respond(uint64_t now);
    void Emit(const Bytes& frame, uint64_t now);
    // Back to NDM; unread output goes too unless keepOutput (the UA to a DISC).
    void Down(uint64_t now, bool keepOutput = false);
    void ResetSequence();
    unsigned ExtraBofs() const;

    IrLapUser& user;
    Options options;
    State state{State::Ndm};
    SirUnwrapper unwrapper;

    // Discovery: the guest's address and the slot we answer in, once per discovery.
    std::array<uint8_t, 4> guest{};
    std::string guestNickname;
    int ourSlot{-1}, lastSlot{-1};
    bool answered{false};

    // Normal response mode.
    uint8_t caddr{0};
    Link link;
    uint8_t vs{0}, vr{0};
    std::deque<Sent> window;
    std::deque<Bytes> queue;
    bool localBusy{false}, remoteBusy{false}, rejected{false};
    uint64_t lastHeard{0};
    // When the link came up, and whether print data has reached the printer on it since.
    uint64_t lastData{0};
    bool dataSeen{false};

    std::vector<uint8_t> out;
    size_t outPos{0};
    uint64_t holdUntil{0};
    Stats stats;
};

}  // namespace frondIr

#endif
