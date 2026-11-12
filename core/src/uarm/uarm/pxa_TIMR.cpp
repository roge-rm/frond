//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "pxa_TIMR.h"

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "cputil.h"
#include "mem.h"
#include "pxa_IC.h"
#include "savestate/savestateAll.h"

#define PXA_TIMR_BASE 0x40A00000UL
#define PXA_TIMR_SIZE 0x00010000UL

#define SAVESTATE_VERSION 1

struct PxaTimr {
    struct PxaIc *ic;

    uint32_t OSMR[4];  // Match Register 0-3
    uint32_t OSCR;     // Counter Register
    uint8_t OIER;      // Interrupt Enable
    uint8_t OWER;      // Watchdog enable
    uint8_t OSSR;      // Status Register

    uint32_t interruptsSuspendCount;
    uint32_t synchronizedTicks;
    uint32_t (*elapsedTicks)(void*);
    void (*reschedule)(void*);
    void* clockContext;

    template <typename T>
    void DoSaveLoad(T &chunkHelper, uint32_t version);
};

static void pxaTimrPrvRaiseLowerInts(struct PxaTimr *timr) {
    if (timr->interruptsSuspendCount > 0) {
        pxaIcInt(timr->ic, PXA_I_TIMR0, false);
        pxaIcInt(timr->ic, PXA_I_TIMR1, false);
        pxaIcInt(timr->ic, PXA_I_TIMR2, false);
        pxaIcInt(timr->ic, PXA_I_TIMR3, false);
    } else {
        pxaIcInt(timr->ic, PXA_I_TIMR0, (timr->OSSR & 1) != 0);
        pxaIcInt(timr->ic, PXA_I_TIMR1, (timr->OSSR & 2) != 0);
        pxaIcInt(timr->ic, PXA_I_TIMR2, (timr->OSSR & 4) != 0);
        pxaIcInt(timr->ic, PXA_I_TIMR3, (timr->OSSR & 8) != 0);
    }
}

static void pxaTimrPrvCheckMatchSingleStep(struct PxaTimr *timr, uint_fast8_t idx) {
    uint_fast8_t v = 1UL << idx;

    if (timr->OSCR == timr->OSMR[idx]) {
        if (idx == 3 && timr->OWER) ERR("WDT fires\n");

        if (timr->OIER & v) timr->OSSR |= v;
    }
}

static void pxaTimrPrvCheckMatchMultiStep(struct PxaTimr *timr, uint_fast8_t idx, uint32_t oscrOld,
                                          uint32_t batchedTicks) {
    uint_fast8_t v = 1UL << idx;

    const uint32_t delta = timr->OSMR[idx] - oscrOld;
    if (delta && batchedTicks >= delta) {
        if (idx == 3 && timr->OWER) ERR("WDT fires\n");

        if (timr->OIER & v) timr->OSSR |= v;
    }
}

static void pxaTimrPrvUpdateSingleStep(struct PxaTimr *timr, bool forceInterruptUpdate = true) {
    uint_fast8_t i;

    for (i = 0; i < 4; i++) pxaTimrPrvCheckMatchSingleStep(timr, i);

    if (forceInterruptUpdate || timr->OSSR) pxaTimrPrvRaiseLowerInts(timr);
}

static void pxaTimrPrvUpdateMultiStep(struct PxaTimr *timr, uint32_t oscrOld,
                                      uint32_t batchedTicks) {
    uint_fast8_t i;

    for (i = 0; i < 4; i++) pxaTimrPrvCheckMatchMultiStep(timr, i, oscrOld, batchedTicks);

    if (timr->OSSR) pxaTimrPrvRaiseLowerInts(timr);
}

static void pxaTimrAdvanceCounter(PxaTimr* timr, uint32_t ticks);
void pxaTimrSynchronize(PxaTimr* timr) {
    if (!timr->elapsedTicks) return;
    const uint32_t elapsed = timr->elapsedTicks(timr->clockContext);
    if (elapsed > timr->synchronizedTicks) {
        pxaTimrAdvanceCounter(timr, elapsed - timr->synchronizedTicks);
        timr->synchronizedTicks = elapsed;
    }
}

static bool pxaTimrPrvMemAccessF(void *userData, uint32_t pa, uint_fast8_t size, bool write,
                                 void *buf) {
    struct PxaTimr *timr = (struct PxaTimr *)userData;
    uint32_t val = 0;

    if (size != 4) {
        fprintf(stderr, "%s: Unexpected %s of %u bytes to 0x%08lx\n", __func__,
                write ? "write" : "read", size, (unsigned long)pa);
        return false;  // we do not support non-word accesses
    }

    pxaTimrSynchronize(timr);
    pa = (pa - PXA_TIMR_BASE) >> 2;

    if (write) {
        val = *(uint32_t *)buf;

        switch (pa) {
            case 0:
            case 1:
            case 2:
            case 3:
                // A match written at the current count still matches; the other channels keep
                // their status, as for the OIER write below.
                timr->OSMR[pa] = val;
                pxaTimrPrvCheckMatchSingleStep(timr, pa);
                pxaTimrPrvRaiseLowerInts(timr);
                break;

            case 4:
                timr->OSCR = val;
                break;

            case 5:
                // A match sets its bit as the counter reaches it, so clearing it while the
                // counter still equals the match leaves it clear. A fast guest's tick handler
                // clears it within the same count; setting it again runs the handler twice.
                timr->OSSR = timr->OSSR & ~val;
                pxaTimrPrvRaiseLowerInts(timr);
                break;

            case 6:
                timr->OWER = val;
                break;

            case 7: {
                // Only newly enabled channels take a match at the current count, as before; a
                // channel that stays enabled already had its match and may have cleared it.
                const uint8_t enabled = val & ~timr->OIER;
                timr->OIER = val;
                for (uint_fast8_t i = 0; i < 4; i++)
                    if (enabled & (1u << i)) pxaTimrPrvCheckMatchSingleStep(timr, i);
                pxaTimrPrvRaiseLowerInts(timr);
                break;
            }
        }
    } else {
        switch (pa) {
            case 0:
            case 1:
            case 2:
            case 3:
                val = timr->OSMR[pa];
                break;

            case 4:
                val = timr->OSCR;
                break;

            case 5:
                val = timr->OSSR;
                break;

            case 6:
                val = timr->OWER;
                break;

            case 7:
                val = timr->OIER;
                break;
        }
        *(uint32_t *)buf = val;
    }

    if (write && timr->reschedule) timr->reschedule(timr->clockContext);
    return true;
}

struct PxaTimr *pxaTimrInit(struct ArmMem *physMem, struct PxaIc *ic) {
    struct PxaTimr *timr = (struct PxaTimr *)malloc(sizeof(*timr));

    if (!timr) ERR("cannot alloc OSTIMER");

    memset(timr, 0, sizeof(*timr));
    timr->ic = ic;

    if (!memRegionAdd(physMem, PXA_TIMR_BASE, PXA_TIMR_SIZE, pxaTimrPrvMemAccessF, timr))
        ERR("cannot add OSTIMER to MEM\n");

    return timr;
}

static void pxaTimrAdvanceCounter(struct PxaTimr *timr, uint32_t batchedTicks) {
    if (!batchedTicks) return;
    if (batchedTicks == 1) {
        timr->OSCR++;
        // Ticks only set status bits; register writes still refresh cleared lines.
        pxaTimrPrvUpdateSingleStep(timr, false);
    } else {
        const uint32_t oscrOld = timr->OSCR;
        timr->OSCR += batchedTicks;

        pxaTimrPrvUpdateMultiStep(timr, oscrOld, batchedTicks);
    }
}

void pxaTimrTick(PxaTimr* timr, uint32_t ticks) {
    // Register accesses may already have materialized part of this batch.
    const uint32_t consumed = std::min(ticks, timr->synchronizedTicks);
    timr->synchronizedTicks -= consumed;
    pxaTimrAdvanceCounter(timr, ticks - consumed);
}

void pxaTimrRestartClock(PxaTimr* timr) { timr->synchronizedTicks = 0; }
void pxaTimrEnableBatching(PxaTimr* timr, uint32_t (*elapsedTicks)(void*),
                           void (*reschedule)(void*), void* context) {
    timr->elapsedTicks = elapsedTicks;
    timr->reschedule = reschedule;
    timr->clockContext = context;
}
uint32_t pxaTimrNextBatch(PxaTimr* timr, uint32_t maximum) {
    uint32_t next = maximum;
    for (unsigned i = 0; i < 4; ++i) {
        if (!(timr->OIER & (1u << i)) && !(i == 3 && timr->OWER)) continue;
        if (timr->OSSR & (1u << i)) { next = 1; break; }
        const uint32_t delta = timr->OSMR[i] - timr->OSCR;
        if (delta && delta < next) next = delta;
    }
    return std::min(uint64_t(UINT32_MAX), uint64_t(timr->synchronizedTicks) + next);
}

uint32_t pxaTimrTicksToNextInterrupt(struct PxaTimr *timr) {
    uint32_t ticksToNextInterrupt = ~0;

    for (size_t i = 0; i < 4; i++) {
        const uint8_t v = 1UL << i;

        if ((timr->OIER & v) == 0) continue;
        if ((timr->OSSR & v) != 0) return 1;

        const uint32_t delta = timr->OSMR[i] - timr->OSCR;

        if (delta < ticksToNextInterrupt && delta != 0) ticksToNextInterrupt = delta;
    }

    return ticksToNextInterrupt;
}

void pxaTimrSuspendInterrupts(struct PxaTimr *timr, bool suspendInterrupts) {
    if (suspendInterrupts) {
        timr->interruptsSuspendCount++;
    } else if (timr->interruptsSuspendCount > 0) {
        timr->interruptsSuspendCount--;
    }

    pxaTimrPrvRaiseLowerInts(timr);
}

template <typename T>
void pxaTimrSave(PxaTimr *timr, T &savestate) {
    const uint32_t version = timr->elapsedTicks ? SAVESTATE_VERSION : 0;
    auto chunk = savestate.GetChunk(ChunkType::pxaTimr, version);
    if (!chunk) ERR("unable to allocate chunk");

    SaveChunkHelper helper(*chunk);
    timr->DoSaveLoad(helper, version);
}

template <typename T>
void pxaTimrLoad(PxaTimr *timr, T &loader) {
    uint32_t version;
    auto chunk = loader.GetChunkOrFail(ChunkType::pxaTimr, SAVESTATE_VERSION, "pxaTimr", version);
    if (!chunk) return;

    LoadChunkHelper helper(*chunk);
    timr->synchronizedTicks = 0;
    timr->DoSaveLoad(helper, version);
}

template <typename T>
void PxaTimr::DoSaveLoad(T &chunkHelper, uint32_t version) {
    chunkHelper.Do32(OSMR[0]).Do32(OSMR[1]).Do32(OSMR[2]).Do32(OSMR[3]).Do32(OSCR).Do(
        typename T::Pack8() << OIER << OWER << OSSR);
    if (version >= 1) chunkHelper.Do32(synchronizedTicks);
}

template void pxaTimrSave<Savestate<ChunkType>>(PxaTimr *timr, Savestate<ChunkType> &savestate);
template void pxaTimrSave<SavestateProbe<ChunkType>>(PxaTimr *timr,
                                                     SavestateProbe<ChunkType> &savestate);
template void pxaTimrLoad<SavestateLoader<ChunkType>>(PxaTimr *timr,
                                                      SavestateLoader<ChunkType> &loader);
