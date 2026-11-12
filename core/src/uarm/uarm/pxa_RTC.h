//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _PXA_RTC_H_
#define _PXA_RTC_H_

#include "CPU.h"
#include "mem.h"
#include "pxa_IC.h"

struct PxaRtc;

void pxaRtcSetWakeCallback(PxaRtc* rtc, void (*callback)(void*), void* context);
void pxaRtcRefreshInterrupts(PxaRtc* rtc);

struct PxaRtc* pxaRtcInit(struct ArmMem* physMem, struct PxaIc* ic);

void pxaRtcTick(struct PxaRtc* rtc);

// Registers a GPIO reset leaves alone (PXA27x 3.4.3).
struct PxaRtcRetained {
    uint64_t lastAlarmCheck;
    uint32_t RTAR, RTTR;
    uint8_t RTSR;
};
PxaRtcRetained pxaRtcRetained(const PxaRtc* rtc);
void pxaRtcRestoreRetained(PxaRtc* rtc, const PxaRtcRetained& retained);

template <typename T>
void pxaRtcSave(struct PxaRtc* rtc, T& savestate);

template <typename T>
void pxaRtcLoad(struct PxaRtc* rtc, T& loader);

#endif
