#ifndef _PACE_PATCH_H_
#define _PACE_PATCH_H_

#include <cstddef>
#include <cstdint>

#define INSTR_PACE_ENTER 0xfff9ace0
#define INSTR_PACE_RESUME 0xfff9ace1
#define INSTR_PACE_RETURN_FROM_CALLOUT 0xfff9ace2
// TG50 automatic library Sleep/Wake: passes the library's refnum to 68k libraries.
#define INSTR_TG50_LIBRARY_REFNUM 0xfff9ace3
// Replaces each "add pc, fp, r0" of the ROM's own PACE, where it dispatches the next 68k
// instruction. Native PACE never runs these; the ROM PACE runs only an instruction native
// PACE faulted on, and this brings it back to native PACE before the next one.
#define INSTR_PACE_DISPATCH 0xfff9ace4

struct PacePatch {
    uint32_t enterPace;
    uint32_t resumePace;
    uint32_t returnFromCallout;

    uint32_t calloutIllegalInstr;
    uint32_t calloutDivu;
    uint32_t calloutDivs;
    uint32_t calloutTrap0;
    uint32_t calloutTrap8;
    uint32_t calloutSyscall;
    uint32_t calloutLine1111;
    uint32_t calloutLine1010;
    uint32_t calloutUnimplementedInstr;

    // The ROM PACE's dispatch sites carry INSTR_PACE_DISPATCH. Its opcode table (16 words at
    // enterPace + 0x68) is left alone and still readable as data.
    bool romDispatchHooked;
    // The patched PACE is in guest RAM, which sessions save (the T|X), so its hooks go with them.
    bool hooksInRam;
};

struct PacePatch* createPacePatch();

void pacePatchInit(struct PacePatch* patch, uint32_t romBase, void* rom, size_t romSize);

// TG50 discovery reads original NOR. Applying changes only the instruction shadow.
// Both functions clear the output before validation and leave memory unchanged on failure.
// Init also hooks the automatic library Sleep/Wake call when its code matches.
bool pacePatchDiscoverTg50(PacePatch* patch, uint32_t romBase, const void* original, size_t size);
bool pacePatchInitTg50(PacePatch* patch, uint32_t romBase, const void* original,
                      void* shadow, size_t size);

#ifndef __EMSCRIPTEN__
bool pacePatchOriginalTg50EntryForDiagnostic(PacePatch* patch, uint32_t romBase,
                                            const void* original, void* shadow, size_t size);
#endif

#endif  // _PACE_PATCH_H_
