#ifndef FROND_IR_IRCOMM_H
#define FROND_IR_IRCOMM_H

// IrCOMM (IrDA IrCOMM 1.0) as the DCE end of the guest's virtual serial port: 9-wire and 3-wire
// cooked over Tiny TP, and 3-wire raw straight on IrLMP. Publishes itself in IAS as
// "IrDA:IrCOMM" and the device as "Device".

#include <cstddef>
#include <cstdint>
#include <string>

#include "Ias.h"
#include "IrLmp.h"
#include "TinyTp.h"

namespace frondIr {

// What sits on the far end of the virtual serial port.
class IrCommPort {
   public:
    // DCE line bits, as in IrCOMM's DCE settings parameter.
    static constexpr uint8_t cts = 0x10, dsr = 0x20, ri = 0x40, cd = 0x80;

    virtual ~IrCommPort() = default;
    virtual void Opened(uint64_t now) = 0;
    virtual void Closed(uint64_t now) = 0;
    virtual void FromGuest(const uint8_t* data, size_t size, uint64_t now) = 0;
    virtual size_t ToGuest(uint8_t* data, size_t max, uint64_t now) = 0;
    virtual void GuestLines(bool dtr, bool rts, uint64_t now) {}
    // CTS, DSR, RI and CD now.
    virtual uint8_t Lines(uint64_t now) = 0;
    // Delta bits for the first DCE line settings after Opened, for lines that changed while the
    // port was closed.
    virtual uint8_t OpenedDeltas(uint64_t now) { return 0; }
};

class IrComm : public TinyTpUser {
   public:
    static constexpr uint8_t cookedLsap = 0x02, rawLsap = 0x03;
    // Service types (IrCOMM section 4.5.2.1).
    static constexpr uint8_t threeWireRaw = 0x01, threeWire = 0x02, nineWire = 0x04;
    enum class Mode { Closed, Cooked, Raw };

    IrComm(IrLmp& lmp, IasServer& ias, IrCommPort& port, const std::string& deviceName);

    Mode GetMode() const { return mode; }
    // The service type the guest asked for, 0 until it says.
    uint8_t ServiceType() const { return serviceType; }
    bool GuestDtr() const { return dtr; }
    uint64_t BytesFromGuest() const { return fromGuest; }
    uint64_t BytesToGuest() const { return toGuest; }

    bool Connect(const Bytes& data, Bytes& reply, uint64_t now) override;
    void Data(const Bytes& sdu, uint64_t now) override;
    void Disconnect(uint64_t now) override;
    void Pump(uint64_t now) override;

   private:
    // 3-wire raw: IrLMP data is the serial data.
    class Raw : public LmService {
       public:
        explicit Raw(IrComm& comm) : comm(comm) {}
        bool Connect(const Bytes& data, Bytes& reply, uint64_t now) override;
        void Data(const Bytes& data, uint64_t now) override;
        void Disconnect(uint64_t now) override;
        void Pump(uint64_t now) override;

       private:
        IrComm& comm;
    };

    void Open(Mode next, uint64_t now);
    void Parameters(const uint8_t* data, size_t size, uint64_t now);
    // Queues a DCE line settings parameter, merged into one already waiting.
    void QueueLines(uint8_t value);

    IrLmp& lmp;
    IrCommPort& port;
    TinyTp tinyTp;
    Raw raw;
    Mode mode{Mode::Closed};
    uint8_t serviceType{0};
    bool dtr{false}, rts{false};
    uint8_t lines{0};
    // Control parameters for the next SDU: at most one DCE line settings (3 bytes).
    Bytes pending;
    uint64_t fromGuest{0}, toGuest{0};
};

}  // namespace frondIr

#endif
