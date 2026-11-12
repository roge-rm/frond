//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _PXA_PWR_CLK_H_
#define _PXA_PWR_CLK_H_

#include "CPU.h"
#include "mem.h"

class SoC;
struct PxaPwrClk;
struct PxaGpio;
struct PxaRtc;
struct PxaKpc;

struct PxaPwrClk* pxaPwrClkInit(struct ArmCpu* cpu, struct ArmMem* physMem, class SoC* soc,
                                bool isPXA270);

void pxaPwrClkReset(struct PxaPwrClk* pc);
// Replaces RCSR after a host reset; 8 is GPR, the reset pin.
void pxaPwrClkSetResetCause(PxaPwrClk* pc, uint32_t rcsr);
void pxaPwrClkConnectWakeSources(PxaPwrClk* pc, PxaGpio* gpio, PxaRtc* rtc, PxaKpc* kpc);
bool pxaPwrClkSleeping(const PxaPwrClk* pc);
bool pxaPwrClkWakePending(PxaPwrClk* pc);
uint32_t pxaPwrClkSleepRetention(const PxaPwrClk* pc);
bool pxaPwrClkRetainPowerI2c(const PxaPwrClk* pc);
void pxaPwrClkFinishSleep(PxaPwrClk* pc);
void pxaPwrClkRestoreInProgress(PxaPwrClk* pc, bool restoring);

template <typename T>
void pxaPwrClkSave(struct PxaPwrClk* pc, T& savestate);

template <typename T>
void pxaPwrClkLoad(struct PxaPwrClk* pc, T& loader);

uint32_t pxaPwrClkClockMask(const PxaPwrClk *);
void pxaPwrClkSetClockObserver(PxaPwrClk *, void *, void (*)(void *));
bool pxaPwrClkI2sClock(const PxaPwrClk *);
struct PxaI2s;
void pxaPwrClkConnectI2s(PxaPwrClk *, PxaI2s *);
struct Pxa255Udc;
void pxaPwrClkConnectUdc(PxaPwrClk *, Pxa255Udc *);
#endif
