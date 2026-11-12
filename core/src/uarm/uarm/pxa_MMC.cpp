//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "pxa_MMC.h"

#include <cstdlib>
#include <cstring>

#include "cputil.h"
#include "mem.h"
#include "pxa_DMA.h"
#include "savestate/savestateAll.h"
#include "sd_log_limit.h"

#define PXA_MMC_BASE 0x41100000UL
#define PXA_MMC_SIZE 0x00001000UL

#define SAVESTATE_VERSION 1

// MMC_I_REG / MMC_I_MASK bits above the PXA25x ones (PXA27x manual 15.8, Linux pxamci.h)
#define I_DAT_ERR 0x0100
#define I_RES_ERR 0x0200
#define I_MASK_PXA27X 0x037f

struct PxaMmc {
    SdLogLimit noReplyLog;  // not saved

    struct PxaDma *dma;
    struct PxaIc *ic;

    uint32_t arg;
    uint16_t stat, readTo, blkLen, numBlks, iMask, iReg;
    uint8_t spi, cmdat, clockSpeed, resTo, cmdReg;
    uint16_t respBuf[8];
    bool clockOn, cmdQueued, dataXferOngoing;

    struct VSD *vsd;
    bool wordFifo;  // PXA27x transfers 1/2/4 bytes per FIFO access; PXA25x uses the low byte.

    // fifo
    uint32_t fifoBytes, fifoOfst;
    uint8_t blockFifo[1024];

    template <typename T>
    void DoSaveLoad(T &chunkHelper, uint32_t version) {
        uint8_t iMaskLo = iMask, iRegLo = iReg, iMaskHi = 0, iRegHi = 0;

        chunkHelper.Do32(arg)
            .Do(typename T::Pack16() << stat << readTo)
            .Do(typename T::Pack16() << blkLen << numBlks)
            .Do(typename T::Pack8() << spi << iMaskLo << iRegLo << cmdat)
            .Do(typename T::Pack8() << clockSpeed << resTo << cmdReg)
            .DoBuffer16(respBuf, sizeof(respBuf) >> 1)
            .Do(typename T::BoolPack() << clockOn << cmdQueued << dataXferOngoing)
            .Do32(fifoBytes)
            .Do32(fifoOfst)
            .DoBuffer(blockFifo, sizeof(blockFifo));
        if (version >= 1) {
            iMaskHi = iMask >> 8;
            iRegHi = iReg >> 8;
            chunkHelper.Do(typename T::Pack8() << iMaskHi << iRegHi);
        } else {
            iMaskHi = wordFifo ? I_MASK_PXA27X >> 8 : 0;  // older states had the new bits masked
            iRegHi = 0;
        }
        iMask = (uint16_t)iMaskHi << 8 | iMaskLo;
        iReg = (uint16_t)iRegHi << 8 | iRegLo;
    }
};

static void pxaMmcPrvIrqUpdate(struct PxaMmc *mmc) {
    uint32_t irqs = mmc->iReg;

    // mask as per mask
    irqs &= ~mmc->iMask;

    // dma masks fifo irqs
    if (mmc->cmdat & 0x80) irqs &= ~0x60;

    pxaIcInt(mmc->ic, PXA_I_MMC, !!irqs);
}

static void pxaMmcPrvRecalcIregAndFifo(struct PxaMmc *mmc) {
    if (mmc->stat & 0x100)
        mmc->iReg &= ~0x10;
    else
        mmc->iReg |= 0x10;

    mmc->iReg &= ~7;
    mmc->iReg |= (mmc->stat >> 11) & 7;

    if (mmc->wordFifo) {  // PXA27x only: data (read TO, write CRC, read CRC) and response (TO, CRC) errors
        mmc->iReg &= ~(I_DAT_ERR | I_RES_ERR);
        if (mmc->stat & 0x000d) mmc->iReg |= I_DAT_ERR;
        if (mmc->stat & 0x0022) mmc->iReg |= I_RES_ERR;
    }

    // FIFO requests start with the command's transfer: TX while blocks are left to send, RX
    // when the FIFO holds data (the RX bits stay in I_REG until it is read). A DMA channel
    // started before the command (Linux pxamci) must not move a burst while the clock is off.
    // After a failed command (stat 0x02) no transfer starts, so TX is still requested while
    // the clock runs and the DMA drains into dropped writes.
    if ((mmc->cmdat & 4) && (mmc->cmdat & 8) && mmc->numBlks &&
        (mmc->dataXferOngoing || (mmc->clockOn && (mmc->stat & 0x02)))) {
        mmc->stat |= 0x40;
        mmc->iReg |= 0x40;
    }

    if ((mmc->cmdat & 4) && !(mmc->cmdat & 8) && mmc->fifoBytes) {
        mmc->stat |= 0x80;
        mmc->iReg |= 0x20;
    }

    if (mmc->cmdat & 0x80) {
        pxaDmaExternalReq(mmc->dma, DMA_CMR_MMC_RX, !!(mmc->iReg & 0x20));
        pxaDmaExternalReq(mmc->dma, DMA_CMR_MMC_TX, !!(mmc->iReg & 0x40));
    }

    pxaMmcPrvIrqUpdate(mmc);
}

// A failed block ends the transfer with DATA_TRAN_DONE and the error in MMC_STAT, as the
// PXA27x does for timed-out or rejected data (manual 15.5). A failed write is not left busy.
static void pxaMmcPrvDataXferFail(struct PxaMmc *mmc, uint16_t error) {
    mmc->stat |= error | 0x0800;
    if (mmc->cmdat & 8) mmc->stat |= 0x1000;
    mmc->dataXferOngoing = false;
    mmc->numBlks = 0;
    mmc->fifoBytes = mmc->fifoOfst = 0;
}

static void pxaMmcPrvDataXferNextBlockRead(struct PxaMmc *mmc) {
    enum SdDataReplyType ret;

    if (!mmc->dataXferOngoing) {
        fprintf(stderr, "Cannot read block if no xfer ongoing\n");
        return;
    }

    ret = vsdDataXferBlockFromCard(mmc->vsd, mmc->blockFifo, mmc->blkLen);
    switch (ret) {
        case SdDataErrWrongBlockSize:  // would manifest as a crc error
            pxaMmcPrvDataXferFail(mmc, 0x0008);  // crc read error
            break;

        case SdDataErrWrongCurrentState:  // would manifest as a timeout
        case SdDataErrBackingStore:
        default:
            pxaMmcPrvDataXferFail(mmc, 0x0001);  // read data timeout
            break;

        case SdDataOk:
            mmc->fifoBytes = mmc->blkLen;
            mmc->fifoOfst = 0;
            mmc->numBlks--;
            break;
    }
}

static void pxaMmcPrvDataXferStart(struct PxaMmc *mmc) {
    mmc->dataXferOngoing = true;

    if (!mmc->blkLen || !mmc->numBlks) {
        fprintf(stderr, "MMC xfer cannot start with zero length (%u B x %u)\n", mmc->blkLen,
                mmc->numBlks);
        return;
    }

    if (mmc->cmdat & 8) {  // write

        mmc->fifoBytes = 0;
    } else {  // read

        pxaMmcPrvDataXferNextBlockRead(mmc);
    }
}

static bool pxaMmcPrvDataFifoW(struct PxaMmc *mmc, uint32_t val) {
    // Bytes past BLKLEN are not transmitted (manual 15.8.4); the bus access still completes.
    if (!(mmc->cmdat & 8) || mmc->fifoBytes >= mmc->blkLen ||
        mmc->fifoBytes >= sizeof(mmc->blockFifo))
        return true;

    mmc->blockFifo[mmc->fifoBytes++] = val;

    if (mmc->fifoBytes == mmc->blkLen) {
        enum SdDataReplyType ret;

        if (!mmc->dataXferOngoing) return true;

        ret = vsdDataXferBlockToCard(mmc->vsd, mmc->blockFifo, mmc->blkLen);
        switch (ret) {
            case SdDataErrWrongBlockSize:     // would manifest as a crc error
            case SdDataErrWrongCurrentState:  // would manifest as a timeout but we report all as
                                              // crc errors
            case SdDataErrBackingStore:
            default:
                pxaMmcPrvDataXferFail(mmc, 0x0004);  // crc write error
                break;

            case SdDataOk:
                mmc->fifoBytes = 0;
                mmc->numBlks--;

                if (!mmc->numBlks) {
                    mmc->stat |= 0x0800;  // data xfer done
                    mmc->stat |= 0x1000;  // not busy
                    mmc->dataXferOngoing = false;
                }
                break;
        }
    }

    pxaMmcPrvRecalcIregAndFifo(mmc);

    return true;
}

static bool pxaMmcPrvDataFifoR(struct PxaMmc *mmc, uint32_t *valP) {
    // An empty RX FIFO reads as zero; the bus access still completes.
    if ((mmc->cmdat & 8) || !mmc->fifoBytes || mmc->fifoOfst >= sizeof(mmc->blockFifo) ||
        mmc->fifoBytes > sizeof(mmc->blockFifo) - mmc->fifoOfst) {
        *valP = 0;
        return true;
    }

    *valP = mmc->blockFifo[mmc->fifoOfst++];
    mmc->fifoBytes--;

    if (!mmc->fifoBytes) {
        if (mmc->numBlks)
            pxaMmcPrvDataXferNextBlockRead(mmc);
        else {
            mmc->stat |= 0x0800;  // data xfer done
            mmc->dataXferOngoing = false;
        }
    }

    pxaMmcPrvRecalcIregAndFifo(mmc);
    return true;
}

static bool mmcSendCommand(struct PxaMmc *mmc) {
    bool success = false, doCrcCheck = true, withBusy = false, crcFail = false;
    enum SdReplyType ret;
    uint8_t reply[17];
    unsigned i, len;

    mmc->cmdQueued = false;

    if (mmc->vsd) {
        //	fprintf(stderr, "sending cmd %u (0x%08x)\n", mmc->cmdReg & 0x3f, mmc->arg);
        ret = vsdCommand(mmc->vsd, mmc->cmdReg & 0x3f, mmc->arg, reply);
    } else {
        //	fprintf(stderr, "MMC unit has no SD card - command ignored\n");
        ret = SdReplyNone;
    }

    switch (ret) {
        case SdReplyNone:
            len = 0;
            if ((mmc->cmdat & 3) == 0)  // no reply as expected
                success = true;
            else if (sdLogAllow(&mmc->noReplyLog, "MMC", "missing replies"))
                fprintf(stderr, "Got no reply, when %u bits were expected\n",
                        ((mmc->cmdat & 3) == 2) ? 136 : 48);
            break;

        case SdReply48bitsAndBusy:
            withBusy = true;
            // fallthrough

        case SdReply48bits:
            len = 5;
            if (mmc->cmdat & 1) {  // 48-bit reply as expected
                success = true;

                doCrcCheck = !(mmc->cmdat & 2);
            } else
                fprintf(stderr, "Got a 48-bit reply with, when %u bits were expected\n",
                        (mmc->cmdat & 3) ? 136 : 0);
            break;

        case SdReply136bits:
            len = 16;
            if ((mmc->cmdat & 3) == 2)  // 136-bit reply as expected
                success = true;
            else
                fprintf(stderr, "Got a 136-bit reply reply, when %u bits were expected\n",
                        (mmc->cmdat & 3) ? 48 : 0);
            break;
    }

    (void)withBusy;  // our card is never busy

    // response to handle?

    if (success && len && doCrcCheck) crcFail = vsdCRC7(reply, len) != reply[len];

    mmc->stat |= 0x2000;  // cmd-resp over
    if (crcFail) mmc->stat |= 0x20;

    if (!success)
        mmc->stat |= 0x02;  // resp timed out if wrong type
    else {
        reply[len] = 0;
        for (i = 0; i < len; i += 2)
            mmc->respBuf[i / 2] = (((uint32_t)reply[i]) << 8) + reply[i + 1];
    }

    // data to handle?
    if (success && (mmc->cmdat & 4)) pxaMmcPrvDataXferStart(mmc);

    // all cases except write, we're not busy by response time
    if (!success || !(mmc->cmdat & 4) || !(mmc->cmdat & 8)) mmc->stat |= 0x1000;  // not busy

    pxaMmcPrvRecalcIregAndFifo(mmc);

    return true;
}

static bool pxaMmcPrvMemAccessF(void *userData, uint32_t pa, uint_fast8_t size, bool write,
                                void *buf) {
    struct PxaMmc *mmc = (struct PxaMmc *)userData;
    uint32_t val = 0;
    bool ret = true;

    const bool packedFifo = mmc->wordFifo && (pa == PXA_MMC_BASE + 0x40 || pa == PXA_MMC_BASE + 0x44);
    if (size != 4 && !(packedFifo && (size == 1 || size == 2))) {
        fprintf(stderr, "%s: Unexpected %s of %u bytes to 0x%08lx\n", __func__,
                write ? "write" : "read", size, (unsigned long)pa);
        return false;
    }

    pa = (pa - PXA_MMC_BASE) >> 2;

    // if (write)
    //	fprintf(stderr, "MMC 0x%08x->[0x%08x]\n", *(uint32_t*)buf, (unsigned)(PXA_MMC_BASE + 4 *
    // pa));

    if (write) memcpy(&val, buf, size);

    switch (pa) {
        case 0:
            if (!write) return false;
            switch (val & 3) {
                case 0:  // do nothing
                    break;
                case 1:  // stop clock
                    //				fprintf(stderr, "MMC: clock off\n");
                    mmc->clockOn = false;
                    mmc->stat &= ~0x100;
                    pxaMmcPrvRecalcIregAndFifo(mmc);
                    break;
                case 2:  // start clock
                    //				fprintf(stderr, "MMC: clock on\n");
                    mmc->clockOn = true;
                    mmc->stat |= 0x100;
                    pxaMmcPrvRecalcIregAndFifo(mmc);

                    if (mmc->cmdQueued) ret = mmcSendCommand(mmc);
                    break;
                default:
                    ret = false;
                    break;
            }
            pxaMmcPrvIrqUpdate(mmc);
            break;

        case 1:
            if (write) {
            }  // ignored
            else
                val = mmc->stat;
            break;

        case 2:
            if (write) {
                val &= 7;
                if (val == 7) {
                    ret = false;
                    break;
                }
                //			if (mmc->clockSpeed != val)
                //				fprintf(stderr, "MMC clock set to %u khz\n", 20000
                //>> val);
                mmc->clockSpeed = val;
            } else
                val = mmc->clockSpeed;
            break;

        case 3:
            if (write)
                mmc->spi = val & 0x0f;
            else
                val = mmc->spi;
            break;

        case 4:
            if (write) {
                mmc->cmdat = val;
                mmc->stat &= 0x100;
                mmc->stat |= 0x40;
                pxaMmcPrvRecalcIregAndFifo(mmc);

                if (mmc->clockOn)
                    ret = mmcSendCommand(mmc);
                else
                    mmc->cmdQueued = true;
            } else
                val = mmc->cmdat;
            break;

        case 5:
            if (write)
                mmc->resTo = val & 0x7f;
            else
                val = mmc->resTo;
            break;

        case 6:
            if (write)
                mmc->readTo = val;
            else
                val = mmc->readTo;
            break;

        case 7:
            if (write)
                mmc->blkLen = val & 0x3ff;
            else
                val = mmc->blkLen;
            break;

        case 8:
            if (write)
                mmc->numBlks = val;
            else
                val = mmc->numBlks;
            break;

        case 9:  // MMC_PRTBUF: partial TX bank swap, automatically cleared.
            // This model stages complete blocks and accepts each FIFO byte immediately;
            // bank swapping never stalls. Zero initializes the register without a swap.
            if (!write) val = 0;
            break;

        case 10:
            if (write) {
                mmc->iMask = val & (mmc->wordFifo ? I_MASK_PXA27X : 0x7f);
                pxaMmcPrvIrqUpdate(mmc);
            } else
                val = mmc->iMask;
            break;

        case 11:
            if (write) {
                // nothing
            } else {
                val = mmc->iReg;

                mmc->iReg &= ~0x60;
                pxaMmcPrvRecalcIregAndFifo(mmc);
            }
            break;

        case 12:
            if (write)
                mmc->cmdReg = 0x40 | (val & 0x3f);
            else
                val = mmc->cmdReg;
            break;

        case 13:
            if (write)
                mmc->arg = (mmc->arg & 0x0000fffful) | (val << 16);
            else
                val = mmc->arg >> 16;
            break;

        case 14:
            if (write)
                mmc->arg = (mmc->arg & 0xffff0000ul) | (val & 0xffff);
            else
                val = mmc->arg & 0xffff;
            break;

        case 15:  // rep buf
            if (write)
                ret = false;
            else {
                val = mmc->respBuf[0];
                memmove(mmc->respBuf, mmc->respBuf + 1,
                        sizeof(mmc->respBuf) - sizeof(*mmc->respBuf));
            }
            break;

        case 16:
            if (write)
                ret = false;
            else {
                const unsigned count = mmc->wordFifo ? size : 1;
                for (unsigned i = 0; i < count && mmc->fifoBytes; i++) {
                    uint32_t byte = 0;
                    ret = pxaMmcPrvDataFifoR(mmc, &byte) && ret;
                    val |= byte << (8 * i);
                }
            }
            break;

        case 17:
            if (write) {
                // Writes outside a write transfer are dropped without a bus error.
                const unsigned count = mmc->wordFifo ? size : 1;
                for (unsigned i = 0; i < count && mmc->dataXferOngoing; i++)
                    ret = pxaMmcPrvDataFifoW(mmc, val >> (8 * i)) && ret;
            } else
                ret = false;
            break;

        default:
            ret = false;
            break;
    }

    if (ret) {
        if (!write) memcpy(buf, &val, size);

        //	if (!write)
        //		fprintf(stderr, "MMC [0x%08x] -> 0x%08x\n", (unsigned)(PXA_MMC_BASE + 4 *
        // pa), val);
    }

    return ret;
}

struct PxaMmc *pxaMmcInit(struct ArmMem *physMem, struct PxaIc *ic, struct PxaDma *dma, bool wordFifo) {
    struct PxaMmc *mmc = (struct PxaMmc *)malloc(sizeof(*mmc));

    if (!mmc) ERR("cannot alloc MMC");

    memset(mmc, 0, sizeof(*mmc));

    mmc->wordFifo = wordFifo;
    mmc->ic = ic;
    mmc->dma = dma;
    mmc->iMask = wordFifo ? I_MASK_PXA27X : 0x7f;
    mmc->cmdat = 0x80;
    mmc->resTo = 0x40;
    mmc->cmdReg = 0x40;
    mmc->stat = 0x40;
    pxaMmcPrvRecalcIregAndFifo(mmc);

    if (!memRegionAdd(physMem, PXA_MMC_BASE, PXA_MMC_SIZE, pxaMmcPrvMemAccessF, mmc))
        ERR("cannot add MMC to MEM\n");

    return mmc;
}

void pxaMmcInsert(struct PxaMmc *mmc, struct VSD *vsd) {
    sdLogReset(&mmc->noReplyLog);
    if (!vsd) {
        // Removal ends the old card's transfer. Never drain its FIFO into a
        // later block request through a null card, or expose it after reinsert.
        // An unfinished block gets no CRC token or data, so it ends as a write CRC
        // error or read timeout with DATA_TRAN_DONE (manual 15.5).
        if (mmc->dataXferOngoing) mmc->stat |= ((mmc->cmdat & 8) ? 4 : 1) | 0x0800;
        if (mmc->cmdQueued) mmc->stat |= 0x2002;
        mmc->dataXferOngoing = mmc->cmdQueued = false;
        mmc->fifoBytes = mmc->fifoOfst = mmc->numBlks = 0;
        mmc->stat = (mmc->stat & ~0xc0) | 0x1000;
        mmc->iReg &= ~0x60;
        pxaDmaExternalReq(mmc->dma, DMA_CMR_MMC_RX, false);
        pxaDmaExternalReq(mmc->dma, DMA_CMR_MMC_TX, false);
        pxaMmcPrvRecalcIregAndFifo(mmc);
    }
    // Reconnecting restored state is not a new transfer or physical card edge.
    mmc->vsd = vsd;
}

template <typename T>
void pxaMmcSave(struct PxaMmc *mmc, T &savestate) {
    auto chunk = savestate.GetChunk(ChunkType::pxaMmc, SAVESTATE_VERSION);
    if (!chunk) ERR("unable to allocate chunk");

    SaveChunkHelper helper(*chunk);
    mmc->DoSaveLoad(helper, SAVESTATE_VERSION);
}

template <typename T>
void pxaMmcLoad(struct PxaMmc *mmc, T &loader) {
    uint32_t version;
    auto chunk = loader.GetChunkOrFail(ChunkType::pxaMmc, SAVESTATE_VERSION, "pxa mmc", version);
    if (!chunk) return;

    LoadChunkHelper helper(*chunk);
    mmc->DoSaveLoad(helper, version);
    if (version < 1 && mmc->wordFifo) {  // error bits follow MMC_STAT; the IRQ line is left alone
        if (mmc->stat & 0x000d) mmc->iReg |= I_DAT_ERR;
        if (mmc->stat & 0x0022) mmc->iReg |= I_RES_ERR;
    }
    if (mmc->blkLen > 0x3ff || mmc->fifoBytes > sizeof(mmc->blockFifo) ||
        mmc->fifoOfst > sizeof(mmc->blockFifo) ||
        (!(mmc->cmdat & 8) && mmc->fifoBytes > sizeof(mmc->blockFifo) - mmc->fifoOfst))
        loader.NotifyError();
}

template void pxaMmcSave<Savestate<ChunkType>>(PxaMmc *mmc, Savestate<ChunkType> &savestate);
template void pxaMmcSave<SavestateProbe<ChunkType>>(PxaMmc *mmc,
                                                    SavestateProbe<ChunkType> &savestate);
template void pxaMmcLoad<SavestateLoader<ChunkType>>(PxaMmc *mmc,
                                                     SavestateLoader<ChunkType> &loader);
