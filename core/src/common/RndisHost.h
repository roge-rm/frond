#ifndef FROND_RNDIS_HOST_H
#define FROND_RNDIS_HOST_H
// The host side of a USB RNDIS function (Remote NDIS 1.0, as Windows and Linux's rndis_host run it):
// control messages go out with SEND_ENCAPSULATED_COMMAND and come back with GET_ENCAPSULATED_RESPONSE
// once the interrupt endpoint says RESPONSE_AVAILABLE, and Ethernet frames go both ways on the bulk
// endpoints wrapped in REMOTE_NDIS_PACKET_MSG. Bring-up is INIT, the MAC address and maximum frame
// size queries and the packet filter; a KEEPALIVE goes out after 5 s with nothing from the device,
// and the device's own KEEPALIVE is answered. UsbBusHost moves the bytes; this only
// builds and parses messages. Single-threaded; time is guest nanoseconds. Nothing here is session state.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

class RndisHost {
   public:
    enum : uint32_t {
        packetMsg = 0x00000001,
        initMsg = 0x00000002,
        haltMsg = 0x00000003,
        queryMsg = 0x00000004,
        setMsg = 0x00000005,
        resetMsg = 0x00000006,
        indicateStatusMsg = 0x00000007,
        keepaliveMsg = 0x00000008,
        completion = 0x80000000,
    };
    enum : uint32_t {
        oidMaximumFrameSize = 0x00010106,
        oidCurrentPacketFilter = 0x0001010e,
        oidMediaConnectStatus = 0x00010114,
        oidPermanentAddress = 0x01010101,
    };
    // Directed, multicast and broadcast, as Windows sets it.
    static constexpr uint32_t packetFilter = 0x0000000b;
    // The largest bulk IN transfer this host takes, sent in INIT as MaxTransferSize.
    static constexpr uint32_t maxTransfer = 0x4000;
    static constexpr uint64_t responseWaitNs = 100000000;     // GET without a notification after this
    static constexpr uint64_t commandTimeoutNs = 5000000000;  // no completion: start again
    static constexpr uint64_t keepaliveNs = 5000000000;
    enum class State { idle, init, query, up, failed };

    // The device was configured: starts with INIT. inPacketSize is the bulk IN max packet size.
    void Start(unsigned inPacketSize, uint64_t now);
    void Stop();
    State GetState() const { return state; }
    bool Up() const { return state == State::up; }
    // The device's MAC address (valid once up).
    const uint8_t* Mac() const { return mac; }
    uint32_t MaxFrame() const { return maxFrame; }

    // Control channel. NextCommand pops a message for SEND_ENCAPSULATED_COMMAND; CommandSent reports how
    // it went. WantResponse says a GET_ENCAPSULATED_RESPONSE is due; Response takes its data (or a failure).
    bool NextCommand(std::vector<uint8_t>& message, uint64_t now);
    void CommandSent(bool ok, uint64_t now);
    bool WantResponse(uint64_t now) const;
    void Response(bool ok, const uint8_t* data, size_t size, uint64_t now);
    // One packet from the interrupt endpoint.
    void Notification(const uint8_t* data, size_t size, uint64_t now);
    // When the host next needs to act by itself (a response wait or a keepalive), UINT64_MAX for never.
    uint64_t NextWake() const;

    // Bulk IN, one packet at a time: a packet shorter than the max packet size ends the transfer.
    void BulkInPacket(const uint8_t* data, size_t size);
    // One whole bulk IN transfer: one or more messages back to back.
    void BulkInTransfer(const uint8_t* data, size_t size);
    // Bulk OUT, one packet at a time: the next packet of at most `max` bytes, a zero-length one ending a
    // transfer that fills its last packet. False with nothing to send.
    bool BulkOutPacket(uint8_t* data, unsigned max, unsigned& size);
    // A frame for the guest, queued as a PACKET_MSG while up (dropped otherwise, or when 64 are waiting).
    void FrameToDevice(const uint8_t* frame, size_t size);
    bool OutPending() const { return outOffset < outTransfer.size() || outZlp || !outQueue.empty(); }
    // Frames from the guest.
    std::function<void(const uint8_t*, size_t)> frameFromDevice;

    // Message builders and parsers, public for tests.
    static std::vector<uint8_t> PacketMessage(const uint8_t* frame, size_t size);
    static std::vector<uint8_t> InitMessage(uint32_t requestId);
    static std::vector<uint8_t> QueryMessage(uint32_t requestId, uint32_t oid, uint32_t bufferLength);
    static std::vector<uint8_t> SetMessage(uint32_t requestId, uint32_t oid, const uint8_t* data, size_t size);
    static std::vector<uint8_t> KeepaliveMessage(uint32_t requestId);
    static std::vector<uint8_t> KeepaliveComplete(uint32_t requestId);

    struct Counters {
        uint64_t framesIn{}, framesOut{}, dropped{}, badMessages{}, keepalives{}, commands{}, responses{};
        uint32_t ups{};  // times the link came up, kept over Stop: a new one means a new link
    };
    const Counters& GetCounters() const { return counters; }
    std::vector<std::string> TakeMessages() { return std::move(messages); }

   private:
    void Send(std::vector<uint8_t> message, uint64_t now, bool wantsReply = true);
    void Next(uint64_t now);
    void Fail(const std::string& why);
    void Message(const std::string& text);
    void HandleControl(const uint8_t* data, size_t size, uint64_t now);

    State state{State::idle};
    unsigned step{0}, attempts{0};
    uint32_t requestId{0}, waitingId{0};
    uint8_t mac[6]{};
    uint32_t maxFrame{1514};
    unsigned inPacket{64};
    // Control channel: one command at a time; its reply (or a device message) is fetched after a notification.
    std::deque<std::vector<uint8_t>> commands;
    bool commandOut{false}, awaiting{false}, notified{false};
    uint64_t sentAt{0}, lastHeard{0}, keepaliveAt{UINT64_MAX};
    // Bulk.
    std::vector<uint8_t> inTransfer;
    bool inDiscard{false};
    std::deque<std::vector<uint8_t>> outQueue;
    std::vector<uint8_t> outTransfer;
    size_t outOffset{0};
    bool outZlp{false};
    Counters counters;
    std::vector<std::string> messages;
};
#endif
