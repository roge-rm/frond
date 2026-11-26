#ifndef FROND_IR_IRPEER_H
#define FROND_IR_IRPEER_H

// A device at the far end of the guest's IR port that is an IrLAP secondary and loses its link
// when the guest's transceiver goes off: the mobile phone and the HotSync PC. The core feeds it the
// guest's IR transmitter bytes (Write), drains its receiver bytes (Read) and calls Service between
// slices. Single-threaded; time is guest nanoseconds.

#include <cstddef>
#include <cstdint>
#include <string>

#include "IrLap.h"

namespace frondIr {

class IrPeer {
   public:
    virtual ~IrPeer() = default;
    virtual void Write(const uint8_t* data, size_t size, uint64_t now) = 0;
    virtual size_t Read(uint8_t* data, size_t max, uint64_t now) = 0;
    virtual size_t Available(uint64_t now) const = 0;
    virtual uint64_t ReadyAt() const = 0;
    virtual void Service(uint64_t now) = 0;
    // No link, as after a session load or the transceiver going off.
    virtual void Drop(uint64_t now) = 0;
    virtual std::string Describe() const = 0;
    virtual const IrLap& Lap() const = 0;
};

}  // namespace frondIr

#endif
