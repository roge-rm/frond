// Channels over one link. Infrared is unreliable: a write that would take its queue past 4 KB
// is dropped whole and it goes out ahead of serial and Bluetooth data. Serial and Bluetooth are
// lossless with credit windows of 4 KB and 64 KB: the sender never has more than the window in
// flight, the receiver gives credit back as its side takes the bytes, and channels are served
// round robin so a stalled one never holds up another.
//
// Not thread-safe; the owner feeds it from one thread.
#pragma once

#include "messages.h"

#include <deque>
#include <map>

namespace frond::link {

struct ChannelEvent {
    enum class Kind {
        OpenRequest,  // the peer opened a channel: answer with accept, waiting or busy
        Accepted,     // our open was joined
        Waiting,      // our open is held until a machine there is set to this link
        Busy,         // refused, machine is the one already using it; the channel is gone
        Data,
        Closed,       // the channel is gone
        State,        // code is a PeerState
    };
    Kind kind;
    uint16_t channel = 0;
    ChannelKind channelKind = ChannelKind::Infrared;
    std::string machine;
    std::string device;
    Bytes data;
    uint8_t code = 0;
};

class Mux {
public:
    static constexpr size_t kInfraredQueue = 4096;
    static constexpr size_t kSerialWindow = 4096;
    static constexpr size_t kBluetoothWindow = 65536;
    static constexpr size_t kMaxChannels = 64;
    static size_t windowFor(ChannelKind kind);

    explicit Mux(bool initiator) : nextId_(initiator ? 1 : 2), initiator_(initiator) {}

    // Returns the channel id, 0 if no more channels can be open.
    uint16_t open(ChannelKind kind, const std::string& machine, const std::string& device);
    // Answers to an OpenRequest. waiting() holds the channel; accept() may follow later.
    bool accept(uint16_t ch, const std::string& machine, const std::string& device);
    bool waiting(uint16_t ch);
    bool busy(uint16_t ch, const std::string& machine, const std::string& device);
    void close(uint16_t ch, CloseReason reason = CloseReason::Normal);
    void sendState(uint16_t ch, PeerState state);

    // Bytes taken. Infrared takes all or nothing; serial and Bluetooth take what fits.
    size_t send(uint16_t ch, const uint8_t* p, size_t n);
    size_t writable(uint16_t ch) const;
    // The owner passed n received bytes on to its side; gives credit back.
    void consumed(uint16_t ch, size_t n);
    bool isOpen(uint16_t ch) const;
    size_t queued(uint16_t ch) const;

    // False on a protocol error; the link should drop.
    bool receive(const Message& m);
    // The next message to send, in priority order. False when there is none.
    bool next(Message& out);
    bool hasOutput() const;
    // Link-level messages (channel 0) go out first.
    void queueControl(Message m) { control_.push_back(std::move(m)); }
    // The link went down: every channel closes.
    void reset(CloseReason reason);

    std::deque<ChannelEvent> events;
    uint64_t infraredDropped() const { return irDropped_; }

private:
    enum class State { Opening, Waiting, Requested, Holding, Open };
    struct Channel {
        uint16_t id;
        ChannelKind kind;
        State state;
        Bytes queue;
        size_t queueAt = 0;
        size_t sendCredit = 0;
        size_t allowance = 0;
        size_t unconsumed = 0;
        size_t pendingCredit = 0;
        size_t pending() const { return queue.size() - queueAt; }
    };
    Channel* find(uint16_t ch);
    const Channel* find(uint16_t ch) const;
    void makeOpen(Channel& c);
    void event(ChannelEvent::Kind kind, const Channel& c);
    bool takeData(Channel& c, size_t max, Message& out);
    bool peerOwns(uint16_t ch) const { return ch != 0 && ((ch & 1) == 1) != initiator_; }

    std::map<uint16_t, Channel> channels_;
    std::deque<Message> control_;
    uint16_t nextId_;
    uint16_t lastServed_ = 0;
    bool initiator_;
    uint64_t irDropped_ = 0;
};

}  // namespace frond::link
