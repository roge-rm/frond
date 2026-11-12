//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "pxa_UART.h"

#include <cstdlib>
#include <cstring>

#include "cputil.h"
#include "mem.h"
#include "savestate/savestateAll.h"

#define UART_FIFO_DEPTH 64

#define PXA_UART_SIZE 0x00010000UL

#define UART_FIFO_EMPTY 0xFF

#define SAVESTATE_VERSION 3

struct UartFifo {
    uint8_t read;
    uint8_t write;
    uint16_t buf[UART_FIFO_DEPTH];

    template <typename T>
    void DoSaveLoad(T &chunkHelper) {
        chunkHelper.Do(typename T::Pack8() << read << write).DoBuffer16(buf, sizeof(buf) >> 1);
    }
};

struct PxaUart {
    struct PxaIc *ic;
    uint32_t baseAddr;
    bool fifoOccupancy;
    bool timedByBoard; // wiring only; never serialized
    void (*modeObserver)(void*);void* modeContext; // wiring only

    struct Reschedule reschedule;

    PxaUartReadF readF;
    PxaUartWriteF writeF;
    PxaUartClientIsActive clientIsActiveF;
    PxaUartClientTick clientTickF;
    void *callbackData;

    struct UartFifo TX, RX;

    uint16_t transmitShift;    // char currently "sending"
    uint16_t transmitHolding;  // holding register for no-fifo mode

    uint16_t receiveHolding;  // char just received

    uint8_t irq;
    uint32_t cyclesSinceRecv;
    uint32_t transmitRemaining; // half-bit * divisor units; one batch advances 18
    uint32_t receiveRemaining;  // same units until the receiver takes the next frame
    bool rxTrailing;            // DMA request for trailing bytes until EOR (FCR[TRAIL])
    bool rxEndOfChain;          // IIR[EOC], cleared by reading IIR
    PxaUartDmaRequestF dmaRequestF;  // SoC wiring, never serialized
    void* dmaContext;
    bool detaching;
    bool externalClient;              // callback ownership is never serialized
    PxaUartClientDetach detachF;
    uint8_t modemInputs;               // live pin levels, not session state
    unsigned ioCheckpoints;    // host-only nesting; never serialized
    // The external client while a state load is tried; never serialized.
    struct SuspendedClient {
        bool active;
        PxaUartReadF readF;
        PxaUartWriteF writeF;
        PxaUartClientIsActive activeF;
        PxaUartClientTick tickF;
        PxaUartClientDetach detachF;
        void* context;
        uint8_t modemInputs;
    } suspended;
    uint32_t disableCount;     // IER[UUE] clears seen, for host peers; never serialized

    bool Paced() const { return (fifoOccupancy || timedByBoard) && baseAddr != PXA_HWUART_BASE; }
    uint32_t Divisor() const { return (uint32_t(DLH) << 8) | DLL; }
    uint32_t CharacterUnits() const {
        const uint32_t dataBits = 5 + (LCR & 3);
        const uint32_t stopHalfBits = (LCR & 4) ? (dataBits == 5 ? 3 : 4) : 2;
        return Divisor() * (2 * (1 + dataBits + ((LCR & 8) ? 1 : 0)) + stopHalfBits);
    }
    bool HasModem() const { return Paced() && (baseAddr == PXA_FFUART_BASE || baseAddr == PXA_BTUART_BASE); }
    // Table 10-25: BTUART has only CTS/RTS; STUART has no modem pins.
    uint8_t ModemInputMask() const { return HasModem() ? (baseAddr == PXA_FFUART_BASE ? 0xf0 : 0x10) : 0; }
    uint8_t ModemOutputMask() const { return HasModem() ? (baseAddr == PXA_FFUART_BASE ? 3 : 2) : 0; }
    bool Loopback() const { return Paced() && (MCR & 0x10); }
    bool CanStartTransmit() const {
        return !HasModem() || !(FCR & 1) || !(MCR & 0x20) || (MSR & 0x10);
    }
    bool Clocked() const { return (IER & 0x40) && (!Paced() || Divisor()); }

    uint8_t IER;  // interrupt enable register
    uint8_t IIR;  // interrupt information register
    uint8_t FCR;  // fifo control register
    uint8_t LCR;  // line control register
    uint8_t LSR;  // line status register
    uint8_t MCR;  // modem control register
    uint8_t MSR;  // modem status register
    uint8_t SPR;  // scratchpad register
    uint8_t DLL;  // divisor latch low
    uint8_t DLH;  // divior latch high;
    uint8_t ISR;  // infrared selection register

    uint32_t ReceiveTimeoutTicks() const {
        // CycleBatch0 runs at 102400 Hz. UART baud is 921600 / divisor;
        // receive timeout is four configured character times, not four batches.
        const uint32_t divisor=DLL || DLH ? (uint32_t(DLH)<<8)|DLL : 1;
        const uint32_t bits=1+5+(LCR&3)+((LCR&8)?1:0)+((LCR&4)?2:1);
        return (4*bits*divisor+8)/9;
    }
    template <typename T>
    void DoSaveLoad(T &chunkHelper, uint32_t version) {
        TX.DoSaveLoad(chunkHelper);
        RX.DoSaveLoad(chunkHelper);

        uint8_t legacyIdle=cyclesSinceRecv>255?255:cyclesSinceRecv;
        chunkHelper.Do(typename T::Pack16() << transmitShift << transmitHolding)
            .Do16(receiveHolding)
            .Do(typename T::Pack8() << irq << legacyIdle << IER << IIR)
            .Do(typename T::Pack8() << FCR << LCR << LSR << MCR)
            .Do(typename T::Pack8() << MSR << SPR << DLL << DLH)
            .Do8(ISR);
        if (version>=1) chunkHelper.Do32(cyclesSinceRecv);
        else cyclesSinceRecv=legacyIdle;
        if (version >= 2) chunkHelper.Do32(transmitRemaining);
        else transmitRemaining = 0;
        if (version >= 3)
            chunkHelper.Do32(receiveRemaining).Do(typename T::BoolPack() << rxTrailing << rxEndOfChain);
        else {
            receiveRemaining = 0;
            rxTrailing = rxEndOfChain = false;
        }
    }
};

#define UART_IER_DMAE 0x80   // DMA enable
#define UART_IER_UUE 0x40    // Uart unit enable
#define UART_IER_NRZE 0x20   // NRZI enable
#define UART_IER_RTOIE 0x10  // receiver timeout interrupt enable
#define UART_IER_MIE 0x08    // modem interrupt enable
#define UART_IER_RLSE 0x04   // receiver line status interrupt enable
#define UART_IER_TIE 0x02    // transmit data request interrupt enable
#define UART_IER_RAVIE 0x01  // receiver data available interrupt enable

#define UART_IIR_FIFOES 0xC0       // fifo enable status
#define UART_IIR_EOC 0x20          // DMA end of descriptor chain (PXA27x)
#define UART_IIR_TOD 0x08          // character timout interrupt pending
#define UART_IIR_RECV_ERR 0x06     // receive error(overrun, parity, framing, break)
#define UART_IIR_RECV_DATA 0x04    // receive data available
#define UART_IIR_RCV_TIMEOUT 0x0C  // receive data in buffer and been a while since we've seen more
#define UART_IIR_SEND_DATA 0x02    // transmit fifo requests data
#define UART_IIR_MODEM_STAT 0x00   // modem lines changed state(CTS, DSR, DI, DCD)
#define UART_IIR_NOINT 0x01        // no interrupt pending

#define UART_FCR_BUS 0x20  // PXA27x 32-bit peripheral bus (FIFO mode only)
#define UART_FCR_TRAIL 0x10  // PXA27x: DMA removes trailing bytes
#define UART_FCR_TIL 0x08    // PXA27x: TX interrupt/DMA request when the FIFO is empty
#define UART_FCR_ITL_MASK 0xC0  // mask for ITL part of FCR
#define UART_FCR_ITL_1 0x00     // interrupt when >=1 byte in recv fifo
#define UART_FCR_ITL_8 0x40     // interrupt when >=8 byte in recv fifo
#define UART_FCR_ITL_16 0x80    // interrupt when >=16 byte in recv fifo
#define UART_FCR_ITL_32 0xC0    // interrupt when >=32 byte in recv fifo
#define UART_FCR_RESETTF 0x04   // reset tranmitter fifo
#define UART_FCR_RESETRF 0x02   // reset receiver fifo
#define UART_FCR_TRFIFOE 0x01   // transmit and receive fifo enable

#define UART_LCR_DLAB 0x80      // divisor latch access bit
#define UART_LCR_SB 0x40        // send break
#define UART_LCR_STKYP 0x20     // sticky parity (send parity bit but dont care what value)
#define UART_LCR_EPS 0x10       // even parity select
#define UART_LCR_PEN 0x08       // parity enable
#define UART_LCR_STB 0x04       // stop bits (1 = 2, 0 = 1)
#define UART_LCR_WLS_MASK 0x03  // mask for WLS values
#define UART_LCR_WLS_8 0x03     // 8 bit words
#define UART_LCR_WLS_7 0x02     // 7 bit words
#define UART_LCR_WLS_6 0x01     // 6 bit words
#define UART_LCR_WLS_5 0x00     // 5 bit words

#define UART_LSR_FIFOE 0x80  // fifo contails an error (framing, parity, or break)
#define UART_LSR_TEMT \
    0x40  // tranmitter empty (shift reg is empty and no more byte sin fifo/no byte in holding reg)
#define UART_LSR_TDRQ 0x20  // transmitter data request (see docs)
#define UART_LSR_BI \
    0x10  // send when char at front of fifo (or in holding reg) was a break char (chr reads as zero
          // by itself)
#define UART_LSR_FE 0x08  // same as above, but for framing errors
#define UART_LSR_PE 0x04  // dame as above, but for parity errors
#define UART_LSR_OE 0x02  // recv fifo overran
#define UART_LSR_DR 0x01  // byte received

#define UART_MCR_LOOP 0x10  // local data and modem-control loopback on PXA27x
#define UART_MCR_OUT2 0x08  // when loop is 0 enables or disables UART interrupts
#define UART_MCR_OUT1 0x04  // force RI to 1
#define UART_MCR_RTS 0x02   // 1 -> nRTS is 0
#define UART_MCR_DTR 0x01   // 0 -> nDTR is 0

#define UART_MSR_DCD 0x80
#define UART_MSR_RI 0x40
#define UART_MSR_DSR 0x20
#define UART_MSR_CTS 0x10
#define UART_MSR_DDCD 0x08  // dcd changed since last read
#define UART_MSR_TERI 0x04  // asserted RI has fallen since last read
#define UART_MSR_DDSR 0x02  // dsr changed since last read
#define UART_MSR_DCTS 0x01  // cts changed since last read

static void pxaUartPrvRecalc(struct PxaUart *uart);

static void pxaUartPrvIrq(struct PxaUart *uart, bool raise) {
    pxaIcInt(uart->ic, uart->irq,
             (uart->IER & UART_IER_UUE) &&
                 (uart->Loopback() || (!(uart->MCR & UART_MCR_LOOP) && (uart->MCR & UART_MCR_OUT2))) &&
                 raise /* only raise if ints are enabled */);
}

static uint_fast16_t pxaUartPrvDefaultRead(
    void *userData)  // these are special funcs since they always get their own userData - the uart
                     // pointer :)
{
    (void)userData;

    return UART_CHAR_NONE;  // we read nothing..as always
}

static void pxaUartPrvDefaultWrite(uint_fast16_t chr,
                                   void *userData)  // these are special funcs since they always get
                                                    // their own userData - the uart pointer :)
{
    (void)chr;
    (void)userData;

    // nothing to do here
}

static uint16_t pxaUartPrvGetchar(struct PxaUart *uart) {
    PxaUartReadF func = uart->readF;

    void *data = (func == pxaUartPrvDefaultRead) ? uart : uart->callbackData;

    return func(data);
}

static void pxaUartPrvPutchar(struct PxaUart *uart, uint_fast16_t chr) {
    PxaUartWriteF func = uart->writeF;
    void *data = (func == pxaUartPrvDefaultWrite) ? uart : uart->callbackData;

    func(chr, data);
}

static uint_fast8_t pxaUartPrvFifoUsed(const struct UartFifo *fifo)  // return num spots used
{
    uint_fast8_t v;

    if (fifo->read == UART_FIFO_EMPTY) return 0;

    v = fifo->write + UART_FIFO_DEPTH - fifo->read;

    if (v > UART_FIFO_DEPTH) v -= UART_FIFO_DEPTH;

    return v;
}

static void pxaUartPrvFifoFlush(struct UartFifo *fifo) {
    fifo->read = UART_FIFO_EMPTY;
    fifo->write = UART_FIFO_EMPTY;
}

static bool pxaUartPrvFifoPut(struct UartFifo *fifo, uint_fast16_t val)  // return success
{
    if (fifo->read == UART_FIFO_EMPTY) {
        fifo->read = 0;
        fifo->write = 1;
        fifo->buf[0] = val;
    } else if (fifo->read != fifo->write) {  // only if not full

        fifo->buf[fifo->write++] = val;
        if (fifo->write == UART_FIFO_DEPTH) fifo->write = 0;
    } else
        return false;

    return true;
}

static uint_fast16_t pxaUartPrvFifoGet(struct UartFifo *fifo) {
    uint_fast16_t ret;

    if (fifo->read == UART_FIFO_EMPTY) {
        ret = 0xFFFF;  // why not?
    } else {
        ret = fifo->buf[fifo->read++];
        if (fifo->read == UART_FIFO_DEPTH) fifo->read = 0;

        if (fifo->read == fifo->write) {  // it is now empty

            fifo->read = UART_FIFO_EMPTY;
            fifo->write = UART_FIFO_EMPTY;
        }
    }
    return ret;
}

static uint_fast16_t pxaUartPrvFifoPeekNth(struct UartFifo *fifo, uint_fast8_t n) {
    uint_fast16_t ret;

    if (fifo->read == UART_FIFO_EMPTY) {
        ret = 0xFFFF;  // why not?
    } else {
        n += fifo->read;
        if (n >= UART_FIFO_DEPTH) n -= UART_FIFO_DEPTH;
        ret = fifo->buf[n];
    }
    return ret;
}

static uint_fast16_t pxaUartPrvFifoPeek(struct UartFifo *fifo) {
    return pxaUartPrvFifoPeekNth(fifo, 0);
}

static void pxaUartPrvSendChar(struct PxaUart *uart, uint_fast16_t v) {
    if (uart->LSR & UART_LSR_TEMT) {  // if transmit, put in shift register immediately if it's idle

        uart->transmitShift = v;
        uart->transmitRemaining = 0;
        uart->LSR &= ~UART_LSR_TEMT;
        uart->reschedule.rescheduleCb(uart->reschedule.ctx, RESCHEDULE_TASK_UART);
    } else if (uart->FCR & UART_FCR_TRFIFOE) {  // put in tx fifo if in fifo mode

        pxaUartPrvFifoPut(&uart->TX, v);
        if (pxaUartPrvFifoUsed(&uart->TX) >
            UART_FIFO_DEPTH / 2)  // we go went below half-full buffer - set appropriate bit...
            uart->LSR &= ~UART_LSR_TDRQ;
    } else if (uart->LSR & UART_LSR_TDRQ) {  // send without fifo if in polled mode

        uart->transmitHolding = v;
        uart->LSR &= ~UART_LSR_TDRQ;
    } else {
        // nothing to do - buffer is full so we ignore this request
    }
}

static void pxaUartPrvModemStatus(PxaUart* uart) {
    if (!uart->Paced()) return;
    if (!uart->HasModem()) { uart->MSR = 0; return; }
    const uint8_t old = uart->MSR & 0xf0;
    const uint8_t pins = uart->Loopback() ?
        ((uart->MCR & 1) ? UART_MSR_DSR : 0) | ((uart->MCR & 2) ? UART_MSR_CTS : 0) |
        ((uart->MCR & 4) ? UART_MSR_RI : 0) | ((uart->MCR & 8) ? UART_MSR_DCD : 0) : uart->modemInputs;
    const uint8_t next = pins & uart->ModemInputMask();
    const uint8_t changed = old ^ next;
    uint8_t delta = ((changed & UART_MSR_CTS) ? UART_MSR_DCTS : 0) |
                    ((changed & UART_MSR_DSR) ? UART_MSR_DDSR : 0) |
                    ((changed & UART_MSR_DCD) ? UART_MSR_DDCD : 0) |
                    ((old & UART_MSR_RI) && !(next & UART_MSR_RI) ? UART_MSR_TERI : 0);
    uart->MSR = next | (uart->MSR & 0x0f) | delta;
}

static bool pxaUartPrvMemAccessF(void *userData, uint32_t pa, uint_fast8_t size, bool write,
                                 void *buf) {
    struct PxaUart *uart = (struct PxaUart *)userData;
    bool DLAB = (uart->LCR & UART_LCR_DLAB) != 0;
    bool recalcValues = false;
    uint_fast8_t t;
    uint32_t val = 0;
    const bool packedFifo = uart->fifoOccupancy && uart->baseAddr != PXA_HWUART_BASE && !DLAB &&
        pa == uart->baseAddr &&
        (uart->FCR & (UART_FCR_BUS | UART_FCR_TRFIFOE)) ==
            (UART_FCR_BUS | UART_FCR_TRFIFOE);

    if (size != 4 && size != 1 && !(size == 2 && packedFifo)) {
        fprintf(stderr, "%s: Unexpected %s of %u bytes to 0x%08lx\n", __func__,
                write ? "write" : "read", size, (unsigned long)pa);
        // Ignored, and a read gives zero, the same in both engines.
        if (!write) memset(buf, 0, size);
        return true;
    }

    pa = (pa - uart->baseAddr) >> 2;

    if (write) {
        recalcValues = true;
        const uint32_t raw = size == 1 ? *(uint8_t *)buf :
            size == 2 ? *(uint16_t *)buf : *(uint32_t *)buf;
        val = raw & 0xff;

        switch (pa) {
            case 0:
                if (DLAB) {  // if DLAB - set "baudrate"...
                    uart->DLL = val;
                    recalcValues = false;
                } else {
                    // PXA27x manual 10.4.5: PIO transfers 1/2/4 bytes,
                    // little-endian. Byte mode ignores the upper bus lanes.
                    for (unsigned i = 0; i < (packedFifo ? size : 1); ++i)
                        pxaUartPrvSendChar(uart, (raw >> (8 * i)) & 0xff);
                }
                break;

            case 1:
                if (DLAB) {
                    uart->DLH = val;
                    recalcValues = false;
                } else {
                    t = uart->IER ^ val;

                    if ((t & UART_IER_DMAE) && !uart->Paced()) {
                        // Only the PXA27x UART DMA interface is modelled.
                        ERR("pxaUART: DMA mode cannot be enabled");
                        t &= ~UART_IER_DMAE;  // undo the change
                    }

                    if ((t & UART_IER_UUE) && !(val & UART_IER_UUE)) ++uart->disableCount;
                    uart->IER ^= t;
                }
                break;

            case 2:
                t = uart->FCR ^ val;
                if (!(val & UART_FCR_TRFIFOE) || (t & UART_FCR_TRFIFOE)) {
                    pxaUartPrvFifoFlush(&uart->TX);
                    pxaUartPrvFifoFlush(&uart->RX);
                    uart->rxTrailing = uart->rxEndOfChain = false;
                    const bool shifting = uart->Paced() && !(uart->LSR & UART_LSR_TEMT);
                    uart->LSR = UART_LSR_TDRQ | (shifting ? 0 : UART_LSR_TEMT);
                    uart->IIR = (val & UART_FCR_TRFIFOE ? UART_IIR_FIFOES : 0) | UART_IIR_NOINT;
                }
                if (val & UART_FCR_TRFIFOE) {
                    if (val & UART_FCR_RESETRF) {
                        pxaUartPrvFifoFlush(&uart->RX);
                        uart->LSR &= ~UART_LSR_DR;
                        uart->rxTrailing = uart->rxEndOfChain = false;
                    }
                    if (val & UART_FCR_RESETTF) {
                        pxaUartPrvFifoFlush(&uart->TX);
                        // RESETTF clears the FIFO, not the current shift frame
                        // (PXA27x manual table 10-12). Keep its remaining time.
                        uart->LSR |= UART_LSR_TDRQ;
                        if (!uart->Paced()) {
                            uart->LSR |= UART_LSR_TEMT;
                            uart->transmitRemaining = 0;
                        }
                    }
                }
                // Reset bits are pulses, including when FIFO mode was already on.
                uart->FCR = val & ~(UART_FCR_RESETRF | UART_FCR_RESETTF);
                break;

            case 3:
                t = uart->LCR ^ val;
                if (t & UART_LCR_SB) {
                    if (val & UART_LCR_SB) {  // break set (tx line pulled low)

                        // nothing
                    } else {  // break cleared (tx line released)

                        pxaUartPrvSendChar(uart, UART_CHAR_BREAK);
                    }
                }
                uart->LCR = val;
                break;

            case 4:
                uart->MCR = val;
                pxaUartPrvModemStatus(uart);
                break;

            case 7:
                uart->SPR = val;
                break;

            case 8:
                uart->ISR = val;
                if (val & 3) {
                    fprintf(stderr, "UART: IrDA mode set on UART\n");
                }
                break;
        }
    } else {
        switch (pa) {
            case 0:
                if (DLAB)
                    val = uart->DLL;
                else if (!(uart->LSR & UART_LSR_DR)) {  // no data-> too bad

                    val = 0;
                } else if (uart->FCR & UART_FCR_TRFIFOE) {  // fifo mode -> read fifo

                    for (unsigned i = 0; i < (packedFifo ? size : 1); ++i) {
                        // Over-reading returns unspecified data in empty lanes;
                        // choose zero without advancing the empty FIFO.
                        if (!pxaUartPrvFifoUsed(&uart->RX)) break;
                        val |= uint32_t(pxaUartPrvFifoGet(&uart->RX) & 0xff) << (8 * i);
                    }
                    if (!pxaUartPrvFifoUsed(&uart->RX)) uart->LSR &= ~UART_LSR_DR;
                    // A FIFO read clears the character timeout and restarts it (10.4.2.1.2).
                    if (uart->Paced()) uart->cyclesSinceRecv = 0;
                    recalcValues = true;  // error bits might have changed
                } else {                  // polled mode -> read rx polled reg

                    val = uart->receiveHolding & 0xff;
                    uart->LSR &= ~UART_LSR_DR;
                    recalcValues = true;
                }
                break;

            case 1:
                if (DLAB)
                    val = uart->DLH;
                else
                    val = uart->IER;
                break;

            case 2:
                val = uart->IIR;
                if (uart->rxEndOfChain) {  // EOC clears on read
                    uart->rxEndOfChain = false;
                    recalcValues = true;
                }
                break;

            case 3:
                val = uart->LCR;
                break;

            case 4:
                val = uart->MCR;
                break;

            case 5:
                val = uart->LSR;
                if (uart->LSR & UART_LSR_OE) {  // overrun clears on read
                    uart->LSR &= ~UART_LSR_OE;
                    recalcValues = true;
                }
                break;

            case 6:
                val = uart->MSR;
                if (uart->Paced()) { uart->MSR &= 0xf0; recalcValues = true; }
                break;

            case 7:
                val = uart->SPR;
                break;

            case 8:
                val = uart->ISR;
                break;
            case 9: // PXA270 receive FIFO occupancy (FOR), read-only
                if (uart->fifoOccupancy && (uart->FCR & UART_FCR_TRFIFOE)) val=pxaUartPrvFifoUsed(&uart->RX);
                break;
        }
        if (size == 1)
            *(uint8_t *)buf = val;
        else if (size == 2)
            *(uint16_t *)buf = val;
        else
            *(uint32_t *)buf = val;
    }

    if (recalcValues) pxaUartPrvRecalc(uart);
    if (write && uart->modeObserver && ((pa==0 && DLAB) || pa==1 || pa==2 || pa==3 || pa==8))
        uart->modeObserver(uart->modeContext);
    if (write && pxaUartTaskRequired(uart))
        uart->reschedule.rescheduleCb(uart->reschedule.ctx,RESCHEDULE_TASK_UART);

    return true;
}

void pxaUartSetFuncs(struct PxaUart *uart, PxaUartReadF readF, PxaUartWriteF writeF,
                     PxaUartClientIsActive clientIsActiveF, PxaUartClientTick clientTickF,
                     void *userData) {
    if (uart->ioCheckpoints || uart->detaching) return;
    pxaUartDetachExternal(uart);
    if (!readF)
        readF = pxaUartPrvDefaultRead;  // these are special funcs since they get their own private
                                        // data - the uart :)
    if (!writeF) writeF = pxaUartPrvDefaultWrite;

    uart->readF = readF;
    uart->writeF = writeF;
    uart->clientIsActiveF = clientIsActiveF;
    uart->clientTickF = clientTickF;
    uart->callbackData = userData;
}

void pxaUartResync(PxaUart* uart) {
    pxaUartPrvModemStatus(uart);
    pxaUartPrvRecalc(uart);
}

bool pxaUartSetModemInputs(PxaUart* uart, uint8_t asserted) {
    if (!uart->HasModem() || uart->ioCheckpoints || uart->detaching || (asserted & ~uart->ModemInputMask())) return false;
    uart->modemInputs = asserted;
    pxaUartResync(uart);
    uart->reschedule.rescheduleCb(uart->reschedule.ctx, RESCHEDULE_TASK_UART);
    return true;
}

static bool pxaUartPrvRtsAsserted(const PxaUart* uart) {
    if (!(uart->MCR & UART_MCR_RTS)) return false;
    if ((uart->FCR & UART_FCR_TRFIFOE) && (uart->MCR & 0x20)) {
        const unsigned threshold[] = {1, 8, 16, 32};
        if (pxaUartPrvFifoUsed(&uart->RX) >= threshold[uart->FCR >> 6]) return false;
    }
    return true;
}

uint8_t pxaUartGetModemOutputs(const PxaUart* uart) {
    if (!uart->HasModem() || uart->Loopback()) return 0;
    uint8_t outputs = uart->MCR & uart->ModemOutputMask();
    if (!pxaUartPrvRtsAsserted(uart)) outputs &= ~UART_MCR_RTS;
    return outputs;
}

bool pxaUartHwReceiveReady(const PxaUart* uart) {
    return uart->baseAddr==PXA_HWUART_BASE && uart->Clocked() && uart->Divisor() &&
        !(uart->MCR & UART_MCR_LOOP) && pxaUartPrvRtsAsserted(uart);
}

void pxaUartDetachExternal(PxaUart* uart) {
    if (!uart->externalClient || uart->ioCheckpoints) return;
    uart->detaching = true;
    struct Guard { PxaUart* uart; ~Guard() { uart->detaching = false; } } guard{uart};
    const auto notify = uart->detachF;
    void* context = uart->callbackData;
    uart->externalClient = false;
    uart->detachF = nullptr;
    uart->readF = pxaUartPrvDefaultRead;
    uart->writeF = pxaUartPrvDefaultWrite;
    uart->clientIsActiveF = nullptr;
    uart->clientTickF = nullptr;
    uart->callbackData = nullptr;
    uart->modemInputs = 0;
    pxaUartResync(uart);
    notify(context); // exactly once; clear ownership before notifying
}

void pxaUartSuspendExternal(PxaUart* uart) {
    if (!uart->externalClient || uart->ioCheckpoints || uart->suspended.active) return;
    uart->suspended = {true, uart->readF, uart->writeF, uart->clientIsActiveF, uart->clientTickF,
                       uart->detachF, uart->callbackData, uart->modemInputs};
    // Same register effect as a detach, without the notification. detaching stays set
    // until resume, so attach, modem input and I/O are refused meanwhile.
    uart->detaching = true;
    uart->externalClient = false;
    uart->detachF = nullptr;
    uart->readF = pxaUartPrvDefaultRead;
    uart->writeF = pxaUartPrvDefaultWrite;
    uart->clientIsActiveF = nullptr;
    uart->clientTickF = nullptr;
    uart->callbackData = nullptr;
    uart->modemInputs = 0;
    pxaUartResync(uart);
}

void pxaUartResumeExternal(PxaUart* uart, bool keep) {
    if (!uart->suspended.active) return;
    const auto client = uart->suspended;
    uart->suspended = {};
    struct Guard { PxaUart* uart; ~Guard() { uart->detaching = false; } } guard{uart};
    if (!keep) {
        client.detachF(client.context);
        return;
    }
    // No resync: the caller loads the UART registers next, with these pins.
    uart->readF = client.readF;
    uart->writeF = client.writeF;
    uart->clientIsActiveF = client.activeF;
    uart->clientTickF = client.tickF;
    uart->detachF = client.detachF;
    uart->callbackData = client.context;
    uart->modemInputs = client.modemInputs;
    uart->externalClient = true;
}

bool pxaUartAttachExternal(PxaUart* uart, PxaUartReadF readF, PxaUartWriteF writeF,
                          PxaUartClientIsActive activeF, PxaUartClientTick tickF,
                          PxaUartClientDetach detachF, void* context) {
    if (!uart->HasModem() || uart->baseAddr != PXA_FFUART_BASE || !detachF ||
        uart->ioCheckpoints || uart->detaching || uart->externalClient || uart->Loopback() ||
        !(uart->LSR & UART_LSR_TEMT) || (uart->LSR & UART_LSR_DR)) return false;
    pxaUartSetFuncs(uart, readF, writeF, activeF, tickF, context);
    uart->externalClient = true;
    uart->detachF = detachF;
    uart->modemInputs = 0; // explicit peer readiness, never saved CTS from an older peer
    pxaUartResync(uart);
    return true;
}

static unsigned pxaUartPrvTriggerLevel(const PxaUart* uart) {
    static const unsigned levels[] = {1, 8, 16, 32};
    return levels[uart->FCR >> 6];
}

static void pxaUartPrvReceive(PxaUart* uart, uint_fast16_t value) {
    uart->cyclesSinceRecv = 0;
    if (uart->FCR & UART_FCR_TRFIFOE) {
        // A full FIFO keeps its bytes and discards the new one (LSR[OE]).
        if (!pxaUartPrvFifoPut(&uart->RX, value)) uart->LSR |= UART_LSR_OE;
        // Reaching the trigger level is a normal DMA request, not trailing bytes.
        if (pxaUartPrvFifoUsed(&uart->RX) >= pxaUartPrvTriggerLevel(uart)) uart->rxTrailing = false;
    } else if (uart->LSR & UART_LSR_DR) {
        uart->LSR |= UART_LSR_OE;
    } else uart->receiveHolding = value;
    uart->LSR |= UART_LSR_DR;
}

struct PxaUart *pxaUartInit(struct ArmMem *physMem, struct Reschedule reschedule, struct PxaIc *ic,
                            uint32_t baseAddr, uint8_t irq, bool fifoOccupancy) {
    struct PxaUart *uart = (struct PxaUart *)malloc(sizeof(*uart));

    if (!uart) ERR("cannot alloc UART at 0x%08x", baseAddr);

    memset(uart, 0, sizeof(*uart));
    uart->ic = ic;
    uart->reschedule = reschedule;
    uart->irq = irq;
    uart->baseAddr = baseAddr;
    uart->fifoOccupancy = fifoOccupancy;
    uart->IIR = UART_IIR_NOINT;
    uart->IER = UART_IER_UUE | UART_IER_NRZE;  // uart on
    uart->LSR = UART_LSR_TEMT | UART_LSR_TDRQ;
    uart->MSR = uart->Paced() && !uart->HasModem() ? 0 : UART_MSR_CTS;
    uart->modemInputs = uart->MSR;
    pxaUartPrvFifoFlush(&uart->TX);
    pxaUartPrvFifoFlush(&uart->RX);

    pxaUartSetFuncs(uart, nullptr, nullptr, nullptr, nullptr, nullptr);

    if (!memRegionAdd(physMem, baseAddr, PXA_UART_SIZE, pxaUartPrvMemAccessF, uart))
        ERR("cannot add UART at 0x%08x to MEM\n", baseAddr);

    return uart;
}

void pxaUartProcess(struct PxaUart *uart)  // advance one serial-clock batch
{
    uint_fast16_t v;
    uint_fast8_t t;

    if (uart->ioCheckpoints || uart->detaching || !uart->Clocked()) return;

    if (!uart->Loopback() && uart->clientTickF) uart->clientTickF(uart->callbackData);

    // 102400 batches/sec; the 921600/divisor baud clock advances nine bits
    // (18 half-bits) per batch. Carry fractional time between queued frames.
    bool receivedLoop = false;
    uint32_t budget = 18;
    while (!(uart->LSR & UART_LSR_TEMT)) {
        if (uart->Paced()) {
            if (!uart->transmitRemaining) {
                if (!uart->CanStartTransmit()) break;
                uart->transmitRemaining = uart->CharacterUnits();
            }
            if (uart->transmitRemaining > budget) {
                uart->transmitRemaining -= budget;
                break;
            }
            budget -= uart->transmitRemaining;
        }
        if (uart->Loopback()) {
            pxaUartPrvReceive(uart, uart->transmitShift);
            receivedLoop = true;
        } else pxaUartPrvPutchar(uart, uart->transmitShift);

        if (uart->FCR & UART_FCR_TRFIFOE) {  // fifo mode

            t = pxaUartPrvFifoUsed(&uart->TX);

            if (t--) {
                uart->transmitShift = pxaUartPrvFifoGet(&uart->TX);
                if (t <= UART_FIFO_DEPTH / 2)
                    uart->LSR |= UART_LSR_TDRQ;  // above half full - clear TDRQ bit
            } else {
                uart->LSR |= UART_LSR_TEMT;
            }
        } else if (uart->LSR & UART_LSR_TDRQ) {
            uart->LSR |= UART_LSR_TEMT;
        } else {
            uart->transmitShift = uart->transmitHolding;
            uart->LSR |= UART_LSR_TDRQ;
        }
        uart->transmitRemaining = 0;
        if (!uart->Paced() || !budget) break;
    }

    // now process receiving
    bool received = receivedLoop;
    if (uart->Loopback()) {
    } else if (!uart->Paced()) {
        v = pxaUartPrvGetchar(uart);
        if (v != UART_CHAR_NONE) {
            pxaUartPrvReceive(uart, v);
            received = true;
        }
    } else {
        // A byte from the peer lands in the receiver at once; the line then
        // carries no other byte for one frame at the programmed baud and LCR.
        // On an idle line a new byte ends in this batch, so no time carries.
        uint32_t receiveBudget = 18;
        bool busy = uart->receiveRemaining != 0;
        while (true) {
            if (uart->receiveRemaining > receiveBudget) {
                uart->receiveRemaining -= receiveBudget;
                break;
            }
            receiveBudget = busy ? receiveBudget - uart->receiveRemaining : 0;
            uart->receiveRemaining = 0;
            v = pxaUartPrvGetchar(uart);
            if (v == UART_CHAR_NONE) break;
            pxaUartPrvReceive(uart, v);
            received = busy = true;
            uart->receiveRemaining = uart->CharacterUnits();
        }
    }
    if (!received && uart->cyclesSinceRecv <= uart->ReceiveTimeoutTicks()) ++uart->cyclesSinceRecv;

    pxaUartPrvRecalc(uart);
}

static void pxaUartPrvRecalcCharBits(struct PxaUart *uart, uint_fast16_t c) {
    if (c & UART_CHAR_BREAK) uart->LSR |= UART_LSR_BI;

    if (c & UART_CHAR_FRAME_ERR) uart->LSR |= UART_LSR_FE;

    if (c & UART_CHAR_PAR_ERR) uart->LSR |= UART_LSR_PE;
}

static void pxaUartPrvRecalc(struct PxaUart *uart) {
    bool errorSet = false;
    uint_fast8_t v;

    uart->LSR &= ~UART_LSR_FIFOE;
    uart->IIR &= UART_IIR_FIFOES;  // clear all other bits, EOC is set again below
    uart->LSR &= ~(UART_LSR_BI | UART_LSR_FE | UART_LSR_PE);

    bool rxRequest = false, txRequest = false;
    const bool dma = uart->Paced() && (uart->IER & UART_IER_DMAE) && (uart->IER & UART_IER_UUE);

    if (uart->FCR & UART_FCR_TRFIFOE) {  // fifo mode

        // FIFOE: any byte in the FIFO has an error (table 10-17).
        bool fifoError = false;
        for (v = 0; v < pxaUartPrvFifoUsed(&uart->RX); v++) {
            if (pxaUartPrvFifoPeekNth(&uart->RX, v) >> 8) {
                fifoError = true;
                break;
            }
        }
        const uint_fast8_t used = pxaUartPrvFifoUsed(&uart->RX);
        const bool frontError = used && (pxaUartPrvFifoPeek(&uart->RX) >> 8);
        if (uart->Paced()) {
            if (fifoError) uart->LSR |= UART_LSR_FIFOE;
            // Line status for the byte at the front or an overrun. With DMA on,
            // any error in the FIFO stops receive requests and interrupts
            // without a mask so the driver drains by PIO (10.4.2.5).
            if (((uart->IER & UART_IER_RLSE) && (frontError || (uart->LSR & UART_LSR_OE))) ||
                (dma && fifoError)) {
                uart->IIR |= UART_IIR_RECV_ERR;
                errorSet = true;
            }
        } else if (fifoError && (uart->IER & UART_IER_RLSE)) {
            uart->LSR |= UART_LSR_FIFOE;
            uart->IIR |= UART_IIR_RECV_ERR;
            errorSet = true;
        }

        v = used;
        if (v) pxaUartPrvRecalcCharBits(uart, pxaUartPrvFifoPeek(&uart->RX));

        switch (uart->FCR & UART_FCR_ITL_MASK) {
            case UART_FCR_ITL_1:
                v = v >= 1;
                break;

            case UART_FCR_ITL_8:
                v = v >= 8;
                break;

            case UART_FCR_ITL_16:
                v = v >= 16;
                break;

            case UART_FCR_ITL_32:
                v = v >= 32;
                break;
        }
        if (v && (uart->IER & UART_IER_RAVIE) && !errorSet) {
            errorSet = true;
            uart->IIR |= UART_IIR_RECV_DATA;
        }
        const bool timedOut = used && uart->cyclesSinceRecv >= uart->ReceiveTimeoutTicks();
        // Leaving DMA or TRAIL mid-trail hands the trailing bytes back to the CPU.
        if (!dma || !(uart->FCR & UART_FCR_TRAIL)) uart->rxTrailing = false;
        // With FCR[TRAIL], DMA takes the trailing bytes after the character
        // timeout until EOR or EOC; otherwise the timeout interrupts the CPU.
        if (dma && (uart->FCR & UART_FCR_TRAIL) && timedOut && !v && !uart->rxEndOfChain)
            uart->rxTrailing = true;
        if (used && (timedOut || uart->rxEndOfChain) && !uart->rxTrailing &&
            (uart->IER & (dma ? UART_IER_DMAE : UART_IER_RAVIE)) && (uart->IER & UART_IER_RTOIE) &&
            !errorSet) {
            errorSet = true;
            uart->IIR |= UART_IIR_RCV_TIMEOUT;
        }
        if (dma) {
            const uint_fast8_t txUsed = pxaUartPrvFifoUsed(&uart->TX);
            rxRequest = !fifoError && (v || uart->rxTrailing);
            txRequest = (uart->FCR & UART_FCR_TIL) ? !txUsed : txUsed < UART_FIFO_DEPTH / 2;
        }
        if (uart->Paced()) {
            // TDRQ: half empty, or empty with FCR[TIL] (table 10-12).
            const uint_fast8_t txUsed = pxaUartPrvFifoUsed(&uart->TX);
            if ((uart->FCR & UART_FCR_TIL) ? !txUsed : txUsed <= UART_FIFO_DEPTH / 2)
                uart->LSR |= UART_LSR_TDRQ;
            else
                uart->LSR &= ~UART_LSR_TDRQ;
        }
    } else {  // polling mode

        uint_fast16_t c = uart->receiveHolding;

        if (uart->LSR & UART_LSR_DR) {
            pxaUartPrvRecalcCharBits(uart, c);

            if (((c >> 8) || (uart->Paced() && (uart->LSR & UART_LSR_OE))) && !errorSet &&
                (uart->IER & UART_IER_RLSE)) {
                uart->IIR |= UART_IIR_RECV_ERR;
                errorSet = true;
            } else if (!errorSet && (uart->IER & UART_IER_RAVIE)) {
                uart->IIR |= UART_IIR_RECV_DATA;
                errorSet = true;
            }
        }
    }

    if (uart->LSR & UART_LSR_TDRQ && !errorSet && (uart->IER & UART_IER_TIE)) {
        errorSet = true;
        uart->IIR |= UART_IIR_SEND_DATA;
    }

    if (!errorSet && uart->HasModem() && (uart->IER & UART_IER_MIE) && (uart->MSR & 0x0f)) {
        errorSet = true;
        uart->IIR |= UART_IIR_MODEM_STAT;
    }
    if (!errorSet) uart->IIR |= UART_IIR_NOINT;
    if (uart->rxEndOfChain) uart->IIR |= UART_IIR_EOC;

    if (uart->dmaRequestF) {
        uart->dmaRequestF(uart->dmaContext, true, rxRequest);
        uart->dmaRequestF(uart->dmaContext, false, txRequest);
    }
    pxaUartPrvIrq(uart, errorSet || uart->rxEndOfChain);
}

void pxaUartSetDma(PxaUart* uart, PxaUartDmaRequestF requestF, void* context) {
    uart->dmaRequestF = requestF;
    uart->dmaContext = context;
    pxaUartPrvRecalc(uart);
}

uint_fast8_t pxaUartDmaValidBytes(PxaUart* uart) {
    if (!(uart->FCR & UART_FCR_TRFIFOE)) return 0;
    // Bytes before the first error; error bytes are left for PIO.
    uint_fast8_t n = 0;
    while (n < pxaUartPrvFifoUsed(&uart->RX) && !(pxaUartPrvFifoPeekNth(&uart->RX, n) >> 8)) ++n;
    return n;
}

bool pxaUartDmaEndOfReceive(PxaUart* uart) {
    if (!uart->rxTrailing || pxaUartPrvFifoUsed(&uart->RX)) return false;
    uart->rxTrailing = false;
    pxaUartPrvRecalc(uart);
    return true;
}

void pxaUartDmaEndOfChain(PxaUart* uart) {
    // 10.4.2.6: only with FCR[TRAIL], and only when bytes are left for the CPU.
    if (!(uart->FCR & UART_FCR_TRAIL) || !pxaUartPrvFifoUsed(&uart->RX)) return;
    uart->rxEndOfChain = true;
    uart->rxTrailing = false;
    pxaUartPrvRecalc(uart);
}

bool pxaUartReceiverEnabled(const PxaUart* uart) {
    return !uart->ioCheckpoints && uart->Clocked() && !uart->Loopback();
}

uint32_t pxaUartDisableCount(const PxaUart* uart) { return uart->disableCount; }

void pxaUartKick(PxaUart* uart) {
    if (pxaUartTaskRequired(uart))
        uart->reschedule.rescheduleCb(uart->reschedule.ctx, RESCHEDULE_TASK_UART);
}

bool pxaUartPlainEnabled(const PxaUart* uart) { return uart->Clocked() && !(uart->ISR & 3); }

bool pxaUartIrReceiveEnabled(const PxaUart *uart) {
    return uart->Clocked() && (uart->ISR & 0x02);
}

bool pxaUartIrTransmitEnabled(const PxaUart* uart) {
    return uart->Clocked() && (uart->ISR & 1);
}
void pxaUartSetPacing(PxaUart* uart, bool enabled) { uart->timedByBoard = enabled; }
void pxaUartSetModeObserver(PxaUart* uart,void (*notify)(void*),void* context) {
    uart->modeObserver=notify;uart->modeContext=context;
}
PxaIrState pxaUartIrState(const PxaUart* uart) {
    return {uart->Divisor() ? 921600u / uart->Divisor() : 0, uart->LCR,
            pxaUartIrReceiveEnabled(uart), pxaUartIrTransmitEnabled(uart),
            bool(uart->LSR & UART_LSR_TEMT) && !(uart->LSR & UART_LSR_DR), !(uart->LSR & UART_LSR_TEMT)};
}

bool pxaUartTaskRequired(struct PxaUart *uart) {
    if (uart->ioCheckpoints || uart->detaching || !uart->Clocked()) return false;
    return (!uart->Loopback() && uart->clientIsActiveF ? uart->clientIsActiveF(uart->callbackData) : false) ||
           ((uart->LSR & UART_LSR_DR) && uart->cyclesSinceRecv < uart->ReceiveTimeoutTicks()) ||
           uart->receiveRemaining ||
           ((uart->LSR & UART_LSR_TEMT) == 0 && (uart->transmitRemaining || uart->CanStartTransmit()));
}

void pxaUartBeginCheckpoint(PxaUart* uart) { ++uart->ioCheckpoints; }
void pxaUartEndCheckpoint(PxaUart* uart, bool kick) {
    if (!uart->ioCheckpoints) ERR("unbalanced UART checkpoint");
    if (!--uart->ioCheckpoints && kick)
        uart->reschedule.rescheduleCb(uart->reschedule.ctx, RESCHEDULE_TASK_UART);
}

template <typename T>
void pxaUartSave(PxaUart *uart, T &savestate, uint32_t index) {
    auto chunk = savestate.GetChunk(ChunkType::pxaUart + index, SAVESTATE_VERSION);
    if (!chunk) ERR("unable to allocate chunk");

    SaveChunkHelper helper(*chunk);
    uart->DoSaveLoad(helper, SAVESTATE_VERSION);
}

template <typename T>
void pxaUartLoad(struct PxaUart *uart, T &loader, uint32_t index) {
    pxaUartDetachExternal(uart);
    uint32_t version;
    auto chunk = loader.GetChunkOrFail(ChunkType::pxaUart + index, SAVESTATE_VERSION, "uart", version);
    if (!chunk) return;

    LoadChunkHelper helper(*chunk);
    uart->DoSaveLoad(helper, version);
    pxaUartResync(uart);
}

template void pxaUartSave<Savestate<ChunkType>>(PxaUart *uart, Savestate<ChunkType> &savestate,
                                                uint32_t index);
template void pxaUartSave<SavestateProbe<ChunkType>>(PxaUart *uart,
                                                     SavestateProbe<ChunkType> &savestate,
                                                     uint32_t index);
template void pxaUartLoad<SavestateLoader<ChunkType>>(PxaUart *uart,
                                                      SavestateLoader<ChunkType> &loader,
                                                      uint32_t index);
