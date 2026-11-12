//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _PXA_UART_H_
#define _PXA_UART_H_

#include <cstdint>

#include "CPU.h"
#include "mem.h"
#include "pxa_IC.h"
#include "reschedule.h"

/*
        PXA255 UARTs

        They are identical, but at diff base addresses. this implements one. instanciate more than
   one to make all 3 work if needed.

        by default we read nothing and write nowhere (buffer drains fast into nothingness)
        this can be changed by addidng appropriate callbacks

*/

#define PXA_FFUART_BASE 0x40100000UL
#define PXA_BTUART_BASE 0x40200000UL
#define PXA_STUART_BASE 0x40700000UL
#define PXA_HWUART_BASE 0x41600000UL  // PXA25x/PXA26x only

#define UART_CHAR_BREAK 0x800
#define UART_CHAR_FRAME_ERR 0x400
#define UART_CHAR_PAR_ERR 0x200
#define UART_CHAR_NONE 0x100

struct PxaUart;

typedef uint_fast16_t (*PxaUartReadF)(void *userData);
typedef void (*PxaUartWriteF)(uint_fast16_t chr, void *userData);
typedef bool (*PxaUartClientIsActive)(void *userData);
typedef void (*PxaUartClientTick)(void *userData);
typedef void (*PxaUartClientDetach)(void *userData);

// Asserted logical levels; physical nCTS/nRTS polarity is abstracted here.
#define PXA_UART_CTS 0x10
#define PXA_UART_DSR 0x20
#define PXA_UART_RI  0x40
#define PXA_UART_DCD 0x80
#define PXA_UART_DTR 0x01
#define PXA_UART_RTS 0x02

struct PxaUart *pxaUartInit(struct ArmMem *physMem, struct Reschedule reschedule, struct PxaIc *ic,
                            uint32_t baseAddr, uint8_t irq, bool fifoOccupancy = false);
void pxaUartProcess(struct PxaUart *uart);  // write out data in TX fifo and read data into RX fifo

// Internal accessory callbacks survive normal restore. Live peers must use AttachExternal.
// Callback bindings cannot be changed during a serial checkpoint.
void pxaUartSetFuncs(struct PxaUart *uart, PxaUartReadF readF, PxaUartWriteF writeF,
                     PxaUartClientIsActive clientIsActiveF, PxaUartClientTick clientTickF,
                     void *userData);

// Model-only external ownership, not a host endpoint. Emulator-thread calls only.
// Attach requires idle TX/RX and a detach notification. Caller must detach before
// destroying its callback data or UART. Reset/normal load detach; checkpoints and
// refused loads do not.
bool pxaUartAttachExternal(PxaUart* uart, PxaUartReadF readF, PxaUartWriteF writeF,
                          PxaUartClientIsActive activeF, PxaUartClientTick tickF,
                          PxaUartClientDetach detachF, void* userData);
void pxaUartDetachExternal(PxaUart* uart);
// Around a state load: suspend takes the client off without notifying it. Resume with
// keep puts it back as it was, before the old state is loaded again; otherwise the
// client gets its detach notification.
void pxaUartSuspendExternal(PxaUart* uart);
void pxaUartResumeExternal(PxaUart* uart, bool keep);
bool pxaUartSetModemInputs(PxaUart* uart, uint8_t asserted);
uint8_t pxaUartGetModemOutputs(const PxaUart* uart);
// Board-wired HWUART RTS/AFE receive permission, including clock and FIFO
// threshold. Keeps the existing HWUART timing path; other ports use modem outputs.
bool pxaUartHwReceiveReady(const PxaUart* uart);
void pxaUartResync(PxaUart* uart);
// Live host peers: the receiver is clocked (IER[UUE] and a divisor), how often the
// guest has turned the UART off, and a way to wake the UART task when bytes wait. Emulator-thread calls only.
bool pxaUartReceiverEnabled(const PxaUart* uart);
uint32_t pxaUartDisableCount(const PxaUart* uart);  // counts IER[UUE] clears; not saved
void pxaUartKick(PxaUart* uart);

bool pxaUartPlainEnabled(const PxaUart* uart);
bool pxaUartIrReceiveEnabled(const PxaUart *uart);
bool pxaUartIrTransmitEnabled(const PxaUart *uart);
// Board wiring, not session state: enable character timing on PXA25x STUART too.
void pxaUartSetPacing(PxaUart* uart, bool enabled);
void pxaUartSetModeObserver(PxaUart* uart, void (*notify)(void*), void* context);
// idle: nothing being sent and no received byte waiting; sending: a byte still going out.
struct PxaIrState { uint32_t baud; uint8_t lcr; bool receive, transmit, idle, sending; };
PxaIrState pxaUartIrState(const PxaUart* uart);

// PXA27x UART DMA (IER[DMAE], 10.4.2.3). The UART raises and drops its RX and
// TX request lines through requestF; the SoC passes the handshakes below to its
// DMA controller. Emulator-thread calls only; wiring is not session state.
typedef void (*PxaUartDmaRequestF)(void *context, bool receive, bool requested);
void pxaUartSetDma(PxaUart *uart, PxaUartDmaRequestF requestF, void *context);
uint_fast8_t pxaUartDmaValidBytes(PxaUart *uart);  // RX bytes DMA may read now
bool pxaUartDmaEndOfReceive(PxaUart *uart);        // after a receive burst: EOR
void pxaUartDmaEndOfChain(PxaUart *uart);          // receive channel ended its chain

// Freeze callbacks and serial progress while temporary guest work is rolled back.
void pxaUartBeginCheckpoint(PxaUart* uart);
// kick reschedules the UART task for work held back meanwhile.
void pxaUartEndCheckpoint(PxaUart* uart, bool kick = true);

bool pxaUartTaskRequired(struct PxaUart *uart);

template <typename T>
void pxaUartSave(struct PxaUart *uart, T &savestate, uint32_t index = 0);

template <typename T>
void pxaUartLoad(struct PxaUart *uart, T &loader, uint32_t index = 0);

#endif
