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
// AD7873 AUX in mV, the connector accessory's ID resistor.
void tg50BoardSetAccessoryAux(Tg50Board *, uint16_t mV);
// Nothing on the connector: AUX at full scale.
void tg50BoardSetConnectorEmpty(Tg50Board *);
// The AD7873 battery input, 4200 mV when full.
void tg50BoardSetBatteryMilliVolts(Tg50Board *, uint16_t mV);
// The charger: uc A13 low is charging, A14 low with A2 low external power (DAL 0x24f88 and 0x22abc). A2 always
// reads low here, so charging pulls A13 and A14 low.
void tg50BoardSetCharging(Tg50Board *, bool charging);
// The AUX conversion result, 12 bits.
uint16_t tg50BoardAccessoryAuxCode(Tg50Board *);
bool tg50BoardUsbCableAttached(Tg50Board *);
bool tg50BoardUsbListening(Tg50Board *);
void tg50BoardSetTransportObserver(Tg50Board *, void (*)(void *), void *);
void tg50BoardAudioSleepReset(Tg50Board *);
void tg50BoardAudioAdvance(Tg50Board *, uint64_t ns);
void tg50BoardSetAudioQueue(Tg50Board *, AudioQueue *);
struct MemoryStickController;
MemoryStickController* tg50BoardMemoryStick(Tg50Board*);
#endif
