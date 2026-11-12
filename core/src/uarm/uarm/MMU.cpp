//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "MMU.h"
#include "MMU_internal.h"
#include "arm920_state.h"

#include <cstdlib>
#include <cstring>

#include "cputil.h"
#include "mem.h"
#include "mem_internal.h"
#include "savestate/savestateAll.h"

#define TRANSLATE_RESULT_FAULT(fsr) ((1ull << 63) | ((uint64_t)(fsr) << 32))

#define SAVESTATE_VERSION 0

static void mmuPrvPermChanged(struct ArmMmu *mmu) {
    hostTlbFlush(&mmu->mem->hostTlb);
    if (mmu->permChanged) mmu->permChanged(mmu->permChangedCtx);
}

void mmuSetPermHook(struct ArmMmu *mmu, void (*hook)(void *), void *ctx) {
    mmu->permChanged = hook;
    mmu->permChangedCtx = ctx;
}

void mmuTlbFlush(struct ArmMmu *mmu) {
    mmuPrvPermChanged(mmu);
    mmu->flushes++;
    mmu->revision = (mmu->revision + 1) & 0x0f;

    if (mmu->revision == 0) {
        mmu->revision = 1;

        for (size_t i = 0; i < TLB_SIZE; i++) mmu->tlb[i].revision = 0;
    }
}

void mmuReset(struct ArmMmu *mmu) {
    mmu->transTablPA = MMU_DISABLED_TTP;
    mmu->dcOn = 0;
    mmu->staleOn = 0;
    mmu->staleTtb = 0;
    mmuTlbFlush(mmu);
}

struct ArmMmu *mmuInit(struct ArmMem *mem, bool xscaleMode) {
    struct ArmMmu *mmu = (struct ArmMmu *)malloc(sizeof(*mmu));

    if (!mmu) ERR("cannot alloc MMU");

    memset(mmu, 0, sizeof(*mmu));
    mmu->mem = mem;
    mmu->xscale = xscaleMode;
    mmuReset(mmu);

    return mmu;
}

bool mmuIsOn(struct ArmMmu *mmu) { return mmu->transTablPA != MMU_DISABLED_TTP; }

// checkRead false skips the permission check for reads (inspection). access, if given, gets
// adr's domain << 2 | AP, and uniform whether the rest of its 4K page has the same AP.
static FORCE_INLINE MMUTranslateResult translateAndCache(struct ArmMmu *mmu, uint32_t adr,
                                                         bool priviledged, bool write, bool inspect = false,
                                                         bool checkRead = true, uint8_t *access = nullptr,
                                                         bool *uniformP = nullptr) {
    bool c = false, b = false;
    bool section = false, coarse = true, pxa_tex_page = false;
    uint32_t va, paPage = 0, sz, t, pa;
    int_fast16_t i;
    uint_fast8_t dom, ap = 0;
    bool uniform = true;
    uint8_t fsr;
    MMUTranslateResult result;

    const auto readTable = [&](uint32_t pa, uint32_t* dest) {
        return (!inspect || memIsBufferRange(mmu->mem,pa,4,false)) &&
               memAccess<4,false>(mmu->mem,pa,dest);
    };
    // read first level table
    if (mmu->transTablPA & 3) return TRANSLATE_RESULT_FAULT(0x01);  // alignment fault

    if (!readTable(mmu->transTablPA + ((adr & 0xFFF00000ul) >> 18), &t))
        return TRANSLATE_RESULT_FAULT(0x0C);

    dom = (t >> 5) & 0x0F;
    switch (t & 3) {
        case 0:  // fault
            return TRANSLATE_RESULT_FAULT(0x05);

        case 1:  // coarse pagetable

            t &= 0xFFFFFC00UL;
            t += (adr & 0x000FF000UL) >> 10;
            break;

        case 2:  // 1MB section

            paPage = t & 0xFFF00000UL;
            va = adr & 0xFFF00000UL;
            sz = 1UL << 20;
            ap = (t >> 10) & 3;
            c = !!(t & 0x08);
            b = !!(t & 0x04);
            section = true;
            goto translated;

        case 3:  // fine page table

            coarse = false;
            t &= 0xFFFFF000UL;
            t += (adr & 0x000FFC00UL) >> 8;
            break;
    }

    // read second level table
    if (!readTable(t, &t)) {
        return TRANSLATE_RESULT_FAULT(0x0E | (dom << 4));
    }

    c = !!(t & 0x08);
            b = !!(t & 0x04);

    switch (t & 3) {
        case 0:  // fault
            return TRANSLATE_RESULT_FAULT(0x07 | (dom << 4));

        case 1:  // 64K mapping

            paPage = t & 0xFFFF0000UL;
            va = adr & 0xFFFF0000UL;
            sz = 65536UL;
            ap = (adr >> 14) & 3;  // in "ap" store which AP we need [of the 4]

            break;

        case 2:  // 4K mapping (1K effective thenks to having 4 AP fields)

            ap = (adr >> 10) & 3;  // in "ap" store which AP we need [of the 4]

        page_size_4k:
            paPage = t & 0xFFFFF000UL;
            va = adr & 0xFFFFF000UL;
            sz = 4096;
            break;

        case 3:  // 1K mapping

            if (coarse) {
                if (mmu->xscale) {
                    pxa_tex_page = true;
                    ap = 0;
                    goto page_size_4k;
                } else {
                    fprintf(stderr, "invalid coarse page table entry\r\n");
                    return TRANSLATE_RESULT_FAULT(0x07);
                }
            }

            paPage = t & 0xFFFFFC00UL;
            va = adr & 0xFFFFFC00UL;
            ap = (t >> 4) & 3;  // in "ap" store the actual AP [and skip quarter-page resolution
                                // later using the goto]
            sz = 1024;
            uniform = false;
            goto translated;
    }

    // handle 4 AP sections

    i = (t >> 4) & 0xFF;
    if (pxa_tex_page ||
        ((i & 0x0F) == (i >> 4) &&
         (i & 0x03) == ((i >> 2) & 0x03)))  // if all domains are the same, add the whole thing
        ap = (t >> 4) & 3;

    else {  // take the quarter that is the one we need

        uniform = false;
        sz /= 4;
        paPage += ((uint32_t)ap) * sz;
        va += ((uint32_t)ap) * sz;
        ap = (t >> (4 + 2 * ap)) & 3;
    }

translated:
    pa = (adr - va) + paPage;

    if (!inspect && sz > 1024) {
        hostTlbFlush(&mmu->mem->hostTlb);
        for (uint32_t offset = 0; offset < sz; offset += 4096) {
            struct TlbEntry *tlbEntry = mmu->tlb + ((va + offset) >> 12);

            tlbEntry->ap = ap;
            tlbEntry->c = c;
            tlbEntry->b = b;
            tlbEntry->domain = dom;
            tlbEntry->section = section;
            tlbEntry->pa = (paPage + offset) >> 12;
            tlbEntry->revision = mmu->revision;
        }
    }

    if (access) {
        *access = uint8_t((dom << 2) | ap);
        *uniformP = uniform;
    }

    if (write || checkRead) {
        fsr = checkPermissions(mmu, ap, dom, section, priviledged, write);
        if (fsr) return TRANSLATE_RESULT_FAULT(fsr);
    }

    result = pa;

    if (c) result |= (1ull << 62);
    if (b) result |= (1ull << 61);

    return result;
}

MMUTranslateResult mmuTranslate(struct ArmMmu *mmu, uint32_t addr, bool priviledged, bool write) {
    if (mmu->transTablPA == MMU_DISABLED_TTP) return addr;

    struct TlbEntry *tlbEntry = mmu->tlb + (addr >> 12);

    if (tlbEntry->revision != mmu->revision) {
        return translateAndCache(mmu, addr, priviledged, write);
    }

    uint8_t fsr = checkPermissions(mmu, tlbEntry->ap, tlbEntry->domain, tlbEntry->section,
                                   priviledged, write);
    if (fsr) return TRANSLATE_RESULT_FAULT(fsr);

    uint64_t result = (addr & 0xfff) + (tlbEntry->pa << 12);

    if (tlbEntry->c) result |= (1ull << 62);
    if (tlbEntry->b) result |= (1ull << 61);

    return result;
}

MMUTranslateResult mmuTranslateInspectAccess(ArmMmu* mmu, uint32_t addr, bool privileged, bool write) {
    if (mmu->transTablPA == MMU_DISABLED_TTP) return addr;
    const auto& entry=mmu->tlb[addr >> 12];
    if (entry.revision != mmu->revision) return translateAndCache(mmu,addr,privileged,write,true);
    auto fsr=checkPermissions(mmu,entry.ap,entry.domain,entry.section,privileged,write);
    if (fsr) return TRANSLATE_RESULT_FAULT(fsr);
    return uint64_t((addr & 0xfff) | (entry.pa << 12));
}
// Privileged, and reads skip the permission check, so a debugger or the JIT's code map
// sees every mapped page.
MMUTranslateResult mmuTranslateInspect(ArmMmu* mmu, uint32_t addr, bool write) {
    if (mmu->transTablPA == MMU_DISABLED_TTP) return addr;
    const auto& entry=mmu->tlb[addr >> 12];
    if (entry.revision != mmu->revision) return translateAndCache(mmu,addr,true,write,true,false);
    if (write) {
        auto fsr=checkPermissions(mmu,entry.ap,entry.domain,entry.section,true,true);
        if (fsr) return TRANSLATE_RESULT_FAULT(fsr);
    }
    return uint64_t((addr & 0xfff) | (entry.pa << 12));
}

MMUTranslateResult mmuTranslateCodeAccess(ArmMmu* mmu, uint32_t addr, uint32_t* access) {
    if (mmu->transTablPA == MMU_DISABLED_TTP) {
        *access = MMU_ACCESS_ANY;
        return addr;
    }
    *access = 0;
    const auto& entry=mmu->tlb[addr >> 12];
    // TLB entries only hold 4K pages with one AP.
    if (entry.revision == mmu->revision) {
        *access = (uint32_t(entry.domain) << 8) | (entry.ap * 0x55u);
        return uint64_t((addr & 0xfff) | (entry.pa << 12));
    }
    uint8_t a;
    bool uniform;
    const MMUTranslateResult r = translateAndCache(mmu,addr,true,false,true,false,&a,&uniform);
    if (!MMU_TRANSLATE_RESULT_OK(r)) return r;
    uint32_t acc = uint32_t(a >> 2) << 8;
    if (uniform) {
        acc |= (a & 3) * 0x55u;
    } else {
        for (uint32_t k = 0; k < 4; ++k) {
            const uint32_t sub = (addr & ~0xfffu) | (k << 10);
            if (MMU_TRANSLATE_RESULT_OK(translateAndCache(mmu,sub,true,false,true,false,&a,&uniform)))
                acc |= uint32_t(a & 3) << (2 * k);
            else
                acc |= 0x1000u << k;
        }
    }
    *access = acc;
    return r;
}

bool mmuAccessReadable(struct ArmMmu *mmu, uint32_t access, bool privileged) {
    if (access == MMU_ACCESS_ANY) return true;
    const uint32_t domain = (access >> 8) & 15;
    for (uint32_t k = 0; k < 4; ++k) {
        if (access & (0x1000u << k)) continue;
        if (checkPermissions(mmu, (access >> (2 * k)) & 3, domain, false, privileged, false))
            return false;
    }
    return true;
}

uint32_t mmuGetTTP(struct ArmMmu *mmu) { return mmu->transTablPA; }

uint32_t mmuFlushCount(const struct ArmMmu *mmu) { return mmu->flushes; }

void mmuSetTTP(struct ArmMmu *mmu, uint32_t ttp) {
    mmuTlbFlush(mmu);
    mmu->transTablPA = ttp;
    if (ttp != MMU_DISABLED_TTP) mmu->staleOn = 0;
}

// Runs at the write, while the old TTB is still in use (M goes off a couple of cycles
// later). The flush as M goes off empties the host TLB and the JIT's page table, and the
// host TLB takes no fills while the window is open.
void mmuControlWrite(struct ArmMmu *mmu, bool c, bool mOff) {
    if (!mmu->xscale) return;
    if (mOff && mmu->dcOn && mmu->transTablPA != MMU_DISABLED_TTP) {
        mmu->staleOn = 1;
        mmu->staleTtb = mmu->transTablPA;
    }
    mmu->dcOn = c;
}

void mmuDcacheInvalidate(struct ArmMmu *mmu) { mmu->staleOn = 0; }

bool mmuStaleDcacheOpen(const struct ArmMmu *mmu) { return mmu->staleOn; }

uint32_t mmuStaleTranslate(struct ArmMmu *mmu, uint32_t va) {
    // Only tables in RAM or ROM are read, so the walk has no side effects.
    const auto read = [mmu](uint32_t pa, uint32_t *dest) {
        return memIsBufferRange(mmu->mem, pa, 4, false) && memAccess<4, false>(mmu->mem, pa, dest);
    };
    uint32_t l1, l2, at;
    if ((mmu->staleTtb & 3) || !read(mmu->staleTtb + ((va & 0xFFF00000ul) >> 18), &l1)) return va;
    switch (l1 & 3) {
        case 2:  // section
            return (l1 & 0x08) ? (l1 & 0xFFF00000ul) | (va & 0x000FFFFFul) : va;
        case 1:  // coarse
            at = (l1 & 0xFFFFFC00ul) + ((va & 0x000FF000ul) >> 10);
            break;
        case 3:  // fine
            at = (l1 & 0xFFFFF000ul) + ((va & 0x000FFC00ul) >> 8);
            break;
        default:
            return va;
    }
    if (!read(at, &l2) || !(l2 & 0x08)) return va;
    switch (l2 & 3) {
        case 1:  // 64K
            return (l2 & 0xFFFF0000ul) | (va & 0xFFFFul);
        case 2:  // 4K
            return (l2 & 0xFFFFF000ul) | (va & 0xFFFul);
        case 3:  // 1K, or the XScale extended 4K page in a coarse table
            if ((l1 & 3) == 1) return (l2 & 0xFFFFF000ul) | (va & 0xFFFul);
            return (l2 & 0xFFFFFC00ul) | (va & 0x3FFul);
        default:
            return va;
    }
}

void mmuSetTTPArm920(struct ArmMmu *mmu, uint32_t ttp) {
    if (mmu->transTablPA == ttp) return;
    mmu->transTablPA = ttp;
    mmu->flushes++;
    mmuPrvPermChanged(mmu);
}

void mmuSetS(struct ArmMmu *mmu, bool on) {
    if (mmu->S == on) return;
    mmu->S = on;
    mmuPrvPermChanged(mmu);
}

void mmuSetR(struct ArmMmu *mmu, bool on) {
    if (mmu->R == on) return;
    mmu->R = on;
    mmuPrvPermChanged(mmu);
}

bool mmuGetS(struct ArmMmu *mmu) { return mmu->S; }

bool mmuGetR(struct ArmMmu *mmu) { return mmu->R; }

uint32_t mmuGetDomainCfg(struct ArmMmu *mmu) { return mmu->domainCfg; }

void mmuSetDomainCfg(struct ArmMmu *mmu, uint32_t val) {
    if (val == mmu->domainCfg) return;
    bool lost = false;
    for (unsigned d = 0; d < 16; d++)
        if (mmuDomainRank((val >> (2 * d)) & 3) < mmuDomainRank((mmu->domainCfg >> (2 * d)) & 3)) lost = true;
    mmu->domainCfg = val;
    // The host TLB only holds accesses that were allowed, and still are if no domain lost rights.
    if (lost)
        hostTlbFlushDomains(&mmu->mem->hostTlb);
    else
        mmu->mem->hostTlb.gains++;
    if (mmu->permChanged) mmu->permChanged(mmu->permChangedCtx);
}

int mmuPageClientAccess(struct ArmMmu *mmu, uint32_t va, bool privileged) {
    if (mmu->transTablPA == MMU_DISABLED_TTP) return (MMU_DOMAIN_NONE << 2) | 3;
    const struct TlbEntry &entry = mmu->tlb[va >> 12];
    if (entry.revision != mmu->revision) return -1;
    return (entry.domain << 2) | (apAllows(mmu, entry.ap, privileged, false) ? 1 : 0) |
           (apAllows(mmu, entry.ap, privileged, true) ? 2 : 0);
}

///////////////////////////  debugging helpers  ///////////////////////////

static uint32_t mmuPrvDebugRead(struct ArmMmu *mmu, uint32_t addr) {
    uint32_t t;

    if (!memAccess(mmu->mem, addr, 4, false, &t)) t = 0xFFFFFFF0UL;

    return t;
}

static void mmuPrvDumpUpdate(uint32_t va, uint32_t pa, uint32_t len, uint8_t dom, uint8_t ap,
                             bool c, bool b, bool valid) {
    static bool wasValid = false;
    static uint32_t expectPa = 0;
    static uint32_t startVa = 0;
    static uint32_t startPa = 0;
    static uint8_t wasDom = 0;
    static uint8_t wasAp = 0;
    static bool wasB = 0;
    static bool wasC = 0;
    uint32_t va_end;

    va_end = (va || len) ? va - 1 : 0xFFFFFFFFUL;

    if (!wasValid && !valid) return;  // no need to bother...

    if (valid != wasValid || dom != wasDom || ap != wasAp || c != wasC || b != wasB ||
        expectPa != pa) {  // not a continuation of what we've been at...

        if (wasValid)
            fprintf(stderr, "0x%08lx - 0x%08lx -> 0x%08lx - 0x%08lx dom%u ap%u %c %c\n",
                    (unsigned long)startVa, (unsigned long)va_end, (unsigned long)startPa,
                    (unsigned long)(startPa + (va_end - startVa)), wasDom, wasAp,
                    (char)(wasC ? 'c' : ' '), (char)(wasB ? 'b' : ' '));

        wasValid = valid;
        if (valid) {  // start of a new range

            wasDom = dom;
            wasAp = ap;
            wasC = c;
            wasB = b;
            startVa = va;
            startPa = pa;
            expectPa = pa + len;
        }
    } else  // continuation of what we've been up to...
        expectPa += len;
}

void mmuDump(struct ArmMmu *mmu) {
    uint32_t i, j, t, sla, va, psz;
    bool coarse = false;
    uint_fast8_t dom;

    for (i = 0; i < 0x1000; i++) {
        t = mmuPrvDebugRead(mmu, mmu->transTablPA + (i << 2));
        va = i << 20;
        dom = (t >> 5) & 0x0F;
        switch (t & 3) {
            case 0:  // done
                mmuPrvDumpUpdate(va, 0, 1UL << 20, 0, 0, false, false, false);
                continue;

            case 1:  // coarse page table
                coarse = true;
                t &= 0xFFFFFC00UL;
                break;

            case 2:  // section
                mmuPrvDumpUpdate(va, t & 0xFFF00000UL, 1UL << 20, dom, (t >> 10) & 3, !!(t & 8),
                                 !!(t & 4), true);
                continue;

            case 3:  // fine page table
                t &= 0xFFFFF000UL;
                break;
        }

        sla = t;
        psz = coarse ? 4096 : 1024;
        for (j = 0; j < ((1UL << 20) / psz); j++) {
            t = mmuPrvDebugRead(mmu, sla + (j << 2));
            va = (i << 20) + (j * psz);
            switch (t & 3) {
                case 0:  // invalid
                    mmuPrvDumpUpdate(va, 0, psz, 0, 0, false, false, false);
                    break;

                case 1:  // large 64k page
                    mmuPrvDumpUpdate(va + 0 * 16384UL, (t & 0xFFFF0000UL) + 0 * 16384UL, 16384, dom,
                                     (t >> 4) & 3, !!(t & 8), !!(t & 4), true);
                    mmuPrvDumpUpdate(va + 1 * 16384UL, (t & 0xFFFF0000UL) + 1 * 16384UL, 16384, dom,
                                     (t >> 6) & 3, !!(t & 8), !!(t & 4), true);
                    mmuPrvDumpUpdate(va + 2 * 16384UL, (t & 0xFFFF0000UL) + 2 * 16384UL, 16384, dom,
                                     (t >> 8) & 3, !!(t & 8), !!(t & 4), true);
                    mmuPrvDumpUpdate(va + 3 * 16384UL, (t & 0xFFFF0000UL) + 3 * 16384UL, 16384, dom,
                                     (t >> 10) & 3, !!(t & 8), !!(t & 4), true);
                    j += coarse ? 15 : 63;
                    break;

                case 2:  // small 4k page
                    mmuPrvDumpUpdate(va + 0 * 1024, (t & 0xFFFFF000UL) + 0 * 1024, 1024, dom,
                                     (t >> 4) & 3, !!(t & 8), !!(t & 4), true);
                    mmuPrvDumpUpdate(va + 1 * 1024, (t & 0xFFFFF000UL) + 1 * 1024, 1024, dom,
                                     (t >> 6) & 3, !!(t & 8), !!(t & 4), true);
                    mmuPrvDumpUpdate(va + 2 * 1024, (t & 0xFFFFF000UL) + 2 * 1024, 1024, dom,
                                     (t >> 8) & 3, !!(t & 8), !!(t & 4), true);
                    mmuPrvDumpUpdate(va + 3 * 1024, (t & 0xFFFFF000UL) + 3 * 1024, 1024, dom,
                                     (t >> 10) & 3, !!(t & 8), !!(t & 4), true);
                    if (!coarse) j += 3;
                    break;

                case 3:  // tiny 1k page or TEX page on pxa
                    if (coarse)
                        mmuPrvDumpUpdate(va, t & 0xFFFFF000UL, 4096, dom, (t >> 4) & 3, !!(t & 8),
                                         !!(t & 4), true);
                    else
                        mmuPrvDumpUpdate(va, t & 0xFFFFFC00UL, 1024, dom, (t >> 4) & 3, !!(t & 8),
                                         !!(t & 4), true);
                    break;
            }
        }
    }
    mmuPrvDumpUpdate(0, 0, 0, 0, 0, false, false, false);  // finish things off
}

template <typename T>
void mmuSave(struct ArmMmu *mmu, T &savestate) {
    auto chunk = savestate.GetChunk(ChunkType::mmu, SAVESTATE_VERSION);
    if (!chunk) ERR("unable to allocate chunk");

    SaveChunkHelper helper(*chunk);
    mmu->DoSaveLoad(helper);

    // XScale D-cache state, in every XScale save so a reused chunk never keeps old bytes.
    if (mmu->xscale) {
        auto dc = savestate.GetChunk(ChunkType::xscaleDcache, 0);
        if (!dc) ERR("unable to allocate chunk");
        SaveChunkHelper dcHelper(*dc);
        dcHelper.Do8(mmu->dcOn).Do8(mmu->staleOn).Do32(mmu->staleTtb);
    }
}

template <typename T>
void mmuLoad(struct ArmMmu *mmu, T &loader) {
    auto chunk = loader.GetChunkOrFail(ChunkType::mmu, SAVESTATE_VERSION, "mmu");
    if (!chunk) return;

    LoadChunkHelper helper(*chunk);
    mmu->DoSaveLoad(helper);

    // Sessions from before the D-cache state ran with C on whenever the MMU was, and no window.
    mmu->dcOn = mmu->xscale && mmu->transTablPA != MMU_DISABLED_TTP;
    mmu->staleOn = 0;
    mmu->staleTtb = 0;
    if (mmu->xscale && loader.HasChunk(ChunkType::xscaleDcache)) {
        auto dc = loader.GetChunkOrFail(ChunkType::xscaleDcache, 0, "xscale dcache");
        if (!dc) return;
        LoadChunkHelper dcHelper(*dc);
        uint8_t dcOn = 0, staleOn = 0;
        uint32_t staleTtb = 0;
        dcHelper.Do8(dcOn).Do8(staleOn).Do32(staleTtb);
        if (dc->HasError() || dcOn > 1 || staleOn > 1) {
            loader.NotifyError();
            return;
        }
        mmu->dcOn = dcOn;
        mmu->staleOn = staleOn;
        mmu->staleTtb = staleTtb;
    }

    mmuTlbFlush(mmu);
}

template <typename T>
void ArmMmu::DoSaveLoad(T &chunkHelper) {
    chunkHelper.Do32(transTablPA).Do32(domainCfg).Do(typename T::Pack8() << S << R);
}

template void mmuSave<Savestate<ChunkType>>(ArmMmu *mmu, Savestate<ChunkType> &savestate);
template void mmuSave<SavestateProbe<ChunkType>>(ArmMmu *mmu, SavestateProbe<ChunkType> &savestate);
template void mmuLoad<SavestateLoader<ChunkType>>(ArmMmu *mmu, SavestateLoader<ChunkType> &loader);

void mmuArm920Capture(const ArmMmu* mmu, Arm920SavedState& s) {
    s.transTablPA = mmu->transTablPA;
    s.domainCfg = mmu->domainCfg;
    s.mmuS = mmu->S;
    s.mmuR = mmu->R;
    s.tlbRevision = mmu->revision;
    s.tlbFlushes = mmu->flushes;
    s.tlb.clear();
    for (uint32_t i = 0; i < TLB_SIZE; ++i) {
        const auto& e = mmu->tlb[i];
        if (e.revision != mmu->revision) continue;
        s.tlb.push_back(i);
        s.tlb.push_back(uint32_t(e.pa) | uint32_t(e.ap) << 20 | uint32_t(e.domain) << 22 |
                        uint32_t(e.c) << 26 | uint32_t(e.section) << 27 | uint32_t(e.b) << 28);
    }
}

void mmuArm920Apply(ArmMmu* mmu, const Arm920SavedState& s) {
    mmu->transTablPA = s.transTablPA;
    mmu->domainCfg = s.domainCfg;
    mmu->S = s.mmuS;
    mmu->R = s.mmuR;
    // Revision 0 is never current, so every other entry is dead.
    memset(mmu->tlb, 0, sizeof(mmu->tlb));
    mmu->revision = s.tlbRevision;
    mmu->flushes = s.tlbFlushes;
    for (size_t n = 0; n + 1 < s.tlb.size(); n += 2) {
        const uint32_t packed = s.tlb[n + 1];
        auto& e = mmu->tlb[s.tlb[n] & (TLB_SIZE - 1)];
        e.pa = packed & 0xfffff;
        e.ap = (packed >> 20) & 3;
        e.domain = (packed >> 22) & 15;
        e.c = (packed >> 26) & 1;
        e.section = (packed >> 27) & 1;
        e.b = (packed >> 28) & 1;
        e.revision = s.tlbRevision;
    }
    mmuPrvPermChanged(mmu);
}

void mmuEmitDiagnostic(const ArmMmu* mmu, const ArmDiagnosticSink& sink) {
    const uint32_t state[] = {mmu->transTablPA,mmu->domainCfg,mmu->S,mmu->R,mmu->revision,mmu->flushes};
    sink.emit("mmu",state,6);
    for (uint32_t i=0;i<TLB_SIZE;++i) {
        const auto& e=mmu->tlb[i];
        if (e.revision != mmu->revision) continue;
        const uint32_t row[] = {i << 12,uint32_t(e.pa) << 12,e.ap,e.domain,e.section,e.c,e.b};
        sink.emit("tlb",row,7);
    }
}
