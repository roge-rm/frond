#ifndef FROND_IR_IRPRINTER_H
#define FROND_IR_IRPRINTER_H

// An IrDA printer reached over infrared: the IrLAP secondary, IrLMP and IAS, taking print data two
// ways. IrLPT is IAS class "IrLPT" with an IrLMP LSAP and no Tiny TP, which PrintBoy's IrMgr opens.
// IrCOMM takes any service type, for apps that print to the IR serial port. Each connection is one
// job: its bytes collect until the guest disconnects the LSAP or the link ends (DISC or the
// guest's disconnect time with no frames), then join the held output that the host saves or
// clears. The guest's transceiver going off doesn't end the link: the Dana's driver turns it off
// and on again while it changes speed after the SNRM. A load, reset or Drop throws a job still coming in away. Single-threaded; time is guest
// nanoseconds. Nothing here is saved with a session.

#include <cstddef>
#include <cstdint>
#include <string>

#include "../RawPrintJob.h"
#include "Ias.h"
#include "IrComm.h"
#include "IrLap.h"
#include "IrLmp.h"

namespace frondIr {

class IrPrinter : private IrCommPort {
   public:
    static constexpr uint8_t lptLsap = 0x04;
    // The most one job, and the held output, can take: the raw output file's limit.
    static constexpr size_t defaultLimit = 16u << 20;

    struct Options {
        IrLap::Options lap = PrinterLap();
        size_t limit = defaultLimit;
    };
    // Address "FrPr", printer hint with IrCOMM, nickname "Frond Printer".
    static IrLap::Options PrinterLap();

    IrPrinter();
    explicit IrPrinter(Options options);

    void Write(const uint8_t* data, size_t size, uint64_t now) {
        lap.Receive(data, size, now);
        heard = true;
        lastHeard = now;
    }
    size_t Read(uint8_t* data, size_t max, uint64_t now) { return lap.Read(data, max, now); }
    size_t Available(uint64_t now) const { return lap.Available(now); }
    uint64_t ReadyAt() const { return lap.ReadyAt(); }
    void Service(uint64_t now) { lap.Service(now); }
    // After a load or reset: the job coming in is dropped, then no link.
    void Drop(uint64_t now);

    // Held output: finished jobs not yet saved or cleared, back to back.
    size_t HeldBytes() const { return held.size(); }
    unsigned HeldJobs() const { return heldJobs; }
    // A job is coming in, and its bytes so far.
    bool Receiving() const { return job.state() == RawPrintJob::State::collecting; }
    size_t ReceivingBytes() const { return Receiving() ? job.size() : 0; }
    // A job is coming in, the guest holds the link, or it sent something (discovery, a connect) in the
    // last guestQuietNs of guest time. Only the guest ends any of these, so a runner keeps the guest
    // going in the background while this is true.
    static constexpr uint64_t guestQuietNs = 5000000000ull;
    bool NeedsGuest(uint64_t now) const {
        return Receiving() || lap.Connected() || (heard && now - lastHeard < guestQuietNs);
    }
    // Publishes the held output through output, which must copy the bytes. The held output is
    // empty after, whatever the result; nothing held is rejected.
    RawPrintJob::State Save(RawPrintJob::Output output, void* context);
    void Clear();
    // Changes whenever a byte arrives or a job ends, for a host-side idle clock.
    uint64_t Activity() const { return activity; }

    // printer idle, printer linked or printer receiving.
    std::string Describe() const;

    struct Stats {
        unsigned jobs{0}, dropped{0}, overflows{0};
    };
    const Stats& GetStats() const { return stats; }
    const IrLap& Lap() const { return lap; }
    const IrComm& Comm() const { return comm; }
    const IasServer& Ias() const { return ias; }

   private:
    // IrLPT: IrLMP data is the print data.
    class Lpt : public LmService {
       public:
        explicit Lpt(IrPrinter& printer) : printer(printer) {}
        bool Connect(const Bytes& data, Bytes& reply, uint64_t now) override;
        void Data(const Bytes& data, uint64_t now) override;
        void Disconnect(uint64_t now) override;

       private:
        IrPrinter& printer;
    };

    void Opened(uint64_t now) override { Begin(); }
    void Closed(uint64_t now) override { End(); }
    void FromGuest(const uint8_t* data, size_t size, uint64_t now) override { Append(data, size); }
    size_t ToGuest(uint8_t* data, size_t max, uint64_t now) override { return 0; }
    // Ready: clear to send, data set ready and carrier.
    uint8_t Lines(uint64_t now) override { return cts | dsr | cd; }

    void Begin();
    void Append(const uint8_t* data, size_t size);
    void End();

    IrLmp lmp;
    IrLap lap;
    IasServer ias;
    Lpt lpt;
    IrComm comm;
    RawPrintJob job, held;
    unsigned heldJobs{0};
    uint64_t activity{0};
    // The guest's last byte to the printer, in guest nanoseconds.
    bool heard{false};
    uint64_t lastHeard{0};
    Stats stats;
};

}  // namespace frondIr

#endif
