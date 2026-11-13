#ifndef FROND_68K_HOST_TRANSPORT_H
#define FROND_68K_HOST_TRANSPORT_H

// Cloudpilot's serial transport for one side of the DragonBall UART (the cradle serial port or
// the IR port) on a host link. Bytes from the host reach the guest no faster than the baud
// rate the guest set, so a burst from the host doesn't overrun the UART's receive FIFO.

#include <algorithm>

#include "EmSession.h"
#include "EmTransportSerial.h"
#include "HostLink.h"

// Bytes the guest could have received since the last read at its baud rate (10 bits a byte), up
// to a FIFO's worth when idle. Time is the guest's system cycles.
class BaudCredit68k {
   public:
    void Restart() { mark = gSession->GetSystemCycles(); }
    long Take(long baud) {
        const uint64_t now = gSession->GetSystemCycles();
        const double perByte = gSession->GetClocksPerSecond() * 10.0 / std::max<long>(baud, 300);
        if (now > mark && perByte > 0) {
            const long more = long(double(now - mark) / perByte);
            if (more > 0) {
                credit = std::min<long>(credit + more, 64);
                mark += uint64_t(more * perByte);
            }
        } else if (now < mark) {
            mark = now;
        }
        return credit;
    }
    BaudCredit68k& operator-=(long used) {
        credit -= used;
        return *this;
    }

   private:
    long credit{0};
    uint64_t mark{0};
};

class HostTransport68k : public EmTransportSerial {
   public:
    explicit HostTransport68k(HostLink68k& link) : link(link) {}

    bool Open() override {
        open = true;
        link.DropReceived();
        credit.Restart();
        return true;
    }
    bool Close() override {
        open = false;
        return true;
    }
    bool IsOpen() const { return open; }

    bool Read(long& count, void* data) override {
        const long allowed = Allowed();
        count = long(link.Take(static_cast<uint8_t*>(data), size_t(std::max(0L, std::min(count, allowed)))));
        credit -= count;
        if (frameGap && count > 0 && static_cast<uint8_t*>(data)[count - 1] == kIrdaEof)
            idleUntil = gSession->GetSystemCycles() + FrameGapCycles();
        return true;
    }
    bool Write(long& count, const void* data) override {
        if (open && count > 0) link.Send(static_cast<const uint8_t*>(data), size_t(count));
        return true;
    }
    bool CanRead() override { return open && link.Available() && Allowed() > 0; }
    bool CanWrite() override { return open; }
    size_t BytesPending() override { return open ? std::min<size_t>(link.Available(), size_t(Allowed())) : 0; }

    // On the IR side, IrDA SIR frames (BOF ... EOF) from the host reach the guest with the line idle
    // between them for at least FrameGapCycles. A Palm's IrDA stack leaves milliseconds between
    // frames, which the host link loses when the guest receiving runs behind the sender.
    void SetFrameGap(bool on) { frameGap = on; }

    void SetRTS(RTSControl) override {}
    void SetDTR(bool) override {}
    void SetBreak(bool) override {}
    bool RequiresSync() override { return false; }
    void OnTransactionStateChange(EmUARTDragonball::TransactionState, EmUARTDragonball::TransactionState) override {}

    // Called between slices. Follows the guest's line driver for this side, which a session
    // load doesn't report; with the port closed, what the host sends is dropped.
    void Service(bool lineDriverOn) {
        if (lineDriverOn != open) lineDriverOn ? Open() : Close();
        if (!open) link.DropReceived();
    }

   private:
    long Credit() { return credit.Take(config.fBaud); }
    // Credit, held at zero during a frame gap and stopping after the frame's EOF.
    long Allowed() {
        const long c = Credit();
        if (!frameGap) return c;
        if (gSession->GetSystemCycles() < idleUntil) return 0;
        return std::min<long>(c, long(link.CountThrough(kIrdaEof)));
    }
    // 5 ms: the T415 drops an SNRM that follows a XID frame much closer than that, and the
    // Palm stacks seen leave 10 ms or more there.
    uint64_t FrameGapCycles() const { return uint64_t(gSession->GetClocksPerSecond() * 0.005); }
    static constexpr uint8_t kIrdaEof = 0xc1;
    bool frameGap{false};
    uint64_t idleUntil{0};

    HostLink68k& link;
    bool open{false};
    BaudCredit68k credit;
};

#endif
