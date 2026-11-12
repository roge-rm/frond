//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr
#ifndef _GDB_STUB_H_
#define _GDB_STUB_H_

#include <cstdint>
#include "CPU.h"

// Native support is compiled in, but no socket is opened without --gdb.
#ifndef __EMSCRIPTEN__
#define GDB_STUB_ENABLED 1
#endif

class SoC;
struct stub;
void gdbStubSetSoc(stub*,SoC*);
struct stub *gdbStubInit(struct ArmCpu *cpu, int port);

#ifdef GDB_STUB_ENABLED
bool cpuOriginalTg50PaceEntryForDiagnostic(ArmCpu* cpu);
bool gdbStubEnabled(struct stub *stub);
void gdbStubDebugBreakRequested(struct stub *stub);
void gdbStubReportPc(struct stub *stub, uint32_t pc, bool thumb);
void gdbStubInstructionExecuted(struct stub *stub);
void gdbStubPaceInstructionCompleted(struct stub *stub, uint32_t instructionPc, unsigned status);
void gdbStubResetPace(struct stub *stub);
#else
#define gdbStubEnabled(...) false
#define gdbStubDebugBreakRequested(...)
#define gdbStubReportPc(...)
#define gdbStubInstructionExecuted(...)
#define gdbStubPaceInstructionCompleted(...)
#define gdbStubResetPace(...)
#endif

#endif
