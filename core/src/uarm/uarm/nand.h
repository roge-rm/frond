//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _NAND_H_
#define _NAND_H_

#include <cstdio>
#include <memory>
#include <vector>

#include "CPU.h"
#include "buffer.h"
#include "mem.h"
#include "pxa_GPIO.h"
#include "reschedule.h"

struct NAND;
struct MemoryBuffer;
class CardBacking;
class CardSnapshot;

typedef void (*NandReadyCbk)(void *userData, bool ready);

// options
#define NAND_FLAG_SAMSUNG_ADDRESSED_VIA_AREAS \
    0x01  // use 0x01 and 0x50 commands to save one bit on byte addressing
#define NAND_HAS_SECOND_READ_CMD 0x02

struct NandSpecs {
    uint32_t bytesPerPage;
    uint32_t blocksPerDevice;
    uint8_t pagesPerBlockLg2;
    uint8_t flags;
    uint8_t devIdLen;
    uint8_t devId[6];
};

struct NAND *nandInit(uint8_t *nandContent, const struct MemoryBuffer *pageBuffer,
                      struct Reschedule reschedule, size_t nandSize, const struct NandSpecs *specs,
                      NandReadyCbk readyCbk, void *readyCbkData);

void nandResetPageBuffer(struct NAND *nand);

void nandSecondReadyCbkSet(struct NAND *nand, NandReadyCbk readyCbk, void *readyCbkData);

bool nandWrite(struct NAND *nand, bool cle, bool ale, uint8_t val);
bool nandRead(struct NAND *nand, bool cle, bool ale, uint8_t *valP);

bool nandIsReady(struct NAND *nand);

void nandPeriodic(struct NAND *nand);

bool nandTaskRequired(struct NAND *nand);

struct Buffer nandGetData(struct NAND *nand);
struct Buffer nandGetDirtyPages(struct NAND *nand);
bool nandIsDirty(struct NAND *nand);
void nandSetDirty(struct NAND *nand, bool isDirty);

uint32_t nandGetWriteCnt(struct NAND *nand);

// Raw NAND bytes with spare, and bytes per raw page (main then spare).
uint64_t nandRawBytes(struct NAND *nand);
uint32_t nandPageBytes(struct NAND *nand);
bool nandReadRawPage(struct NAND *nand, uint32_t page, uint8_t *dest);
// Reads nand.frondnand's stored bytes (main bytes of each page, zeros to sideOffset, then spare
// bytes of each page), whether the NAND is in RAM or on a backing laid out that way.
bool nandReadStored(struct NAND *nand, uint64_t offset, uint64_t length, uint8_t *dest, uint64_t sideOffset);
// Moves the NAND onto `backing` in that layout. RAM content goes, freed by the release function if set.
bool nandAdoptBacking(struct NAND *nand, std::unique_ptr<CardBacking> backing, uint64_t sideOffset);
CardBacking *nandBacking(struct NAND *nand);
void nandSetDataRelease(struct NAND *nand, void (*release)(void *));

// What a read-only guest call restores: a copy of RAM content, or a pin of the backing.
struct NandCheckpoint {
    std::vector<uint8_t> raw;
    std::shared_ptr<const CardSnapshot> pin;
    std::vector<uint8_t> dirtyPages;
    bool dirty{};
};
bool nandSaveCheckpoint(struct NAND *nand, NandCheckpoint &checkpoint);
bool nandRestoreCheckpoint(struct NAND *nand, const NandCheckpoint &checkpoint);

template <typename T>
void nandSave(struct NAND *nand, T &savestate);

template <typename T>
void nandLoad(struct NAND *nand, T &loader);

#endif
