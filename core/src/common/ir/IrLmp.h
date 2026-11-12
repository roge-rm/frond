#ifndef FROND_IR_IRLMP_H
#define FROND_IR_IRLMP_H

// IrLMP's multiplexer (IrLMP 1.1 section 4) on top of an IrLAP secondary: the guest connects to
// our service access points (LSAPs) and we answer. One connection at a time a local LSAP.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "IrLap.h"

namespace frondIr {

class IrLmp;

// A service bound to a local LSAP.
class LmService {
   public:
    virtual ~LmService() = default;
    // The guest's connect request with its user data; reply is the confirm's user data. False
    // refuses it.
    virtual bool Connect(const Bytes& data, Bytes& reply, uint64_t now) = 0;
    virtual void Data(const Bytes& data, uint64_t now) = 0;
    // The guest disconnected, or the link went.
    virtual void Disconnect(uint64_t now) = 0;
    // Called before each IrLAP response, to queue data.
    virtual void Pump(uint64_t now) {}
};

class IrLmp : public IrLapUser {
   public:
    // Disconnect reasons (section 4.4.2).
    static constexpr uint8_t userRequest = 0x01, unexpectedLapDisconnect = 0x02, noClient = 0x08;

    explicit IrLmp(IrLap& lap);

    void Bind(uint8_t lsap, LmService& service);
    // A data PDU from a local LSAP to its peer; size at most MaxData().
    void Send(uint8_t lsap, const uint8_t* data, size_t size);
    void Disconnect(uint8_t lsap, uint8_t reason, uint64_t now);
    bool Connected(uint8_t lsap) const { return lsap < 0x80 && ports[lsap].connected; }
    size_t MaxData() const { return lap.DataSize() - 2; }
    // I frames waiting to go; services hold back past a few.
    size_t Queued() const { return lap.Queued(); }

    unsigned Connects() const { return connects; }

    void LinkUp(uint64_t now) override;
    void LinkDown(uint64_t now) override;
    void Data(const uint8_t* data, size_t size, uint64_t now) override;
    void Pump(uint64_t now) override;

   private:
    struct Port {
        LmService* service{nullptr};
        bool connected{false};
        uint8_t peer{0};
    };

    void Control(uint8_t local, uint8_t peer, const uint8_t* data, size_t size, uint64_t now);
    void SendControl(uint8_t local, uint8_t peer, Bytes body);

    IrLap& lap;
    std::array<Port, 0x80> ports{};
    unsigned connects{0};
};

}  // namespace frondIr

#endif
