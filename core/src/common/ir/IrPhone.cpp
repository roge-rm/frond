#include "IrPhone.h"

namespace frondIr {

using frondModem::HayesModem;

IrPhone::IrPhone(frondModem::ModemLine& line) : IrPhone(line, Options{}) {}

IrPhone::IrPhone(frondModem::ModemLine& line, Options options)
    : modem(line, options.modem),
      lmp(lap),
      lap(lmp, options.lap),
      ias(lmp),
      comm(lmp, ias, *this, options.lap.nickname) {}

std::string IrPhone::Describe() const {
    switch (modem.GetState()) {
        case HayesModem::State::Dialling: return "phone dialling";
        case HayesModem::State::Online:
        case HayesModem::State::OnlineCommand: return "phone online " + std::to_string(modem.LineSpeed());
        default: return lap.Connected() ? "phone linked" : "phone idle";
    }
}

void IrPhone::Service(uint64_t now) {
    modem.Service(now);
    lap.Service(now);
}

bool IrPhone::CallDropped(const std::string& saved) {
    // Checked on a scratch modem, so bad text is refused now rather than at the next open.
    struct NoLine : frondModem::ModemLine {
        void Answer(uint64_t) override {}
        void HangUp() override {}
        void FromModem(const uint8_t*, size_t, uint64_t) override {}
        size_t ToModem(uint8_t*, size_t, uint64_t) override { return 0; }
        bool Carrier(uint64_t) const override { return false; }
    } none;
    HayesModem check(none);
    droppedCall.clear();
    if (!check.LineDropped(saved, 0)) return false;
    droppedCall = saved;
    return true;
}

// The phone's modem is on while the guest has the IrCOMM port open; closing it ends the call.
void IrPhone::Opened(uint64_t now) {
    cdDropped = !droppedCall.empty() && modem.LineDropped(droppedCall, now);
    droppedCall.clear();
    if (!cdDropped) modem.SetPower(true, now);
}

uint8_t IrPhone::OpenedDeltas(uint64_t now) {
    const bool dropped = cdDropped;
    cdDropped = false;
    return dropped ? uint8_t(cd >> 4) : 0;
}

void IrPhone::Closed(uint64_t now) { modem.SetPower(false, now); }

void IrPhone::FromGuest(const uint8_t* data, size_t size, uint64_t now) { modem.Write(data, size, now); }

size_t IrPhone::ToGuest(uint8_t* data, size_t max, uint64_t now) {
    modem.Service(now);
    return modem.Read(data, max);
}

void IrPhone::GuestLines(bool dtr, bool rts, uint64_t now) { modem.SetDTR(dtr, now); }

uint8_t IrPhone::Lines(uint64_t now) {
    const auto state = modem.GetState();
    const bool carrier = state == HayesModem::State::Online || state == HayesModem::State::OnlineCommand;
    return cts | dsr | (carrier ? cd : 0);
}

}  // namespace frondIr
