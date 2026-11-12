//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _PXA270_IMC_H_
#define _PXA270_IMC_H_

#include "mem.h"

struct PxaImc;

struct PxaImc* pxaImcInit(struct ArmMem* physMem);

#include "savestate/ChunkTypeUarm.h"
template <typename T> class SavestateLoader;
template <typename T> void pxaImcSave(PxaImc *state, T &savestate);
void pxaImcLoad(PxaImc *state, SavestateLoader<ChunkType> &loader);

#endif
