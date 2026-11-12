//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include "nand.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "CPU.h"
#include "CardBacking.h"
#include "cputil.h"
#include "mem.h"
#include "memory_buffer.h"
#include "savestate/savestateAll.h"

#define SAVESTATE_VERSION 0

enum K9nandState : uint8_t {
    K9nandStateReset,
    K9nandStateReadId,
    K9nandStateProgramAddrRxing,
    K9nandStateProgramDataRxing,
    K9nandStateEraseAddrRxing,
    K9nandStateReading,
    K9nandStateStatusReading,
};

enum K9nandArea : uint8_t {  // K9nandAreaB, K9nandAreaC only used if
                             // flags.NAND_FLAG_SAMSUNG_ADDRESSED_VIA_AREAS is set
    K9nandAreaA,
    K9nandAreaB,
    K9nandAreaC,
};

struct NAND {
    NandReadyCbk readyCbk[2];
    void *readyCbkData[2];

    // geometry and id
    uint32_t bytesPerPage;  // main plus spare
    uint32_t blocksPerDevice;
    uint16_t areaSize;
    uint8_t pagesPerBlockLg2;  // log base 2 (eg: if device has 32 pages per block, this will be 5)
    uint8_t deviceId[6];
    uint8_t deviceIdLen;
    uint8_t byteAddrBytes;
    uint8_t pageAddrBytes;
    uint8_t flags;

    // runtime state
    enum K9nandState state;
    enum K9nandArea area;
    uint8_t addrBytesRxed;
    uint8_t addrBytes[8];
    uint32_t pageNo;    // read & program only
    uint32_t pageOfst;  // for read ops only
    uint32_t busyCt;
    struct MemoryBuffer pageBuf;

    // data: raw pages (main then spare) in `data`, or in `backing` once a file holds them
    size_t dataSize;
    uint8_t *data;
    void (*releaseData)(void *);  // frees `data` when a backing takes over; null leaves it to its owner

    // A backing holds the main bytes of every page at page * mainBytes and the spare bytes at
    // sideOffset + page * spareBytes (nand.frondnand's stored bytes).
    uint32_t mainBytes;
    uint32_t spareBytes;
    std::unique_ptr<CardBacking> backing;
    uint64_t sideOffset;
    std::vector<uint8_t> rawView;  // nandGetData's copy of a backed NAND, made when asked for
    bool rawViewValid;

    size_t dirtyPagesSize;
    uint32_t *dirtyPages;
    bool dirty;

    uint32_t writeCnt;

    // A program or erase the backing couldn't store: the status read reports it failed, as a
    // worn chip would, and the core carries on. Not saved, like the rest of the status.
    bool lastOpFailed;
    uint32_t storeFailures;

    struct Reschedule reschedule;

    template <typename T>
    void DoSaveLoad(T &chunkHelper) {
        uint8_t state8 = static_cast<uint8_t>(state);
        uint8_t area8 = static_cast<uint8_t>(area);

        chunkHelper.Do(typename T::Pack8() << state8 << area8 << addrBytesRxed)
            .DoBuffer(addrBytes, sizeof(addrBytes))
            .Do32(pageNo)
            .Do32(pageOfst)
            .Do32(busyCt);

        state = static_cast<K9nandState>(state8);
        area = static_cast<K9nandArea>(area8);
    }
};

void nandSecondReadyCbkSet(struct NAND *nand, NandReadyCbk readyCbk, void *readyCbkData) {
    nand->readyCbk[1] = readyCbk;
    nand->readyCbkData[1] = readyCbkData;
}

static void nandPrvCallReadyCbks(struct NAND *nand, bool ready) {
    uint_fast8_t i;

    for (i = 0; i < sizeof(nand->readyCbk) / sizeof(*nand->readyCbk); i++) {
        if (nand->readyCbk[i]) nand->readyCbk[i](nand->readyCbkData[i], ready);
    }
}

static void nandPrvBusy(struct NAND *nand, uint32_t count) {
    if (nand->busyCt)
        ERR("NAND was already busy\n");
    else {
        nand->busyCt = count;
        nandPrvCallReadyCbks(nand, false);

        nand->reschedule.rescheduleCb(nand->reschedule.ctx, RESCHEDULE_TASK_NAND);
    }
}

static uint32_t nandPrvGetAddrValLE(struct NAND *nand, uint_fast8_t startByte,
                                    uint_fast8_t numBytes) {
    uint32_t ret = 0;
    uint_fast8_t i;

    for (i = 0; i < numBytes; i++) {
        ret <<= 8;
        ret += nand->addrBytes[startByte + numBytes - 1 - i];
    }

    return ret;
}

static uint32_t nandPrvGetPageAddr(struct NAND *nand, uint_fast8_t addrOfst) {
    return nandPrvGetAddrValLE(nand, addrOfst, nand->pageAddrBytes);
}

static uint32_t nandPrvGetByteAddr(struct NAND *nand) {
    return nandPrvGetAddrValLE(nand, 0, nand->byteAddrBytes);
}

static uint32_t nandPrvPageCount(const struct NAND *nand) {
    return nand->blocksPerDevice << nand->pagesPerBlockLg2;
}

// One raw page, main then spare. A NAND with neither data nor backing reads erased.
static bool nandPrvReadPage(struct NAND *nand, uint32_t page, uint8_t *dest) {
    if (nand->backing)
        return nand->backing->Read(uint64_t(page) * nand->mainBytes, nand->mainBytes, dest) &&
               nand->backing->Read(nand->sideOffset + uint64_t(page) * nand->spareBytes, nand->spareBytes,
                                   dest + nand->mainBytes);
    if (nand->data)
        memcpy(dest, nand->data + size_t(page) * nand->bytesPerPage, nand->bytesPerPage);
    else
        memset(dest, 0xff, nand->bytesPerPage);
    return true;
}

static bool nandPrvWritePage(struct NAND *nand, uint32_t page, const uint8_t *src) {
    nand->rawViewValid = false;
    if (nand->backing)
        return nand->backing->Write(uint64_t(page) * nand->mainBytes, nand->mainBytes, src) &&
               nand->backing->Write(nand->sideOffset + uint64_t(page) * nand->spareBytes, nand->spareBytes,
                                    src + nand->mainBytes);
    if (!nand->data) return false;
    memcpy(nand->data + size_t(page) * nand->bytesPerPage, src, nand->bytesPerPage);
    return true;
}

static bool nandPrvEraseBlock(struct NAND *nand, uint32_t firstPage) {
    const uint32_t pages = 1u << nand->pagesPerBlockLg2;
    nand->rawViewValid = false;
    if (nand->backing) {
        std::vector<uint8_t> ones(size_t(pages) * nand->mainBytes, 0xff);
        return nand->backing->Write(uint64_t(firstPage) * nand->mainBytes, uint64_t(pages) * nand->mainBytes,
                                    ones.data()) &&
               nand->backing->Write(nand->sideOffset + uint64_t(firstPage) * nand->spareBytes,
                                    uint64_t(pages) * nand->spareBytes, ones.data());
    }
    if (!nand->data) return false;
    memset(nand->data + size_t(firstPage) * nand->bytesPerPage, 0xff, size_t(nand->bytesPerPage) << nand->pagesPerBlockLg2);
    return true;
}

// The backing refused a write (disk full, folder read-only). Logs the first one of a run.
static void nandPrvStoreFailed(struct NAND *nand) {
    nand->lastOpFailed = true;
    if (!nand->storeFailures++)
        fprintf(stderr, "NAND: a write to the machine's NAND file failed (disk full or folder "
                        "read-only?); Palm OS sees the write fail\n");
}

static bool nandPrvBlockErase(struct NAND *nand) {
    uint32_t addr = nandPrvGetPageAddr(nand, 0);

    if (addr & ((1UL << nand->pagesPerBlockLg2) - 1)) return false;

    if (addr >= nandPrvPageCount(nand)) return false;
    if (!nandPrvEraseBlock(nand, addr)) {
        nandPrvStoreFailed(nand);
        return true;
    }

    for (size_t storagePage = (addr * nand->bytesPerPage) / 4224;
         storagePage <=
         (addr * nand->bytesPerPage + (nand->bytesPerPage << nand->pagesPerBlockLg2)) / 4224;
         storagePage++) {
        nand->dirtyPages[storagePage >> 5] |= (1u << (storagePage & 0x1f));
    }

    nand->dirty = true;
    nand->writeCnt++;
    return true;
}

static bool nandPrvPageProgram(struct NAND *nand) {
    uint32_t i;

    // fprintf(stderr, " NAND PROGRAM PAGE %u (addr %u.%u)\n", nand->pageNo, nand->pageNo /
    // NAND_PAGES_PER_BLOCK, nand->pageNo % NAND_PAGES_PER_BLOCK);

    // Programming only clears bits, as on the chip.
    uint8_t page[6 * 528];
    if (nand->bytesPerPage > sizeof(page)) return false;
    if (!nandPrvReadPage(nand, nand->pageNo, page)) {
        nandPrvStoreFailed(nand);
        return true;
    }
    for (i = 0; i < nand->bytesPerPage; i++) page[i] &= nand->pageBuf.buffer[i];
    if (!nandPrvWritePage(nand, nand->pageNo, page)) {
        nandPrvStoreFailed(nand);
        return true;
    }

    size_t storagePage = (nand->pageNo * nand->bytesPerPage) / 4224;
    nand->dirtyPages[storagePage >> 5] |= (1u << (storagePage & 0x1f));
    nand->dirty = true;
    nand->writeCnt++;

    return true;
}

static bool nandPrvAcceptProgramData(struct NAND *nand, uint8_t val) {
    if (nand->pageOfst >= nand->bytesPerPage) {
        // real hardware ignored extra bytes being written, and so do we
        return true;
    }

    MEMORY_BUFFER_MARK_DIRTY(nand->pageBuf, nand->pageOfst);
    nand->pageBuf.buffer[nand->pageOfst++] = val;

    return true;
}

bool nandWrite(struct NAND *nand, bool cle, bool ale, uint8_t val) {
    if (cle && !ale) {  // command

        switch (val) {
            case 0x00:
                nand->area = K9nandAreaA;
                nand->state = K9nandStateReading;
                nand->addrBytesRxed = 0;
                break;

            case 0x01:
                if (nand->flags & NAND_FLAG_SAMSUNG_ADDRESSED_VIA_AREAS) {
                    nand->area = K9nandAreaB;
                    nand->state = K9nandStateReading;
                    nand->addrBytesRxed = 0;
                } else
                    return false;
                break;

            case 0x30:
                if ((nand->flags & NAND_HAS_SECOND_READ_CMD) &&
                    nand->addrBytesRxed == nand->byteAddrBytes + nand->pageAddrBytes)
                    nand->addrBytesRxed = 0xfe;  // special value
                else
                    return false;
                break;

            case 0x50:
                if (nand->flags & NAND_FLAG_SAMSUNG_ADDRESSED_VIA_AREAS) {
                    nand->area = K9nandAreaC;
                    nand->state = K9nandStateReading;
                    nand->addrBytesRxed = 0;
                } else
                    return false;
                break;

            case 0x60:
                if (nand->state != K9nandStateReset && nand->state != K9nandStateReading &&
                    nand->state != K9nandStateReadId && nand->state != K9nandStateStatusReading)
                    return false;
                nand->state = K9nandStateEraseAddrRxing;
                nand->addrBytesRxed = 0;
                nand->lastOpFailed = false;
                break;

            case 0xd0:
                if (nand->area == K9nandAreaB) nand->area = K9nandAreaA;
                if (nand->state != K9nandStateEraseAddrRxing || nand->addrBytesRxed != 2)
                    return false;
                if (!nandPrvBlockErase(nand)) return false;
                nandPrvBusy(nand, 100);
                nand->state = K9nandStateReset;
                nand->addrBytesRxed = 0;
                break;

            case 0x80:
                if (nand->state != K9nandStateReset && nand->state != K9nandStateReading &&
                    nand->state != K9nandStateReadId && nand->state != K9nandStateStatusReading)
                    return false;
                nand->state = K9nandStateProgramAddrRxing;
                nand->addrBytesRxed = 0;
                nand->lastOpFailed = false;
                break;

            case 0x10:
                if (nand->area == K9nandAreaB) nand->area = K9nandAreaA;
                if (nand->state != K9nandStateProgramDataRxing) return false;
                // fprintf(stderr, "comitting page write for page %u, cur ofst %u\n", nand->pageNo,
                // nand->pageOfst);
                if (!nandPrvPageProgram(nand)) return false;
                nandPrvBusy(nand, 10);
                nand->state = K9nandStateReset;
                nand->addrBytesRxed = 0;
                break;

            case 0x90:
                if (nand->state != K9nandStateReset && nand->state != K9nandStateReading &&
                    nand->state != K9nandStateReadId && nand->state != K9nandStateStatusReading)
                    return false;
                nand->state = K9nandStateReadId;
                nand->addrBytesRxed = 0;
                break;

            case 0x70:
                nand->state = K9nandStateStatusReading;
                nand->addrBytesRxed = 0;
                break;

            case 0xff:
                nand->area = K9nandAreaA;
                nand->state = K9nandStateReset;
                nand->addrBytesRxed = 0;
                break;

            default:
                fprintf(stderr, "unknown command 0x%02x. halt.\n", val);
                uarmAbort();
        }
    } else if (!cle && ale) {  // addr

        switch (nand->state) {
            case K9nandStateReadId:
                if (nand->addrBytesRxed >= 1) return false;
                break;
            case K9nandStateProgramAddrRxing:
                if (nand->addrBytesRxed >= nand->byteAddrBytes + nand->pageAddrBytes)
                    return true;  // ignore
                break;

            case K9nandStateEraseAddrRxing:
                if (nand->addrBytesRxed >= nand->pageAddrBytes) return true;  // ignore
                break;
            case K9nandStateReading:
                if (nand->addrBytesRxed >= nand->byteAddrBytes + nand->pageAddrBytes)
                    return true;  // ignore
                if (nand->addrBytesRxed ==
                    nand->byteAddrBytes + nand->pageAddrBytes - 1)  // about to become enough
                    nandPrvBusy(nand, 1);
                break;
            default:
                return false;
        }
        nand->addrBytes[nand->addrBytesRxed++] = val;
    } else if (!cle && !ale) {  // data

        switch (nand->state) {
            case K9nandStateProgramAddrRxing:
                if (nand->addrBytesRxed != nand->byteAddrBytes + nand->pageAddrBytes) return false;
                nand->state = K9nandStateProgramDataRxing;
                nand->pageNo = nandPrvGetPageAddr(nand, nand->byteAddrBytes);

                // fprintf(stderr, "writing page %5u (block addr %4u.%2u)\n", nand->pageNo,
                // nand->pageNo / NAND_PAGES_PER_BLOCK, nand->pageNo % NAND_PAGES_PER_BLOCK);

                if (nand->pageNo >= (nand->blocksPerDevice << nand->pagesPerBlockLg2)) return false;

                memset(nand->pageBuf.buffer, 0xff, nand->bytesPerPage);
                memoryBufferMarkRangeDirty(&nand->pageBuf, 0, nand->bytesPerPage);

                switch (nand->area) {
                    case K9nandAreaA:
                        nand->pageOfst = 0;
                        break;
                    case K9nandAreaB:
                        nand->area = K9nandAreaA;
                        nand->pageOfst = nand->areaSize;
                        break;
                    case K9nandAreaC:
                        nand->pageOfst = 2 * nand->areaSize;
                        break;
                    default:
                        return false;
                }
                nand->pageOfst += nandPrvGetByteAddr(nand);
                // fallthrough

            case K9nandStateProgramDataRxing:
                if (!nandPrvAcceptProgramData(nand, val)) return false;
                break;

            default:
                return false;
        }
    } else
        return false;

    return true;
}

bool nandRead(struct NAND *nand, bool cle, bool ale, uint8_t *valP) {
    if (cle || ale) return false;

    switch (nand->state) {
        case K9nandStateReadId:
            if (nand->addrBytesRxed != 1) return false;
            if (nand->addrBytes[0] >= nand->deviceIdLen) return false;
            *valP = nand->deviceId[nand->addrBytes[0]++];
            return true;

        case K9nandStateStatusReading:
            // Bit 6 ready, bit 0 the last program or erase failed.
            *valP = (nand->busyCt ? 0x00 : 0x40) | (nand->lastOpFailed ? 0x01 : 0x00);
            return true;

        case K9nandStateReading:

            if (!(nand->flags & NAND_HAS_SECOND_READ_CMD) &&
                nand->addrBytesRxed == nand->byteAddrBytes + nand->pageAddrBytes)
                nand->addrBytesRxed = 0xfe;  // special marker

            if (nand->addrBytesRxed == 0xfe) {
                nand->pageNo = nandPrvGetPageAddr(nand, nand->byteAddrBytes);
                nand->addrBytesRxed = 0xff;  // special marker

                // fprintf(stderr, "reading page %5u (block addr %4u.%2u) area %c offset %u\n",
                //	nand->pageNo, nand->pageNo / NAND_PAGES_PER_BLOCK, nand->pageNo %
                // NAND_PAGES_PER_BLOCK, 'A' + (unsigned)nand->area, nand->addr[0]);

                if (nand->pageNo >= (nand->blocksPerDevice << nand->pagesPerBlockLg2)) {
                    fprintf(stderr, "page number ouf of bounds\n");
                    return false;
                }

                if (!nandPrvReadPage(nand, nand->pageNo, nand->pageBuf.buffer)) return false;
                memoryBufferMarkRangeDirty(&nand->pageBuf, 0, nand->bytesPerPage);

                switch (nand->area) {
                    case K9nandAreaA:
                        nand->pageOfst = 0;
                        break;
                    case K9nandAreaB:
                        nand->pageOfst = nand->areaSize;
                        nand->area = K9nandAreaA;
                        break;
                    case K9nandAreaC:
                        nand->pageOfst = 2 * nand->areaSize;
                        break;
                }
                nand->pageOfst += nandPrvGetByteAddr(nand);
            }
            if (nand->addrBytesRxed != 0xff) return false;
            if (nand->pageOfst == nand->bytesPerPage) {  // read next page

                nand->pageNo++;
                nand->pageOfst = (nand->area == K9nandAreaC) ? 2 * nand->areaSize : 0;

                // fprintf(stderr, "reading page %5u (block addr %4u.%2u)\n", nand->pageNo,
                // nand->pageNo / NAND_PAGES_PER_BLOCK, nand->pageNo % NAND_PAGES_PER_BLOCK);

                if (nand->pageNo >= nandPrvPageCount(nand) ||
                    !nandPrvReadPage(nand, nand->pageNo, nand->pageBuf.buffer))
                    return false;
                memoryBufferMarkRangeDirty(&nand->pageBuf, 0, nand->bytesPerPage);
            }
            *valP = nand->pageBuf.buffer[nand->pageOfst++];
            // fprintf(stderr, "[%3x] = 0x%02x\n", nand->pageOfst - 1, *valP);
            return true;

        default:
            return false;
    }
}

void nandPeriodic(struct NAND *nand) {
    if (nand->busyCt && !--nand->busyCt) nandPrvCallReadyCbks(nand, true);
}

bool nandTaskRequired(struct NAND *nand) { return nand->busyCt > 0; }

struct Buffer nandGetData(struct NAND *nand) {
    if (nand->backing) {
        // A copy for readers of the whole raw image (save-nand, tests); writes to it are lost.
        if (!nand->rawViewValid) {
            nand->rawView.resize(nand->dataSize);
            for (uint32_t page = 0; page < nandPrvPageCount(nand); page++)
                if (!nandPrvReadPage(nand, page, nand->rawView.data() + size_t(page) * nand->bytesPerPage))
                    return Buffer{0, nullptr};
            nand->rawViewValid = true;
        }
        return Buffer{nand->rawView.size(), nand->rawView.data()};
    }
    struct Buffer buffer = {.size = nand->data ? nand->dataSize : 0, .data = nand->data};

    return buffer;
}

uint64_t nandRawBytes(struct NAND *nand) { return nand->dataSize; }

uint32_t nandPageBytes(struct NAND *nand) { return nand->bytesPerPage; }

bool nandReadRawPage(struct NAND *nand, uint32_t page, uint8_t *dest) {
    return page < nandPrvPageCount(nand) && nandPrvReadPage(nand, page, dest);
}

bool nandReadStored(struct NAND *nand, uint64_t offset, uint64_t length, uint8_t *dest, uint64_t sideOffset) {
    if (nand->backing && nand->sideOffset == sideOffset) return nand->backing->Read(offset, length, dest);
    const uint64_t mainTotal = uint64_t(nand->mainBytes) * nandPrvPageCount(nand);
    const uint64_t spareTotal = uint64_t(nand->spareBytes) * nandPrvPageCount(nand);
    if (sideOffset < mainTotal || offset + length > sideOffset + spareTotal) return false;
    std::vector<uint8_t> page(nand->bytesPerPage);
    while (length) {
        uint64_t n;
        if (offset < mainTotal) {
            const uint32_t p = uint32_t(offset / nand->mainBytes), at = uint32_t(offset % nand->mainBytes);
            n = std::min<uint64_t>(length, nand->mainBytes - at);
            if (!nandPrvReadPage(nand, p, page.data())) return false;
            memcpy(dest, page.data() + at, size_t(n));
        } else if (offset < sideOffset) {
            n = std::min<uint64_t>(length, sideOffset - offset);
            memset(dest, 0, size_t(n));
        } else {
            const uint64_t side = offset - sideOffset;
            const uint32_t p = uint32_t(side / nand->spareBytes), at = uint32_t(side % nand->spareBytes);
            n = std::min<uint64_t>(length, nand->spareBytes - at);
            if (!nandPrvReadPage(nand, p, page.data())) return false;
            memcpy(dest, page.data() + nand->mainBytes + at, size_t(n));
        }
        offset += n;
        dest += n;
        length -= n;
    }
    return true;
}

bool nandAdoptBacking(struct NAND *nand, std::unique_ptr<CardBacking> backing, uint64_t sideOffset) {
    const uint64_t pages = nandPrvPageCount(nand);
    if (!backing || sideOffset < pages * nand->mainBytes || backing->Size() != sideOffset + pages * nand->spareBytes)
        return false;
    nand->backing = std::move(backing);
    nand->sideOffset = sideOffset;
    if (nand->data && nand->releaseData) nand->releaseData(nand->data);
    nand->data = nullptr;
    nand->rawView.clear();
    nand->rawView.shrink_to_fit();
    nand->rawViewValid = false;
    return true;
}

CardBacking *nandBacking(struct NAND *nand) { return nand->backing.get(); }

void nandSetDataRelease(struct NAND *nand, void (*release)(void *)) { nand->releaseData = release; }

bool nandSaveCheckpoint(struct NAND *nand, NandCheckpoint &checkpoint) {
    checkpoint = {};
    if (nand->backing) {
        checkpoint.pin = nand->backing->Snapshot();
        if (!checkpoint.pin) return false;
    } else if (nand->data) {
        checkpoint.raw.assign(nand->data, nand->data + nand->dataSize);
    }
    checkpoint.dirtyPages.assign(reinterpret_cast<uint8_t *>(nand->dirtyPages),
                                 reinterpret_cast<uint8_t *>(nand->dirtyPages) + nand->dirtyPagesSize);
    checkpoint.dirty = nand->dirty;
    return true;
}

bool nandRestoreCheckpoint(struct NAND *nand, const NandCheckpoint &checkpoint) {
    nand->rawViewValid = false;
    if (checkpoint.pin) {
        if (!nand->backing || !nand->backing->Restore(*checkpoint.pin)) return false;
    } else if (!checkpoint.raw.empty()) {
        if (!nand->data || checkpoint.raw.size() != nand->dataSize) return false;
        memcpy(nand->data, checkpoint.raw.data(), nand->dataSize);
    }
    if (checkpoint.dirtyPages.size() == nand->dirtyPagesSize)
        memcpy(nand->dirtyPages, checkpoint.dirtyPages.data(), nand->dirtyPagesSize);
    nand->dirty = checkpoint.dirty;
    return true;
}

struct Buffer nandGetDirtyPages(struct NAND *nand) {
    struct Buffer buffer = {.size = nand->dirtyPagesSize, .data = nand->dirtyPages};

    return buffer;
}

bool nandIsDirty(struct NAND *nand) { return nand->dirty; }

void nandSetDirty(struct NAND *nand, bool isDirty) { nand->dirty = isDirty; }

uint32_t nandGetWriteCnt(struct NAND *nand) { return nand->writeCnt; }

bool nandIsReady(struct NAND *nand) { return !nand->busyCt; }

struct NAND *nandInit(uint8_t *nandContent, const struct MemoryBuffer *pageBuffer,
                      struct Reschedule reschedule, size_t nandSize, const struct NandSpecs *specs,
                      NandReadyCbk readyCbk, void *readyCbkData) {
    if (specs->bytesPerPage % 528) ERR("invalid NAND page size");
    if (((specs->bytesPerPage << specs->pagesPerBlockLg2) * specs->blocksPerDevice) % 4224)
        ERR("invalid NAND size");

    struct NAND *nand = new NAND{};
    uint32_t nandSz, nandPages, t;

    nand->reschedule = reschedule;
    nand->readyCbk[0] = readyCbk;
    nand->readyCbkData[0] = readyCbkData;

    nand->flags = specs->flags;
    nand->bytesPerPage = specs->bytesPerPage;
    nand->pagesPerBlockLg2 = specs->pagesPerBlockLg2;
    nand->blocksPerDevice = specs->blocksPerDevice;
    nand->mainBytes = specs->bytesPerPage / 528 * 512;
    nand->spareBytes = specs->bytesPerPage / 528 * 16;
    if (specs->devIdLen > sizeof(nand->deviceId))
        ERR("Device ID too long\n");
    else
        nand->deviceIdLen = specs->devIdLen;
    memcpy(nand->deviceId, specs->devId, nand->deviceIdLen);

    nandPages = nand->blocksPerDevice << nand->pagesPerBlockLg2;
    nandSz = nand->bytesPerPage * nandPages;

    t = 31 - __builtin_clz(specs->bytesPerPage - 1);
    if (specs->flags &
        NAND_FLAG_SAMSUNG_ADDRESSED_VIA_AREAS)  // one bit of address goes away via "area" commands
        t--;

    nand->byteAddrBytes = (t + 7) / 8;
    nand->areaSize = 1 << t;
    nand->pageAddrBytes = (31 - __builtin_clz(nandPages - 1) + 7) / 8;

    if (nand->bytesPerPage > pageBuffer->size) ERR("not enough space for page buffer");
    memcpy(&nand->pageBuf, pageBuffer, sizeof(*pageBuffer));

    if (nandSize != nandSz) {
        fprintf(stderr, "Cannot use nand. got %lu, wanted %lu\n", (unsigned long)nandSize,
                (unsigned long)nandSz);
        delete nand;

        return NULL;
    }
    // No content: the NAND file is attached with nandAdoptBacking before the guest runs.
    nand->data = nandContent;

    nand->dataSize = nandSz;

    size_t dirtyPagesSize4 = nandSz / (528 * 8 * 32);
    if (dirtyPagesSize4 * 528 * 8 * 32 < nandSz) dirtyPagesSize4++;

    nand->dirtyPages = (uint32_t *)malloc(4 * dirtyPagesSize4);
    memset(nand->dirtyPages, 0, 4 * dirtyPagesSize4);

    nand->dirtyPagesSize = dirtyPagesSize4 * 4;

    nandPrvBusy(nand, 1);  // we start busy for a little bit

    return nand;
}

void nandResetPageBuffer(struct NAND *nand) {
    memset(nand->pageBuf.buffer, 0, nand->bytesPerPage);
    memoryBufferMarkRangeDirty(&nand->pageBuf, 0, nand->bytesPerPage);
}

template <typename T>
void nandSave(struct NAND *nand, T &savestate) {
    auto chunk = savestate.GetChunk(ChunkType::nand, SAVESTATE_VERSION);
    if (!chunk) abort();

    SaveChunkHelper helper(*chunk);
    nand->DoSaveLoad(helper);
}

template <typename T>
void nandLoad(struct NAND *nand, T &loader) {
    auto chunk = loader.GetChunkOrFail(ChunkType::nand, SAVESTATE_VERSION, "nand");
    if (!chunk) return;

    LoadChunkHelper helper(*chunk);
    nand->DoSaveLoad(helper);
}

template void nandSave<Savestate<ChunkType>>(NAND *nand, Savestate<ChunkType> &savestate);
template void nandSave<SavestateProbe<ChunkType>>(NAND *nand, SavestateProbe<ChunkType> &savestate);
template void nandLoad<SavestateLoader<ChunkType>>(NAND *nand, SavestateLoader<ChunkType> &loader);
