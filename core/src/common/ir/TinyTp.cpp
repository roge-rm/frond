#include "TinyTp.h"

#include <algorithm>

namespace frondIr {

namespace {

// First byte of each PDU: more-to-come (data) or parameters-present (connect), then credit.
constexpr uint8_t flagBit = 0x80, creditMask = 0x7f;
constexpr uint8_t piMaxSdu = 0x01;
// Reassembled SDUs past this are dropped.
constexpr size_t reassemblyLimit = 4096;
// I frames queued at IrLAP before Tiny TP holds back, so a poll isn't flooded.
constexpr size_t lapBacklog = 7;

}  // namespace

TinyTp::TinyTp(IrLmp& lmp, uint8_t lsap, TinyTpUser& user) : lmp(lmp), lsap(lsap), user(user) {
    lmp.Bind(lsap, *this);
}

size_t TinyTp::MaxSdu() const {
    const size_t pdu = lmp.MaxData() - 1;
    return guestMaxSdu ? std::min(pdu, guestMaxSdu) : pdu;
}

void TinyTp::Send(Bytes sdu) { queue.push_back(std::move(sdu)); }

bool TinyTp::Connect(const Bytes& data, Bytes& reply, uint64_t now) {
    if (data.empty()) return false;
    sendCredit = data[0] & creditMask;
    guestMaxSdu = 0;
    size_t i = 1;
    if (data[0] & flagBit) {
        // Parameters: their length, then PI, PL, PV; only MaxSduSize is defined.
        const size_t length = i < data.size() ? data[i++] : 0;
        const size_t end = std::min(data.size(), i + length);
        while (i + 2 <= end) {
            const uint8_t pi = data[i], pl = data[i + 1];
            if (i + 2 + pl > end) break;
            if (pi == piMaxSdu) {
                size_t value = 0;
                for (size_t k = 0; k < pl; ++k) value = value << 8 | data[i + 2 + k];
                guestMaxSdu = value;
            }
            i += 2 + pl;
        }
        i = end;
    }
    Bytes userReply;
    if (!user.Connect(Bytes(data.begin() + std::min(i, data.size()), data.end()), userReply, now)) return false;
    remoteCredit = initialCredit;
    freeCredit = 0;
    queue.clear();
    reassembly.clear();
    // We send no parameters, so the guest's SDUs should fit one PDU; segmented ones are
    // reassembled anyway.
    reply.push_back(uint8_t(initialCredit));
    reply.insert(reply.end(), userReply.begin(), userReply.end());
    return true;
}

void TinyTp::Data(const Bytes& data, uint64_t now) {
    if (data.empty()) return;
    sendCredit = std::min(127u, sendCredit + (data[0] & creditMask));
    // A PDU with no user data is a credit update and uses no credit.
    if (data.size() == 1) return;
    if (remoteCredit) --remoteCredit;
    ++freeCredit;
    reassembly.insert(reassembly.end(), data.begin() + 1, data.end());
    if (reassembly.size() > reassemblyLimit) {
        reassembly.clear();
        return;
    }
    if (data[0] & flagBit) return;
    Bytes sdu;
    sdu.swap(reassembly);
    user.Data(sdu, now);
}

void TinyTp::Disconnect(uint64_t now) {
    queue.clear();
    reassembly.clear();
    sendCredit = remoteCredit = freeCredit = 0;
    user.Disconnect(now);
}

void TinyTp::Pump(uint64_t now) {
    user.Pump(now);
    Flush();
}

void TinyTp::Flush() {
    while (!queue.empty() && sendCredit && lmp.Queued() < lapBacklog) {
        const uint8_t give = uint8_t(std::min(freeCredit, 127u));
        Bytes pdu{give};
        pdu.insert(pdu.end(), queue.front().begin(), queue.front().end());
        queue.pop_front();
        --sendCredit;
        freeCredit -= give;
        remoteCredit += give;
        lmp.Send(lsap, pdu.data(), pdu.size());
    }
    // With nothing to carry it, freed credit goes in a dataless PDU once the guest runs low.
    if (freeCredit && remoteCredit <= initialCredit / 2) {
        const uint8_t give = uint8_t(std::min(freeCredit, 127u));
        const Bytes pdu{give};
        freeCredit -= give;
        remoteCredit += give;
        lmp.Send(lsap, pdu.data(), pdu.size());
    }
}

}  // namespace frondIr
