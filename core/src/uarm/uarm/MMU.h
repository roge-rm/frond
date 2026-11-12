//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _MMU_H_
#define _MMU_H_

#include "arm_diagnostic.h"
#include <cstdint>

#include "mem.h"

struct ArmMmu;

typedef uint64_t MMUTranslateResult;

#define MMU_TRANSLATE_RESULT_OK(x) (!(x & (1ull << 63)))
#define MMU_TRANSLATE_RESULT_CACHEABLE(x) (x & (1ull << 62))
#define MMU_TRANSLATE_RESULT_BUFFERABLE(x) (x & (1ull << 61))
#define MMU_TRANSLATE_RESULT_PA(x) ((uint32_t)x)
#define MMU_TRANSLATE_RESULT_FSR(x) ((uint8_t)((x >> 32) & 0xff))

#define MMU_DISABLED_TTP 0xFFFFFFFFUL

#define MMU_MAPPING_CACHEABLE 0x0001

struct ArmMmu *mmuInit(struct ArmMem *mem, bool xscaleMode);
void mmuReset(struct ArmMmu *mmu);

MMUTranslateResult mmuTranslate(struct ArmMmu *mmu, uint32_t va, bool priviledged, bool write);

bool mmuIsOn(struct ArmMmu *mmu);

MMUTranslateResult mmuTranslateInspect(ArmMmu*, uint32_t, bool write);
// Moves on with every TLB flush, which every mapping change (TTB, MMU on or off, TLB
// maintenance, reset, load) goes through.
uint32_t mmuFlushCount(const struct ArmMmu *mmu);
MMUTranslateResult mmuTranslateInspectAccess(ArmMmu*, uint32_t, bool privileged, bool write);

// The translation of a code address for the JIT's code map (as mmuTranslateInspect), and in
// access its 4K page's domain and the AP of each 1K subpage, for mmuAccessReadable:
// AP of subpage k in bits 2k-2k+1, the domain in bits 8-11, bit 12 + k set when subpage k
// has no mapping. MMU_ACCESS_ANY with the MMU off.
#define MMU_ACCESS_ANY 0x10000u
MMUTranslateResult mmuTranslateCodeAccess(ArmMmu*, uint32_t va, uint32_t* access);
// Whether reads of every mapped subpage of a page with that access are allowed now in
// this mode.
bool mmuAccessReadable(struct ArmMmu *mmu, uint32_t access, bool privileged);
// hook(ctx) runs when the TLB, domains, S or R change.
void mmuSetPermHook(struct ArmMmu *mmu, void (*hook)(void *), void *ctx);
uint32_t mmuGetTTP(struct ArmMmu *mmu);
void mmuSetTTP(struct ArmMmu *mmu, uint32_t ttp);
// XScale stale D-cache window (no-ops on other cores). The D-cache is tagged by VA and
// still hits after the MMU goes off, so a c1 write that turns M off while C was on opens
// the window: data accesses then translate through the old page tables, cacheable
// mappings only. A whole D-cache invalidate (c7,c6,0 or c7,c7,0), M on and reset close it.
// c is the C bit the write leaves, mOff whether it turns M off.
void mmuControlWrite(struct ArmMmu *mmu, bool c, bool mOff);
void mmuDcacheInvalidate(struct ArmMmu *mmu);
bool mmuStaleDcacheOpen(const struct ArmMmu *mmu);
// The PA a data access to va gets inside the window: va itself unless the old page tables
// map it cacheable. Never faults.
uint32_t mmuStaleTranslate(struct ArmMmu *mmu, uint32_t va);
// ARM920 TTBR and M changes preserve guest TLB entries until guest maintenance.
void mmuSetTTPArm920(struct ArmMmu *mmu, uint32_t ttp);

void mmuSetS(struct ArmMmu *mmu, bool on);
void mmuSetR(struct ArmMmu *mmu, bool on);
bool mmuGetS(struct ArmMmu *mmu);
bool mmuGetR(struct ArmMmu *mmu);

uint32_t mmuGetDomainCfg(struct ArmMmu *mmu);
// How a domain's two DACR bits rank: no access (and reserved) below client below manager.
// Each allows everything the ones below it do.
static inline unsigned mmuDomainRank(uint32_t bits) { return bits == 3 ? 2 : bits == 1 ? 1 : 0; }
// The TLB entry for va: its domain << 2 (MMU_DOMAIN_NONE with the MMU off, which allows
// both) and what it allows in this mode while its domain is a client, bit 0 reads and bit 1
// writes. A manager domain allows both, no access and reserved neither. -1 when the TLB
// doesn't hold va.
#define MMU_DOMAIN_NONE 16
#define MMU_DOMAIN_UNKNOWN 17
int mmuPageClientAccess(struct ArmMmu *mmu, uint32_t va, bool privileged);
void mmuSetDomainCfg(struct ArmMmu *mmu, uint32_t val);

void mmuTlbFlush(struct ArmMmu *mmu);

void mmuDump(struct ArmMmu *mmu);  // for calling in GDB :)

template <typename T>
void mmuSave(struct ArmMmu *mmu, T &savestate);

template <typename T>
void mmuLoad(struct ArmMmu *mmu, T &loader);

void mmuEmitDiagnostic(const ArmMmu*, const ArmDiagnosticSink&);
// Registers and the live TLB entries. Apply keeps the saved revision and flush count.
struct Arm920SavedState;
void mmuArm920Capture(const ArmMmu*, Arm920SavedState&);
void mmuArm920Apply(ArmMmu*, const Arm920SavedState&);
#endif
