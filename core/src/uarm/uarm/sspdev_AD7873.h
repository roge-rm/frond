//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _AD7873_H_
#define _AD7873_H_

#include <functional>
#include <stdbool.h>
#include <stdint.h>

#include "pxa_SSP.h"
#include "pxa_GPIO.h"

struct Ad7873;



struct Ad7873* ad7873Init(struct PxaSsp* ssp, struct PxaGpio* gpio, int8_t penIntGpio /* negative for none */);
void ad7873Periodic(struct Ad7873 *ad);

//external
void ad7873PenInput(struct Ad7873 *ad, int16_t x, int16_t y);	//negative for pen up

void ad7873setVbatt(struct Ad7873 *ad, uint16_t mV);
void ad7873setVaux(struct Ad7873 *ad, uint16_t mV);
uint16_t ad7873getVaux(const Ad7873 *ad);



void ad7873Reset(Ad7873 *);
// Destroy only after the owning SoC can no longer dispatch its bus callbacks.
void ad7873Destroy(Ad7873 *);
template <typename T> void ad7873Save(Ad7873 *, T &);
template <typename T> bool ad7873Load(Ad7873 *, T &, bool preserveInput = false);
std::function<void()> ad7873CaptureInput(Ad7873 *);
#endif
