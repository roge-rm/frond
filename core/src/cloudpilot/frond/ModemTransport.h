#ifndef FROND_68K_MODEM_TRANSPORT_H
#define FROND_68K_MODEM_TRANSPORT_H

// The serial port's transport: a host link (HostTransport68k), direct PPP or a snap-on
// Hayes modem whose calls reach PPP and slirp, or for the HotSync number a second host
// link (the modem HotSync link) that pilot-link answers. The modem is powered while the guest's
// serial line driver is on. Its clock is guest time in nanoseconds that never runs backwards,
// so a session load (which moves the cycle count) doesn't upset its timers. All on the
// emulator thread.

#include <cstdlib>
#include <memory>
#include <string>

#include "HostTransport.h"
#include "modem/HayesModem.h"
#include "modem/LineSwitch.h"
#include "modem/PppServer.h"
#include "net/SlirpIp.h"
#include "serial/PortableKeyboard.h"

class ModemTransport68k : public EmTransportSerial {
   public:
    // sync is the modem HotSync link, used only while a HotSync call is up.
    ModemTransport68k(HostLink68k& link, HostLink68k& sync) : host(link), syncLink(sync) {}

    HostTransport68k& Host() { return host; }
    const HostTransport68k& Host() const { return host; }
    bool ModemMode() const { return engine && !engine->direct; }
    bool DirectMode() const { return engine && engine->direct; }
    bool KeyboardMode() const { return keyboard.Attached(); }
    frondKeyboard::PortableKeyboard& Keyboard() { return keyboard; }
    frondKeyboard::PortableKeyboardTypist& Typist() { return typist; }

    // The Palm Portable Keyboard plugged into the serial port or taken out. It takes the place of
    // the host link, the modem and direct PPP.
    void SetKeyboardMode(bool on) {
        if (on == KeyboardMode()) return;
        typist.Clear();
        if (!on) {
            keyboard.Detach();
            return;
        }
        engine.reset();
        keyboard.Attach(rtsHigh && powered, Now());
        keyboardCredit.Restart();
    }
    // A key from the host. Fn keys come with Fn held around them.
    bool KeyboardKey(uint8_t code, bool down) { return keyboard.Key(code, down, Now()); }

    // Modem mode on or off. Turning it on starts slirp and fails if slirp can't start.
    bool SetNetworkMode(bool modem, bool direct, std::string& error) {
        if (modem == ModemMode() && direct == DirectMode()) return true;
        if (!modem && !direct) {
            engine.reset();
            return true;
        }
        auto made = std::make_unique<Engine>(direct);
        if (!made->network.Valid()) {
            error = "slirp did not start";
            return false;
        }
        engine = std::move(made);
        credit.Restart();
        if (modem && powered) engine->modem.SetPower(true, Now());
        // A session saved mid-call and started before modem mode came on.
        if (modem) {
            if (!droppedCall.empty()) engine->modem.LineDropped(droppedCall, Now());
            droppedCall.clear();
        }
        return true;
    }

    // modem idle, modem dialling or modem online SPEED.
    std::string Describe() const {
        if (KeyboardMode()) return std::string("keyboard ") + keyboard.Describe();
        if (!engine) return "off";
        if (DirectMode()) {
            if (engine->ppp.IpcpOpen()) return "ppp online";
            const auto phase = engine->ppp.GetPhase();
            return phase == frondModem::PppServer::Phase::Idle ||
                   phase == frondModem::PppServer::Phase::Down ? "ppp idle" : "ppp connecting";
        }
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
        if (!ModemMode()) return "off";
        return std::string("power=") + (engine->modem.GetState() == frondModem::HayesModem::State::Off ? "off" : "on") +
               " dtr=" + (engine->modem.Dtr() ? "on" : "off") + " dtr_rises=" + std::to_string(engine->dtrRises) +
               " dtr_drops=" + std::to_string(engine->dtrDrops);
    }

    bool Open() override {
        powered = true;
        if (!engine) return host.Open();
        credit.Restart();
        if (ModemMode()) engine->modem.SetPower(true, Now());
        return true;
    }
    bool Close() override {
        powered = false;
        if (!engine) return host.Close();
        if (DirectMode()) engine->ppp.HangUp();
        else engine->modem.SetPower(false, Now());
        return true;
    }

    bool Read(long& count, void* data) override {
        if (KeyboardMode()) {
            const long n = long(std::min<size_t>(size_t(std::max(0L, count)), KeyboardReadable()));
            count = long(keyboard.Read(static_cast<uint8_t*>(data), size_t(n)));
            keyboardCredit -= count;
            return true;
        }
        if (!engine) return host.Read(count, data);
        const long n = std::max(0L, std::min(count, credit.Take(config.fBaud)));
        auto* bytes = static_cast<uint8_t*>(data);
        count = long(DirectMode() ? engine->ppp.ToModem(bytes, size_t(n), Now())
                                  : engine->modem.Read(bytes, size_t(n)));
        credit -= count;
        return true;
    }
    bool Write(long& count, const void* data) override {
        // The keyboard has no line from the Palm but RTS.
        if (KeyboardMode()) return true;
        if (!engine) return host.Write(count, data);
        if (!powered || count <= 0) return true;
        const auto* bytes = static_cast<const uint8_t*>(data);
        const auto now = Now();
        if (DirectMode()) {
            const auto phase = engine->ppp.GetPhase();
            if (phase == frondModem::PppServer::Phase::Idle || phase == frondModem::PppServer::Phase::Down) {
                const auto* flag = std::find(bytes, bytes + count, uint8_t(0x7e));
                if (flag == bytes + count) return true;
                engine->ppp.Answer(now);
            }
            engine->ppp.FromModem(bytes, size_t(count), now);
        } else {
            engine->modem.Write(bytes, size_t(count), now);
        }
        return true;
    }
    bool CanRead() override {
        if (KeyboardMode()) return KeyboardReadable() > 0;
        if (!engine) return host.CanRead();
        return powered && Available() && credit.Take(config.fBaud) > 0;
    }
    bool CanWrite() override { return KeyboardMode() ? powered : engine ? powered : host.CanWrite(); }
    size_t BytesPending() override {
        if (KeyboardMode()) return KeyboardReadable();
        if (!engine) return host.BytesPending();
        return powered ? std::min<size_t>(Available(), size_t(credit.Take(config.fBaud))) : 0;
    }

    void SetConfig(const Config& next) override {
        EmTransportSerial::SetConfig(next);
        host.SetConfig(next);
    }
    // Auto is RTS driven by the receive FIFO, which is high while it has room.
    void SetRTS(RTSControl control) override {
        rtsHigh = control != kRTSOff;
        if (KeyboardMode()) KeyboardRts();
        host.SetRTS(control);
    }
    void SetDTR(bool dtr) override {
        host.SetDTR(dtr);
        if (!ModemMode()) return;
        if (dtr != engine->modem.Dtr()) ++(dtr ? engine->dtrRises : engine->dtrDrops);
        engine->modem.SetDTR(dtr, Now());
    }
    // The guest's DTR as it is now, for a new modem or after a load; not counted as an edge.
    void SyncDTR(bool dtr) {
        if (ModemMode()) engine->modem.SetDTR(dtr, Now());
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
        if (KeyboardMode()) {
            powered = lineDriverOn;
            KeyboardRts();
            const uint64_t now = Now();
            typist.Service(keyboard, now);
            keyboard.Service(now);
            // The RS-232 receiver is off with the line driver, so what the keyboard sends is lost.
            if (!powered) keyboard.Drop();
            for (int pulses = keyboard.TakePulses(); pulses > 0; --pulses) {
                gSession->QueueButtonEvent(ButtonEvent(ButtonEvent::Button::cradle, ButtonEvent::Type::press));
                gSession->QueueButtonEvent(ButtonEvent(ButtonEvent::Button::cradle, ButtonEvent::Type::release));
            }
            return;
        }
        if (!engine) {
            powered = lineDriverOn;
            return host.Service(lineDriverOn);
        }
        if (lineDriverOn != powered) lineDriverOn ? Open() : Close();
        const uint64_t now = Now();
        if (DirectMode()) {
            if (powered) {
                engine->ppp.Service(now);
                if (!engine->ppp.Carrier(now)) engine->ppp.HangUp();
            }
            return;
        }
        PumpHotSync();
        engine->modem.Service(now);
        PumpHotSync();
    }

    // True while a call to the HotSync number is up.
    bool HotSyncCall() const {
        return ModemMode() && engine->line.HotSyncCall() && engine->hotsync.InCall();
    }

    // For a session save: the modem's settings if the guest is off hook, or empty.
    std::string SavedCall() const { return ModemMode() ? engine->modem.SavedCall() : droppedCall; }

    // After a session load. call is what SavedCall gave when it was saved: with a call up, the
    // line has dropped since, so the guest gets NO CARRIER and has to dial again. Without one the
    // modem is idle. A call saved outside modem mode waits for modem mode to come on.
    void SessionLoaded(const std::string& call) {
        // A plugged-in keyboard powers up again, as after a reset of the Palm.
        if (KeyboardMode()) {
            typist.Clear();
            keyboard.Attach(rtsHigh && powered, Now());
            keyboardCredit.Restart();
        }
        droppedCall.clear();
        if (!ModemMode()) droppedCall = call;
        else if (call.empty() || !engine->modem.LineDropped(call, Now())) engine->modem.Drop(Now());
        if (DirectMode()) engine->ppp.HangUp();
        credit.Restart();
    }

   private:
    struct Engine {
        explicit Engine(bool direct) : direct(direct) {}
        const bool direct;
        frondNet::SlirpIp network;
        frondModem::PppServer ppp{network, {!direct, false}};
        frondModem::PipeLine hotsync;
        frondModem::LineSwitch line{ppp, hotsync};
        frondModem::HayesModem modem{line};
        // The HotSync link had a peer during this HotSync call.
        bool syncPeerSeen{false};
        unsigned dtrRises{0}, dtrDrops{0};
    };

    // The keyboard sees RTS through the line driver: with the port closed the line is low, so
    // opening the port with RTS on is the rising edge that gets the ID.
    void KeyboardRts() {
        const bool high = rtsHigh && powered;
        // The ID starts a byte time after RTS rises.
        if (high && !keyboard.Awake()) keyboardCredit.Reset(KeyboardCycles());
        keyboard.SetRts(high, Now());
    }

    // Bytes the guest can take now: the keyboard sends at 9600 baud and a UART set to another rate
    // gets nothing it can use.
    size_t KeyboardReadable() {
        // Credit only builds while bytes wait, so they go out a byte time apart.
        if (!keyboard.Available()) {
            keyboardCredit.Reset(KeyboardCycles());
            return 0;
        }
        if (!powered || std::labs(config.fBaud - frondKeyboard::PortableKeyboard::kBaud) > 200) return 0;
        return std::min<size_t>(keyboard.Available(), size_t(keyboardCredit.Take(frondKeyboard::PortableKeyboard::kBaud, KeyboardCycles())));
    }

    static uint64_t KeyboardCycles() { return gCPU68K ? gCPU68K->CycleNow() : gSession->GetSystemCycles(); }

    size_t Available() const { return DirectMode() ? engine->ppp.Available() : engine->modem.Available(); }

    // Guest nanoseconds, carried on across a load that moves the cycle count back. The keyboard
    // reads the CPU's own count, which moves mid-slice, as drivers wait for its ID in a busy loop.
    uint64_t Now() {
        const uint64_t cycles =
            KeyboardMode() && gCPU68K ? gCPU68K->CycleNow() : gSession->GetSystemCycles();
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
            if (engine->syncPeerSeen && syncLink.GetKind() == HostLink68k::Kind::syncManager) {
                // A new call starts with a fresh HotSync stream, including after an interrupted sync.
                std::string error;
                syncLink.Open(HostLink68k::Kind::syncManager, 0, error);
            }
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
    frondKeyboard::PortableKeyboard keyboard;
    frondKeyboard::PortableKeyboardTypist typist;
    BaudCredit68k keyboardCredit;
    // The guest's RTS as last set; low until it sets it.
    bool rtsHigh{false};
    uint64_t cycleMark{0};
    double clock{0};
};

#endif
