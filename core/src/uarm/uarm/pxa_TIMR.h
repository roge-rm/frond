//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _PXA_TIMR_H_
#define _PXA_TIMR_H_

#include <cstdint>

#include "CPU.h"
#include "mem.h"
#include "pxa_IC.h"

struct PxaTimr;

void pxaTimrRestartClock(PxaTimr* timr);
void pxaTimrSynchronize(PxaTimr* timr);

struct PxaTimr* pxaTimrInit(struct ArmMem* physMem, struct PxaIc* ic);

void pxaTimrTick(struct PxaTimr* timr, uint32_t batchedClocks);

uint32_t pxaTimrTicksToNextInterrupt(struct PxaTimr* timr);
// Optional clock-aware batching. Callbacks and context are host bindings, never saved.
void pxaTimrEnableBatching(PxaTimr* timr, uint32_t (*elapsedTicks)(void*),
                           void (*reschedule)(void*), void* context);
uint32_t pxaTimrNextBatch(PxaTimr* timr, uint32_t maximum = 32);

void pxaTimrSuspendInterrupts(struct PxaTimr* timr, bool suspendInterrupts);

template <typename T>
void pxaTimrSave(struct PxaTimr* timr, T& savestate);

template <typename T>
void pxaTimrLoad(struct PxaTimr* timr, T& loader);

#endif
