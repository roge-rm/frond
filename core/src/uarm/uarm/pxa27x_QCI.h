#ifndef _PXA27X_QCI_H_
#define _PXA27X_QCI_H_

#include <cstdint>

#include "get_emu_time.h"
#include "mem.h"
#include "pxa_DMA.h"
#include "pxa_IC.h"
#include "reschedule.h"

// PXA27x quick capture interface (developer's manual chapter 27) at 0x50000000, IRQ 33, DMA
// requests 68-70, fed by an OV9640. All calls run on the emulator thread.
struct PxaQci;
struct Ov9640;
struct CameraSource;

#define PXA_QCI_BASE 0x50000000UL

struct PxaQci *pxaQciInit(struct ArmMem *physMem, struct PxaIc *ic, struct PxaDma *dma,
                          struct Ov9640 *sensor, struct GetEmuTime getTime,
                          struct Reschedule reschedule);
void pxaQciDestroy(struct PxaQci *);
// Moves the frame on to the current guest time; run from the board's tier 0 task.
void pxaQciPeriodic(struct PxaQci *);
bool pxaQciTaskRequired(struct PxaQci *);
// Cold register values, as after a reset or the PXA27x sleep reset.
void pxaQciReset(struct PxaQci *);

// Host picture source; nullptr or a disarm goes back to the test pattern.
void pxaQciSetSource(struct PxaQci *, const CameraSource *);
void pxaQciFenceSource(struct PxaQci *, bool fence);
void pxaQciDisarmSource(struct PxaQci *);
// True while the interface is enabled with a running sensor, for frontends' readouts.
bool pxaQciCapturing(const struct PxaQci *);
uint64_t pxaQciFrameNumber(const struct PxaQci *);

template <typename T> void pxaQciSave(struct PxaQci *, T &savestate);
// Sessions from before the camera have no QCI chunk; the interface then starts reset.
template <typename T> bool pxaQciLoad(struct PxaQci *, T &loader);

#endif
