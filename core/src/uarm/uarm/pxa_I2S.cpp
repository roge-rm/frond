//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "pxa_I2S.h"

#include <cstdlib>
#include <cstring>

#include "cputil.h"
#include "pxa_DMA.h"
#include "pxa_IC.h"
#include "savestate/savestateAll.h"

#define PXA_I2S_BASE 0x40400000UL
#define PXA_I2S_SIZE 0x00010000UL

#define SAVESTATE_VERSION 1

struct PxaI2s {
    struct PxaDma *dma;
    struct PxaIc *ic;

    uint16_t sacr0;
    uint16_t sasr0;
    uint8_t sacr1;
    uint8_t sadiv;
    uint8_t saimr;

    uint32_t txFifo[16];
    uint32_t rxFifo[16];
    uint8_t txFifoEnts;
    uint8_t rxFifoEnts;

    void *codec{};
    PxaI2sFrame frame{};
    PxaI2sBoundary boundary{};
    uint32_t externalHz{};
    uint32_t lastTx{};
    uint64_t phase{};
    bool clockEnabled{true};
    bool legacyState{};

    template <typename T>
    void DoSaveLoad(T &chunkHelper) {
        chunkHelper.Do(typename T::Pack16() << sacr0 << sasr0)
            .Do(typename T::Pack8() << sacr1 << sadiv << saimr << txFifoEnts)
            .Do8(rxFifoEnts)
            .DoBuffer32(txFifo, sizeof(txFifo) >> 2)
            .DoBuffer32(rxFifo, sizeof(rxFifo) >> 2);
    }
};

static bool pxaI2sEnabled(const PxaI2s *i2s) {
    return (i2s->sacr0 & 9) == 1 && i2s->clockEnabled;
}

static void pxaI2sPrvRecalc(PxaI2s *i2s) {
    const bool enabled = pxaI2sEnabled(i2s);
    i2s->sasr0 &= 0x60;
    i2s->sasr0 |= (i2s->txFifoEnts & 15) << 8;
    i2s->sasr0 |= (i2s->rxFifoEnts & 15) << 12;
    if (i2s->txFifoEnts < 16) i2s->sasr0 |= 1;
    if (i2s->rxFifoEnts) i2s->sasr0 |= 2;
    if (enabled && ((i2s->sacr0 & 4) ? i2s->sadiv != 0 : i2s->externalHz != 0)) i2s->sasr0 |= 4;
    if (enabled && !(i2s->sacr1 & 0x10) &&
        i2s->txFifoEnts < ((i2s->sacr0 >> 8) & 15) + 1) i2s->sasr0 |= 8;
    if (enabled && !(i2s->sacr1 & 8) &&
        i2s->rxFifoEnts >= ((i2s->sacr0 >> 12) & 15) + 1) i2s->sasr0 |= 0x10;
    pxaIcInt(i2s->ic, PXA_I_I2S, enabled && (i2s->sasr0 & i2s->saimr & 0x78));
    pxaDmaExternalReq(i2s->dma, DMA_CMR_I2S_TX, !!(i2s->sasr0 & 8));
    pxaDmaExternalReq(i2s->dma, DMA_CMR_I2S_RX, !!(i2s->sasr0 & 0x10));
}

static void pxaI2sPrvBoundary(PxaI2s *i2s) {
    i2s->phase = 0;
    if (i2s->boundary) i2s->boundary(i2s->codec);
}

static void pxaI2sPrvResetRegisters(PxaI2s *i2s) {
    i2s->sacr1 = i2s->saimr = 0;
    i2s->sadiv = 26;
    i2s->txFifoEnts = i2s->rxFifoEnts = 0;
    i2s->sasr0 = 1;
    i2s->lastTx = 0;
    memset(i2s->txFifo, 0, sizeof(i2s->txFifo));
    memset(i2s->rxFifo, 0, sizeof(i2s->rxFifo));
    pxaI2sPrvBoundary(i2s);
}

static bool pxaI2sPrvFifoW(struct PxaI2s *i2s, uint32_t val) {
    if ((i2s->sacr0 & 0x30) == 0x30) {
        if (i2s->rxFifoEnts < 16) i2s->rxFifo[i2s->rxFifoEnts++] = val;
        pxaI2sPrvRecalc(i2s);
        return true;
    }
    if ((i2s->sacr0 & 8) || (i2s->sacr1 & 0x10)) return true;
    if (i2s->txFifoEnts == sizeof(i2s->txFifo) / sizeof(*i2s->txFifo)) {
        fprintf(stderr, "TX fifo overrun\n");
        return true;
    }
    i2s->txFifo[i2s->txFifoEnts++] = val;
    pxaI2sPrvRecalc(i2s);

    return true;
}

static bool pxaI2sPrvFifoR(struct PxaI2s *i2s, uint32_t *valP) {
    if ((i2s->sacr0 & 0x30) == 0x10) {
        *valP = i2s->txFifoEnts ? i2s->txFifo[0] : 0;
        if (i2s->txFifoEnts) memmove(i2s->txFifo, i2s->txFifo + 1,
            sizeof(*i2s->txFifo) * --i2s->txFifoEnts);
        pxaI2sPrvRecalc(i2s);
        return true;
    }
    if ((i2s->sacr0 & 8) || (i2s->sacr1 & 8)) { *valP = 0; return true; }
    if (!i2s->rxFifoEnts) {
        fprintf(stderr, "RX fifo underrun\n");
        return false;
    }
    *valP = i2s->rxFifo[0];
    memmove(i2s->rxFifo + 0, i2s->rxFifo + 1, sizeof(*i2s->rxFifo) * --i2s->rxFifoEnts);
    pxaI2sPrvRecalc(i2s);

    return true;
}

static bool pxaI2sPrvMemAccessF(void *userData, uint32_t pa, uint_fast8_t size, bool write,
                                void *buf) {
    struct PxaI2s *i2s = (struct PxaI2s *)userData;
    uint32_t val = 0;

    if (size != 4) {
        fprintf(stderr, "%s: Unexpected %s of %u bytes to 0x%08lx\n", __func__,
                write ? "write" : "read", size, (unsigned long)pa);
        return false;
    }

    pa = (pa - PXA_I2S_BASE) >> 2;

    if (write) val = *(uint32_t *)buf;
    if (write && pa != 0 && (i2s->sacr0 & 8)) return true;

    switch (pa) {
        case 0:
            if (write) {
                const auto previous = i2s->sacr0;
                i2s->sacr0 = val & 0xff3d;
                if (val & 8) pxaI2sPrvResetRegisters(i2s);
                else if ((previous ^ i2s->sacr0) & 5) pxaI2sPrvBoundary(i2s);
            } else
                val = i2s->sacr0;
            break;

        case 1:
            if (write) {
                const auto changed = i2s->sacr1 ^ (val & 0x39);
                i2s->sacr1 = val & 0x39;
                if (val & 0x10) { i2s->txFifoEnts = 0; i2s->lastTx = 0; }
                if (val & 8) i2s->rxFifoEnts = 0;
                if (changed & 0x11) pxaI2sPrvBoundary(i2s);
            } else
                val = i2s->sacr1;
            break;

        case 3:
            if (write)
                return false;
            else
                val = i2s->sasr0;
            break;

        case 5:
            if (write)
                i2s->saimr = val & 0x78;
            else
                val = i2s->saimr;
            break;

        case 6:
            if (write)
                i2s->sasr0 &= ~(val & 0x60);
            else
                return false;
            break;

        case 24:
            if (write) {
                if (i2s->sadiv != (val & 0x7f)) pxaI2sPrvBoundary(i2s);
                i2s->sadiv = val & 0x7f;
            } else
                val = i2s->sadiv;
            break;

        case 32:
            if (write)
                return pxaI2sPrvFifoW(i2s, val);
            else
                return pxaI2sPrvFifoR(i2s, (uint32_t *)buf);
            break;

        default:
            return false;
    }

    if (write) pxaI2sPrvRecalc(i2s);
    if (!write) *(uint32_t *)buf = val;

    return true;
}

struct PxaI2s *pxaI2sInit(struct ArmMem *physMem, struct PxaIc *ic, struct PxaDma *dma) {
    struct PxaI2s *i2s = (struct PxaI2s *)malloc(sizeof(*i2s));

    if (!i2s) ERR("cannot alloc I2C");

    memset(i2s, 0, sizeof(*i2s));
    i2s->clockEnabled = true;
    i2s->ic = ic;
    i2s->dma = dma;
    i2s->sacr0 = 0x7700;
    i2s->sasr0 = 0x0001;
    i2s->sadiv = 0x001a;

    if (!memRegionAdd(physMem, PXA_I2S_BASE, PXA_I2S_SIZE, pxaI2sPrvMemAccessF, i2s))
        ERR("cannot add I2S to MEM\n");

    return i2s;
}

void pxaI2sAttach(PxaI2s *i2s, void *codec, PxaI2sFrame frame, PxaI2sBoundary boundary) {
    pxaI2sPrvBoundary(i2s);
    i2s->codec = codec;
    i2s->frame = frame;
    i2s->boundary = boundary;
    pxaI2sPrvBoundary(i2s);
}

void pxaI2sSetExternalRate(PxaI2s *i2s, uint32_t hz, bool discontinuity) {
    if (hz == i2s->externalHz) return;
    i2s->externalHz = hz;
    if (discontinuity) pxaI2sPrvBoundary(i2s);
}

void pxaI2sSetClockEnabled(PxaI2s *i2s, bool enabled, bool discontinuity) {
    if (i2s->clockEnabled != enabled) {
        i2s->clockEnabled = enabled;
        if (discontinuity) pxaI2sPrvBoundary(i2s);
        pxaI2sPrvRecalc(i2s);
    }
}

void pxaI2sPeriodic(struct PxaI2s *i2s) {
    if (!pxaI2sEnabled(i2s)) return;
    uint32_t val = 0;
    const bool playing = !(i2s->sacr1 & 0x10);
    if (playing) {
        if ((i2s->sacr0 & 0x30) == 0x10) {
            val = 0;
        } else if (i2s->txFifoEnts) {
            i2s->lastTx = i2s->txFifo[0];
            memmove(i2s->txFifo, i2s->txFifo + 1, sizeof(*i2s->txFifo) * --i2s->txFifoEnts);
        } else {
            i2s->sasr0 |= 0x20;
        }
        if ((i2s->sacr0 & 0x30) != 0x10) val = i2s->lastTx;
    }
    // A disconnected ADC supplies zeros, never samples from the host microphone.
    uint32_t rx = i2s->frame ? i2s->frame(i2s->codec, val, playing, !!(i2s->sacr1 & 1),
        (i2s->sacr0 & 4) ? 576000 : i2s->externalHz, (i2s->sacr0 & 4) ? i2s->sadiv : 1) : 0;
    if (i2s->sacr1 & 0x20) rx = val;
    if (!(i2s->sacr1 & 8) && (i2s->sacr0 & 0x30) != 0x30) {
        if (i2s->rxFifoEnts < 16) i2s->rxFifo[i2s->rxFifoEnts++] = rx;
        else i2s->sasr0 |= 0x40;
    }
    pxaI2sPrvRecalc(i2s);
}

// The scheduler supplies exact 20 us quanta. Phase retains sub-frame time.
void pxaI2sAdvance(PxaI2s *i2s, uint64_t elapsedNs) {
    if (!pxaI2sEnabled(i2s)) return;
    const bool master = i2s->sacr0 & 4;
    const uint32_t numerator = master ? 576000 : i2s->externalHz;
    const uint32_t denominator = master ? i2s->sadiv : 1;
    if (!numerator || !denominator) return;
    const uint64_t limit = 1000000000ull * denominator;
    // Bound intermediate arithmetic without silently dropping emulated time.
    while (elapsedNs) {
        const auto slice = elapsedNs > 1000000 ? 1000000 : elapsedNs;
        i2s->phase += slice * numerator;
        elapsedNs -= slice;
        while (i2s->phase >= limit) {
            i2s->phase -= limit;
            pxaI2sPeriodic(i2s);
        }
    }
}

bool pxaI2sHasCodec(const PxaI2s *i2s) { return i2s && i2s->frame; }

template <typename T>
void pxaI2sSave(struct PxaI2s *i2s, T &savestate) {
    auto chunk = savestate.GetChunk(ChunkType::pxaI2s, SAVESTATE_VERSION);
    if (!chunk) ERR("unable to allocate chunk");

    SaveChunkHelper helper(*chunk);
    i2s->DoSaveLoad(helper);
    helper.Do32(i2s->lastTx).Do64(i2s->phase);
}

template <typename T>
void pxaI2sLoad(struct PxaI2s *i2s, T &loader) {
    uint32_t version{};
    auto chunk = loader.GetChunkOrFail(ChunkType::pxaI2s, SAVESTATE_VERSION, "pxa i2s", version);
    if (!chunk) return;
    auto next = *i2s;
    LoadChunkHelper helper(*chunk);
    next.DoSaveLoad(helper);
    next.lastTx = 0;
    next.phase = 0;
    if (version >= 1) helper.Do32(next.lastTx).Do64(next.phase);
    const auto denominator = (next.sacr0 & 4) ? next.sadiv : 1;
    if (chunk->HasError() || next.txFifoEnts > 16 || next.rxFifoEnts > 16 ||
        (next.sacr0 & ~0xff3d) || (next.sacr1 & ~0x39) || (next.saimr & ~0x78) ||
        next.sadiv > 127 || (!denominator && next.phase) ||
        (denominator && next.phase >= 1000000000ull * denominator)) {
        loader.NotifyError();
        return;
    }
    next.legacyState = version == 0;
    *i2s = next;
    // Reconcile IRQ/DMA only after the board, GPIO, IC and DMA loads complete.

}

template void pxaI2sSave<Savestate<ChunkType>>(PxaI2s *i2s, Savestate<ChunkType> &savestate);
template void pxaI2sSave<SavestateProbe<ChunkType>>(PxaI2s *i2s,
                                                    SavestateProbe<ChunkType> &savestate);
template void pxaI2sLoad<SavestateLoader<ChunkType>>(PxaI2s *i2s,
                                                     SavestateLoader<ChunkType> &loader);

void pxaI2sRefresh(PxaI2s *i2s) { pxaI2sPrvRecalc(i2s); }

void pxaI2sOutputBoundary(PxaI2s *i2s) { pxaI2sPrvBoundary(i2s); }
bool pxaI2sLegacyState(const PxaI2s *i2s) { return i2s->legacyState; }
void pxaI2sGetRate(const PxaI2s *i2s, uint32_t &numerator, uint32_t &denominator) {
    numerator = (i2s->sacr0 & 4) ? 576000 : i2s->externalHz;
    denominator = (i2s->sacr0 & 4) ? i2s->sadiv : 1;
}

bool pxaI2sClocking(const PxaI2s *i2s) {
    uint32_t numerator{}, denominator{};
    pxaI2sGetRate(i2s, numerator, denominator);
    return pxaI2sEnabled(i2s) && numerator && denominator;
}
bool pxaI2sClockStateValid(const PxaI2s *i2s) {
    return pxaI2sClocking(i2s) || !i2s->phase;
}

bool pxaI2sLinkEnabled(const PxaI2s *i2s) { return pxaI2sEnabled(i2s); }
