//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _PXA255_UDC_H_
#define _PXA255_UDC_H_

#include <cstdio>

#include "CPU.h"
#include "mem.h"
#include "pxa_DMA.h"
#include "pxa_IC.h"

struct Pxa255Udc;

struct Pxa255Udc *pxa255UdcInit(struct ArmMem *physMem, struct PxaIc *ic, struct PxaDma *dma);

template <typename T>
void pxa255UdcSave(struct Pxa255Udc *udc, T &savestate);

#include "pxa270_UDC.h" // Common host transaction result; the register models are separate.
#include "savestate/ChunkTypeUarm.h"
template<class T> class SavestateLoader;
void pxa255UdcLoad(Pxa255Udc*, SavestateLoader<ChunkType>&);

// Clock changes stop transfers without discarding endpoint state.
void pxa255UdcSetClockEnabled(Pxa255Udc*, bool enabled, bool refresh = true);
bool pxa255UdcHostReset(Pxa255Udc*);
bool pxa255UdcHostFrame(Pxa255Udc*);
bool pxa255UdcHostEnabled(const Pxa255Udc*);
bool pxa255UdcHostListening(const Pxa255Udc*);
bool pxa255UdcHostSofWanted(const Pxa255Udc*);
uint32_t pxa255UdcHostEnableCount(const Pxa255Udc*);
uint8_t pxa255UdcHostAddress(const Pxa255Udc*);
void pxa255UdcHostSetNotify(Pxa255Udc*, void (*)(void*), void*);
Pxa270UdcHostResult pxa255UdcHostSetup(Pxa255Udc*, const uint8_t[8]);
Pxa270UdcHostResult pxa255UdcHostOut(Pxa255Udc*, const uint8_t*, unsigned);
Pxa270UdcHostResult pxa255UdcHostIn(Pxa255Udc*, uint8_t[16], unsigned*);
Pxa270UdcHostResult pxa255UdcHostEpOut(Pxa255Udc*, uint8_t, const uint8_t*, unsigned);
Pxa270UdcHostResult pxa255UdcHostEpIn(Pxa255Udc*, uint8_t, uint8_t[64], unsigned*);
Pxa270UdcHostResult pxa255UdcHostPeekIn(Pxa255Udc*, uint8_t, unsigned*);
#endif
