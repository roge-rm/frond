#ifndef _PEEPHOLE_H_
#define _PEEPHOLE_H_

#include <cstddef>
#include <cstdint>

#define INSTR_PEEPHOLE_ADS_UDIVMOD 0xfff9ee90
#define INSTR_PEEPHOLE_ADS_SDIVMOD 0xfff9ee91
#define INSTR_PEEPHOLE_ADS_UDIV10 0xfff9ee92
#define INSTR_PEEPHOLE_ADS_SDIV10 0xfff9ee93
#define INSTR_PEEPHOLE_ADS_MEMCPY 0xfff9ee94

#define FIRST_INSTRUCTION_ADS_UDIVMOD 0xE3A02000
#define FIRST_INSTRUCTION_ADS_SDIVMOD 0xE2102480

void peepholeOptimize(uint32_t* code, size_t size);

// Words of the ADS routine a peephole instruction replaces, in order, for the cost model
// (peephole_cost.h). The divisions' branch to their division by zero is never taken.
const uint32_t* peepholeRoutine(uint32_t instr, size_t* count);

constexpr bool peepholeInstr(uint32_t instr) {
    return instr >= INSTR_PEEPHOLE_ADS_UDIVMOD && instr <= INSTR_PEEPHOLE_ADS_MEMCPY;
}

#endif  // _PEEPHOLE_H_
