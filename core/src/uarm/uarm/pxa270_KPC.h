//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _PXA270_KPC_H_
#define _PXA270_KPC_H_

#include <cstdint>

#include "mem.h"
#include "pxa_IC.h"

struct PxaKpc;

struct PxaKpc *pxaKpcInit(struct ArmMem *physMem, struct PxaIc *ic);

// keep in mind that colums are out and rows are in
void pxaKpcMatrixKeyChange(struct PxaKpc *kpc, uint_fast8_t row, uint_fast8_t col, bool isDown);
void pxaKpcDirectKeyChange(struct PxaKpc *kpc, uint_fast8_t keyIdx, bool isDown);
// KPDK reports pin levels. Upstream boards idle high (pressed reads 0); the Zire 72 idles low.
void pxaKpcSetDirectKeysActiveHigh(struct PxaKpc *kpc, bool activeHigh);
void pxaKpcJogInput(struct PxaKpc *kpc, uint_fast8_t jogIdx, bool up);  // else down

uint8_t pxaKpcMatrixWakeLevels(const PxaKpc* kpc);
uint8_t pxaKpcDirectWakeLevels(const PxaKpc* kpc);
void pxaKpcSleepReset(PxaKpc* kpc);

#include "savestate/ChunkTypeUarm.h"
template <typename T> class SavestateLoader;
template <typename T> void pxaKpcSave(PxaKpc *state, T &savestate);
void pxaKpcLoad(PxaKpc *state, SavestateLoader<ChunkType> &loader);

#endif
