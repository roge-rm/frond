// EZ UART receive with the receiver off, and the SPIM interrupt following its control bits.
#include <cassert>
#include <cstdio>
#include <deque>

#include "EmHAL.h"
#include "EmRegsEZ.h"
#include "EmSession.h"
#include "EmTransportSerial.h"
#include "savestate/Savestate.h"
#include "savestate/SavestateLoader.h"

class Board : public EmRegsEZ {
   public:
    Board() { SetSubBankHandlers(); }
    Bool GetLCDScreenOn() override { return true; }
    Bool GetLCDBacklightOn() override { return false; }
    void GetKeyInfo(int* r, int* c, uint16*, Bool*) override { *r = *c = 0; }
    EmUARTDeviceType GetUARTDevice(int) override { return kUARTIR; }
    // Writes go through the register handlers, as from the CPU.
    void Put(emuptr addr, int size, uint32 value) {
        if (size == 4) SetLong(addr, value);
        else SetWord(addr, value);
    }
    void SetPending(uint16 lo) { f68EZ328Regs.intPendingLo = lo; }
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
                                  EmUARTDragonball::TransactionState) override {}
    void Send(int n, uint8 first) {
        for (int i = 0; i < n; ++i) bytes.push_back(uint8(first + i));
    }
    std::deque<uint8> bytes;
};

constexpr emuptr USTCNT = 0xfffff900, URX = 0xfffff904, SPIMCONT = 0xfffff802,
                 UMISC = 0xfffff908, IPR_LO = 0xfffff312, ISR_LO = 0xfffff30e, IMR = 0xfffff304;
constexpr uint16 UEN = 0x8000, RXEN = 0x4000, TXEN = 0x2000, DATA_READY = 0x2000;
constexpr uint16 SPIM_IRQ = 0x0080, SPIM_IRQEN = 0x0040, SPIM_XCH = 0x0100,
                 SPIM_EN = 0x0200, SPIM_PENDING = 0x0001;

static void CheckUart(Board& board, Line& line) {
    board.Put(UMISC, 2, 0x00a0);  // IrDA, RTS from the FIFO
    // The line runs while the receiver is off, as when Palm OS IR turns RX off to transmit.
    board.Put(USTCNT, 2, UEN | TXEN | 0x0100);
    line.Send(40, 0x10);
    for (int i = 0; i < 4; ++i) assert(!(board.GetWord(URX) & DATA_READY));
    assert(line.bytes.empty());

    // The UART itself off drops bytes too.
    board.Put(USTCNT, 2, 0);
    line.Send(5, 0x60);
    assert(!(board.GetWord(URX) & DATA_READY));
    assert(line.bytes.empty());

    // Once the receiver is on, only what arrives after that is read.
    board.Put(USTCNT, 2, UEN | RXEN | TXEN | 0x0100);
    assert(!(board.GetWord(URX) & DATA_READY));
    line.Send(3, 0x41);
    for (uint8 want = 0x41; want < 0x44; ++want) {
        const uint16 urx = board.GetWord(URX);
        assert(urx & DATA_READY);
        assert((urx & 0xff) == want);
    }
    assert(!(board.GetWord(URX) & DATA_READY));
    puts("EZ UART drops bytes while the receiver is off");
}

static void CheckSpim(Board& board) {
    board.Put(IMR, 4, 0);  // nothing masked
    // An exchange with IRQEN set raises the interrupt.
    board.Put(SPIMCONT, 2, SPIM_EN | SPIM_XCH | SPIM_IRQEN | 7);
    assert(board.GetWord(SPIMCONT) == (SPIM_EN | SPIM_IRQ | SPIM_IRQEN | 7));
    assert(board.GetWord(IPR_LO) & SPIM_PENDING);
    assert(board.GetWord(ISR_LO) & SPIM_PENDING);
    assert(board.GetInterruptLevel() == 4);

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
    SavestateLoader<ChunkType> loader;
    assert(loader.Load(stale.GetBuffer(), stale.GetSize(), board));
    assert(!(board.GetWord(IPR_LO) & SPIM_PENDING));
    assert(!(board.GetWord(ISR_LO) & SPIM_PENDING));
    puts("EZ SPIM interrupt follows IRQ and IRQEN, also after a load");
}

int main() {
    Board board;  // registers itself as the HAL root
    auto clock = EmHAL::onSystemClockChange.AddHandler([] { gSession->SetClockFactor(1); });
    gSession->SetClockFactor(1);
    board.Initialize();
    Line* line = new Line;
    gSession->SetTransportSerial(kUARTIR, line);
    CheckUart(board, *line);
    CheckSpim(board);
    board.Dispose();
    EmHAL::onSystemClockChange.RemoveHandler(clock);
}
