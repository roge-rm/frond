// EZ and VZ UART receive with the receiver off, and the SPIM interrupt following its control bits.
#include <cassert>
#include <cstdio>
#include <deque>
#include <vector>

#include "EmHAL.h"
#include "EmRegsEZ.h"
#include "EmRegsVZ.h"
#include "EmSession.h"
#include "EmTransportSerial.h"
#include "EmUARTDragonball.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"

static uint64 gNow = 0;

template <typename Regs>
class Board : public Regs {
   public:
    Board() { this->SetSubBankHandlers(); }
    Bool GetLCDScreenOn() override { return true; }
    Bool GetLCDBacklightOn() override { return false; }
    void GetKeyInfo(int* r, int* c, uint16*, Bool*) override { *r = *c = 0; }
    EmUARTDeviceType GetUARTDevice(int uart) override { return uart == 0 ? kUARTIR : kUARTNone; }
    // Writes go through the register handlers, as from the CPU.
    void Put(emuptr addr, int size, uint32 value) {
        if (size == 4) this->SetLong(addr, value);
        else this->SetWord(addr, value);
    }
};

class EzBoard : public Board<EmRegsEZ> {
   public:
    void SetPending(uint16 lo) { f68EZ328Regs.intPendingLo = lo; }
};

class VzBoard : public Board<EmRegsVZ> {
   public:
    VzBoard() { fPortDDataCount = 0xffffffff; }
    void ApplySdctl() override {}
    void SetPending(uint16 lo) { f68VZ328Regs.intPendingLo = lo; }
};

class Line : public EmTransportSerial {
   public:
    bool Open() override { return true; }
    bool Close() override { return true; }
    bool Read(long& n, void* out) override {
        long i = 0;
        for (; i < n && !bytes.empty(); ++i) {
            static_cast<uint8*>(out)[i] = bytes.front();
            bytes.pop_front();
        }
        n = i;
        return true;
    }
    bool Write(long&, const void*) override { return true; }
    bool CanRead() override { return !bytes.empty(); }
    bool CanWrite() override { return true; }
    size_t BytesPending() override { return bytes.size(); }
    void SetRTS(RTSControl) override {}
    void SetDTR(bool) override {}
    void SetBreak(bool) override {}
    bool RequiresSync() override { return false; }
    void OnTransactionStateChange(EmUARTDragonball::TransactionState,
                                  EmUARTDragonball::TransactionState to) override {
        states.push_back(to);
    }
    void Send(int n, uint8 first) {
        for (int i = 0; i < n; ++i) bytes.push_back(uint8(first + i));
    }
    std::deque<uint8> bytes;
    std::vector<EmUARTDragonball::TransactionState> states;
};

constexpr emuptr USTCNT = 0xfffff900, UBAUD = 0xfffff902, URX = 0xfffff904, SPIMCONT = 0xfffff802,
                 UMISC = 0xfffff908, IPR_LO = 0xfffff312, ISR_LO = 0xfffff30e, IMR = 0xfffff304;
constexpr uint16 UEN = 0x8000, RXEN = 0x4000, TXEN = 0x2000, DATA_READY = 0x2000;
constexpr uint16 SPIM_IRQ = 0x0080, SPIM_IRQEN = 0x0040, SPIM_XCH = 0x0100,
                 SPIM_EN = 0x0200, SPIM_PENDING = 0x0001;

// Guest cycles for one byte (10 bits) at the line's baud rate.
static uint64 ByteTime(Line& line) {
    return uint64(gSession->GetClocksPerSecond()) * 10 / line.GetConfig().fBaud;
}

template <typename B>
static void ReadBytes(B& board, uint8 first, int n) {
    for (int i = 0; i < n; ++i) {
        const uint16 urx = board.GetWord(URX);
        assert(urx & DATA_READY);
        assert((urx & 0xff) == uint8(first + i));
    }
    assert(!(board.GetWord(URX) & DATA_READY));
}

template <typename B>
static void CheckUart(B& board, Line& line, const char* name) {
    const uint16 rxOff = UEN | TXEN | 0x0100, rxOn = UEN | RXEN | TXEN | 0x0100;
    board.Put(UMISC, 2, 0x00a0);  // IrDA, RTS from the FIFO
    board.Put(UBAUD, 2, 0x003f);  // prescaler 2, divide by 1
    board.Put(USTCNT, 2, rxOn);
    const uint64 byte = ByteTime(line);
    assert(byte > 0);

    // The receiver off, as when Palm OS IR turns RX off to transmit. Bytes count from when
    // they're first seen and go one byte time each, through CycleSlowly with no register access.
    board.Put(USTCNT, 2, rxOff);
    line.Send(40, 0x10);
    board.CycleSlowly(false);
    assert(line.bytes.size() == 40);
    gNow += 10 * byte;
    board.CycleSlowly(false);
    assert(line.bytes.size() == 30);
    gNow += 40 * byte;
    board.CycleSlowly(false);
    assert(line.bytes.empty());
    assert(!(board.GetWord(URX) & DATA_READY));

    // A reply due while RX is off is dropped, and one due just after it comes back on is read.
    line.Send(5, 0x20);
    board.CycleSlowly(false);
    gNow += 5 * byte;
    board.CycleSlowly(false);
    assert(line.bytes.empty());
    gNow += 3 * byte;
    line.Send(3, 0x41);
    board.CycleSlowly(false);
    gNow += byte / 2;
    board.Put(USTCNT, 2, rxOn);
    ReadBytes(board, 0x41, 3);

    // The UART off drops bytes the same way.
    board.Put(USTCNT, 2, 0);
    line.Send(5, 0x60);
    board.CycleSlowly(false);
    gNow += 5 * byte;
    assert(!(board.GetWord(URX) & DATA_READY));
    assert(line.bytes.empty());
    board.Put(USTCNT, 2, rxOn);
    assert(!(board.GetWord(URX) & DATA_READY));

    // RX off with bytes in the FIFO flushes it and clears DATA_READY; the next byte read after
    // RX is back on is a new one.
    line.Send(3, 0x70);
    board.CycleSlowly(false);
    assert(line.bytes.empty());
    assert((board.GetWord(URX) & (DATA_READY | 0xff)) == (DATA_READY | 0x70));
    board.Put(USTCNT, 2, rxOff);
    assert(!(board.GetWord(URX) & DATA_READY));
    board.Put(USTCNT, 2, rxOn);
    assert(!(board.GetWord(URX) & DATA_READY));
    line.Send(1, 0x80);
    ReadBytes(board, 0x80, 1);

    // UART_ENABLE low with RX_ENABLE still high flushes it too.
    line.Send(3, 0x90);
    board.CycleSlowly(false);
    assert(line.bytes.empty());
    assert((board.GetWord(URX) & (DATA_READY | 0xff)) == (DATA_READY | 0x90));
    board.Put(USTCNT, 2, RXEN | TXEN | 0x0100);
    assert(!(board.GetWord(URX) & DATA_READY));
    board.Put(USTCNT, 2, rxOn);
    assert(!(board.GetWord(URX) & DATA_READY));
    line.Send(1, 0xa0);
    ReadBytes(board, 0xa0, 1);
    printf("%s UART drops bytes while the receiver is off\n", name);
}

// Polls far apart, a baud change in the middle of a drop and bursts over the 64-byte buffer.
template <typename B>
static void CheckUartDropTiming(B& board, Line& line, const char* name) {
    const uint16 rxOff = UEN | TXEN | 0x0100, rxOn = UEN | RXEN | TXEN | 0x0100;
    const uint64 byte = ByteTime(line);

    // A reply that arrives after a long gap with no poll starts its own clock, so it isn't
    // dropped for time that passed before it came.
    board.Put(USTCNT, 2, rxOff);
    line.Send(5, 0x20);
    board.CycleSlowly(false);
    gNow += 50 * byte;
    line.Send(3, 0x41);
    board.CycleSlowly(false);
    assert(line.bytes.size() == 3);
    gNow += byte / 2;
    board.Put(USTCNT, 2, rxOn);
    ReadBytes(board, 0x41, 3);

    // Bytes that come while earlier ones still wait queue behind them.
    board.Put(USTCNT, 2, rxOff);
    line.Send(10, 0x10);
    board.CycleSlowly(false);
    gNow += 4 * byte;
    line.Send(3, 0x50);
    board.CycleSlowly(false);
    assert(line.bytes.size() == 9);
    gNow += 7 * byte;
    board.CycleSlowly(false);
    assert(line.bytes.size() == 2);
    assert(line.bytes.front() == 0x51);
    gNow += 2 * byte;
    board.CycleSlowly(false);
    assert(line.bytes.empty());

    // A baud change mid-drop: time before it goes at the old rate, time after at the new one.
    line.Send(40, 0x10);
    board.CycleSlowly(false);
    gNow += 10 * byte;
    board.Put(UBAUD, 2, 0x033f);  // divide by 8
    assert(line.bytes.size() == 30);
    const uint64 slow = ByteTime(line);
    assert(slow == 8 * byte);
    gNow += 20 * byte;
    board.CycleSlowly(false);
    assert(line.bytes.size() == 28);
    board.Put(UBAUD, 2, 0x003f);
    gNow += 28 * byte;
    board.CycleSlowly(false);
    assert(line.bytes.empty());

    // A burst over 64 bytes drops at the line's rate in more than one read.
    line.Send(200, 0);
    board.CycleSlowly(false);
    gNow += 150 * byte;
    board.CycleSlowly(false);
    assert(line.bytes.size() == 50);
    assert(line.bytes.front() == 150);
    gNow += 50 * byte;
    board.CycleSlowly(false);
    assert(line.bytes.empty());

    // And one read with the receiver on arrives whole and in order through the FIFO.
    board.Put(USTCNT, 2, rxOn);
    line.Send(100, 0);
    ReadBytes(board, 0, 100);
    assert(line.bytes.empty());
    printf("%s UART drop timing: sparse polls, baud change, long bursts\n", name);
}

// Sync mode: bytes reaching a receiver that's off don't start a receive, and RX going off
// mid-receive leaves it waiting for data.
template <typename B>
static void CheckUartSync(B& board, Line& line, const char* name) {
    using TS = EmUARTDragonball::TransactionState;
    const uint16 rxOff = UEN | TXEN | 0x0100, rxOn = UEN | RXEN | TXEN | 0x0100;
    const uint64 byte = ByteTime(line);
    board.SetUARTSync(true);
    line.states.clear();

    board.Put(USTCNT, 2, rxOff);
    line.Send(5, 0x20);
    board.CycleSlowly(false);
    gNow += 5 * byte;
    board.CycleSlowly(false);
    assert(line.bytes.empty());
    assert(line.states.empty());

    board.Put(USTCNT, 2, rxOn);
    line.Send(3, 0x30);
    board.CycleSlowly(false);
    assert(line.states.size() == 1 && line.states.back() == TS::receiving);
    line.Send(2, 0x40);
    board.Put(USTCNT, 2, rxOff);
    board.CycleSlowly(false);
    assert(line.states.size() == 2 && line.states.back() == TS::waitingForData);
    assert(!(board.GetWord(URX) & DATA_READY));
    // The bytes left over go at the line's rate and start nothing.
    gNow += 2 * byte;
    board.CycleSlowly(false);
    assert(line.bytes.empty());
    assert(line.states.size() == 2);

    board.SetUARTSync(false);
    board.Put(USTCNT, 2, rxOn);
    printf("%s UART sync mode: RX off drops and starts no receive\n", name);
}

template <typename B>
static void CheckSpim(B& board, const char* name) {
    board.Put(IMR, 4, 0);  // nothing masked
    // An exchange with IRQEN set raises the interrupt.
    board.Put(SPIMCONT, 2, SPIM_EN | SPIM_XCH | SPIM_IRQEN | 7);
    assert(board.GetWord(SPIMCONT) == (SPIM_EN | SPIM_IRQ | SPIM_IRQEN | 7));
    assert(board.GetWord(IPR_LO) & SPIM_PENDING);
    assert(board.GetWord(ISR_LO) & SPIM_PENDING);
    assert(board.GetInterruptLevel() > 0);

    // Clearing the status bit drops it.
    board.Put(SPIMCONT, 2, SPIM_EN | SPIM_IRQEN | 7);
    assert(!(board.GetWord(IPR_LO) & SPIM_PENDING));
    assert(!(board.GetWord(ISR_LO) & SPIM_PENDING));
    assert(board.GetInterruptLevel() == -1);

    // Clearing the enable with the status bit still set drops it too, and setting it back raises it.
    board.Put(SPIMCONT, 2, SPIM_EN | SPIM_XCH | SPIM_IRQEN | 7);
    assert(board.GetWord(IPR_LO) & SPIM_PENDING);
    board.Put(SPIMCONT, 2, SPIM_EN | SPIM_IRQ | 7);
    assert(!(board.GetWord(IPR_LO) & SPIM_PENDING));
    board.Put(SPIMCONT, 2, SPIM_EN | SPIM_IRQ | SPIM_IRQEN | 7);
    assert(board.GetWord(IPR_LO) & SPIM_PENDING);

    // A session saved with IRQ and IRQEN set loads with the pending bit.
    Savestate<ChunkType> live;
    assert(live.Save(board));
    board.SetPending(0);
    {
        SavestateLoader<ChunkType> loader;
        assert(loader.Load(live.GetBuffer(), live.GetSize(), board));
    }
    assert(board.GetWord(IPR_LO) & SPIM_PENDING);
    assert(board.GetWord(ISR_LO) & SPIM_PENDING);

    // Without IRQEN an exchange sets only the status bit.
    board.Put(SPIMCONT, 2, 0);
    board.Put(SPIMCONT, 2, SPIM_EN | SPIM_XCH | 7);
    assert(board.GetWord(SPIMCONT) & SPIM_IRQ);
    assert(!(board.GetWord(IPR_LO) & SPIM_PENDING));

    // A session saved with a stale pending bit and the control register clear loads without it.
    board.Put(SPIMCONT, 2, 0);
    board.SetPending(SPIM_PENDING);
    Savestate<ChunkType> stale;
    assert(stale.Save(board));
    board.SetPending(0);
    {
        SavestateLoader<ChunkType> loader;
        assert(loader.Load(stale.GetBuffer(), stale.GetSize(), board));
    }
    assert(!(board.GetWord(IPR_LO) & SPIM_PENDING));
    assert(!(board.GetWord(ISR_LO) & SPIM_PENDING));
    printf("%s SPIM interrupt follows IRQ and IRQEN, also after a load\n", name);
}

template <typename B>
static void Check(const char* name) {
    B board;  // registers itself as the HAL root
    auto clock = EmHAL::onSystemClockChange.AddHandler([] { gSession->SetClockFactor(1); });
    gSession->SetClockFactor(1);
    board.Initialize();
    Line* line = new Line;  // owned by the session
    gSession->SetTransportSerial(kUARTIR, line);
    CheckUart(board, *line, name);
    CheckUartDropTiming(board, *line, name);
    CheckUartSync(board, *line, name);
    CheckSpim(board, name);
    board.Dispose();
    EmHAL::onSystemClockChange.RemoveHandler(clock);
}

int main() {
    EmUARTDragonball::testClock = [] { return gNow; };
    Check<EzBoard>("EZ");
    Check<VzBoard>("VZ");
}
