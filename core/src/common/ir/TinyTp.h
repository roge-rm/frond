#ifndef FROND_IR_TINYTP_H
#define FROND_IR_TINYTP_H

// Tiny TP (IrDA TinyTP 1.1) on an IrLMP LSAP: credit flow control and reassembly of segmented
// SDUs from the guest. Our own SDUs are never segmented.

#include <cstddef>
#include <cstdint>
#include <deque>

#include "IrLmp.h"

namespace frondIr {

class TinyTpUser {
   public:
    virtual ~TinyTpUser() = default;
    // The guest's connect user data (after Tiny TP's parameters); reply is ours.
    virtual bool Connect(const Bytes& data, Bytes& reply, uint64_t now) = 0;
    virtual void Data(const Bytes& sdu, uint64_t now) = 0;
    virtual void Disconnect(uint64_t now) = 0;
    // Room to send: queue SDUs with Send.
    virtual void Pump(uint64_t now) {}
};

class TinyTp : public LmService {
   public:
    // Credit we give at the start and keep topped up: SDUs the guest may send unanswered.
    static constexpr unsigned initialCredit = 7;

    TinyTp(IrLmp& lmp, uint8_t lsap, TinyTpUser& user);

    // Queues an SDU of at most MaxSdu() bytes; it goes when the guest gives credit.
    void Send(Bytes sdu);
    size_t MaxSdu() const;
    // SDUs waiting for credit.
    size_t Queued() const { return queue.size(); }
    unsigned SendCredit() const { return sendCredit; }
    bool Connected() const { return lmp.Connected(lsap); }
    void Disconnect(uint8_t reason, uint64_t now) { lmp.Disconnect(lsap, reason, now); }

    bool Connect(const Bytes& data, Bytes& reply, uint64_t now) override;
    void Data(const Bytes& data, uint64_t now) override;
    void Disconnect(uint64_t now) override;
    void Pump(uint64_t now) override;

   private:
    void Flush();

    IrLmp& lmp;
    uint8_t lsap;
    TinyTpUser& user;
    // Credit the guest gave us, credit we gave it and haven't seen used, and credit freed since.
    unsigned sendCredit{0}, remoteCredit{0}, freeCredit{0};
    size_t guestMaxSdu{0};
    std::deque<Bytes> queue;
    Bytes reassembly;
};

}  // namespace frondIr

#endif
