//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _AN32502A_H_
#define _AN32502A_H_

#include "pxa_I2C.h"

struct An32502A;


struct An32502A* an32502aInit(struct PxaI2c* i2c);


void an32502aReset(An32502A *);
// Destroy only after the owning SoC can no longer dispatch its bus callbacks.
void an32502aDestroy(An32502A *);
template <typename T> void an32502aSave(An32502A *, T &);
template <typename T> bool an32502aLoad(An32502A *, T &, bool preserveInput = false);
#endif
