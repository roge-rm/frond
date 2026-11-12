#include "IrLmp.h"

#include <algorithm>

namespace frondIr {

namespace {

// LM-PDU header: the control bit and destination LSAP, then the source LSAP.
constexpr uint8_t controlBit = 0x80;
// Control opcodes; bit 7 marks a confirm.
constexpr uint8_t connect = 0x01, disconnect = 0x02, accessMode = 0x03, confirm = 0x80;
constexpr uint8_t connectionless = 0x70;

}  // namespace

IrLmp::IrLmp(IrLap& lap) : lap(lap) {}

void IrLmp::Bind(uint8_t lsap, LmService& service) {
    if (lsap < connectionless) ports[lsap].service = &service;
}

void IrLmp::Send(uint8_t lsap, const uint8_t* data, size_t size) {
    if (!Connected(lsap)) return;
    Bytes pdu{ports[lsap].peer, lsap};
    pdu.insert(pdu.end(), data, data + size);
    lap.Send(std::move(pdu));
}

void IrLmp::Disconnect(uint8_t lsap, uint8_t reason, uint64_t now) {
    if (!Connected(lsap)) return;
    Port& port = ports[lsap];
    port.connected = false;
    SendControl(lsap, port.peer, {disconnect, reason});
    port.service->Disconnect(now);
}

void IrLmp::LinkUp(uint64_t now) {}

void IrLmp::LinkDown(uint64_t now) {
    for (Port& port : ports)
        if (port.connected) {
            port.connected = false;
            port.service->Disconnect(now);
        }
}

void IrLmp::Data(const uint8_t* data, size_t size, uint64_t now) {
    if (size < 2) return;
    const uint8_t local = data[0] & 0x7f, peer = data[1] & 0x7f;
    if (data[0] & controlBit) return Control(local, peer, data + 2, size - 2, now);
    if (local >= connectionless) return;
    Port& port = ports[local];
    if (!port.connected || port.peer != peer) {
        // Data for a connection that isn't there (section 4.4.2, reason 6).
        SendControl(local, peer, {disconnect, 0x06});
        return;
    }
    port.service->Data(Bytes(data + 2, data + size), now);
}

void IrLmp::Control(uint8_t local, uint8_t peer, const uint8_t* data, size_t size, uint64_t now) {
    if (!size) return;
    const uint8_t opcode = data[0];
    if (opcode == connect) {
        Port* port = local < connectionless ? &ports[local] : nullptr;
        Bytes reply;
        if (!port || !port->service) {
            SendControl(local, peer, {disconnect, noClient});
            return;
        }
        // A second connect replaces the first, as after a guest restart.
        if (port->connected) port->service->Disconnect(now);
        // Connect carries a reserved parameter byte before the user data.
        const Bytes user(data + std::min<size_t>(size, 2), data + size);
        if (!port->service->Connect(user, reply, now)) {
            port->connected = false;
            SendControl(local, peer, {disconnect, noClient});
            return;
        }
        port->connected = true;
        port->peer = peer;
        ++connects;
        Bytes body{uint8_t(connect | confirm), 0x00};
        body.insert(body.end(), reply.begin(), reply.end());
        SendControl(local, peer, std::move(body));
    } else if (opcode == disconnect) {
        if (local < connectionless && ports[local].connected && ports[local].peer == peer) {
            ports[local].connected = false;
            ports[local].service->Disconnect(now);
        }
    } else if (opcode == accessMode) {
        // Exclusive mode isn't offered: confirm with status "failure" and multiplexed mode.
        SendControl(local, peer, {uint8_t(accessMode | confirm), 0x01, 0x00});
    }
}

void IrLmp::SendControl(uint8_t local, uint8_t peer, Bytes body) {
    Bytes pdu{uint8_t(controlBit | peer), local};
    pdu.insert(pdu.end(), body.begin(), body.end());
    lap.Send(std::move(pdu));
}

void IrLmp::Pump(uint64_t now) {
    for (Port& port : ports)
        if (port.connected) port.service->Pump(now);
}

}  // namespace frondIr
