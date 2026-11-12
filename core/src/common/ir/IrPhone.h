#ifndef FROND_IR_IRPHONE_H
#define FROND_IR_IRPHONE_H

// A mobile phone with a data modem, reached over infrared: the IrLAP secondary, IrLMP, IAS,
// Tiny TP and IrCOMM, with the IrCOMM port joined to a HayesModem whose calls go to a
// ModemLine. The core feeds it the guest's IR transmitter bytes (Write), drains its receiver
// bytes (Read) and calls Service between slices. Single-threaded; time is guest nanoseconds.

#include <cstddef>
#include <cstdint>
#include <string>

#include "Ias.h"
#include "IrComm.h"
#include "IrLap.h"
#include "IrLmp.h"
#include "modem/HayesModem.h"

namespace frondIr {

class IrPhone : private IrCommPort {
   public:
    struct Options {
        IrLap::Options lap;
        frondModem::HayesModem::Options modem;
    };

    explicit IrPhone(frondModem::ModemLine& line);
    IrPhone(frondModem::ModemLine& line, Options options);

    void Write(const uint8_t* data, size_t size, uint64_t now) { lap.Receive(data, size, now); }
    size_t Read(uint8_t* data, size_t max, uint64_t now) { return lap.Read(data, max, now); }
    size_t Available(uint64_t now) const { return lap.Available(now); }
    uint64_t ReadyAt() const { return lap.ReadyAt(); }
    void Service(uint64_t now);
    // After a session load: no link, no call.
    void Drop(uint64_t now) { lap.Drop(now); }
    // For a session save: the modem's settings while a call is up or being dialled, or empty.
    std::string SavedCall() const { return modem.SavedCall(); }
    // After loading a session saved mid-call: the call has dropped. When the guest next opens
    // IrCOMM the modem has the saved settings and says NO CARRIER, and the first DCE line settings
    // carry a CD drop. False, and nothing kept, for text SavedCall didn't write.
    bool CallDropped(const std::string& saved);
    // phone idle, phone linked (IrLAP up), phone dialling or phone online SPEED.
    std::string Describe() const;

    const IrLap& Lap() const { return lap; }
    const IrComm& Comm() const { return comm; }
    const IasServer& Ias() const { return ias; }
    const frondModem::HayesModem& Modem() const { return modem; }

   private:
    void Opened(uint64_t now) override;
    void Closed(uint64_t now) override;
    void FromGuest(const uint8_t* data, size_t size, uint64_t now) override;
    size_t ToGuest(uint8_t* data, size_t max, uint64_t now) override;
    void GuestLines(bool dtr, bool rts, uint64_t now) override;
    uint8_t Lines(uint64_t now) override;
    uint8_t OpenedDeltas(uint64_t now) override;

    frondModem::HayesModem modem;
    IrLmp lmp;
    IrLap lap;
    IasServer ias;
    IrComm comm;
    // A call dropped while the guest wasn't linked, told on the next IrCOMM open.
    std::string droppedCall;
    bool cdDropped{false};
};

}  // namespace frondIr

#endif
