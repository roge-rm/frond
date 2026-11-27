#include "Dcf660w.h"
#include "wifi_access_point.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <utility>

namespace {
constexpr uint32_t cor = 0x3e0;
constexpr uint16_t commandEvent = 0x10, allocateEvent = 8;

uint16_t RunCommand(Dcf660w& card, uint16_t command, uint16_t param = 0) {
    card.WriteIo(2, param);
    card.WriteIo(0, command);
    assert(card.ReadIo(0x30) & commandEvent);
    const uint16_t result = card.ReadIo(8);
    card.WriteIo(0x34, commandEvent);
    return result;
}

void SelectBap(Dcf660w& card, unsigned path, uint16_t id, uint16_t offset) {
    card.WriteIo(path == 0 ? 0x18 : 0x1a, id);
    card.WriteIo(path == 0 ? 0x1c : 0x1e, offset);
}

void CheckCis(Dcf660w& card) {
    bool manfid = false, network = false, config = false, ioAndIrq = false;
    for (uint32_t pos = 0; pos < 128;) {
        const uint8_t type = card.ReadAttribute(pos);
        if (type == 0xff) break;
        assert(card.ReadAttribute(pos + 1) == 0xff);
        const uint8_t length = card.ReadAttribute(pos + 2);
        auto body = [&](unsigned byte) { return card.ReadAttribute(pos + 4 + 2 * byte); };
        switch (type) {
            case 0x20:
                manfid = length == 4 && body(0) == 0x01 && body(1) == 0xd6 && body(2) == 0x05 && body(3) == 0;
                break;
            case 0x21: network = length == 2 && body(0) == 6; break;
            case 0x1a: config = length == 5 && body(2) == 0xe0 && body(3) == 3; break;
            case 0x1b: ioAndIrq = length == 8 && body(6) == 0x46 && body(7) == 0x20; break;
        }
        pos += 2 * (length + 2);
    }
    assert(manfid && network && config && ioAndIrq);
}

void CheckStockProbe(Dcf660w& card) {
    assert(RunCommand(card, 0) == 0);
    for (unsigned i = 0; i < 10; ++i) {
        assert(RunCommand(card, 0x0a, i < 9 ? 0x93e : 0x7c8) == 0x0a);
        assert(card.ReadIo(0x30) == allocateEvent);
        assert(card.ReadIo(0x22) == 0x100 + i);
        card.WriteIo(0x34, allocateEvent);
    }
    card.WriteIo(0x28, 0x4a2d);
    assert(card.ReadIo(0x28) == 0x4a2d);
    constexpr std::array<std::pair<uint16_t, unsigned>, 12> ridLengths{{
        {0xfd0b, 8}, {0xfd02, 8}, {0xfd20, 8}, {0xfd0c, 10}, {0xfd0d, 10}, {0xfd03, 10},
        {0xfd21, 10}, {0xfd04, 10}, {0xfd23, 10}, {0xfd22, 10}, {0xfd0a, 12}, {0xfc01, 6},
    }};
    for (const auto& [rid, size] : ridLengths) {
        assert(RunCommand(card, 0x21, rid) == 0x21);
        SelectBap(card, 1, rid, 0);
        assert(card.ReadIo(0x38) == 1 + size / 2);
        assert(card.ReadIo(0x38) == rid);
        for (unsigned word = 0; word < size / 2; ++word) card.ReadIo(0x38);
        assert(card.ReadIo(0x1e) == size + 4);
    }
    assert(RunCommand(card, 0x21, 0xfd4f) == 0x21);
    SelectBap(card, 0, 0xfd4f, 4);
    assert(card.ReadIo(0x36) == 0);
    assert(RunCommand(card, 0x21, 0xfdff) == 0x7f21);
}

void CheckTransfers(Dcf660w& card) {
    card.WriteIo(0x32, commandEvent | allocateEvent);
    card.WriteIo(2, 12);
    card.WriteIo(0, 0x0a);
    const auto fid = card.ReadIo(0x22);
    assert(card.IrqAsserted() && card.ReadIo(0x30) == (commandEvent | allocateEvent));
    card.WriteIo(0x34, commandEvent);
    assert(card.IrqAsserted() && card.ReadIo(0x30) == allocateEvent);
    card.WriteIo(0x34, allocateEvent);
    assert(!card.IrqAsserted());

    SelectBap(card, 0, fid, 0);
    card.WriteIo(0x36, 0xabcd);
    card.WriteIo(0x36, 0xef01);
    SelectBap(card, 1, fid, 0);
    assert(card.ReadIo(0x38) == 0xabcd && card.ReadIo(0x38) == 0xef01);
    assert(card.ReadIo(0x1c) == 4 && card.ReadIo(0x1e) == 4);
    SelectBap(card, 1, fid, 12);
    assert(card.ReadIo(0x38) == 0xffff && (card.ReadIo(0x1e) & 0x4000));

    SelectBap(card, 0, 0xfc03, 0);
    card.WriteIo(0x36, 2);
    card.WriteIo(0x36, 0xfc03);
    SelectBap(card, 0, 0xfc03, 4);
    card.WriteIo(0x36, 11);
    assert(RunCommand(card, 0x0121, 0xfc03) == 0x21);
    assert(RunCommand(card, 0x21, 0xfc03) == 0x21);
    SelectBap(card, 1, 0xfc03, 4);
    assert(card.ReadIo(0x38) == 11);
    SelectBap(card, 0, 0xfc03, 0);
    card.WriteIo(0x36, 3);
    card.WriteIo(0x36, 0xfc03);
    card.WriteIo(0x36, 6);
    assert(RunCommand(card, 0x0121, 0xfc03) == 0x7f21);
    assert(RunCommand(card, 0x21, 0xfc03) == 0x21);
    SelectBap(card, 1, 0xfc03, 4);
    assert(card.ReadIo(0x38) == 11);
}
void WriteConfig(Dcf660w& card, uint16_t id, const std::vector<uint16_t>& words) {
    SelectBap(card, 0, id, 0);
    card.WriteIo(0x36, uint16_t(1 + words.size()));
    card.WriteIo(0x36, id);
    SelectBap(card, 0, id, 4);
    for (const auto word : words) card.WriteIo(0x36, word);
    assert(RunCommand(card, 0x0121, id) == 0x21);
}

void CheckNetworking(Dcf660w& card) {
    constexpr uint16_t receiveEvent = 1, transmitEvent = 2, infoEvent = 0x80;
    card.WriteIo(0x32, receiveEvent | transmitEvent | allocateEvent | infoEvent);
    card.SetNetworkAvailable(true);
    assert(RunCommand(card, 0x21, 0xfd40) == 0x21);
    SelectBap(card, 1, 0xfd40, 4);
    assert(card.ReadIo(0x38) == 1);  // Disabled before the stock scanner enables its port.
    WriteConfig(card, 0xfc00, {0});
    assert(RunCommand(card, 1) == 1 && !card.Associated());
    std::vector<uint16_t> request(19);
    request[0] = 1u << (WifiAccessPoint::apChannel - 1);
    request[1] = 2;
    WriteConfig(card, 0xfce5, request);
    assert(card.IrqAsserted() && card.ReadIo(0x30) == infoEvent);
    SelectBap(card, 1, card.ReadIo(0x10), 0);
    assert(card.ReadIo(0x38) == 35 && card.ReadIo(0x38) == 0xf103);
    assert(card.ReadIo(0x38) == 1 && card.ReadIo(0x38) == 0);
    assert(card.ReadIo(0x38) == WifiAccessPoint::apChannel);
    card.WriteIo(0x34, infoEvent);
    assert(!card.IrqAsserted());
    request[0] = 1;
    WriteConfig(card, 0xfce5, request);
    SelectBap(card, 1, card.ReadIo(0x10), 0);
    assert(card.ReadIo(0x38) == 3 && card.ReadIo(0x38) == 0xf103 && card.ReadIo(0x38) == 0);
    card.WriteIo(0x34, infoEvent);

    assert(RunCommand(card, 2) == 2);
    WriteConfig(card, 0xfc00, {1});
    assert(RunCommand(card, 1) == 1 && card.Associated());
    SelectBap(card, 1, card.ReadIo(0x10), 0);
    assert(card.ReadIo(0x38) == 2 && card.ReadIo(0x38) == 0xf200 && card.ReadIo(0x38) == 1);
    card.WriteIo(0x34, infoEvent);

    const std::array<uint8_t, 6> serverMac{0x02, 0x10, 0x20, 0x30, 0x40, 0x50};
    const std::array<uint8_t, 6> stationMac{0x02, 0, 0, 0, 0, 3};
    std::vector<uint8_t> txDescriptor(70);
    txDescriptor[14] = 8;
    txDescriptor[15] = 1;  // Data, ToDS.
    std::copy(serverMac.begin(), serverMac.end(), txDescriptor.begin() + 30);
    txDescriptor[44] = 9;
    const std::array<uint8_t, 9> txData{0xaa, 0xaa, 3, 0, 0, 0, 8, 6, 0x42};
    std::copy(txData.begin(), txData.end(), txDescriptor.begin() + 60);
    std::vector<uint8_t> sent;
    card.transmitFrame = [&](const auto& frame) { sent = frame; };
    assert(RunCommand(card, 0x0a, 100) == 0x0a);
    const auto fid = card.ReadIo(0x22);
    card.WriteIo(0x34, allocateEvent);
    SelectBap(card, 0, fid, 0);
    for (size_t at = 0; at < txDescriptor.size(); at += 2)
        card.WriteIo(0x36, uint16_t(txDescriptor[at]) | uint16_t(txDescriptor[at + 1]) << 8);
    assert(RunCommand(card, 0x010b, fid) == 0x0b);
    assert(card.ReadIo(0x30) == (allocateEvent | transmitEvent));
    assert(card.ReadIo(0x22) == fid && card.ReadIo(0x24) == fid);
    card.WriteIo(0x34, allocateEvent | transmitEvent);
    assert((sent == std::vector<uint8_t>{0x02, 0x10, 0x20, 0x30, 0x40, 0x50,
                                         0x02, 0, 0, 0, 0, 3, 8, 6, 0x42}));
    std::vector<uint8_t> rx(stationMac.begin(), stationMac.end());
    rx.insert(rx.end(), serverMac.begin(), serverMac.end());
    rx.insert(rx.end(), {8, 6, 0x43});
    assert(card.ReceiveFrame(rx));
    rx.back() = 0x41;
    assert(card.ReceiveFrame(rx));
    const auto snapshot = card.Save();
    Dcf660w restored;
    assert(restored.Load(snapshot.data(), snapshot.size()) && restored.Save() == snapshot);
    assert(restored.Associated() && restored.IrqAsserted());
    auto broken = snapshot;
    broken[3] = 2;  // Invalid bool.
    assert(!restored.Load(broken.data(), broken.size()) && restored.Save() == snapshot);
    assert(!restored.Load(snapshot.data(), snapshot.size() - 1) && restored.Save() == snapshot);
    assert(card.IrqAsserted() && card.ReadIo(0x30) == receiveEvent);
    SelectBap(card, 1, card.ReadIo(0x20), 44);
    assert(card.ReadIo(0x38) == 9);
    SelectBap(card, 1, card.ReadIo(0x20), 60);
    assert(card.ReadIo(0x38) == 0xaaaa && card.ReadIo(0x38) == 3 && card.ReadIo(0x38) == 0);
    assert(card.ReadIo(0x38) == 0x0608 && card.ReadIo(0x38) == 0x43);
    card.WriteIo(0x34, receiveEvent);
    assert(card.IrqAsserted() && card.ReadIo(0x30) == receiveEvent);
    SelectBap(card, 1, card.ReadIo(0x20), 68);
    assert(card.ReadIo(0x38) == 0x41);
    card.WriteIo(0x34, receiveEvent);
    assert(!card.IrqAsserted());
    card.SetNetworkAvailable(false);
    assert(!card.Associated() && !card.ReceiveFrame(rx));
    SelectBap(card, 1, card.ReadIo(0x10), 4);
    assert(card.ReadIo(0x38) == 2);
    card.transmitFrame = {};
}
}  // namespace

int main() {
    Dcf660w card;
    assert(card.ReadIo(0) == 0xffff);
    CheckCis(card);
    card.WriteAttribute(cor, 0x41);
    CheckStockProbe(card);
    CheckTransfers(card);
    card.WriteAttribute(cor, 0xc1);
    assert(card.ReadAttribute(cor) == 0xc1 && card.ReadIo(0) == 0xffff && !card.IrqAsserted());
    card.WriteAttribute(cor, 0x41);
    assert(card.ReadIo(0x30) == 0 && card.ReadIo(0x28) == 0);
    CheckStockProbe(card);
    CheckNetworking(card);
    card.Reset();
    assert(card.ReadIo(0) == 0xffff && card.ReadAttribute(cor) == 0);
    puts("PASS: DCF-660W CIS, Sharp probe, BAP, host scan, association, RX, TX/reclaim, IRQ and reset");
}
