//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _PXA270_UDC_H_
#define _PXA270_UDC_H_

#include <cstdio>

#include "CPU.h"
#include "mem.h"
#include "pxa_DMA.h"
#include "pxa_IC.h"

struct Pxa270Udc;

struct Pxa270Udc *pxa270UdcInit(struct ArmMem *physMem, struct PxaIc *ic, struct PxaDma *dma);

// Host-side bus events as the UDC sees them on the wire. Call on the emulator thread
// between guest slices. The caller owns bus timing: an IN may only complete after
// the packet's bus time has passed since the packet became ready, and transfers run
// on the caller's 1 ms frame schedule. Timeout means no handshake at all (endpoint
// not in the active configuration, or an OUT packet longer than its maximum).
#include "UsbPacketResult.h"
// Bus reset: drops all FIFOs, endpoint status, address and configuration, latches IRRS.
bool pxa270UdcHostReset(struct Pxa270Udc *udc);
// Start of frame: advances UDCFNR and latches IRSOF.
bool pxa270UdcHostFrame(struct Pxa270Udc *udc);
// UDCCR.UDE: the guest has the UDC on, so the device pulls D+ up and can be reset.
bool pxa270UdcHostEnabled(const struct Pxa270Udc *udc);
// UDE set, and since that enable the guest has written UDCICR with the EP0 and reset
// (IERS) interrupts on. The model has no D+ pull-up; a host treats this as the device
// being on the bus. The TX ROM sets UDE well before it can answer EP0, and a reset or
// SETUP before then is lost. UDE=0 leaves UDCICR as it was, so the ICR write has to be
// a new one. A loaded session with UDE set counts as written.
bool pxa270UdcHostListening(const struct Pxa270Udc *udc);
// Called on every guest write to the UDC, register or FIFO, so a sleeping host can
// look again. nullptr removes it. Not saved.
void pxa270UdcHostSetNotify(struct Pxa270Udc *udc, void (*notify)(void *), void *context);
// Counts UDE 0->1 writes. UDE=0 takes the D+ pull-up away, so a change means the device
// left the bus and came back, even between two host looks at UDE.
uint32_t pxa270UdcHostEnableCount(const struct Pxa270Udc *udc);
// The guest has the SOF interrupt (IRSOF) enabled.
bool pxa270UdcHostSofWanted(const struct Pxa270Udc *udc);
// Device address set by the last completed SET_ADDRESS, 0 after reset.
uint8_t pxa270UdcHostAddress(const struct Pxa270Udc *udc);
// SETUP packet. Standard requests the UDC answers itself (SET_ADDRESS, SET/CLEAR_FEATURE,
// GET_STATUS, GET/SET_CONFIGURATION, GET/SET_INTERFACE) never reach the guest. Others drop
// any ready IN packet, load 8 bytes, set SA/RNE/OPC and IR0[0].
Pxa270UdcHostResult pxa270UdcHostSetup(struct Pxa270Udc *udc, const uint8_t setup[8]);
// EP0 OUT data (0-16 bytes): NAK while OPC is set, otherwise drops any ready IN packet.
Pxa270UdcHostResult pxa270UdcHostOut(struct Pxa270Udc *udc, const uint8_t *data, unsigned size);
// EP0 IN token: returns the ready packet (0-16 bytes) and ACKs it, or NAK/STALL.
Pxa270UdcHostResult pxa270UdcHostIn(struct Pxa270Udc *udc, uint8_t data[16], unsigned *size);
// Bulk/interrupt IN token to USB endpoint 1-15: returns a packet (up to 64 bytes).
Pxa270UdcHostResult pxa270UdcHostEpIn(struct Pxa270Udc *udc, uint8_t endpoint, uint8_t data[64],
                                      unsigned *size);
// Bulk/interrupt OUT packet to USB endpoint 1-15.
Pxa270UdcHostResult pxa270UdcHostEpOut(struct Pxa270Udc *udc, uint8_t endpoint, const uint8_t *data,
                                       unsigned size);
// What an IN token to `endpoint` (0-15) would get now, without changing any state. On Ack,
// `size` is the length of the ready packet.
Pxa270UdcHostResult pxa270UdcHostPeekIn(const struct Pxa270Udc *udc, uint8_t endpoint, unsigned *size);

#include "savestate/ChunkTypeUarm.h"
template <typename T> class SavestateLoader;
template <typename T> void pxa270UdcSave(Pxa270Udc *state, T &savestate);
void pxa270UdcLoad(Pxa270Udc *state, SavestateLoader<ChunkType> &loader);

#endif
