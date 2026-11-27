//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "pxa_RTC.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "cputil.h"
#include "mem.h"
#include "pxa_IC.h"
#include "savestate/savestateAll.h"
#include "timeutil.h"

#define PXA_RTC_BASE 0x40900000UL
#define PXA_RTC_SIZE 0x00001000UL

#define SAVESTATE_VERSION 2

struct PxaRtc {
    struct PxaIc *ic;

    uint64_t lastAlarmCheck;
    uint32_t counterOffset;
    bool unixEpoch;

    uint32_t RTAR;  // RTC alarm
    uint32_t RTTR;  // RTC trim - we ignore this alltogether
    uint8_t RTSR;   // RTC status
    void (*wakeCallback)(void*);
    void* wakeContext;

    template <typename T>
    void DoSaveLoad(T &chunkHelper, uint32_t version) {
        if (version == 0) {
            uint32_t lastSeenTime = 0, RCNR = 0;
            chunkHelper.Do32(lastSeenTime).Do32(RCNR).Do32(RTAR).Do32(RTTR).Do8(RTSR);
        } else {
            chunkHelper.Do32(RTAR).Do32(RTTR).Do8(RTSR).Do64(lastAlarmCheck);
        }
        if (version >= 2) chunkHelper.Do32(counterOffset);
    }
};

static void pxaRtcPrvUpdateInterrupts(struct PxaRtc *rtc) {
    pxaIcInt(rtc->ic, PXA_I_RTC_ALM, !!(rtc->RTSR & 1));
    pxaIcInt(rtc->ic, PXA_I_RTC_HZ, !!(rtc->RTSR & 2));
}

static uint64_t pxaRtcCounter(const PxaRtc* rtc) {
    return rtc->unixEpoch ? uint32_t(std::time(nullptr)) + rtc->counterOffset : palmEpochSeconds();
}

static void pxaRtcAlarmMatch(PxaRtc* rtc) {
    if (rtc->RTSR & 0x04) rtc->RTSR |= 1;
    // PXA27x 21.4.6.2: sleep wake is independent of RTSR[ALE].
    if (rtc->wakeCallback) rtc->wakeCallback(rtc->wakeContext);
}

static void pxaRtcPrvUpdateAlarm(struct PxaRtc *rtc) {
    const uint64_t now = pxaRtcCounter(rtc);
    if (rtc->unixEpoch) {
        // Compare elapsed counts so a match can cross the 32-bit counter rollover.
        const uint32_t elapsed = now - rtc->lastAlarmCheck;
        const uint32_t untilMatch = rtc->RTAR - rtc->lastAlarmCheck;
        if (elapsed < 0x80000000 && untilMatch && untilMatch <= elapsed) pxaRtcAlarmMatch(rtc);
        rtc->lastAlarmCheck = now;
    } else if (rtc->lastAlarmCheck < rtc->RTAR) {
        if (now >= rtc->RTAR) pxaRtcAlarmMatch(rtc);
        rtc->lastAlarmCheck = now;
    }

    pxaIcInt(rtc->ic, PXA_I_RTC_ALM, !!(rtc->RTSR & 1));
}

PxaRtcRetained pxaRtcRetained(const PxaRtc* rtc) {
    return {rtc->lastAlarmCheck, rtc->counterOffset, rtc->RTAR, rtc->RTTR, rtc->RTSR};
}

void pxaRtcRestoreRetained(PxaRtc* rtc, const PxaRtcRetained& retained) {
    rtc->lastAlarmCheck = retained.lastAlarmCheck;
    rtc->counterOffset = retained.counterOffset;
    rtc->RTAR = retained.RTAR;
    rtc->RTTR = retained.RTTR;
    rtc->RTSR = retained.RTSR;
    pxaRtcPrvUpdateInterrupts(rtc);
}

static void pxaRtcTickHz(struct PxaRtc *rtc) {
    if (rtc->RTSR & 0x8)  // send HZ interrupt
        rtc->RTSR |= 2;

    pxaIcInt(rtc->ic, PXA_I_RTC_HZ, !!(rtc->RTSR & 2));
}

static bool pxaRtcPrvMemAccessF(void *userData, uint32_t pa, uint_fast8_t size, bool write,
                                void *buf) {
    struct PxaRtc *rtc = (struct PxaRtc *)userData;
    uint32_t val = 0;

    if (size != 4) {
        fprintf(stderr, "%s: Unexpected %s of %u bytes to 0x%08lx\n", __func__,
                write ? "write" : "read", size, (unsigned long)pa);
        return false;
    }

    pa = (pa - PXA_RTC_BASE) >> 2;

    if (write) {
        val = *(uint32_t *)buf;

        switch (pa) {
            case 0:
                if (rtc->unixEpoch) {
                    rtc->counterOffset = val - uint32_t(std::time(nullptr));
                    rtc->lastAlarmCheck = val;
                    if (val == rtc->RTAR) pxaRtcAlarmMatch(rtc);
                    pxaRtcPrvUpdateInterrupts(rtc);
                }
                break;

            case 1: {
                rtc->RTAR = val;
                const uint64_t now = pxaRtcCounter(rtc);
                rtc->lastAlarmCheck = rtc->unixEpoch ? uint32_t(now - 1) : now - 1;
                pxaRtcPrvUpdateAlarm(rtc);
                break;
            }

            case 2:
                rtc->RTSR = (val & ~3UL) | ((rtc->RTSR & ~val) & 3UL);
                pxaRtcPrvUpdateInterrupts(rtc);

                break;

            case 3:
                if (!(rtc->RTTR & 0x80000000UL)) rtc->RTTR = val;
                break;
        }
    } else {
        switch (pa) {
            case 0:
                val = pxaRtcCounter(rtc);
                break;

            case 1:
                val = rtc->RTAR;
                break;

            case 2:
                val = rtc->RTSR;
                break;

            case 3:
                val = rtc->RTTR;
                break;
        }
        *(uint32_t *)buf = val;
    }

    return true;
}

struct PxaRtc *pxaRtcInit(struct ArmMem *physMem, struct PxaIc *ic, bool unixEpoch) {
    struct PxaRtc *rtc = (struct PxaRtc *)malloc(sizeof(*rtc));

    if (!rtc) ERR("cannot alloc RTC");

    memset(rtc, 0, sizeof(*rtc));
    rtc->ic = ic;
    rtc->unixEpoch = unixEpoch;
    if (unixEpoch) rtc->lastAlarmCheck = pxaRtcCounter(rtc);
    rtc->RTTR = 0x7FFF;  // nice default value

    if (!memRegionAdd(physMem, PXA_RTC_BASE, PXA_RTC_SIZE, pxaRtcPrvMemAccessF, rtc))
        ERR("cannot add RTC to MEM\n");

    return rtc;
}

void pxaRtcTick(struct PxaRtc *rtc) {
    pxaRtcPrvUpdateAlarm(rtc);
    pxaRtcTickHz(rtc);
}

void pxaRtcSetWakeCallback(PxaRtc* rtc, void (*callback)(void*), void* context) {
    rtc->wakeCallback = callback;
    rtc->wakeContext = context;
}

void pxaRtcRefreshInterrupts(PxaRtc* rtc) { pxaRtcPrvUpdateInterrupts(rtc); }

template <typename T>
void pxaRtcSave(struct PxaRtc *rtc, T &savestate) {
    const uint32_t version = rtc->unixEpoch ? SAVESTATE_VERSION : 1;
    auto chunk = savestate.GetChunk(ChunkType::pxaRtc, version);
    if (!chunk) ERR("unable to allocate chunk");

    SaveChunkHelper helper(*chunk);
    rtc->DoSaveLoad(helper, version);
}

template <typename T>
void pxaRtcLoad(struct PxaRtc *rtc, T &loader) {
    uint32_t gotVersion;
    auto chunk = loader.GetChunkOrFail(ChunkType::pxaRtc, SAVESTATE_VERSION, "pxa rtc", gotVersion);
    if (!chunk) return;

    LoadChunkHelper helper(*chunk);
    rtc->DoSaveLoad(helper, gotVersion);

    if (rtc->unixEpoch && gotVersion < 2) {
        // Older Zaurus sessions read the Palm counter. Move any future alarm
        // with the epoch shift, retaining the current host UTC as the clock.
        rtc->counterOffset = 0;
        const uint64_t now = pxaRtcCounter(rtc);
        const uint64_t oldNow = palmEpochSeconds();
        if (rtc->RTAR >= oldNow) rtc->RTAR -= oldNow - now;
        rtc->lastAlarmCheck = now;
    } else if (gotVersion == 0) {
        rtc->lastAlarmCheck = pxaRtcCounter(rtc);
    }
}

template void pxaRtcSave<Savestate<ChunkType>>(PxaRtc *rtc, Savestate<ChunkType> &savestate);
template void pxaRtcSave<SavestateProbe<ChunkType>>(PxaRtc *rtc,
                                                    SavestateProbe<ChunkType> &savestate);
template void pxaRtcLoad<SavestateLoader<ChunkType>>(PxaRtc *rtc,
                                                     SavestateLoader<ChunkType> &loader);
