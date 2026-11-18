//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _PXA_MEM_CTRL_H_
#define _PXA_MEM_CTRL_H_

#include <cstdint>

#include "CPU.h"
#include "mem.h"

struct PxaMemCtrlr;

// cpu takes the SDRAM width from MDCNFG for the cost model's data cache charges.
struct PxaMemCtrlr* pxaMemCtrlrInit(struct ArmMem* physMem, struct ArmCpu* cpu, uint_fast8_t socRev);

// PXA25x sleep exit: RAM is out of the address map until MDCNFG enables SDRAM.
void pxaMemCtrlrHoldSdram(struct PxaMemCtrlr* mc);

template <typename T>
void pxaMemCtrlrSave(struct PxaMemCtrlr* pxaMemCtrlr, T& savestate);

template <typename T>
void pxaMemCtrlrLoad(struct PxaMemCtrlr* pxaMemCtrlr, T& loader);

#endif
