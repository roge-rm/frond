//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _MMIO_MS_CTRLR_H_
#define _MMIO_MS_CTRLR_H_


#include "mem.h"
#include <memory>
#include <vector>
#include "reschedule.h"
class CardSnapshot;
#include "MemoryStickProCard.h"
struct PxaGpio;struct PxaDma;


struct MemoryStickController;


struct MemoryStickController* msCtrlrInit(struct ArmMem *physMem, uint32_t baseAddr,PxaGpio* gpio=nullptr,PxaDma* dma=nullptr,Reschedule reschedule={});



void msCtrlrReset(MemoryStickController *);
void msCtrlrDiscardSaveImage(MemoryStickController *);
// Destroy only after the owning SoC can no longer dispatch its bus callbacks.
void msCtrlrDestroy(MemoryStickController *);
template <typename T> void msCtrlrSave(MemoryStickController *, T &);
template <typename T> bool msCtrlrLoad(MemoryStickController *, T &, bool preserveInput = false);
bool msCtrlrTaskRequired(MemoryStickController*);
void msCtrlrPeriodic(MemoryStickController*);
void msCtrlrRefresh(MemoryStickController*);
bool msCtrlrPresent(MemoryStickController*);
bool msCtrlrInserted(MemoryStickController*);
bool msCtrlrReadOnly(MemoryStickController*);
bool msCtrlrProtect(MemoryStickController*,bool);
bool msCtrlrAttach(MemoryStickController*,const std::vector<uint8_t>&);
bool msCtrlrAttach(MemoryStickController*,std::vector<uint8_t>&&);
// Attaches a classic stick taken out of the slot, so its write protection can be set before it goes in.
bool msCtrlrAttachEjected(MemoryStickController*,std::vector<uint8_t>&& physical,bool protect);
// Attaches the classic stick whose v2 .frondms stored bytes are in the card store (sdcard.h).
// Detaching it resets the store.
bool msCtrlrAttachClassicStore(MemoryStickController*,bool inserted,bool protect);
// The classic stick is on the card store, not in its own RAM.
bool msCtrlrClassicOnStore(MemoryStickController*);
// Moves a classic stick from its own RAM into the empty card store as card `id`, with nothing the
// guest can see changing. True when it is on the store already.
bool msCtrlrMoveClassicToStore(MemoryStickController*,const char* id);
// For a load whose stick was changed while the machine was off: the next msCtrlrLoad keeps the stick
// attached now (or none) and leaves it ejected, with fresh transfer state. The detect line keeps
// its saved level until msCtrlrSwapEject, called once the load is done, which drops it.
void msCtrlrSwapOnLoad(MemoryStickController*,bool);
void msCtrlrSwapEject(MemoryStickController*);
// Attaches a Memory Stick Pro whose sectors live in `store`, replacing any stick.
bool msCtrlrAttachPro(MemoryStickController*,MsProStore store,bool inserted=true);
bool msCtrlrPro(MemoryStickController*);
// A classic stick whose contents differ from the image it was attached with.
bool msCtrlrChanged(MemoryStickController*);
// The stick's size in MB (MiB for a Pro), or 0 with none.
uint32_t msCtrlrMegabytes(MemoryStickController*);
// Removes any stick, classic or Pro.
void msCtrlrDetach(MemoryStickController*);
bool msCtrlrInsert(MemoryStickController*,bool);
bool msCtrlrExport(MemoryStickController*,std::vector<uint8_t>&);
bool msCtrlrSaveCheckpoint(MemoryStickController*, std::shared_ptr<const CardSnapshot>&, bool& protect);
bool msCtrlrRestoreCheckpoint(MemoryStickController*, const std::shared_ptr<const CardSnapshot>&, bool protect);
#endif
