#ifndef FROND_USB_BUS_HOST_H
#define FROND_USB_BUS_HOST_H
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "UsbDevicePort.h"

// Full-speed USB host for the PXA25x/PXA27x UDC, run on emulated time by whoever calls Tick:
// connect debounce, port reset, enumeration, SET_CONFIGURATION, Palm's
// GET_EXT_CONNECTION_INFORMATION (vendor request 0x04, or 0x03 if that stalls), then
// bulk IN/OUT polling on the HotSync port, with the bytes going to and from `Link`.
// Rules kept on the bus:
// - SOF every 1 ms of emulated time. A transaction never crosses the end of a frame.
// - SETUP and OUT data reach the UDC, and an IN is taken from it, only once the whole
//   packet has been on the wire. A NAKed token is retried 10 us after the guest next
//   writes to the UDC (see Poke), or at the start of the next frame.
// - A standard control request that only gets NAKs sends its SETUP again every 100 ms,
//   as the device may have lost it, until the 500 ms deadline. The TX ROM reads a SETUP
//   and then drops it at times while it starts HotSync.
// - A control transfer starts on a frame boundary at least 1 ms after the previous one.
// - Tokens go to the host's idea of the device address; another address gets no answer.
// - An optional adapter credit query can defer bulk OUT requests before token submission.
//   Pending bytes stay owned by the host until an actual OUT ACK. Control and IN are independent.
// Nothing here is session state. Every call runs on the emulator thread.
class UsbBusHost {
   public:
    struct Link {
        std::function<size_t()> sendSpace;                       // guest to host room
        std::function<size_t(const uint8_t*, size_t)> send;      // guest to host
        std::function<size_t(uint8_t*, size_t)> receive;         // host to guest
        std::function<size_t()> pending;                         // host to guest bytes waiting
    };
    enum class Phase { detached, waitEnable, connect, reset, enumerate, running, failed };
    static constexpr uint64_t frameNs = 1000000;
    static constexpr uint64_t nakRetryNs = 10000;
    static constexpr uint64_t connectNs = 100 * frameNs;   // USB 2.0 7.1.7.3 debounce
    static constexpr uint64_t resetNs = 20 * frameNs;      // 10 ms reset, 10 ms recovery
    static constexpr uint64_t controlTimeoutNs = 500 * frameNs;
    static constexpr uint64_t setupRetryNs = 100 * frameNs;
    static constexpr uint64_t retryNs = 250 * frameNs;  // after a failed enumeration
    static constexpr unsigned maxAttempts = 5;
    static constexpr uint8_t deviceAddress = 2;

    struct Counters {
        uint64_t frames{}, transactions{}, naks{}, bytesIn{}, bytesOut{}, enumerations{}, setupRetries{}, ticks{};
    };

    UsbBusHost(std::function<UsbDevicePort()> udc, Link link);

    void Attach();     // cable in: waits for the device to listen, then resets and enumerates
    bool ReadStringDescriptor(uint8_t index, uint16_t language);
    void Detach();     // cable out: forgets the device
    void Tick(uint64_t nowNs);
    // Emulated time the next Tick is needed by: 0 for at once, UINT64_MAX for none
    // (detached, failed, or waiting for the guest to turn the UDC on).
    uint64_t NextWake() const;
    // The guest wrote to the UDC: tick soon and retry a NAKed token.
    void Poke() { poked = true; }
    // Bytes or room arrived on the link while the bridge was idle; Poke if so.
    bool LinkWaiting() const;
    Phase GetPhase() const { return phase; }
    static const char* PhaseName(Phase phase);

    uint16_t Vendor() const { return vendor; }
    uint16_t Product() const { return product; }
    const std::string& PortFunction() const { return portFunction; }
    uint8_t InEndpoint() const { return inEndpoint; }
    uint8_t OutEndpoint() const { return outEndpoint; }
    const Counters& GetCounters() const { return counters; }
    std::vector<std::string> TakeMessages() { return std::move(messages); }

    // Optional record of every transaction's bus interval [start, end) in ns.
    std::vector<std::pair<uint64_t, uint64_t>>* log{nullptr};
    // Optional completed control transfer trace; bytes are supplied by the device.
    std::function<void(const uint8_t*,const std::vector<uint8_t>&,bool)> controlTrace;

   private:
    enum class Token : uint8_t { none, setup, out0, in0, bulkIn, bulkOut };
    enum class Stage : uint8_t { setup, dataIn, dataOut, statusIn, statusOut };
    enum class Completion { enumeration, external, clearHalt };
    struct Control {
        Completion completion{Completion::enumeration};
        bool active{false};
        uint8_t setup[8]{};
        std::vector<uint8_t> out, data;
        Stage stage{Stage::setup};
        size_t sent{0};
        unsigned errors{0};
        uint64_t startAt{0}, deadline{0}, setupAt{0};
    };
    enum class Result { ok, stall, timeout, error };

    void Restart(const char* why);
    void StartControl(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint16_t length, Completion completion = Completion::enumeration);
    void ControlDone(Result result);
    void EnumerationStep(Result result);
    bool ParseConfiguration();
    bool SelectEndpoints();
    bool StartTransaction();
    void FinishTransaction();
    bool Begin(Token token, unsigned bytes);
    void Message(const std::string& text);
    void Nak();
    bool Running() const { return phase == Phase::reset || phase == Phase::enumerate || phase == Phase::running; }
    bool Addressed();

    std::function<UsbDevicePort()> udc;
    Link link;
    Phase phase{Phase::detached};
    uint64_t now{0}, phaseAt{0}, nextSof{0}, lastSof{0}, busyUntil{0}, retryAt{0}, lastControlEnd{0};
    bool sofRunning{false}, poked{false}, waitSpace{false};
    uint64_t nakAt{0};
    Token inFlight{Token::none};
    uint8_t inFlightEndpoint{0};
    unsigned attempts{0};
    uint8_t address{0}, pendingAddress{0};
    uint64_t addressAt{0};
    Control control;
    unsigned step{0};
    uint8_t maxPacket0{8}, configuration{0};
    uint16_t configLength{0}, vendor{0}, product{0};
    std::vector<uint8_t> configDescriptor;
    uint8_t endpointMps[256]{};
    uint8_t inMps{0}, outMps{0};
    std::string portFunction;
    uint8_t inEndpoint{0}, outEndpoint{0};
    // Running: the OUT packet being sent and which direction goes next.
    uint8_t outPacket[64]{};
    unsigned outSize{0}, outFlightSize{0};
    bool outPending{false}, preferOut{false}, clearHalt{false};
    uint8_t haltedEndpoint{0};
    unsigned bulkTimeouts{0};
    // Independent MPS-sized requests, resubmitted 1 ms after each ACK (including a ZLP).
    uint64_t inRequestAt{0}, outRequestAt{0};
    uint32_t enableCount{0};
    uint64_t requeryAt{0};  // the UDC's UDE count when this connection began
    Counters counters;
    std::vector<std::string> messages;
};
#endif
