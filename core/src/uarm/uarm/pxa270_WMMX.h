//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _PXA270_WMMX_H_
#define _PXA270_WMMX_H_

#include "CPU.h"
#include "mem.h"

struct Pxa270wmmx;

struct Pxa270wmmx* pxa270wmmxInit(struct ArmCpu* cpu);

// The instructions on their own, for the Fast engine; the interpreter's coprocessor entries
// call the same code. Each returns false, changing nothing, for an encoding that is
// undefined. op1, CRd, CRn, CRm and op2 are the CDP and MCR/MRC fields; for MCRR and MRRC
// op is bits 7:4 and wR is CRm. TMIA (MCR op1 1) needs two core registers and isn't here.
bool pxa270wmmxCdp(struct Pxa270wmmx* w, bool cp1, unsigned op1, unsigned CRd, unsigned CRn, unsigned CRm,
                   unsigned op2);
bool pxa270wmmxMcr(struct Pxa270wmmx* w, bool cp1, unsigned op1, unsigned CRn, unsigned CRm, unsigned op2,
                   uint32_t value);
bool pxa270wmmxMrc(struct Pxa270wmmx* w, bool cp1, unsigned op1, unsigned CRn, unsigned CRm, unsigned op2,
                   uint32_t* value);
bool pxa270wmmxMcrr(struct Pxa270wmmx* w, bool cp1, unsigned op, unsigned wR, uint32_t lo, uint32_t hi);
bool pxa270wmmxMrrc(struct Pxa270wmmx* w, bool cp1, unsigned op, unsigned wR, uint64_t* value);
// A failed WLDR or WSTR access, for the data abort: its address, fault status (0: no
// fault), size in bytes and direction.
struct Pxa270wmmxFault {
    uint32_t addr;
    uint8_t fsr, size;
    bool write;
};
// WLDR and WSTR to or from wR at ea, as the interpreter does them: bytes or halfwords (N)
// on coprocessor 0, words or doublewords (N) on coprocessor 1, checked for the CPU's current
// mode. False, with wR unchanged and *fault set, when an access fails; a WSTRD whose second
// word fails has written the first.
bool pxa270wmmxTransfer(struct Pxa270wmmx* w, struct ArmCpu* cpu, bool cp1, bool N, bool store, unsigned wR,
                        uint32_t ea, struct Pxa270wmmxFault* fault);

// Whether the ARM word is a coprocessor 0 or 1 instruction the Fast engine runs: CDP, MCR,
// MRC, MCRR, MRRC, LDC or STC, defined for iWMMXt, not a cond NV (two) form, not TMIA, MCR
// with Rt not r15, MCRR and MRRC with two different registers other than r15, LDC and STC
// with a base other than r15 and not the option form. Depends only on the word; the CPU's
// CPAR is checked separately.
bool pxa270wmmxJitValid(uint32_t word);

#include "savestate/ChunkTypeUarm.h"
template <typename T> class SavestateLoader;
template <typename T> void pxa270wmmxSave(Pxa270wmmx *state, T &savestate);
void pxa270wmmxLoad(Pxa270wmmx *state, SavestateLoader<ChunkType> &loader);

#endif
