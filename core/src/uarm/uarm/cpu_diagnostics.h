#ifndef FROND_CPU_DIAGNOSTICS_H
#define FROND_CPU_DIAGNOSTICS_H

#include "arm_diagnostic.h"
#include <cstdint>

struct ArmCpu;
// Counts exception entries, independently of explicit CPSR mode changes. Reset clears it.
uint64_t cpuExceptionSequence(const ArmCpu* cpu);
// Read the actual CP15 iA/nF bits without issuing a guest register access.
uint32_t cpuArm920ClockMode(const ArmCpu* cpu);

// Copies retained state only, without memory access, translation or cache maintenance.
void cpuEmitArm920Diagnostic(const ArmCpu*, const ArmDiagnosticSink&);
#endif
