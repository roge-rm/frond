#ifndef FROND_68K_IR_PHONE_TRANSPORT_H
#define FROND_68K_IR_PHONE_TRANSPORT_H

// The IR port's transport: a host link (HostTransport68k), in phone mode a mobile phone with a data
// modem (frondIr::IrPhone) whose calls reach a PPP server and slirp, or in printer mode an IrDA
// printer (frondIr::IrPrinter). The guest's IR bytes are raw SIR; phone and printer are the IrLAP
// secondary. Its clock is guest time in nanoseconds that never
// runs backwards, so a session load doesn't upset its timers. All on the emulator thread.

#include <memory>
#include <string>

#include "EmCPU68K.h"
#include "EmHAL.h"
#include "HostTransport.h"
#include "ir/IrPhone.h"
#include "ir/IrPrinter.h"
#include "ir/IrPrinterControl.h"
#include "modem/PppServer.h"
#include "net/SlirpIp.h"

class IrPhoneTransport68k : public EmTransportSerial {
   public:
    explicit IrPhoneTransport68k(HostLink68k& link) : host(link) {}

    HostTransport68k& Host() { return host; }
    const HostTransport68k& Host() const { return host; }
    bool PhoneMode() const { return bool(engine); }
    bool PrinterMode() const { return bool(printer); }
    // The printer needs the guest to run (frondIr::IrPrinter::NeedsGuest).
    bool PrinterNeedsGuest() { return printer && printer->NeedsGuest(Now()); }
    frondIr::IrPrinter* Printer() { return printer.get(); }
    frondIr::IrPrinterControl& PrinterControl() { return printerControl; }

    // Printer mode on or off. Turning it off throws away held output that wasn't saved.
    void SetPrinterMode(bool on) {
        if (on == PrinterMode()) return;
        printerControl.Reset();
        if (!on) {
            printer.reset();
            return;
        }
        engine.reset();
        printer = std::make_unique<frondIr::IrPrinter>();
        credit.Restart();
    }

    // A reset from the host: a job coming in to the printer is thrown away.
    void DeviceReset() {
        if (printer) printer->Drop(Now());
    }

    // Phone mode on or off. Turning it on starts slirp and fails if slirp can't start.
    bool SetPhoneMode(bool on, std::string& error) {
        if (on == PhoneMode()) return true;
        if (!on) {
            engine.reset();
            return true;
        }
        auto made = std::make_unique<Engine>();
        if (!made->network.Valid()) {
            error = "slirp did not start";
            return false;
        }
        SetPrinterMode(false);
        engine = std::move(made);
        credit.Restart();
        // A session saved mid-call and started before phone mode came on.
        if (!droppedCall.empty()) engine->phone.CallDropped(droppedCall);
        droppedCall.clear();
        return true;
    }

    // phone idle, phone linked, phone dialling or phone online SPEED.
    std::string Describe() const { return engine ? engine->phone.Describe() : printer ? printer->Describe() : "off"; }

    // The phone's IrLAP counters, or off outside phone mode.
    std::string Stats() const {
        if (!Peer()) return "off";
        const auto& lap = engine ? engine->phone.Lap() : printer->Lap();
        const auto& s = lap.GetStats();
        return std::string("port=") + (powered ? "open" : "closed") + " frames_in=" + std::to_string(s.framesIn) +
               " frames_out=" + std::to_string(s.framesOut) + " bad=" + std::to_string(lap.BadFrames()) +
               " connects=" + std::to_string(s.connects) + " resent=" + std::to_string(s.resent) +
               " timeouts=" + std::to_string(s.timeouts);
    }

    bool Open() override {
        powered = true;
        if (!Peer()) return host.Open();
        credit.Restart();
        return true;
    }
    // With the transceiver off the phone hears nothing more of this link: it drops the link and
    // any unread answer, so a guest reset or a later link starts from discovery.
    // The printer keeps its link: the guest's disconnect time ends it if the guest has really gone.
    bool Close() override {
        powered = false;
        if (!Peer()) return host.Close();
        if (engine) engine->phone.Drop(Now());
        return true;
    }

    bool Read(long& count, void* data) override {
        if (!Peer()) return host.Read(count, data);
        const long n = std::max(0L, std::min(count, credit.Take(config.fBaud)));
        count = long(PeerRead(static_cast<uint8_t*>(data), size_t(n), Now()));
        credit -= count;
        return true;
    }
    bool Write(long& count, const void* data) override {
        if (!Peer()) return host.Write(count, data);
        if (powered && count > 0) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            if (engine) engine->phone.Write(bytes, size_t(count), Now());
            else printer->Write(bytes, size_t(count), Now());
            WakeForAnswer();
        }
        return true;
    }
    bool CanRead() override {
        if (!Peer()) return host.CanRead();
        return powered && PeerAvailable(Now()) && credit.Take(config.fBaud) > 0;
    }
    bool CanWrite() override { return Peer() ? powered : host.CanWrite(); }
    size_t BytesPending() override {
        if (!Peer()) return host.BytesPending();
        return powered ? std::min<size_t>(PeerAvailable(Now()), size_t(credit.Take(config.fBaud))) : 0;
    }

    void SetConfig(const Config& next) override {
        EmTransportSerial::SetConfig(next);
        host.SetConfig(next);
    }
    void SetRTS(RTSControl control) override { host.SetRTS(control); }
    void SetDTR(bool dtr) override { host.SetDTR(dtr); }
    void SetBreak(bool on) override { host.SetBreak(on); }
    bool RequiresSync() override { return false; }
    void OnTransactionStateChange(EmUARTDragonball::TransactionState from,
                                  EmUARTDragonball::TransactionState to) override {
        host.OnTransactionStateChange(from, to);
    }

    // Called between slices with the guest's IR transceiver state, which a session load doesn't
    // report.
    void Service(bool lineDriverOn) {
        if (!Peer()) {
            powered = lineDriverOn;
            return host.Service(lineDriverOn);
        }
        if (lineDriverOn != powered) lineDriverOn ? Open() : Close();
        if (engine) engine->phone.Service(Now());
        else printer->Service(Now());
        printerControl.Observe(printer.get());
        WakeForAnswer();
    }

    // For a session save: the phone modem's settings while a call is up, or empty.
    std::string SavedCall() const { return engine ? engine->phone.SavedCall() : droppedCall; }

    // After a session load: no link and no call. call is what SavedCall gave when it was saved:
    // with a call up, the guest's IrLAP finds the link gone, and when it links again the phone
    // drops CD and says NO CARRIER. A call saved outside phone mode waits for phone mode.
    void SessionLoaded(const std::string& call) {
        droppedCall.clear();
        if (engine) {
            engine->phone.Drop(Now());
            if (!call.empty()) engine->phone.CallDropped(call);
        } else {
            droppedCall = call;
        }
        // The printer's job coming in is gone with the guest that sent it; held output stays.
        if (printer) printer->Drop(Now());
        credit.Restart();
    }

   private:
    struct Engine {
        frondNet::SlirpIp network;
        frondModem::PppServer ppp{network};
        frondIr::IrPhone phone{ppp};
    };

    // A guest that sleeps while it waits for the answer wakes when the answer can be read, as the
    // UART's receive interrupt would, rather than at its next timer tick.
    void WakeForAnswer() {
        if (!powered) return;
        const uint64_t ready = engine ? engine->phone.ReadyAt() : printer->ReadyAt(), now = Now();
        if (ready == UINT64_MAX) return;
        const double wait = ready > now ? double(ready - now) * gSession->GetClocksPerSecond() / 1e9 : 0;
        EmHAL::WakeAt(cycleMark + uint64_t(wait) + 1, cycleMark);
    }

    // Guest nanoseconds, carried on across a load that moves the cycle count back. The CPU's own
    // count, so bytes written mid-slice get the time they were written.
    uint64_t Now() {
        const uint64_t cycles = gCPU68K ? gCPU68K->CycleNow() : gSession->GetSystemCycles();
        if (cycles > cycleMark) clock += double(cycles - cycleMark) * 1e9 / gSession->GetClocksPerSecond();
        cycleMark = cycles;
        return uint64_t(clock);
    }

    bool Peer() const { return engine || printer; }
    size_t PeerRead(uint8_t* data, size_t max, uint64_t now) {
        return engine ? engine->phone.Read(data, max, now) : printer->Read(data, max, now);
    }
    size_t PeerAvailable(uint64_t now) const {
        return engine ? engine->phone.Available(now) : printer->Available(now);
    }

    HostTransport68k host;
    std::unique_ptr<Engine> engine;
    std::unique_ptr<frondIr::IrPrinter> printer;
    frondIr::IrPrinterControl printerControl;
    std::string droppedCall;
    bool powered{false};
    BaudCredit68k credit;
    uint64_t cycleMark{0};
    double clock{0};
};

#endif
