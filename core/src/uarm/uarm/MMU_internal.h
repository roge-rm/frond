// Internal decoded TLB layout shared by the native CPU fast path.
#ifndef FROND_MMU_INTERNAL_H
#define FROND_MMU_INTERNAL_H
#include "MMU.h"
#include "cputil.h"
#define TLB_SIZE (1 << 20)

struct TlbEntry {
    uint32_t pa : 20;

    uint32_t revision : 4;
    uint32_t ap : 2;
    uint32_t domain : 4;
    uint32_t c : 1;
    uint32_t section : 1;
    uint32_t b : 1;
};

struct ArmMmu {
    struct ArmMem *mem;
    uint32_t transTablPA;
    uint8_t S;
    uint8_t R;
    uint8_t xscale;
    uint32_t domainCfg;

    struct TlbEntry tlb[TLB_SIZE];
    uint16_t revision;
    // XScale only: dcOn is the C bit as last written (it doesn't read back from c1),
    // staleOn the stale D-cache window, staleTtb the page tables in use when it opened.
    uint8_t dcOn;
    uint8_t staleOn;
    uint32_t staleTtb;
    uint32_t flushes;  // counts mmuTlbFlush calls, for the JIT; not saved
    // Called when the TLB, domains, S or R change, so anything that checked a permission
    // before checks it again (the I-cache's fetch key). Not saved.
    void (*permChanged)(void *ctx);
    void *permChangedCtx;

    template <typename T>
    void DoSaveLoad(T &chunkHelper);
};

// Whether AP allows an access in this mode in a client domain.
static inline bool apAllows(struct ArmMmu *mmu, uint_fast8_t ap, bool priviledged, bool write) {
    switch (ap) {
        case 0:
            return !write && (mmu->R ? !mmu->S : (mmu->S && priviledged));
        case 1:
            return priviledged;
        case 2:
            return priviledged || !write;
        default:
            return true;
    }
}

// The fault status for an access in this mode, or 0 if it's allowed. Reads (and fetches)
// follow AP and the control register's S and R bits; AP 0 with S and R both set is
// unpredictable and treated as no access.
static inline uint8_t checkPermissions(struct ArmMmu *mmu, uint_fast8_t ap, uint_fast8_t domain,
                                       bool section, bool priviledged, bool write) {
    switch ((mmu->domainCfg >> (domain * 2)) & 3) {
        case 0:  // NO ACCESS:
        case 2:  // RESERVED: unpredictable	(treat as no access)
            return (section ? 0x09 : 0x0B) | (domain << 4);  // section or page domain fault

        case 1:  // CLIENT: check permissions
            break;

        case 3:  // MANAGER: allow all access
            return 0;
    }

    if (apAllows(mmu, ap, priviledged, write)) return 0;
    return (section ? 0x0D : 0x0F) | (domain << 4);  // section or subpage permission fault
}

// Match mmuTranslate's cached translations and share its permission check.
// Misses keep the authoritative page walk. No new translation state. Data accesses (data
// true) with the MMU off go through the stale D-cache window while it's open.
template <bool write, bool data = false>
static FORCE_INLINE MMUTranslateResult mmuTranslateCached(ArmMmu* mmu, uint32_t addr,
                                                        bool privileged) {
    if (mmu->transTablPA == MMU_DISABLED_TTP) {
        if constexpr (data)
            if (__builtin_expect(mmu->staleOn, 0)) return mmuStaleTranslate(mmu, addr);
        return addr;
    }
    const auto& entry = mmu->tlb[addr >> 12];
    if (entry.revision != mmu->revision)
        return mmuTranslate(mmu, addr, privileged, write);
    const auto fsr = checkPermissions(mmu, entry.ap, entry.domain, entry.section, privileged, write);
    if (fsr) return (1ull << 63) | (uint64_t(fsr) << 32);
    return uint64_t((addr & 0xfff) | (entry.pa << 12)) | (uint64_t(entry.c) << 62) | (uint64_t(entry.b) << 61);
}
#endif
