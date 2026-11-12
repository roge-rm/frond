#ifndef _PXA_I2S_H_
#define _PXA_I2S_H_

#include "CPU.h"
#include "mem.h"
#include "pxa_DMA.h"
#include "pxa_IC.h"

struct PxaI2s;

struct PxaI2s *pxaI2sInit(struct ArmMem *physMem, struct PxaIc *ic, struct PxaDma *dma);
// The codec returns a serial input frame. MSB justified is a separate wire format.
using PxaI2sFrame = uint32_t (*)(void *, uint32_t, bool playing, bool msbJustified, uint32_t rateNumerator, uint32_t rateDenominator);
using PxaI2sBoundary = void (*)(void *);
void pxaI2sAttach(PxaI2s *, void *, PxaI2sFrame, PxaI2sBoundary);
void pxaI2sSetExternalRate(PxaI2s *, uint32_t hz, bool discontinuity = true);
void pxaI2sSetClockEnabled(PxaI2s *, bool, bool discontinuity = true);
void pxaI2sOutputBoundary(PxaI2s *);
bool pxaI2sLinkEnabled(const PxaI2s *);
bool pxaI2sClocking(const PxaI2s *);
bool pxaI2sClockStateValid(const PxaI2s *);
bool pxaI2sLegacyState(const PxaI2s *);
void pxaI2sGetRate(const PxaI2s *, uint32_t &numerator, uint32_t &denominator);
void pxaI2sAdvance(PxaI2s *, uint64_t elapsedNs);
void pxaI2sRefresh(PxaI2s *);
bool pxaI2sHasCodec(const PxaI2s *);
void pxaI2sPeriodic(struct PxaI2s *i2s);

template <typename T>
void pxaI2sSave(struct PxaI2s *i2s, T &savestate);

template <typename T>
void pxaI2sLoad(struct PxaI2s *i2s, T &loader);

#endif  // _PXA_I2S_H_
