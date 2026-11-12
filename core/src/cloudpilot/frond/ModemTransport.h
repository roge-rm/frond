#ifndef FROND_68K_MODEM_TRANSPORT_H
#define FROND_68K_MODEM_TRANSPORT_H

// The serial port's transport: a host link (HostTransport68k) or, in modem mode, a snap-on
// Hayes modem whose calls reach a PPP server and slirp, or for the HotSync number a second host
// link (the modem HotSync link) that pilot-link answers. The modem is powered while the guest's
// serial line driver is on. Its clock is guest time in nanoseconds that never runs backwards,
// so a session load (which moves the cycle count) doesn't upset its timers. All on the
// emulator thread.

#include <memory>
#include <string>

#include "HostTransport.h"
#include "modem/HayesModem.h"
#include "modem/LineSwitch.h"
#include "modem/PppServer.h"
#include "net/SlirpIp.h"

class ModemTransport68k : public EmTransportSerial {
   public:
    // sync is the modem HotSync link, used only while a HotSync call is up.
    ModemTransport68k(HostLink68k& link, HostLink68k& sync) : host(link), syncLink(sync) {}

    HostTransport68k& Host() { return host; }
    const HostTransport68k& Host() const { return host; }
    bool ModemMode() const { return bool(engine); }

    // Modem mode on or off. Turning it on starts slirp and fails if slirp can't start.
    bool SetModemMode(bool on, std::string& error) {
        if (on == ModemMode()) return true;
        if (!on) {
            engine.reset();
            return true;
        }
        auto made = std::make_unique<Engine>();
        if (!made->network.Valid()) {
            error = "slirp did not start";
            return false;
        }
        engine = std::move(made);
        credit.Restart();
        if (powered) engine->modem.SetPower(true, Now());
        // A session saved mid-call and started before modem mode came on.
        if (!droppedCall.empty()) engine->modem.LineDropped(droppedCall, Now());
        droppedCall.clear();
        return true;
    }

    // modem idle, modem dialling or modem online SPEED.
    std::string Describe() const {
        if (!engine) return "off";
        switch (engine->modem.GetState()) {
            case frondModem::HayesModem::State::Dialling: return "modem dialling";
            case frondModem::HayesModem::State::Online:
            case frondModem::HayesModem::State::OnlineCommand:
                return "modem online " + std::to_string(engine->modem.LineSpeed());
            default: return "modem idle";
        }
    }

    // power=on|off dtr=on|off as the modem sees them and the guest's DTR edges since modem mode
    // came on, or off outside modem mode.
    std::string Stats() const {
        if (!engine) return "off";
        return std::string("power=") + (engine->modem.GetState() == frondModem::HayesModem::State::Off ? "off" : "on") +
               " dtr=" + (engine->modem.Dtr() ? "on" : "off") + " dtr_rises=" + std::to_string(engine->dtrRises) +
               " dtr_drops=" + std::to_string(engine->dtrDrops);
    }

    bool Open() override {
        powered = true;
        if (!engine) return host.Open();
        credit.Restart();
        engine->modem.SetPower(true, Now());
        return true;
    }
    bool Close() override {
        powered = false;
        if (!engine) return host.Close();
        engine->modem.SetPower(false, Now());
        return true;
    }

    bool Read(long& count, void* data) override {
        if (!engine) return host.Read(count, data);
        const long n = std::max(0L, std::min(count, credit.Take(config.fBaud)));
        count = long(engine->modem.Read(static_cast<uint8_t*>(data), size_t(n)));
        credit -= count;
        return true;
    }
    bool Write(long& count, const void* data) override {
        if (!engine) return host.Write(count, data);
        if (powered && count > 0) engine->modem.Write(static_cast<const uint8_t*>(data), size_t(count), Now());
        return true;
    }
    bool CanRead() override {
        if (!engine) return host.CanRead();
        return powered && engine->modem.Available() && credit.Take(config.fBaud) > 0;
    }
    bool CanWrite() override { return engine ? powered : host.CanWrite(); }
    size_t BytesPending() override {
        if (!engine) return host.BytesPending();
        return powered ? std::min<size_t>(engine->modem.Available(), size_t(credit.Take(config.fBaud))) : 0;
    }

    void SetConfig(const Config& next) override {
        EmTransportSerial::SetConfig(next);
        host.SetConfig(next);
    }
    void SetRTS(RTSControl control) override { host.SetRTS(control); }
    void SetDTR(bool dtr) override {
        host.SetDTR(dtr);
        if (!engine) return;
        if (dtr != engine->modem.Dtr()) ++(dtr ? engine->dtrRises : engine->dtrDrops);
        engine->modem.SetDTR(dtr, Now());
    }
    // The guest's DTR as it is now, for a new modem or after a load; not counted as an edge.
    void SyncDTR(bool dtr) {
        if (engine) engine->modem.SetDTR(dtr, Now());
    }
    void SetBreak(bool on) override { host.SetBreak(on); }
    bool RequiresSync() override { return false; }
    void OnTransactionStateChange(EmUARTDragonball::TransactionState from,
                                  EmUARTDragonball::TransactionState to) override {
        host.OnTransactionStateChange(from, to);
    }

    // Called between slices with the guest's line driver for this side, which a session load
    // doesn't report.
    void Service(bool lineDriverOn) {
        if (!engine) {
            powered = lineDriverOn;
            return host.Service(lineDriverOn);
        }
        if (lineDriverOn != powered) lineDriverOn ? Open() : Close();
        const uint64_t now = Now();
        PumpHotSync();
        engine->modem.Service(now);
        PumpHotSync();
    }

    // True while a call to the HotSync number is up.
    bool HotSyncCall() const {
        return engine && engine->line.HotSyncCall() && engine->hotsync.InCall();
    }

    // For a session save: the modem's settings if the guest is off hook, or empty.
    std::string SavedCall() const { return engine ? engine->modem.SavedCall() : droppedCall; }

    // After a session load. call is what SavedCall gave when it was saved: with a call up, the
    // line has dropped since, so the guest gets NO CARRIER and has to dial again. Without one the
    // modem is idle. A call saved outside modem mode waits for modem mode to come on.
    void SessionLoaded(const std::string& call) {
        droppedCall.clear();
        if (!engine) droppedCall = call;
        else if (call.empty() || !engine->modem.LineDropped(call, Now())) engine->modem.Drop(Now());
        credit.Restart();
    }

   private:
    struct Engine {
        frondNet::SlirpIp network;
        frondModem::PppServer ppp{network};
        frondModem::PipeLine hotsync;
        frondModem::LineSwitch line{ppp, hotsync};
        frondModem::HayesModem modem{line};
        // The HotSync link had a peer during this HotSync call.
        bool syncPeerSeen{false};
        unsigned dtrRises{0}, dtrDrops{0};
    };

    // Guest nanoseconds, carried on across a load that moves the cycle count back.
    uint64_t Now() {
        const uint64_t cycles = gSession->GetSystemCycles();
        if (cycles > cycleMark) clock += double(cycles - cycleMark) * 1e9 / gSession->GetClocksPerSecond();
        cycleMark = cycles;
        return uint64_t(clock);
    }

    // Moves bytes between a HotSync call and the HotSync link. Bytes on the link outside a call
    // are dropped, and a TCP or Unix peer (or a PTY's reader) leaving during the call ends it.
    void PumpHotSync() {
        auto& pipe = engine->hotsync;
        pipe.SetReachable(syncLink.GetKind() != HostLink68k::Kind::off);
        if (!pipe.InCall() || !engine->line.HotSyncCall()) {
            engine->syncPeerSeen = false;
            syncLink.DropReceived();
            return;
        }
        uint8_t buffer[512];
        while (pipe.OutAvailable()) {
            const size_t n = pipe.TakeOut(buffer, sizeof buffer);
            syncLink.Send(buffer, n);
        }
        while (syncLink.Available() && pipe.InRoom()) {
            const size_t n = syncLink.Take(buffer, std::min(sizeof buffer, pipe.InRoom()));
            if (!n) break;
            pipe.PutIn(buffer, n);
        }
        const bool connected = syncLink.Connected();
        if (connected) engine->syncPeerSeen = true;
        else if (engine->syncPeerSeen) pipe.PeerGone();
    }

    HostTransport68k host;
    HostLink68k& syncLink;
    std::unique_ptr<Engine> engine;
    std::string droppedCall;
    bool powered{false};
    BaudCredit68k credit;
    uint64_t cycleMark{0};
    double clock{0};
};

#endif
