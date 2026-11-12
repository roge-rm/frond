#include "IrComm.h"

#include <algorithm>

namespace frondIr {

namespace {

// Control parameters (IrCOMM section 4.5).
constexpr uint8_t piServiceType = 0x00, piPortType = 0x01, piDteLines = 0x20, piDceLines = 0x21, piPoll = 0x22;
// DTE line settings: delta DTR, delta RTS, DTR, RTS.
constexpr uint8_t dteDtr = 0x04, dteRts = 0x08;
// SDUs or frames held ready ahead of credit, so data waits in the port and not here.
constexpr size_t ahead = 2;

}  // namespace

IrComm::IrComm(IrLmp& lmp, IasServer& ias, IrCommPort& port, const std::string& deviceName)
    : lmp(lmp), port(port), tinyTp(lmp, cookedLsap, *this), raw(*this) {
    lmp.Bind(rawLsap, raw);
    ias.AddString("Device", "DeviceName", deviceName);
    // IrLMP version 1, IAS and LM-MUX support bits.
    ias.AddOctets("Device", "IrLMPSupport", {0x01, 0x00, 0x00});
    ias.AddInteger("IrDA:IrCOMM", "IrDA:TinyTP:LsapSel", cookedLsap);
    ias.AddInteger("IrDA:IrCOMM", "IrDA:IrLMP:LsapSel", rawLsap);
    // Service types we take, and a serial port.
    ias.AddOctets("IrDA:IrCOMM", "Parameters",
                  {piServiceType, 1, uint8_t(threeWireRaw | threeWire | nineWire), piPortType, 1, 0x01});
}

void IrComm::Open(Mode next, uint64_t now) {
    if (mode != Mode::Closed) port.Closed(now);
    mode = next;
    serviceType = next == Mode::Raw ? threeWireRaw : 0;
    dtr = rts = true;
    pending.clear();
    port.Opened(now);
    port.GuestLines(dtr, rts, now);
    // The first SDU tells the guest the DCE lines; deltas follow as they change.
    lines = port.Lines(now);
    const uint8_t deltas = port.OpenedDeltas(now);
    if (next == Mode::Cooked) QueueLines(uint8_t(lines | deltas));
}

bool IrComm::Connect(const Bytes& data, Bytes& reply, uint64_t now) {
    if (mode == Mode::Raw) lmp.Disconnect(rawLsap, IrLmp::userRequest, now);
    Open(Mode::Cooked, now);
    // Connect user data is a control channel like the start of an SDU.
    if (!data.empty()) Parameters(data.data() + 1, std::min<size_t>(data[0], data.size() - 1), now);
    return true;
}

void IrComm::Data(const Bytes& sdu, uint64_t now) {
    if (sdu.empty() || mode != Mode::Cooked) return;
    const size_t clen = std::min<size_t>(sdu[0], sdu.size() - 1);
    Parameters(sdu.data() + 1, clen, now);
    const size_t start = 1 + clen;
    if (start < sdu.size()) {
        fromGuest += sdu.size() - start;
        port.FromGuest(sdu.data() + start, sdu.size() - start, now);
    }
}

void IrComm::Parameters(const uint8_t* data, size_t size, uint64_t now) {
    for (size_t i = 0; i + 2 <= size;) {
        const uint8_t pi = data[i], pl = data[i + 1];
        if (i + 2 + pl > size) break;
        const uint8_t* pv = data + i + 2;
        if (pi == piServiceType && pl) {
            serviceType = pv[0];
        } else if (pi == piDteLines && pl) {
            dtr = pv[0] & dteDtr;
            rts = pv[0] & dteRts;
            port.GuestLines(dtr, rts, now);
        } else if (pi == piPoll) {
            lines = port.Lines(now);
            QueueLines(lines);
        }
        i += 2 + pl;
    }
}

void IrComm::Disconnect(uint64_t now) {
    if (mode == Mode::Cooked) {
        mode = Mode::Closed;
        port.Closed(now);
    }
}

void IrComm::Pump(uint64_t now) {
    if (mode != Mode::Cooked) return;
    // 3-wire cooked has no line settings, so only 9-wire (or a guest that hasn't said) gets them.
    const uint8_t current = port.Lines(now);
    if (current != lines && serviceType != threeWire) {
        // Deltas for CTS, DSR, RI and CD sit four bits below their states.
        const uint8_t deltas = uint8_t((current ^ lines) >> 4);
        QueueLines(uint8_t(current | deltas));
    }
    lines = current;
    while (tinyTp.Queued() < ahead && tinyTp.Queued() < tinyTp.SendCredit()) {
        // An SDU never exceeds the guest's limit: the parameters go whole, or wait if the limit
        // can't take them with the length byte.
        const size_t limit = tinyTp.MaxSdu();
        if (limit == 0) break;
        const size_t clen = pending.size() < limit ? pending.size() : 0;
        const size_t room = limit - 1 - clen;
        Bytes sdu(1 + clen + room);
        sdu[0] = uint8_t(clen);
        std::copy(pending.begin(), pending.begin() + long(clen), sdu.begin() + 1);
        const size_t n = port.ToGuest(sdu.data() + 1 + clen, room, now);
        if (!n && !clen) break;
        sdu.resize(1 + clen + n);
        if (clen) pending.clear();
        toGuest += n;
        tinyTp.Send(std::move(sdu));
    }
}

void IrComm::QueueLines(uint8_t value) {
    // States sit in the high four bits and deltas in the low four: the newest states, and every
    // delta since the guest last heard.
    if (pending.size() == 3) pending[2] = uint8_t((value & 0xf0) | ((pending[2] | value) & 0x0f));
    else pending = {piDceLines, 1, value};
}

bool IrComm::Raw::Connect(const Bytes& data, Bytes& reply, uint64_t now) {
    // One port: a raw connect while cooked is open takes it over.
    if (comm.mode == Mode::Cooked) comm.tinyTp.Disconnect(IrLmp::userRequest, now);
    comm.Open(Mode::Raw, now);
    return true;
}

void IrComm::Raw::Data(const Bytes& data, uint64_t now) {
    if (comm.mode != Mode::Raw) return;
    comm.fromGuest += data.size();
    comm.port.FromGuest(data.data(), data.size(), now);
}

void IrComm::Raw::Disconnect(uint64_t now) {
    if (comm.mode == Mode::Raw) {
        comm.mode = Mode::Closed;
        comm.port.Closed(now);
    }
}

void IrComm::Raw::Pump(uint64_t now) {
    if (comm.mode != Mode::Raw) return;
    while (comm.lmp.Queued() < ahead) {
        Bytes data(comm.lmp.MaxData());
        const size_t n = comm.port.ToGuest(data.data(), data.size(), now);
        if (!n) break;
        comm.toGuest += n;
        comm.lmp.Send(rawLsap, data.data(), n);
    }
}

}  // namespace frondIr
