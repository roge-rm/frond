//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr
#ifndef FROND_DEVICE_SONY_TG50_H
#define FROND_DEVICE_SONY_TG50_H
#include <functional>
#include "device.h"

#include "i2sdev_AK4534.h"
struct Tg50Board;
AudioInputDemand tg50BoardInputDemand(Tg50Board *);
void tg50BoardSetInputSource(Tg50Board *, const AudioInputSource *);
void tg50BoardFenceInput(Tg50Board *, bool);
void tg50BoardDisarmInput(Tg50Board *);
constexpr uint32_t tg50PhysicalRamSize = 16u << 20;
// The extra MiB and the mirror reproduce uARM's RAM-probe workaround.
constexpr uint32_t tg50RamBackingSize = 17u << 20;

Tg50Board *tg50BoardSetup(SocPeriphs *, Keypad *,Reschedule,GetEmuTime);
class CsrBcspRadio;
CsrBcspRadio* tg50BoardBluetooth(Tg50Board *);
void tg50BoardTouch(Tg50Board *, int x, int y);
void tg50BoardKey(Tg50Board *, uint32_t key, bool down);
bool tg50BoardHoldSwitchOn(const Tg50Board *);
bool tg50BoardPanelEnabled(const Tg50Board *);
bool tg50BoardTaskRequired(Tg50Board*);
void tg50BoardPeriodic(Tg50Board *);
void tg50BoardReset(Tg50Board *, bool clearSram);
void tg50BoardBeginReset(Tg50Board *);
void tg50BoardEndReset(Tg50Board *);
std::function<void()> tg50BoardCaptureInput(Tg50Board *);
// Called on the stopped SoC's thread, removes all bus registrations before freeing.
void tg50BoardDestroy(Tg50Board *);
template <typename T> void tg50BoardSave(Tg50Board *, T &);
template <typename T> bool tg50BoardLoad(Tg50Board *, T &, bool preserveInput);
bool tg50BoardIrPowerEnabled(Tg50Board *);
void tg50BoardSetUsbCable(Tg50Board *, bool connected);
bool tg50BoardUsbCableAttached(Tg50Board *);
bool tg50BoardUsbListening(Tg50Board *);
void tg50BoardSetTransportObserver(Tg50Board *, void (*)(void *), void *);
void tg50BoardAudioSleepReset(Tg50Board *);
void tg50BoardAudioAdvance(Tg50Board *, uint64_t ns);
void tg50BoardSetAudioQueue(Tg50Board *, AudioQueue *);
struct MemoryStickController;
MemoryStickController* tg50BoardMemoryStick(Tg50Board*);
#endif
