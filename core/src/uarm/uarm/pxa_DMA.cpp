//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "pxa_DMA.h"

#include <cstdlib>
#include <cstring>

#include "cputil.h"
#include "mem.h"
#include "pxa_IC.h"
#include "savestate/savestateAll.h"

#define PXA_DMA_BASE 0x40000000UL
#define PXA_DMA_SIZE 0x00002000UL

#define REG_DAR 0  // descriptor
#define REG_SAR 1  // source
#define REG_TAR 2  // dest
#define REG_CR 3   // command
#define REG_CSR 4  // status

#define SAVESTATE_VERSION 0

struct PxaDmaChannel {
    uint32_t DAR;  // descriptor address register
    uint32_t SAR;  // source address register
    uint32_t TAR;  // target address register
    uint32_t CR;   // command register
    uint32_t CSR;  // control and status register

    bool dsAddrWriten;
    bool dtAddrWriten;
    bool dcmdAddrWritten;

    template <typename T>
    void DoSaveLoad(T& chunkHelper) {
        chunkHelper.Do32(DAR).Do32(SAR).Do32(TAR).Do32(CR).Do32(CSR).Do(
            typename T::BoolPack() << dsAddrWriten << dtAddrWriten << dcmdAddrWritten);
    }
};

struct PxaDma {
    struct PxaIc* ic;
    struct ArmMem* mem;
    struct Reschedule reschedule;

    uint32_t dalgn, dpcsr;
    uint32_t DINT;
    struct PxaDmaChannel channels[32];
    uint8_t CMR[75];  // channel map registers	[  we store lower 8 bits only :-)  ]
    struct PxaDmaPeripheral peripherals[75];  // wiring by request line, never serialized

    template <typename T>
    void DoSaveLoad(T& chunkHelper) {
        chunkHelper.Do32(dalgn).Do32(dpcsr).Do32(DINT).DoBuffer(CMR, sizeof(CMR));

        for (size_t i = 0; i < 32; i++) channels[i].DoSaveLoad(chunkHelper);
    }
};

static void pxaDmaPrvChannelIrqRecalc(struct PxaDma* dma, uint_fast8_t channel) {
    dma->DINT &= ~(1 << channel);

    if (dma->channels[channel].CSR & 0x20000000ul) {  // stop irq enabled?

        if (dma->channels[channel].CSR & 0x8) {  // channel in stoped state

            dma->DINT |= 1 << channel;
        }
    }
    if (dma->channels[channel].CSR & 4) {  // endintr requested and on?

        dma->DINT |= 1 << channel;
    }
    if (dma->channels[channel].CSR & 2) {  // startintr requested and on?

        dma->DINT |= 1 << channel;
    }
    if (dma->channels[channel].CSR & 1) {  // busints on?

        dma->DINT |= 1 << channel;
    }
    if ((dma->channels[channel].CSR & 0x10000200ul) == 0x10000200ul) {  // EORINT with EORIRQEN

        dma->DINT |= 1 << channel;
    }

    pxaIcInt(dma->ic, PXA_I_DMA, !!dma->DINT);
}

static void pxaDmaPrvChannelStop(struct PxaDma* dma, struct PxaDmaChannel* ch) {
    ch->dsAddrWriten = false;
    ch->dtAddrWriten = false;
    ch->dcmdAddrWritten = false;
    ch->CSR &= ~0x80000000ul;
    ch->CSR |= 0x08;
}

static bool pxaDmaPrvChannelRunningByCsrVal(uint32_t csr) {
    // previously was !(ch->CSR & 8)

    return (csr & 0x80000008ul) == 0x80000000ul;
}

static bool pxaDmaPrvChannelRunning(struct PxaDma* dma, struct PxaDmaChannel* ch) {
    return pxaDmaPrvChannelRunningByCsrVal(ch->CSR);
}

static void pxaDmaPrvChannelDescrFetch(
    struct PxaDma* dma,
    struct PxaDmaChannel* ch)  // you must call pxaDmaPrvChannelIrqRecalc() after this func
{
    uint32_t nextD, nextS, nextT, nextC, dar = ch->DAR & ~0x0f;

    if ((ch->DAR & 2) && (ch->CSR & 0x0400))  // branch mode
        dar += 32;

    if (!memAccess<4, false>(dma->mem, dar + 0, &nextD) ||
        !memAccess<4, false>(dma->mem, dar + 4, &nextS) ||
        !memAccess<4, false>(dma->mem, dar + 8, &nextT) ||
        !memAccess<4, false>(dma->mem, dar + 12, &nextC)) {
        // ERROR
        fprintf(stderr, "DMA descriptor fetch error\n");
        ch->CSR |= 1;  // signal bus error, not running

        pxaDmaPrvChannelStop(dma, ch);
    } else {
        ch->DAR = nextD;
        ch->SAR = nextS & ~0x03;
        ch->TAR = nextT & ~0x03;
        ch->CR = nextC;

        if (nextC & 0x00400000ul)  // start irq requested?
            ch->CSR |= 2;
    }

    // fprintf(stderr, "DESCR fetched from 0x%08x: {0x%08x 0x%08x 0x%08x 0x%08x}\n", dar, nextD,
    // nextS, nextT, nextC);
}

// The handshake-capable peripheral mapped to this channel, if it is the only
// request line mapped there. A stale mapping shared with another line is ignored.
static struct PxaDmaPeripheral* pxaDmaPrvPeripheral(struct PxaDma* dma, uint_fast8_t channel) {
    struct PxaDmaPeripheral* found = nullptr;
    for (unsigned i = 0; i < sizeof(dma->CMR); i++) {
        if (!(dma->CMR[i] & 0x80) || (dma->CMR[i] & 0x1f) != channel) continue;
        if (found || !dma->peripherals[i].validBytes) return nullptr;
        found = &dma->peripherals[i];
    }
    return found;
}

// A receive channel stopping at the end of its chain signals EOC to its peripheral.
static void pxaDmaPrvEndOfChain(struct PxaDma* dma, uint_fast8_t channel) {
    if (!(dma->channels[channel].CR & 0x20000000ul)) return;
    struct PxaDmaPeripheral* peripheral = pxaDmaPrvPeripheral(dma, channel);
    if (peripheral && peripheral->endOfChain) peripheral->endOfChain(peripheral->ctx);
}

static bool pxaDmaPrvChannelCheckForEnd(
    struct PxaDma* dma, uint_fast8_t channel)  // return true if irq need updating after what we did
{
    struct PxaDmaChannel* ch = &dma->channels[channel];
    bool irqUpdate = false;

    if (!pxaDmaPrvChannelRunning(dma, ch))  // stopped? not much to do...
        return false;

    if (!(ch->CR & 0x1fff)) {
        // fprintf(stderr, "channel %u ran out of data\n", channel);

        if (ch->CR & 0x00200000ul) {  // end irq requested?

            ch->CSR |= 4;
            irqUpdate = true;
        }

        if (ch->CSR & 0x40000000ul) {  // no descr fetch mode?	same as no descriptors

            // fprintf(stderr, "end cause no descr\n");
            pxaDmaPrvChannelStop(dma, ch);
            pxaDmaPrvEndOfChain(dma, channel);
            irqUpdate = true;
        } else if (ch->DAR & 1) {  // no more descriptors?

            // fprintf(stderr, "end cause DAR\n");
            pxaDmaPrvChannelStop(dma, ch);
            pxaDmaPrvEndOfChain(dma, channel);
            irqUpdate = true;
        } else {  // fetch next descriptor

            // fprintf(stderr, "fetching descriptor\n");
            pxaDmaPrvChannelDescrFetch(dma, ch);
            irqUpdate = true;
        }
    }

    return irqUpdate;
}

static bool pxaDmaPrvBusError(PxaDma* dma, PxaDmaChannel* ch) {
    ch->CSR |= 1;
    pxaDmaPrvChannelStop(dma, ch);
    return true; // caller must recalculate IRQ
}

static bool pxaDmaPrvChannelDoBurst(
    struct PxaDma* dma, uint_fast8_t channel)  // return true if irq need updating after what we did
{
    struct PxaDmaChannel* ch = &dma->channels[channel];
    uint32_t num = 4 << ((ch->CR >> 16) & 3);
    if (num > (ch->CR & 0x1fff)) num = ch->CR & 0x1fff;
    // Flow-controlled FIFO peripherals: reads take only the valid bytes, and a
    // short tail goes to or from the peripheral in byte lanes (PXA27x 5.4.6).
    struct PxaDmaPeripheral* peripheral =
        (ch->CR & 0x30000000u) ? pxaDmaPrvPeripheral(dma, channel) : nullptr;
    const bool fromPeripheral = peripheral && (ch->CR & 0x20000000u);
    const bool toPeripheral = peripheral && !fromPeripheral && (ch->CR & 0x10000000u);
    if (fromPeripheral || (toPeripheral && peripheral->externalBusAddress)) {
        const uint32_t valid = peripheral->validBytes(peripheral->ctx);
        if (num > valid) num = valid;
    }
    const bool memoryMove = !(ch->CR & 0x30000000u) &&
        memIsBufferRange(dma->mem, ch->SAR, num, false) &&
        memIsBufferRange(dma->mem, ch->TAR, num, true);
    const uint32_t width = (ch->CR >> 14) & 3;
    // PXA255 DCMD: WIDTH=00 is required for memory and companion-chip transfers.
    // On-chip peripherals keep their documented nonzero widths.
    // A draining receive FIFO is read with WIDTH=00 (PXA27x 27.4.4, as Linux pxa_camera).
    const bool drainingFifo = fromPeripheral && peripheral->drain;
    const bool externalMemory = peripheral && peripheral->externalBusAddress &&
        ((fromPeripheral && !(ch->CR&0x80000000u) && ch->SAR == peripheral->externalBusAddress &&
          memIsBufferRange(dma->mem,ch->TAR,num,true)) ||
         (toPeripheral && !(ch->CR&0x40000000u) && ch->TAR == peripheral->externalBusAddress &&
          memIsBufferRange(dma->mem,ch->SAR,num,false)));
    if (!width && !memoryMove && !externalMemory && !drainingFifo) return pxaDmaPrvBusError(dma, ch);
    const uint32_t each = width ? 1u << (width - 1) : 4;

    // A short memory-to-memory tail can be split into byte lanes. Never do
    // that to MMIO: each read/write may consume FIFO data or acknowledge status.
    const uint32_t tail = num % each;
    const uint64_t tailSource = uint64_t(ch->SAR) + ((ch->CR & 0x80000000u) ? num - tail : 0);
    const uint64_t tailTarget = uint64_t(ch->TAR) + ((ch->CR & 0x40000000u) ? num - tail : 0);
    if (tail && (fromPeripheral || toPeripheral) &&
        (tailSource > 0xffffffffull || tailTarget > 0xffffffffull ||
         !memIsBufferRange(dma->mem, fromPeripheral ? tailTarget : tailSource, tail, fromPeripheral)))
        return pxaDmaPrvBusError(dma, ch);
    if (tail && !fromPeripheral && !toPeripheral &&
        (tailSource > 0xffffffffull || tailTarget > 0xffffffffull || (ch->CR & 0x30000000u) ||
                 !memIsBufferRange(dma->mem, tailSource, tail, false) ||
                 !memIsBufferRange(dma->mem, tailTarget, tail, true)))
        return pxaDmaPrvBusError(dma, ch);

    while (num) {
        const uint32_t count = num < each ? num : each;
        // The physical memory map cannot represent a wrapping access.
        if (uint64_t(ch->SAR) + count > 0xffffffffull ||
            uint64_t(ch->TAR) + count > 0xffffffffull)
            return pxaDmaPrvBusError(dma, ch);
        uint32_t value = 0;
        bool ok = true;
        if (count == each) {
            ok = memAccess(dma->mem, ch->SAR, each, false, &value);
            if (ok) {
                ok = memAccess(dma->mem, ch->TAR, each, true, &value);
            }
        } else if (fromPeripheral) {
            // A short read takes exactly count bytes from the peripheral FIFO,
            // in halfword and byte reads, then lands in memory byte by byte.
            uint8_t bytes[4]{};
            for (unsigned i = 0; i < count && ok;) {
                const unsigned piece = count - i >= 2 ? 2 : 1;
                uint32_t part = 0;
                ok = memAccess(dma->mem, ch->SAR, piece, false, &part);
                for (unsigned k = 0; k < piece; ++k) bytes[i + k] = part >> (8 * k);
                i += piece;
            }
            for (unsigned i = 0; i < count && ok; ++i)
                ok = memAccess(dma->mem, ch->TAR + i, 1, true, &bytes[i]);
        } else {
            uint8_t bytes[4]{};
            for (unsigned i = 0; i < count && ok; ++i)
                ok = memAccess(dma->mem, ch->SAR + i, 1, false, &bytes[i]);
            for (unsigned i = 0; i < count && ok; ++i)
                ok = memAccess(dma->mem, ch->TAR + (toPeripheral ? 0 : i), 1, true, &bytes[i]);
        }
        if (!ok) return pxaDmaPrvBusError(dma, ch);
        if (ch->CR & 0x80000000u) ch->SAR += count;
        if (ch->CR & 0x40000000u) ch->TAR += count;
        ch->CR -= count;
        num -= count;
    }

    // End of receive (figure 5-9): stop, jump to the next descriptor or keep waiting.
    if (fromPeripheral && peripheral->endOfReceive && peripheral->endOfReceive(peripheral->ctx)) {
        ch->CSR |= 0x200;
        if (!(ch->CSR & 0x40000000ul) && (ch->CR & 0x1fff)) {
            if (ch->CSR & 0x04000000ul) {
                pxaDmaPrvChannelStop(dma, ch);
            } else if (ch->CSR & 0x08000000ul) {
                if (ch->DAR & 1)
                    pxaDmaPrvChannelStop(dma, ch);
                else
                    pxaDmaPrvChannelDescrFetch(dma, ch);
            }
        }
        pxaDmaPrvChannelCheckForEnd(dma, channel);
        return true;
    }

    // check for end
    return pxaDmaPrvChannelCheckForEnd(dma, channel);
}

static void pxaDmaPrvChannelActIfNeeded(struct PxaDma* dma, uint_fast8_t channel) {
    bool irqUpdate = false, doWork = false, justOne = true;
    struct PxaDmaChannel* ch = &dma->channels[channel];

    if (!pxaDmaPrvChannelRunning(dma, ch))  // stopped? not much to do...
        return;

    // check for end
    if (pxaDmaPrvChannelCheckForEnd(dma, channel)) irqUpdate = true;

    // CSR.8 might change from above call, so this cannot be removed despite the earlier check

    if (pxaDmaPrvChannelRunning(dma, ch)) {  // if we are running, get working...

        // check for work
        if (ch->CSR & 0x100)  // request?
            doWork = true;

        if (!(ch->CR & 0x30000000ul)) {  // no flow control?

            doWork = true;
            justOne = false;  // do it all
        }
        // A draining peripheral is read for as long as it holds its request.
        bool drain = false;
        if (doWork && justOne) {
            const struct PxaDmaPeripheral* peripheral = pxaDmaPrvPeripheral(dma, channel);
            drain = peripheral && peripheral->drain && (ch->CR & 0x20000000u);
        }

        if (doWork) {
            // A descriptor chain may be a ring. Yield even for unpaced memory
            // transfers so guest execution, reset and checkpoint requests can
            // run. One maximum-length descriptor still fits in this budget
            // at the smallest burst size; running channels remain scheduled.
            unsigned bursts = 0;
            do {
                if (pxaDmaPrvChannelDoBurst(dma, channel)) irqUpdate = true;

            } while (++bursts < 2048 && pxaDmaPrvChannelRunning(dma, ch) &&
                     (ch->CR & 0x1fff) && (!justOne || (drain && (ch->CSR & 0x100))));
        }
    }

    if (irqUpdate) pxaDmaPrvChannelIrqRecalc(dma, channel);
}

static void pxaDmaPrvChannelMaybeStart(struct PxaDma* dma, struct PxaDmaChannel* ch,
                                       uint32_t prevCsrVal) {
    if (pxaDmaPrvChannelRunning(dma, ch) && !pxaDmaPrvChannelRunningByCsrVal(prevCsrVal)) {
        // fprintf(stderr, "channel %u started\n", (unsigned)(ch - dma->channels));

        dma->reschedule.rescheduleCb(dma->reschedule.ctx, RESCHEDULE_TASK_DMA);

        // REQPEND reflects the peripheral request level. Starting a channel must not
        // discard a request that was asserted before RUN was set.
        if (!(ch->CSR & 0x40000000ul)) {  // not no-fetch mode

            ch->CR &= ~0x1fff;  // so we fatch descr on first request

            pxaDmaPrvChannelDescrFetch(dma, ch);
        }
    }
}

static bool pxaDmaPrvChannelRegWrite(struct PxaDma* dma, uint_fast8_t channel, uint_fast8_t reg,
                                     uint32_t val) {
    struct PxaDmaChannel* ch = &dma->channels[channel];
    bool checkForStart = false, maybeStart = false;
    uint32_t prevCsr = ch->CSR;

    if (reg == REG_DAR) {
        // fprintf(stderr, "DAR[%d] set to 0x%08x\n", channel, val);
        ch->DAR = val;

        // only in descr fetch mode
        if (val && !(ch->CSR & 0x40000000ul)) maybeStart = true;
    } else if (reg == REG_SAR) {
        if (ch->CSR & 0x40000000ul) checkForStart = true;
        ch->dsAddrWriten = true;
        ch->SAR = val & ~0x03;
    } else if (reg == REG_TAR) {
        if (ch->CSR & 0x40000000ul) checkForStart = true;
        ch->dtAddrWriten = true;
        ch->TAR = val & ~0x03;
    } else if (reg == REG_CR) {
        if (ch->CSR & 0x40000000ul) checkForStart = true;
        ch->dcmdAddrWritten = true;
        ch->CR = val;
    } else {  // CSR

        uint32_t newVal = ((prevCsr & 0x0000031ful) | (val & 0xfcc00400ul)) & ~(val & 0x00000217ul);

        if (val & 0x02000000ul)
            newVal |= 0x0400;
        else if (val & 0x01000000ul)
            newVal &= ~0x0400;

        if (newVal & 0x00400000ul)  // MaskRun
            newVal = (newVal & ~0x80000000ul) | (prevCsr & 0x80000000ul);

        // fprintf(stderr, "csr %u write 0x%08x->0x%08x (via write of 0x%08x) \n", channel, prevCsr,
        // newVal, val);

        ch->CSR = newVal;

        pxaDmaPrvChannelMaybeStart(dma, &dma->channels[channel], prevCsr);

        if (!(newVal & 0x80000000ul) && (prevCsr & 0x80000000ul)) {  // just stopped

            // fprintf(stderr, "channel %u stopped\n", channel);
            pxaDmaPrvChannelStop(dma, ch);
        }

        pxaDmaPrvChannelIrqRecalc(dma, channel);

        // fprintf(stderr, "dma->DINT is now 0x%08x\n", dma->DINT);
    }

    if (checkForStart && ch->dsAddrWriten && ch->dtAddrWriten && ch->dcmdAddrWritten)
        maybeStart = true;

    if (maybeStart) {
        ch->CSR &= ~0x08;
        // fprintf(stderr, "maybe start 0x%08x -> 0x%08x\n", prevCsr, ch->CSR);
        pxaDmaPrvChannelMaybeStart(dma, ch, prevCsr);
    }

    return true;
}

static bool pxaDmaPrvChannelRegRead(struct PxaDma* dma, uint_fast8_t channel, uint_fast8_t reg,
                                    uint32_t* retP) {
    switch (reg) {
        case REG_DAR:
            *retP = dma->channels[channel].DAR;
            break;

        case REG_SAR:
            *retP = dma->channels[channel].SAR;
            break;

        case REG_TAR:
            *retP = dma->channels[channel].TAR;
            break;

        case REG_CR:
            *retP = dma->channels[channel].CR;
            break;

        case REG_CSR:
            *retP = dma->channels[channel].CSR;
            break;

        default:
            return false;
    }

    return true;
}

void pxaDmaExternalReq(struct PxaDma* dma, uint_fast8_t chNum, bool requested) {
    uint32_t cfg = dma->CMR[chNum];

    if (cfg & 0x80) {  // is it mapped to a channel at all?

        uint32_t ch = cfg & 0x1f;

        if (!requested)
            dma->channels[ch].CSR &= ~0x100;
        else if (!(dma->channels[ch].CSR & 0x100)) {  // already pending? do nothing

            dma->channels[ch].CSR |= 0x100;  // req pend
        }
    }
}

void pxaDmaSetPeripheral(struct PxaDma* dma, uint_fast8_t chNum,
                         const struct PxaDmaPeripheral* peripheral) {
    if (chNum >= sizeof(dma->CMR)) return;
    if (peripheral)
        dma->peripherals[chNum] = *peripheral;
    else
        dma->peripherals[chNum] = PxaDmaPeripheral{};
}

// PXA250 5.1.2.1: internal PREQs are level sensitive. Query-backed lines
// can assert while unmapped; sampling a new mapping must not lose that level.
static void pxaDmaPrvRefreshRequest(PxaDma* dma, unsigned line) {
    const auto& p = dma->peripherals[line];
    if (p.requested) pxaDmaExternalReq(dma, line, p.requested(p.ctx));
}

void pxaDmaPeriodic(struct PxaDma* dma) {
    uint32_t i;

    for (i = 0; i < 32; i++) pxaDmaPrvChannelActIfNeeded(dma, i);
}

static bool pxaDmaPrvMemAccessF(void* userData, uint32_t pa, uint_fast8_t size, bool write,
                                void* buf) {
    struct PxaDma* dma = (struct PxaDma*)userData;
    uint_fast8_t reg, set;
    uint32_t val = 0;

    if (size != 4) {
        fprintf(stderr, "%s: Unexpected %s of %u bytes to 0x%08lx\n", __func__,
                write ? "write" : "read", size, (unsigned long)pa);
        return false;
    }

    pa = (pa - PXA_DMA_BASE) >> 2;

    if (write) {
        val = *(uint32_t*)buf;

        switch (pa >> 6) {  // weird, but quick way to avoid repeated if-then-elses. this is faster
            case 0:
                if (pa == 0x28) {
                    dma->dalgn = val;
                    break;
                } else if (pa == 0x28) {
                    dma->dpcsr = val;
                    break;
                } else if (pa == 0x3c) {
                    // no part of DINT is writeable but it MUST accept writes
                    break;
                }

                if (pa >= 32) return false;

                reg = REG_CSR;
                set = pa;
                if (!pxaDmaPrvChannelRegWrite(dma, set, reg, val)) return false;
                break;

            case 17:  // DRCMR64-74 at 0x1100: on to CMR[64] after case 1 takes 64 off
                pa -= 1088;
                pa += 128;
                // fallthrough
            case 1:
                pa -= 64;
                if (pa >= 75) return false;
                if (dma->peripherals[pa].requested && (dma->CMR[pa] & 0x80))
                    pxaDmaExternalReq(dma, pa, false);
                dma->CMR[pa] = val;
                pxaDmaPrvRefreshRequest(dma, pa);
                break;

            case 2:
            case 3:
                pa -= 128;
                set = pa >> 2;
                reg = pa & 3;
                if (set >= 32) return false;
                if (!pxaDmaPrvChannelRegWrite(dma, set, reg, val)) return false;
                break;

            default:
                // The rest of the 0x1000 page is reserved and takes writes without a fault;
                // the Zire 72 camera driver writes DRCMR68 there by mistake.
                if ((pa >> 6) >= 16 && (pa >> 6) < 32) break;
                return false;
        }
    } else {
        switch (pa >> 6) {  // weird, but quick way to avoide repeated if-then-elses. this is faster
            case 0:
                if (pa == 0x28) {
                    val = dma->dalgn;
                    break;
                } else if (pa == 0x28) {
                    val = dma->dpcsr;
                    break;
                } else if (pa == 0x3c) {
                    val = dma->DINT;
                    break;
                }

                if (pa >= 32) return false;

                reg = REG_CSR;
                set = pa;
                if (!pxaDmaPrvChannelRegRead(dma, set, reg, &val)) return false;
                break;

            case 17:  // DRCMR64-74 at 0x1100: on to CMR[64] after case 1 takes 64 off
                pa -= 1088;
                pa += 128;
                // fallthrough
            case 1:
                pa -= 64;
                if (pa >= 75) return false;

                val = dma->CMR[pa];
                break;

            case 2:
            case 3:
                pa -= 128;
                set = pa >> 2;
                reg = pa & 3;
                if (set >= 32) return false;
                if (!pxaDmaPrvChannelRegRead(dma, set, reg, &val)) return false;
                break;

            default:
                if ((pa >> 6) >= 16 && (pa >> 6) < 32) break;  // reserved, reads 0
                return false;
        }

        *(uint32_t*)buf = val;
    }

    return true;
}

struct PxaDma* pxaDmaInit(struct ArmMem* physMem, struct Reschedule reschedule, struct PxaIc* ic) {
    struct PxaDma* dma = (struct PxaDma*)malloc(sizeof(*dma));
    uint_fast8_t i;

    if (!dma) ERR("cannot alloc DMA");

    memset(dma, 0, sizeof(*dma));
    dma->ic = ic;
    dma->reschedule = reschedule;
    dma->mem = physMem;

    for (i = 0; i < 32; i++) dma->channels[i].CSR = 8;  // stopped or uninitialized

    if (!memRegionAdd(physMem, PXA_DMA_BASE, PXA_DMA_SIZE, pxaDmaPrvMemAccessF, dma))
        ERR("cannot add DMA to MEM\n");

    return dma;
}

bool pxaDmaTaskRequired(struct PxaDma* dma) {
    for (int i = 0; i < 32; i++) {
        if (pxaDmaPrvChannelRunning(dma, dma->channels + i)) return true;
    }

    return false;
}

template <typename T>
void pxaDmaSave(PxaDma* dma, T& savestate) {
    auto chunk = savestate.GetChunk(ChunkType::pxaDma, SAVESTATE_VERSION);
    if (!chunk) ERR("unable to allocate chunk");

    SaveChunkHelper helper(*chunk);
    dma->DoSaveLoad(helper);
}

template <typename T>
void pxaDmaLoad(PxaDma* dma, T& loader) {
    auto chunk = loader.GetChunkOrFail(ChunkType::pxaDma, SAVESTATE_VERSION, "pxaDma");
    if (!chunk) return;

    LoadChunkHelper helper(*chunk);
    dma->DoSaveLoad(helper);
}

template void pxaDmaSave<Savestate<ChunkType>>(PxaDma* dma, Savestate<ChunkType>& savestate);
template void pxaDmaSave<SavestateProbe<ChunkType>>(PxaDma* dma,
                                                    SavestateProbe<ChunkType>& savestate);
template void pxaDmaLoad<SavestateLoader<ChunkType>>(PxaDma* dma,
                                                     SavestateLoader<ChunkType>& loader);
