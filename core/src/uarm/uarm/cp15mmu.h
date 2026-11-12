//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _CP15_MMU_H_
#define _CP15_MMU_H_

#include "arm_diagnostic.h"
#include <cstdint>
#include "arm920_dcache.h"

struct ArmCP15MMU;
struct ArmCpu;
struct ArmMmu;
struct icache;

struct ArmCP15MMU* cp15MMUInit(struct ArmCpu* cpu, struct ArmMmu* mmu, struct icache* ic,
                               uint32_t cpuid, uint32_t cacheId, bool xscale, bool omap);
void cp15MMUSetFaultStatus(struct ArmCP15MMU* cp15, uint32_t addr, uint_fast8_t faultStatus);
void cp15MMUArm920SetIcacheIndexCompatibility(ArmCP15MMU*, bool);
uint32_t cp15MMUArm920Control(const ArmCP15MMU*);
Arm920DataCache* cp15MMUArm920DataCache(ArmCP15MMU*);
void cp15MMUCycle(struct ArmCP15MMU* cp15);
bool cp15MMUAlignmentEnabled(const struct ArmCP15MMU* cp15);
uint32_t cp15MMUClockMode(const struct ArmCP15MMU* cp15);
// Bootstrap subset, low vectors and caches/MMU disabled. Unknown operations raise undefined.
void cp15MMUConfigureArm920Bootstrap(struct ArmCP15MMU* cp15);

template <typename T>
void cp15MMUSave(struct ArmCP15MMU* cp15, T& savestate);

template <typename T>
void cp15MMULoad(struct ArmCP15MMU* cp15, T& loader);

void cp15MMUEmitArm920Diagnostic(const ArmCP15MMU*, const ArmDiagnosticSink&);
struct Arm920SavedState;
void cp15MMUArm920Capture(const ArmCP15MMU*, Arm920SavedState&);
void cp15MMUArm920Apply(ArmCP15MMU*, const Arm920SavedState&);
#endif  // _CP15_MMU_H_
