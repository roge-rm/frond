#ifndef FROND_IR_IRSYNC_H
#define FROND_IR_IRSYNC_H

// A PC with an IR port running HotSync, for the HotSync app's "IR to a PC/Handheld": the IrLAP
// secondary, IrLMP, IAS, Tiny TP and IrCOMM, with the IrCOMM port joined to a Sync Manager
// endpoint (frondSync::Transport::infrared). The guest's HotSync runs the serial protocol over
// IrCOMM as it does over the cradle. Single-threaded apart from the endpoint's own thread; time is
// guest nanoseconds.

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

#include "Ias.h"
#include "IrComm.h"
#include "IrLap.h"
#include "IrLmp.h"
#include "IrPeer.h"
#include "sync/SyncManager.h"

namespace frondIr {

class IrSync : public IrPeer, private IrCommPort {
   public:
    // Hints for a computer that offers IrCOMM (IrLMP 1.1 section 3.4.1).
    static IrLap::Options DefaultLap();

    IrSync() : IrSync(DefaultLap()) {}
    explicit IrSync(IrLap::Options options);

    void Write(const uint8_t* data, size_t size, uint64_t now) override { lap.Receive(data, size, now); }
    size_t Read(uint8_t* data, size_t max, uint64_t now) override { return lap.Read(data, max, now); }
    size_t Available(uint64_t now) const override { return lap.Available(now); }
    uint64_t ReadyAt() const override { return lap.ReadyAt(); }
    void Service(uint64_t now) override;
    void Drop(uint64_t now) override { lap.Drop(now); }
    // sync idle, sync linked (IrLAP up) or sync open (IrCOMM open), then the Sync Manager's state.
    std::string Describe() const override;
    const IrLap& Lap() const override { return lap; }
    const IrComm& Comm() const { return comm; }

   private:
    void Opened(uint64_t now) override;
    void Closed(uint64_t now) override;
    void FromGuest(const uint8_t* data, size_t size, uint64_t now) override;
    size_t ToGuest(uint8_t* data, size_t max, uint64_t now) override;
    uint8_t Lines(uint64_t now) override { return cts | dsr | cd; }
    // Moves bytes the pipe had no room for.
    void Flush();

    frondSync::SyncEndpoint endpoint;
    IrLmp lmp;
    IrLap lap;
    IasServer ias;
    IrComm comm;
    // Bytes from the guest the pipe had no room for yet, at most pendingMax.
    static constexpr size_t pendingMax = 65536;
    std::deque<uint8_t> pending;
};

}  // namespace frondIr

#endif
