//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _MMIO_TG50_UC_H_
#define _MMIO_TG50_UC_H_


#include <functional>
#include "pxa_GPIO.h"
#include "mem.h"

struct TG50uc;


struct TG50uc* tg50ucInit(struct ArmMem *physMem, struct PxaGpio *gpio, int8_t gpioChangeIrqNo, const uint32_t *keyMap, int8_t gpioIoIrqNo = -1);
void tg50ucGpioSetInState(struct TG50uc* uc, unsigned port, unsigned pin, bool hi);
bool tg50ucGpioInState(const TG50uc*, unsigned port, unsigned pin);
bool tg50ucGpioState(const TG50uc*, unsigned port, unsigned pin);
void tg50ucSetKeyPressed(struct TG50uc* uc, uint32_t sdlKey, bool pressed);



void tg50ucReset(TG50uc *);
// Destroy only after the owning SoC can no longer dispatch its bus callbacks.
void tg50ucDestroy(TG50uc *);
template <typename T> void tg50ucSave(TG50uc *, T &);
template <typename T> bool tg50ucLoad(TG50uc *, T &, bool preserveInput = false);
void tg50ucRefreshIrq(TG50uc *);
bool tg50ucGpioIsOutput(const TG50uc *, unsigned port, unsigned pin);
void tg50ucSetOutputObserver(TG50uc *, const uint16_t masks[4], void (*)(void *), void *);
std::function<void()> tg50ucCaptureInput(TG50uc *);
#endif
