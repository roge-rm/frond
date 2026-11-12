//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#ifndef _DEVICE_H_
#define _DEVICE_H_

#include <functional>
#include <string>

#include <cstdint>
#include <cstdio>

#include "SoC.h"
#include "device_type5.h"
#include "get_emu_time.h"
#include "keys.h"
#include "mem.h"
#include "nand.h"
#include "pxa_AC97.h"
#include "pxa_GPIO.h"
#include "pxa_I2C.h"
#include "pxa_I2S.h"
#include "pxa_SSP.h"
#include "pxa_UART.h"
#include "reschedule.h"
#include "vSD.h"

#define DEVICE_PERIODIC_TIER0 0
#define DEVICE_PERIODIC_TIER1 1

struct AudioQueue;
struct MemoryStickController;
struct PxaDma;

struct SocPeriphs {
    // in to deviceSetup
    struct PxaAC97 *ac97;
    struct PxaGpio *gpio;
    struct PxaDma *dma;
    struct PxaI2c *i2c;
    struct PxaI2s *i2s;
    struct PxaPwm *pwm0;
    struct PxaPwrClk *pwrClk;
    struct PxaSsp *ssp;
    struct PxaSsp *ssp2;  // assp for xscale
    struct PxaSsp *ssp3;  // nssp for scale
    struct ArmMem *mem;
    class SoC *soc;

    // PXA order: ffUart, hwUart, stUart, btUart
    struct PxaUart *uarts[4];

    void *adc;  // some cases need this
    void *kpc;  // some cases need this

    // out from deviceSetup
    struct NAND *nand;
    struct PxaUart *dbgUart;
    struct PxaUart *btUart;
};

enum RamTermination {  // what's after ram in phys map? (some devices probe)
    RamTerminationMirror,
    RamTerminationWriteIgnore,
    RamTerminationNone,
};

struct Device;
struct MemoryBuffer;

// simple queries
enum RamTermination deviceGetRamTerminationStyle(void);
uint_fast8_t deviceGetSocRev(void);

// device handling
// Stop execution and finish all bus dispatch before destroying a board.
void deviceDestroy(struct Device *dev);
void deviceBeginReset(struct Device *dev);
void deviceEndReset(struct Device *dev);
struct Device *deviceSetup(enum DeviceType5 type, struct SocPeriphs *sp,
                           struct Reschedule reschedule, struct GetEmuTime getEmuTime,
                           struct Keypad *kp, struct VSD *vsd, uint8_t *nandContent,
                           size_t nandSize, const struct MemoryBuffer *nandPageBuffer);
void deviceKey(struct Device *dev, uint32_t key, bool down);
bool devicePanelEnabled(Device*);
bool deviceHoldSwitchOn(Device*);
void devicePeriodic(struct Device *dev, uint32_t tier);
void deviceNetworkPeriodic(Device* dev);
struct AudioInputSource;
struct AudioInputDemand;
AudioInputDemand deviceInputDemand(Device *);
void deviceSetInputSource(Device *, const AudioInputSource *);
void deviceFenceInput(Device *, bool);
void deviceDisarmInput(Device *);
void deviceAudioSleepReset(Device *);
void deviceAudioAdvance(Device *, uint64_t ns);
void devicePcmPeriodic(struct Device *dev);
void deviceTouch(struct Device *dev, int x, int y);

bool deviceTaskRequired(struct Device *dev, uint32_t tier);

void deviceSetAudioQueue(struct Device *dev, struct AudioQueue *audioQueue);

bool deviceI2sConnected();
bool deviceTg50IrPowerEnabled(Device *);
void deviceTg50SetUsbCable(Device *, bool connected);
bool deviceTg50UsbCableAttached(Device *);
bool deviceTg50UsbListening(Device *);
void deviceSetTransportObserver(Device *, void (*)(void *), void *);

void deviceSetSdCardInserted(struct Device *dev, bool inserted);

enum DeviceType5 deviceGetType(struct Device *dev);

template <typename T>
void deviceSave(struct Device *dev, T &savestate);

template <typename T>
void deviceLoad(struct Device *dev, T &loader, bool preserveInput = false);
std::function<void()> deviceCaptureInput(struct Device *dev);

// Optional virtual H4 controller, native Unix socket. No host adapter ownership.
bool deviceSetBluetooth(Device*,const std::string& socketPath,std::string& error);
std::string deviceBluetoothState(Device*);
void deviceBeginBluetoothCheckpoint(Device*);
void deviceEndBluetoothCheckpoint(Device*,bool sleeping=false);
void deviceBluetoothStateReplaced(Device*);
void deviceSuspendBluetooth(Device*);
void deviceServiceBluetoothReset(Device*);
void deviceBluetoothPinsChanged(Device*);
void deviceStopBluetooth(Device*);


MemoryStickController* deviceMemoryStick(Device*);
#endif
