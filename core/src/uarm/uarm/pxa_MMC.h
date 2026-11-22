//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _PXA_MMC_H_
#define _PXA_MMC_H_

#include "CPU.h"
#include "mem.h"
#include "pxa_DMA.h"
#include "pxa_IC.h"
#include "vSD.h"

struct PxaMmc;
class SdioCard;

struct PxaMmc* pxaMmcInit(struct ArmMem* physMem, struct PxaIc* ic, struct PxaDma* dma, bool wordFifo = false);

void pxaMmcInsert(struct PxaMmc* mmc, struct VSD* vsd);  // NULL also acceptable
// Replaces the slot contents. The caller owns the card and must detach it before destroying it.
// The board drives card detect and any external SDIO interrupt routing.
// The card owner must save and restore its state before using this in a machine session.
void pxaMmcInsertSdio(PxaMmc* mmc, SdioCard* card);
// Call on the emulation thread after changing a card IRQ source.
void pxaMmcSdioIrqChanged(PxaMmc* mmc);
// With no card in: the guest has stopped the MMC clock since the card went (its driver handling the
// removal). Host only, false again after a session load.
bool pxaMmcClockStoppedSinceRemoval(struct PxaMmc* mmc);

template <typename T>
void pxaMmcSave(struct PxaMmc* mmc, T& savestate);

template <typename T>
void pxaMmcLoad(struct PxaMmc* mmc, T& loader);

#endif
