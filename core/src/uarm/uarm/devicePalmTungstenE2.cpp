//(c) uARM project    https://github.com/uARM-Palm/uARM    uARM@dmitry.gr

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <array>
#include <random>

#include "ac97dev_WM9712L.h"
#include "cputil.h"
#include "device.h"
#include "deviceSonyTG50.h"
#include "CsrBcspRadio.h"
#include "device_type5.h"
#include "device_profile5.h"
#include "pxa_battery.h"
#include "mmiodev_DirectNAND.h"
#include "savestate/savestateAll.h"
#include "uartdev_bcm2035.h"
#include "pxa270_KPC.h"
#include "LibertasCF.h"
#include "i2cdev_OV9640.h"
#include "i2cdev_WM8750.h"
#include "pxa27x_QCI.h"
#include "soc_PXA.h"
#include "spitz_nand.h"
#include "spitz_pcmcia.h"

// clang-format off
/*
		GPIO	DIR	VAL	AFR	EDGE	NOTES
		0		IN		0
		1		IN		1			gpio reset
		2		IN		0
		3		IN		0
		4		IN		0	FE 		checked in FIQ handler of all places.... if high, ignored. probbly cradle detect (low if cradled)
		5		IN		0
		6		OUT	LO	1			MMC clock
		7		IN		0
		8		OUT	LO	1			MMC CS0
		9		IN		0
		10		IN		0			sd card detect (Active low)
		11		IN		0
		12		IN		0	RE FE 	headphone detect (active high)
		13		IN		0
		14		IN		0
		15		OUT	LO	2			nCS1
		16		OUT	LO	2			PWM0
		17		OUT	HI	0
		18		OUT	LO	0
		19		IN		0
		20		IN		0
		21		IN		0
		22		IN		0
		23		OUT	LO	0
		24		OUT	LO	0
		25		IN		0
		26		OUT	HI	0			WM9712L power? enabled when enabling pen. hi for power
		27		OUT	LO	0
		28		IN		1			AC97 bit_clk
		29		IN		1			AC97 Sdata_in0
		30		OUT	LO	2			AC97 Sdata_out
		31		OUT	LO	2			AC97 sync
		32		OUT	HI	0
		33		OUT	LO	0
		34		IN		1			FFUART RX
		35		IN		0
		36		OUT	LO	0
		37		OUT	HI	0
		38		OUT	LO	0
		39		OUT	LO	2			FFUART TX
		40		IN		0	FE 		hotsync button?		[GUESS]
		41		OUT	LO	0
		42		IN		3			HWUART RX
		43		OUT	LO	3			HWUART TX
		44		IN		3			HWUART CTS
		45		OUT	LO	3			HWUART RTS
		46		IN		2			STUART RX
		47		OUT	LO	1			STUART TX
		48		OUT	LO	0
		49		IN		0
		50		IN		0	RE 		probably pen detect
		51		IN		0			card write protect switch (active high)
		52		IN		0			low on boot causes manufacturing test to be entered
		53		IN		0
		54		OUT	LO	0
		55		OUT	LO	0			SD power (active high)
		56		OUT	HI	0
		57		OUT	HI	0
		58		OUT	LO	2			LDD[0]
		59		OUT	LO	2			LDD[1]
		60		OUT	LO	2			LDD[2]
		61		OUT	LO	2			LDD[3]
		62		OUT	LO	2			LDD[4]
		63		OUT	LO	2			LDD[5]
		64		OUT	LO	2			LDD[6]
		65		OUT	LO	2			LDD[7]
		66		OUT	LO	2			LDD[8]
		67		OUT	LO	2			LDD[9]
		68		OUT	LO	2			LDD[10]
		69		OUT	LO	2			LDD[11]
		70		OUT	LO	2			LDD[12]
		71		OUT	LO	2			LDD[13]
		72		OUT	LO	2			LDD[14]
		73		OUT	LO	2			LDD[15]
		74		OUT	LO	2			LCD FCLK
		75		OUT	LO	2			LCD LCLK
		76		OUT	LO	2			LCD PCLK
		77		OUT	LO	2			LCD AC BIAS
		78		OUT	LO	0
		79		IN		0			NAND nRDY
		80		OUT	LO	0
		81		OUT	LO	0
		82		OUT	LO	0
		83		OUT	LO	0
		84		OUT	LO	0
		85		IN		0
*/
// clang-format on

struct Device {
    Tg50Board *tg50;
    SpitzNand* spitzNand;
    SpitzPcmcia* spitzPcmcia;
    PxaSsp* spitzSsp;
    PxaI2c* spitzI2c;
    PxaI2s* spitzI2s;
    Wm8750* spitzCodec;
    uint8_t spitzAdcPhase, spitzAdcSample;
    bool spitzRemoteInserted;
    SpitzRemoteKey spitzRemoteKey;
    bool spitzBatteryDoorLocked;
    void (*spitzTransportObserver)(void*);
    void* spitzTransportContext;
    uint16_t spitzTouchX, spitzTouchY, spitzTouchSample;
    bool spitzPenDown;
    uint8_t spitzPanelRegisters[8];
    uint8_t spitzPanelWritten;
    struct WM9712L *wm9712L;
    struct DirectNAND *nand;
    struct Bcm2035 *bcm2035;
    struct PxaGpio *gpio;
    struct Ov9640 *ov9640;
    struct PxaQci *qci;

    struct Reschedule reschedule;

    enum DeviceType5 type;
    struct PxaKpc *kpc;
    struct VSD *vsd;
    LibertasCF *wifi;
    uint32_t noRamMarker;
    // The Bluetooth controller's address, least significant byte first, all zero until
    // Bluetooth is first used. It's kept with the session so peers keep trusting this machine
    // across reconnects and restarts. A reset reloads the power-on state, which mustn't
    // replace it.
    std::array<uint8_t, 6> bluetoothAddress;
    bool resetting;
    BatteryPower battery;
};

// The battery input on each board: the TG50's AD7873 VBAT, the WM9712L BMON input on the E2, TX and Zire 72, at
// the count each DAL reads as the set percent (pxa_battery.h). The charger: the TG50's uc A13 and A14 (tg50BoardSetCharging), the Zire 72's
// GPIO99 low (DAL dock status 0x2009c344), the TX's GPIO90 high (DAL dock status 0x2009b7d0) and
// the E2's GPIO9 high (DAL dock status 0x2009c66c).
namespace {
constexpr unsigned spitzFatalBattery = 21, spitzBatteryDoor = 90, spitzChargeFull = 101, spitzAcIn = 115;
constexpr uint16_t spitzDiskLed = 1u << 1, spitzChargeOn = 1u << 3, spitzChargeLed = 1u << 7,
                   spitzTemperatureOn = 1u << 9;

AudioInputDemand spitzMicDemand(const Device& dev, uint32_t numerator, uint32_t denominator) {
    constexpr uint16_t micBias = 0x200;
    return dev.spitzCodec->InputDemand(pxaI2sReceiveEnabled(dev.spitzI2s),
        spitzPcmciaGpioOutputs(dev.spitzPcmcia, 1) & micBias, numerator, denominator);
}

uint8_t spitzBatteryCount(const Device& dev) {
    // MAX1111 counts for 0%, 10%, ... 100%, from the SharpSL battery thresholds.
    constexpr uint16_t battery[] = {169, 178, 182, 186, 189, 191, 195, 199, 203, 207, 214};
    constexpr uint16_t charger[] = {181, 181, 185, 189, 192, 194, 198, 202, 206, 210, 214};
    const auto outputs = spitzPcmciaGpioOutputs(dev.spitzPcmcia, 0);
    const auto& curve = dev.battery.ExternalPower() && (outputs & spitzChargeOn) ? charger : battery;
    return BatteryPower::CurveCentivolts(curve, dev.battery.Percent());
}

void spitzApplyBattery(Device& dev) {
    const bool ac = dev.battery.ExternalPower();
    const auto outputs = spitzPcmciaGpioOutputs(dev.spitzPcmcia, 0);
    pxaGpioSetState(dev.gpio, spitzAcIn, !ac);
    pxaGpioSetState(dev.gpio, spitzBatteryDoor, dev.spitzBatteryDoorLocked);
    pxaGpioSetState(dev.gpio, spitzFatalBattery, ac || dev.battery.Percent() != 0);
    pxaGpioSetState(dev.gpio, spitzChargeFull,
                   ac && dev.spitzBatteryDoorLocked && (outputs & spitzChargeOn) && dev.battery.Percent() == 100);
}
}

static void deviceApplyBattery(Device *dev) {
    const bool charging = dev->battery.ExternalPower();
    const unsigned percent = dev->battery.Percent();
    if (dev->spitzPcmcia) {
        spitzApplyBattery(*dev);
        return;
    }
    if (dev->tg50) {
        tg50BoardSetBatteryCount(dev->tg50, pxaBatteryCount(PxaBatteryBoard::tg50, percent, charging));
        tg50BoardSetCharging(dev->tg50, charging, dev->battery.ChargeInProgress());
        return;
    }
    if (!dev->wm9712L) return;
    const PxaBatteryBoard board = dev->type == DeviceType5::deviceTypeZire72 ? PxaBatteryBoard::zire72
                                  : dev->type == DeviceType5::deviceTypeTX   ? PxaBatteryBoard::tx
                                                                              : PxaBatteryBoard::e2;
    wm9712LsetAuxCount(dev->wm9712L, WM9712LauxPinBmon, pxaBatteryCount(board, percent, charging));
    if (dev->type == DeviceType5::deviceTypeZire72) pxaGpioSetState(dev->gpio, 99, !charging);
    else if (dev->type == DeviceType5::deviceTypeTX) pxaGpioSetState(dev->gpio, 90, charging);
    else if (deviceProfileIsE2Board5(dev->type)) pxaGpioSetState(dev->gpio, 9, charging);
}

bool deviceBatterySupported(Device *dev) { return dev->spitzPcmcia || dev->tg50 || dev->wm9712L; }

bool deviceSetBattery(Device *dev, const BatteryPower &battery) {
    if (!deviceBatterySupported(dev)) return false;
    dev->battery = battery;
    deviceApplyBattery(dev);
    return true;
}

bool deviceSetSpitzPower(Device* dev, const BatteryPower& battery, bool locked) {
    if (!dev->spitzPcmcia) return false;
    dev->spitzBatteryDoorLocked = locked;
    return deviceSetBattery(dev, battery);
}

bool deviceSetSpitzBatteryDoorLocked(Device* dev, bool locked) {
    if (!dev->spitzPcmcia) return false;
    dev->spitzBatteryDoorLocked = locked;
    spitzApplyBattery(*dev);
    return true;
}

bool deviceGetSpitzPower(const Device* dev, SpitzPowerState& state) {
    if (!dev->spitzPcmcia) return false;
    const auto outputs = spitzPcmciaGpioOutputs(dev->spitzPcmcia, 0);
    state = {dev->battery.Percent(), dev->battery.ExternalPower(), dev->spitzBatteryDoorLocked,
             bool(outputs & spitzChargeOn), bool(outputs & spitzChargeLed), bool(outputs & spitzDiskLed)};
    return true;
}

void deviceSetCodecHeadphones(Device* dev, bool in) {
    // The Zire 72 DAL configures WM9712 GPIO1 at 0x2009865c and reads its state at 0x20098e3c.
    wm9712LsetHeadphones(dev->wm9712L, in);
}

// A random locally administered address, as btvirt gives a controller.
static std::array<uint8_t, 6> deviceNewBluetoothAddress() {
    std::random_device random;
    std::array<uint8_t, 6> address{};
    for (unsigned i = 0; i < 5; ++i) address[i] = uint8_t(random());
    address[5] = 0x02;
    return address;
}

static void deviceApplyBluetoothAddress(Device *dev) {
    if (dev->bluetoothAddress == std::array<uint8_t, 6>{}) return;
    if (dev->tg50) tg50BoardBluetooth(dev->tg50)->SetAddress(dev->bluetoothAddress);
    else bcm2035SetAddress(dev->bcm2035, dev->bluetoothAddress);
}

static bool deviceHasBluetooth(Device *dev) { return dev->tg50 || dev->bcm2035; }

// The guest may have programmed its own address since it was last read.
static void deviceSyncBluetoothAddress(Device *dev) {
    std::array<uint8_t, 6> current{};
    if (dev->tg50 ? tg50BoardBluetooth(dev->tg50)->Address(current) : bcm2035Address(dev->bcm2035, current))
        dev->bluetoothAddress = current;
}

// TX RAM probing expects writes at this address not to read back unchanged.
static bool txNoRamAccess(void *ctx, uint32_t pa, uint_fast8_t size, bool write, void *buf) {
    if (pa != 0xa5fffffc || size != 4) return false;
    auto *dev = static_cast<Device *>(ctx);
    if (write) dev->noRamMarker = *static_cast<uint32_t *>(buf);
    else *static_cast<uint32_t *>(buf) = ~dev->noRamMarker;
    return true;
}

// Nothing answers on the Zire 72's chip select 1. Its small ROM clears the RTC registers
// at 0x04090000 there (a mistyped 0x40900000), so the bus takes writes and reads zero.
static bool zireEmptyBusAccess(void *, uint32_t, uint_fast8_t size, bool write, void *buf) {
    if (!write) memset(buf, 0, size);
    return true;
}

enum RamTermination deviceGetRamTerminationStyle(void) { return RamTerminationMirror; }

uint_fast8_t deviceGetSocRev(void) {
    return 0;  // PXA25x
}

static void devicePrvReschedule(void *ctx, uint32_t task) {
    struct Device *dev = (struct Device *)ctx;

    if (task == RESCHEDULE_TASK_NAND)
        dev->reschedule.rescheduleCb(dev->reschedule.ctx, RESCHEDULE_TASK_DEVICE_TIER0);
}

constexpr unsigned spitzAdcSelect = 20;
constexpr unsigned spitzPenIrq = 11;
constexpr unsigned spitzRemoteIrq = 13;
constexpr uint16_t spitzRemotePullup = 1u << 2;
constexpr unsigned spitzTouchSelect = 14;
constexpr unsigned spitzPowerKey = 95;
constexpr unsigned spitzHingeA = 97;
constexpr unsigned spitzHingeB = 96;
constexpr unsigned spitzPanelSelect = 53;

enum SpitzPanelRegister { panelResolution = 0, panelDuty = 2, panelPower = 4, panelPicture = 6 };
const auto spitzAdcChunk = ChunkType::spitzRegulator + 1;

static void spitzUpdateRemoteIrq(Device& board) {
    const bool pullup = spitzPcmciaGpioOutputs(board.spitzPcmcia, 1) & spitzRemotePullup;
    // An idle CE-RH2 holds the interrupt line low with the pull-up on.
    const bool idleRemote = board.spitzRemoteInserted && board.spitzRemoteKey == SpitzRemoteKey::none;
    pxaGpioSetState(board.gpio, spitzRemoteIrq, pullup && !idleRemote);
}

bool deviceSpitzRemoteInserted(const Device* dev) { return dev->spitzRemoteInserted; }

void deviceSetSpitzRemote(Device* dev, bool inserted) {
    if (dev->spitzRemoteInserted == inserted) return;
    dev->spitzRemoteInserted = inserted;
    dev->spitzRemoteKey = SpitzRemoteKey::none;
    spitzUpdateRemoteIrq(*dev);
}

bool deviceSetSpitzRemoteKey(Device* dev, SpitzRemoteKey key, bool down) {
    if (!dev->spitzRemoteInserted) return false;
    if (down) dev->spitzRemoteKey = key;
    else if (dev->spitzRemoteKey == key) dev->spitzRemoteKey = SpitzRemoteKey::none;
    spitzUpdateRemoteIrq(*dev);
    return true;
}

static uint8_t spitzRemoteSample(const Device& board) {
    // Each value sits inside the corresponding band in the Sharp driver.
    constexpr uint8_t ladder[] = {255, 220, 185, 151, 119, 89, 59, 27};
    return board.spitzRemoteInserted ? ladder[unsigned(board.spitzRemoteKey)] : 0;
}

static uint_fast16_t spitzSspTransfer(void* context, uint_fast8_t bits, uint_fast16_t value) {
    auto& board = *static_cast<Device*>(context);
    if (bits == 12 && pxaGpioGetState(board.gpio, spitzTouchSelect) == PxaGpioStateLow) {
        // Sharp clocks a command and reads the preceding ADS7846 conversion in each SSP word.
        const auto previous = board.spitzTouchSample;
        if (!board.spitzPenDown || !(value & 0x80)) {
            board.spitzTouchSample = 0;
        } else {
            switch ((value >> 4) & 7) {
                case 1: board.spitzTouchSample = board.spitzTouchY; break;
                case 3: board.spitzTouchSample = 1000; break;  // Z1
                case 4: board.spitzTouchSample = 1300; break;  // Z2
                case 5: board.spitzTouchSample = board.spitzTouchX; break;
                default: board.spitzTouchSample = 0; break;
            }
        }
        return previous;
    }

    if (bits == 8 && pxaGpioGetState(board.gpio, spitzPanelSelect) == PxaGpioStateLow) {
        const unsigned address = (value >> 5) & 7;
        board.spitzPanelRegisters[address] = value & 31;
        board.spitzPanelWritten |= 1u << address;
    }

    if (bits != 8 || pxaGpioGetState(board.gpio, spitzAdcSelect) != PxaGpioStateLow) return 0;

    switch (board.spitzAdcPhase) {
        case 1:
            board.spitzAdcPhase = 2;
            return board.spitzAdcSample >> 2;
        case 2:
            board.spitzAdcPhase = 0;
            return (board.spitzAdcSample & 3) << 6;
        default:
            if (value & 0x80) {
                const auto outputs = spitzPcmciaGpioOutputs(board.spitzPcmcia, 0);
                const uint8_t samples[] = {spitzRemoteSample(board), spitzBatteryCount(board),
                    uint8_t(outputs & spitzTemperatureOn ? 210 : 0),
                    uint8_t(board.battery.ExternalPower() ? 128 : 0)};
                const unsigned channel = ((value >> 6) & 1) | ((value >> 4) & 2);
                board.spitzAdcSample = samples[channel];
                board.spitzAdcPhase = 1;
            }
            return 0;
    }
}

struct Device *deviceSetup(enum DeviceType5 type, struct SocPeriphs *sp,
                           struct Reschedule reschedule, struct GetEmuTime getTime,
                           struct Keypad *kp, struct VSD *vsd, uint8_t *nandContent,
                           size_t nandSize, const struct MemoryBuffer *pageBuffer) {
    static const struct NandSpecs nandSpecs = {
        .bytesPerPage = 528,
        .blocksPerDevice = 2048,
        .pagesPerBlockLg2 = 5,
        .flags = NAND_FLAG_SAMSUNG_ADDRESSED_VIA_AREAS,
        .devIdLen = 2,
        .devId = {0xec, 0x75},
    };

    struct Device *dev;

    dev = (struct Device *)calloc(1, sizeof(*dev));
    if (!dev) ERR("cannot alloc device");

    dev->type = type;
    dev->battery = BatteryPower{};
    dev->wifi = nullptr;

    dev->gpio = sp->gpio;
    dev->vsd = vsd;

    struct Reschedule deviceReschedule = {.rescheduleCb = devicePrvReschedule, .ctx = dev};

    dev->reschedule = reschedule;

    if (type == deviceTypeSpitz) {
        dev->spitzNand = spitzNandInit(sp->mem, deviceReschedule, nandContent, nandSize, pageBuffer);
        dev->spitzPcmcia = spitzPcmciaInit(sp->mem, sp->gpio);
        dev->spitzSsp = sp->ssp2;
        dev->spitzI2c = sp->i2c;
        dev->spitzI2s = sp->i2s;
        dev->spitzCodec = new Wm8750;
        if (!dev->spitzCodec->Attach(dev->spitzI2c)) ERR("cannot attach the Spitz audio codec");
        dev->spitzBatteryDoorLocked = true;
        if (!pxaSspAddClient(dev->spitzSsp, spitzSspTransfer, dev))
            ERR("cannot attach the Spitz SSP devices");
        pxaGpioSetNotif(sp->gpio, spitzAdcSelect, [](void* context, uint32_t, bool, bool high) {
            if (high) static_cast<Device*>(context)->spitzAdcPhase = 0;
        }, dev);
        if (!dev->spitzNand || !dev->spitzPcmcia || !keypadAddGpioKey(kp, keyIdPower, spitzPowerKey, true) ||
            !keypadAddGpioKey(kp, keyIdHotSync, 16, true))
            ERR("cannot initialise Spitz board");
        spitzPcmciaObserveGpioOutputs(dev->spitzPcmcia, [](void* context, unsigned slot) {
            auto& board = *static_cast<Device*>(context);
            if (slot == 0) spitzApplyBattery(board);
            else {
                spitzUpdateRemoteIrq(board);
                if (board.spitzTransportObserver) board.spitzTransportObserver(board.spitzTransportContext);
            }
        }, dev);
        // Spitz drives 11 strobes high and reads seven sense lines with pull-downs.
        constexpr int8_t strobes[] = {88, 23, 24, 25, 26, 27, 52, 103, 107, 108, 114};
        constexpr int8_t senses[] = {12, 17, 91, 34, 36, 38, 39};
        for (unsigned i = 0; i < std::size(strobes); ++i) keypadDefineRow(kp, i, strobes[i]);
        for (unsigned i = 0; i < std::size(senses); ++i) keypadDefineCol(kp, i, senses[i]);
        for (unsigned sense = 0; sense < std::size(senses); ++sense) {
            for (unsigned strobe = 0; strobe < std::size(strobes); ++strobe) {
                const KeyId key = spitzKey(sense, strobe);
                if (key != keyInvalid && !keypadAddMatrixKey(kp, key, strobe, sense))
                    ERR("cannot wire a Spitz matrix key");
            }
        }
        keypadSetWakeGpio(kp, 0, spitzPowerKey);
        deviceSetSpitzLid(dev, false, false);
        pxaI2sAttach(sp->i2s, dev, [](void* context, uint32_t frame, bool playing, bool msb,
                                      uint32_t numerator, uint32_t denominator) -> uint32_t {
            const auto* board = static_cast<Device*>(context);
            board->spitzCodec->Output(frame, playing, msb, numerator, denominator,
                                       pxaGpioInputLevel(board->gpio, 116),
                                       spitzPcmciaGpioOutputs(board->spitzPcmcia, 0));
            return board->spitzCodec->InputFrame(spitzMicDemand(*board, numerator, denominator));
        }, [](void* context) { static_cast<Device*>(context)->spitzCodec->Boundary(); });
        sp->nand = spitzNandGetNand(dev->spitzNand);
        for (unsigned pin : {1u, 9u, spitzPenIrq, spitzUsbDeviceRole, 83u, 84u, 94u})
            pxaGpioSetState(sp->gpio, pin, true);
        return dev;
    }

    if (type == deviceTypeTG50) {
        dev->tg50 = tg50BoardSetup(sp, kp,reschedule,getTime);
        return dev;
    }

    if (type == DeviceType5::deviceTypeTX) {
        // Board wiring and flash geometry from upstream uARM's devicePalmTX.c.
        static const struct NandSpecs txNandSpecs = {
            .bytesPerPage = 2112, .blocksPerDevice = 1024, .pagesPerBlockLg2 = 6,
            .flags = NAND_HAS_SECOND_READ_CMD, .devIdLen = 5,
            .devId = {0xec, 0xf1, 0x00, 0x95, 0x40},
        };
        dev->bcm2035 = bcm2035Init(getTime,sp->gpio,true);
        sp->btUart=sp->uarts[3];
        bcm2035RegisterWithUart(dev->bcm2035,sp->btUart);
        for(unsigned pin:{17u,83u})pxaGpioSetNotif(sp->gpio,pin,
            [](void* context,uint32_t,bool,bool){bcm2035PowerChanged(static_cast<Bcm2035*>(context));},dev->bcm2035);
        dev->kpc = static_cast<PxaKpc *>(sp->kpc);
        dev->noRamMarker = 0;
        dev->nand = directNandInit(sp->mem, deviceReschedule, 0x06000000, 0x05000000,
                                  0x04000000, 0x00ffffff, sp->gpio, 18, &txNandSpecs,
                                  nandContent, nandSize, pageBuffer);
        if (!memRegionAdd(sp->mem, 0xa5fffffc, 4, txNoRamAccess, dev))
            ERR("cannot map TX RAM probe marker");
        // TX connects codec GPIO2/IRQ to PXA GPIO27; E2 uses GPIO3/PENDOWN directly.
        dev->wm9712L = wm9712LInit(sp->ac97, sp->gpio, -1, 27);
        if (!dev->nand || !dev->wm9712L || !dev->kpc) ERR("cannot initialize TX board");
        if (!keypadAddGpioKey(kp, keyIdPower, 0, false)) ERR("cannot initialize TX power key");
        wm9712LsetAuxVoltage(dev->wm9712L, WM9712LauxPinBmon, 4200 / 3);
        pxaGpioSetState(sp->gpio, 0, true);
        pxaGpioSetState(sp->gpio, 1, true);
        if (!keypadAddGpioKey(kp, keyIdHotSync, 10, false)) ERR("cannot initialize TX HotSync line");  // palmtx.h HOTSYNC_BUTTON_N
        pxaGpioSetState(sp->gpio, 14, true);  // SD absent
        pxaGpioSetState(sp->gpio, 37, true);
        pxaGpioSetState(sp->gpio, 13, true);  // no USB cable (palmtx.h USB_DETECT_N)
        // GPIO90, external power, follows the battery setting (deviceApplyBattery).
        sp->soc->wirelessDevice = std::make_unique<LibertasCF>(
            sp->mem, sp->gpio, reschedule, getTime, &sp->soc->networkSimulation);
        dev->wifi = sp->soc->wirelessDevice.get();
        sp->nand = directNandGetNand(dev->nand);
        return dev;
    }

    if (type == DeviceType5::deviceTypeZire72) {
        // XIP NOR, no NAND, Wi-Fi or serial. Pins from the small ROM's GPIO table and Linux palmz72.
        // BCM2035 on the BTUART with power and reset on GPIO17 and 83, as on the TX.
        dev->bcm2035 = bcm2035Init(getTime, sp->gpio, true);
        sp->btUart = sp->uarts[3];
        bcm2035RegisterWithUart(dev->bcm2035, sp->btUart);
        for (unsigned pin : {17u, 83u})
            pxaGpioSetNotif(sp->gpio, pin, [](void* context, uint32_t, bool, bool) {
                bcm2035PowerChanged(static_cast<Bcm2035*>(context));
            }, dev->bcm2035);
        dev->kpc = static_cast<PxaKpc *>(sp->kpc);
        // Direct key 7 (GPIO13, the ROM's key bit 0x2000) reads 1 when pressed; a stuck 1
        // makes a soft reset wait forever for the keys to come up.
        pxaKpcSetDirectKeysActiveHigh(dev->kpc, true);
        dev->noRamMarker = 0;
        dev->nand = nullptr;
        if (!memRegionAdd(sp->mem, 0x04000000, 0x04000000, zireEmptyBusAccess, dev))
            ERR("cannot map Zire 72 chip select 1");
        // OV9640 camera on the standard I2C behind the quick capture interface. The CameraDriver
        // drives PWDN on GPIO56 and pulses RESET on GPIO57 (both active high, 0x20106774 and
        // 0x201067b8). Linux palmz72 names GPIO91 the camera supply; the ROM holds it low as an
        // output whenever the camera runs, so low is taken as on.
        dev->ov9640 = ov9640Init(sp->i2c, sp->gpio, Ov9640Pins{91, false, 57, 56});
        dev->qci = pxaQciInit(sp->mem, sp->ic, sp->dma, dev->ov9640, getTime, reschedule);
        dev->wm9712L = wm9712LInit(sp->ac97, sp->gpio, -1, 27);
        if (!dev->wm9712L || !dev->kpc) ERR("cannot initialize Zire 72 board");
        // Built-in microphone on the codec's MIC1 input, recorded through the PCM-in slots
        // (DRCMR11) by Voice Memo.
        wm9712LEnableAdc(dev->wm9712L, getTime);
        // The power key is on the matrix and on GPIO0, which the DAL reads as pressed when low
        // (0x20093ce4). Held low, a soft reset waits forever for the key to come up.
        if (!keypadAddGpioKey(kp, keyIdPower, 0, false)) ERR("cannot initialize Zire 72 power key");
        wm9712LsetAuxVoltage(dev->wm9712L, WM9712LauxPinBmon, 4200 / 3);
        pxaGpioSetState(sp->gpio, 0, true);   // power key up
        pxaGpioSetState(sp->gpio, 1, true);   // reset button up
        pxaGpioSetState(sp->gpio, 14, true);  // SD absent
        pxaGpioSetState(sp->gpio, 15, false); // USB cable absent (VBUS, active high)
        pxaGpioSetState(sp->gpio, 37, true);  // low starts MfgTestExt's serial test loop
        pxaGpioSetState(sp->gpio, 94, false); // 32 MiB RAM
        pxaGpioSetState(sp->gpio, 99, true);  // low is external power (the charging bolt); deviceApplyBattery sets it
        // GPIO98 high cuts the card's supply (palmz72.h SD_POWER_N). The ROM switches it off
        // between card accesses and starts the card again from CMD0.
        pxaGpioSetNotif(sp->gpio, 98, [](void *context, uint32_t, bool oldState, bool newState) {
            if (newState && !oldState) vsdPowerOff(static_cast<VSD *>(context));
        }, vsd);
        sp->nand = nullptr;
        return dev;
    }

    dev->wm9712L = wm9712LInit(sp->ac97, sp->gpio, 50);
    if (!dev->wm9712L) ERR("Cannot init WM9712L");

    dev->bcm2035 = bcm2035Init(getTime,sp->gpio);

    dev->nand =
        directNandInit(sp->mem, deviceReschedule, 0x04000002UL, 0x04000004UL, 0x04000000UL,
                       0x00fffff9ul, sp->gpio, 79, &nandSpecs, nandContent, nandSize, pageBuffer);
    if (!dev->nand) ERR("Cannot init NAND");

    if (!keypadAddGpioKey(kp, keyIdHard1, 11, false)) ERR("Cannot init hardkey1 (datebook)\n");

    if (!keypadAddGpioKey(kp, keyIdHard2, 13, false)) ERR("Cannot init hardkey2 (address)\n");

    if (!keypadAddGpioKey(kp, keyIdHard3, 7, false)) ERR("Cannot init hardkey3 (ToDo)\n");

    if (!keypadAddGpioKey(kp, keyIdHard4, 5, false)) ERR("Cannot init hardkey4 (Memos)\n");

    if (!keypadAddGpioKey(kp, keyIdDown, 21, false)) ERR("Cannot init down key\n");

    if (!keypadAddGpioKey(kp, keyIdUp, 22, false)) ERR("Cannot init up key\n");

    if (!keypadAddGpioKey(kp, keyIdLeft, 19, false)) ERR("Cannot init left key\n");

    if (!keypadAddGpioKey(kp, keyIdRight, 20, false)) ERR("Cannot init right key\n");

    if (!keypadAddGpioKey(kp, keyIdSelect, 14, false)) ERR("Cannot init select key\n");

    if (!keypadAddGpioKey(kp, keyIdPower, 2, false))  // XXX: change this to tru eot boot with
                                                      // poewr key inverted. press ESC to release.
        ERR("Cannot init power key\n");

    sp->gpio = sp->gpio;

    pxaGpioSetState(sp->gpio, 1, true);  // reset button

    // HotSync line (palmte2.h HOTSYNC_BUTTON_N), high is not in cradle; low for over 5 s fires the FIQ watchdog.
    if (!keypadAddGpioKey(kp, keyIdHotSync, 4, false)) ERR("Cannot init HotSync line\n");

    pxaGpioSetState(sp->gpio, 35, true); // USB cable absent (palmte2.h USB_DETECT_N)
    pxaGpioSetState(sp->gpio, 40, true);  // no hotsync button pressed [??]

    pxaGpioSetState(sp->gpio, 52, true);  // no manufacturing test mode please

    wm9712LsetAuxVoltage(dev->wm9712L, WM9712LauxPinBmon, 4200 / 3);  // main battery is 4.2V

    deviceSetSdCardInserted(dev, false);

    // yes, this is really swapped
    sp->dbgUart = sp->uarts[3];  // BTUART
    sp->btUart = sp->uarts[1];   // HWUART

    sp->nand = directNandGetNand(dev->nand);
    bcm2035RegisterWithUart(dev->bcm2035, sp->btUart);
    for(unsigned pin:{32u,80u})pxaGpioSetNotif(sp->gpio,pin,
        [](void* context,uint32_t,bool,bool){bcm2035PowerChanged(static_cast<Bcm2035*>(context));},dev->bcm2035);

    return dev;
}

void devicePeriodic(struct Device *dev, uint32_t tier) {
    if (dev->tg50) {
        if (tier == DEVICE_PERIODIC_TIER0) tg50BoardPeriodic(dev->tg50);
        return;
    }
    if (dev->spitzNand) {
        if (tier == DEVICE_PERIODIC_TIER0) nandPeriodic(spitzNandGetNand(dev->spitzNand));
        return;
    }
    if (tier == DEVICE_PERIODIC_TIER0) {if(dev->nand)directNandPeriodic(dev->nand);
if(dev->qci)pxaQciPeriodic(dev->qci);
if(dev->wifi)dev->wifi->Tick();
}
}

void devicePcmPeriodic(struct Device *dev) { if (dev->wm9712L) wm9712Lperiodic(dev->wm9712L); }

void deviceTouch(struct Device *dev, int x, int y) {
    if (dev->tg50) { tg50BoardTouch(dev->tg50, x, y); return; }
    if (dev->spitzNand) {
        dev->spitzPenDown = x >= 0 && y >= 0;
        if (dev->spitzPenDown) {
            // Panel X increases down the scanout. Panel Y decreases toward the right.
            const int panelY = y == spitzTabletKeyY ? y : std::min(y, 639);
            dev->spitzTouchX = 250 + 3500 * panelY / 639;
            dev->spitzTouchY = 3750 - 3500 * std::min(x, 479) / 479;
        }
        pxaGpioSetState(dev->gpio, spitzPenIrq, !dev->spitzPenDown);
        return;
    }
    if (!dev->wm9712L) return;
    if (dev->type == DeviceType5::deviceTypeTX) {
        if (x < 0 || y < 0) {
            wm9712LsetPen(dev->wm9712L, -1, -1, 0);
        } else {
            // Both ADC axes decrease toward the right/bottom. The TX has no
            // calibration at first boot, so the panel follows the ROM's default
            // calibration: raw X 3749 to 313 across 0..319 and Y 3943 to 124
            // down 0..479, measured with the pen probe (tests/palm/pen68k-probe).
            const int adcX = 3749 - (3436 * x + 159) / 319;
            const int adcY = 3943 - (3819 * y + 239) / 479;
            // The TX driver treats high resistance readings (>=320) as pen up.
            wm9712LsetPen(dev->wm9712L, adcX, adcY, 100);
        }
        return;
    }
    if (dev->type == DeviceType5::deviceTypeZire72) {
        // Same panel scheme as the TX; the panel runs 320x440, the screen and then the silkscreen
        // below it. The panel follows the ROM's default calibration (PenResetCalibration): raw X
        // 3837 to 397 across 0..319 and Y 3676 to 420 down 0..439, measured with the pen probe.
        // The first-boot calibration then lands every tap where it was made.
        if (x < 0 || y < 0) wm9712LsetPen(dev->wm9712L, -1, -1, 0);
        else wm9712LsetPen(dev->wm9712L, 3837 - (3440 * x + 159) / 319,
                           3676 - (3256 * y + 219) / 439, 100);
        return;
    }
    wm9712LsetPen(dev->wm9712L, (x >= 0 && y >= 0) ? 280 + 10 * x : -1,
                  (x >= 0 && y >= 0) ? 210 + 7 * y : -1, 1000);
}

void deviceKey(struct Device *dev, uint32_t key, bool down) {
    if (dev->tg50) { tg50BoardKey(dev->tg50, key, down); return; }
    if (dev->type != DeviceType5::deviceTypeTX && dev->type != DeviceType5::deviceTypeZire72) return;
    // [MKOUT column][MKIN row]
    static const KeyId txMatrix[3][4] = {
        {keyIdPower, keyIdHard2, keyIdUp, keyIdRight},
        {keyIdHard1, keyIdHard3, keyInvalid, keyInvalid},
        {keyIdSelect, keyIdHard4, keyIdDown, keyIdLeft},
    };
    // Zire 72 keyBits table at NOR 0x6028.
    static const KeyId zireMatrix[3][4] = {
        {keyIdPower, keyIdHard1, keyIdUp, keyIdRight},
        {keyIdHard4, keyIdHard2, keyInvalid, keyInvalid},
        {keyIdSelect, keyIdHard3, keyIdDown, keyIdLeft},
    };
    if (dev->type == DeviceType5::deviceTypeZire72 && key == keyIdVoice) {
        pxaKpcDirectKeyChange(dev->kpc, 7, down);
        return;
    }
    const auto& matrix = dev->type == DeviceType5::deviceTypeTX ? txMatrix : zireMatrix;
    for (uint8_t col = 0; col < 3; col++)
        for (uint8_t row = 0; row < 4; row++)
            if (matrix[col][row] != keyInvalid && matrix[col][row] == key)
                pxaKpcMatrixKeyChange(dev->kpc, row, col, down);
}

bool deviceTaskRequired(struct Device *dev, uint32_t tier) {
    if (dev->tg50) return tier==DEVICE_PERIODIC_TIER0&&tg50BoardTaskRequired(dev->tg50);
    if (dev->spitzNand)
        return tier == DEVICE_PERIODIC_TIER0 && nandTaskRequired(spitzNandGetNand(dev->spitzNand));
    if (tier == DEVICE_PERIODIC_TIER0) {
        return (dev->nand && directNandTaskRequired(dev->nand))
            || (dev->qci && pxaQciTaskRequired(dev->qci))
            || (dev->wifi && dev->wifi->NeedsTick())
            ;
    } else {
        return true;
    }
}

void deviceSetAudioQueue(struct Device *dev, struct AudioQueue *audioQueue) {
    if (dev->tg50) { tg50BoardSetAudioQueue(dev->tg50, audioQueue); return; }
    if (dev->spitzCodec) dev->spitzCodec->SetQueue(audioQueue);
    if (dev->wm9712L) wm9712LsetAudioQueue(dev->wm9712L, audioQueue);
}

bool deviceI2sConnected() { return false; }

void deviceSetSdCardInserted(struct Device *dev, bool inserted) {
    if (dev->tg50) return;
    const unsigned pin = dev->spitzNand ? 9 : (dev->type == deviceTypeTX ||
                         dev->type == deviceTypeZire72 ? 14 : 10);
    pxaGpioSetState(dev->gpio, pin, !inserted);
}

bool devicePanelEnabled(Device *dev) {
    return !dev->tg50 || tg50BoardPanelEnabled(dev->tg50);
}
uint8_t deviceSpitzPanelBrightness(const Device* dev) {
    constexpr uint16_t backlightOn = 1u << 7, backlightCont = 1u << 6;
    constexpr uint8_t panelRails = 7, picturePowerDown = 4;
    const auto& regs = dev->spitzPanelRegisters;
    const uint8_t written = dev->spitzPanelWritten;
    if (((written & (1u << panelPower)) && (regs[panelPower] & panelRails) != panelRails) ||
        (regs[panelPicture] & picturePowerDown)) return 0;

    const uint16_t pins = spitzPcmciaGpioOutputs(dev->spitzPcmcia, 1);
    // Early boot and Z1 checkpoints can lack the timing generator's backlight write.
    if (!(written & (1u << panelDuty))) return 255;
    if (!(pins & backlightOn)) return 0;

    // The active-low CONT pin selects the high range. The stock steps skip duty values 17..32.
    const unsigned duty = regs[panelDuty] + (pins & backlightCont ? 0 : 16);
    return std::min(duty, 32u) * 255 / 32;
}

bool deviceSpitzPanelQvga(const Device* dev) {
    return dev->spitzPanelRegisters[panelResolution] & 1;
}

bool deviceHoldSwitchOn(Device *dev) {
    return dev->tg50 && tg50BoardHoldSwitchOn(dev->tg50);
}

void deviceSetSpitzLid(Device* dev, bool closed, bool swivelled) {
    pxaGpioSetState(dev->gpio, spitzHingeA, closed);
    pxaGpioSetState(dev->gpio, spitzHingeB, closed || swivelled);
}

bool deviceSpitzLidClosed(const Device* dev) {
    return pxaGpioInputLevel(dev->gpio, spitzHingeA);
}

bool deviceSpitzSwivelled(const Device* dev) {
    return !deviceSpitzLidClosed(dev) && pxaGpioInputLevel(dev->gpio, spitzHingeB);
}

enum DeviceType5 deviceGetType(struct Device *dev) { return dev->type; }

bool deviceMountMicrodrive(Device* dev, const char* path, bool writable, const char* mediaId) {
    return dev->spitzPcmcia && spitzPcmciaMount(dev->spitzPcmcia, path, writable, mediaId);
}

bool deviceFlushMicrodrive(const Device* dev) {
    return !dev->spitzPcmcia || spitzPcmciaFlushMicrodrive(dev->spitzPcmcia);
}

bool deviceForEachMicrodriveChange(const Device* dev, const CardBacking::OverlayFn& fn) {
    return !dev->spitzPcmcia || spitzPcmciaForEachMicrodriveChange(dev->spitzPcmcia, fn);
}

bool deviceWriteMicrodriveBase(Device* dev, uint64_t offset, const void* data, size_t count) {
    return dev->spitzPcmcia && spitzPcmciaWriteMicrodriveBase(dev->spitzPcmcia, offset, data, count);
}

bool deviceResetMicrodriveOverlay(Device* dev) {
    return !dev->spitzPcmcia || spitzPcmciaResetMicrodriveOverlay(dev->spitzPcmcia);
}

SpitzPcmcia* deviceSpitzPcmcia(Device* dev) { return dev->spitzPcmcia; }

bool deviceInsertCompactFlash(Device* dev, bool writeProtected) {
    return dev->spitzPcmcia && spitzPcmciaInsertCompactFlash(dev->spitzPcmcia, writeProtected);
}

void deviceEjectCompactFlash(Device* dev) {
    if (dev->spitzPcmcia) spitzPcmciaEjectCompactFlash(dev->spitzPcmcia);
}

bool deviceCompactFlashInserted(const Device* dev) {
    return dev->spitzPcmcia && spitzPcmciaCompactFlashInserted(dev->spitzPcmcia);
}

void deviceSetCompactFlashWriteProtect(Device* dev, bool locked) {
    if (dev->spitzPcmcia) spitzPcmciaSetCompactFlashWriteProtect(dev->spitzPcmcia, locked);
}

void deviceSetCompactFlashSwapOnLoad(Device* dev, bool allow) {
    if (dev->spitzPcmcia) spitzPcmciaSetCompactFlashSwapOnLoad(dev->spitzPcmcia, allow);
}

bool deviceSpitzSdPowered(const Device* dev) {
    return dev->spitzPcmcia && spitzPcmciaSdPowered(dev->spitzPcmcia);
}

void deviceObserveSpitzSdPower(Device* dev, void (*callback)(void*), void* context) {
    if (dev->spitzPcmcia) spitzPcmciaObserveSdPower(dev->spitzPcmcia, callback, context);
}

template <typename T>
void deviceSave(struct Device *dev, T &savestate) {
    if (deviceHasBluetooth(dev)) {
        deviceSyncBluetoothAddress(dev);
        auto chunk = savestate.GetChunk(ChunkType::bluetoothAddress, 0);
        if (!chunk) ERR("cannot allocate Bluetooth address state");
        SaveChunkHelper helper(*chunk);
        helper.DoBuffer(dev->bluetoothAddress.data(), dev->bluetoothAddress.size());
    }
    if (dev->spitzNand) {
        spitzNandSave(dev->spitzNand, savestate);
        spitzPcmciaSave(dev->spitzPcmcia, savestate);
        dev->spitzCodec->Save(savestate);
        auto chunk = savestate.GetChunk(spitzAdcChunk, 0);
        if (!chunk) ERR("cannot allocate Spitz ADC state");
        SaveChunkHelper helper(*chunk);
        helper.Do(typename decltype(helper)::Pack8() << dev->spitzAdcPhase << dev->spitzAdcSample);
        auto touch = savestate.GetChunk(ChunkType::spitzTouch, 0);
        if (!touch) ERR("cannot allocate Spitz touch state");
        SaveChunkHelper touchHelper(*touch);
        touchHelper.Do16(dev->spitzTouchSample);

        auto remote = savestate.GetChunk(ChunkType::spitzRemote, 0);
        if (!remote) ERR("cannot allocate Spitz remote state");
        SaveChunkHelper(*remote).DoBool(dev->spitzRemoteInserted);

        auto panel = savestate.GetChunk(ChunkType::spitzPanel, 0);
        if (!panel) ERR("cannot allocate Spitz panel state");
        SaveChunkHelper panelHelper(*panel);
        panelHelper.DoBuffer(dev->spitzPanelRegisters, sizeof(dev->spitzPanelRegisters))
            .Do8(dev->spitzPanelWritten);
        return;
    }
    if (dev->tg50) { tg50BoardSave(dev->tg50, savestate); return; }
    if (dev->type == DeviceType5::deviceTypeTX) {
        auto chunk = savestate.GetChunk(ChunkType::palmTx, 0);
        if (!chunk) ERR("cannot allocate TX board state");
        SaveChunkHelper helper(*chunk);
        helper.Do32(dev->noRamMarker);
    }
    if(dev->wifi)dev->wifi->Save(savestate);
    wm9712Lsave(dev->wm9712L, savestate);
    if (dev->ov9640) ov9640Save(dev->ov9640, savestate);
    if (dev->qci) pxaQciSave(dev->qci, savestate);
    if (dev->nand) directNandSave(dev->nand, savestate);
    if (dev->bcm2035) bcm2035Save(dev->bcm2035, savestate);
}

std::function<void()> deviceCaptureInput(struct Device *dev) {
    if (dev->tg50) return tg50BoardCaptureInput(dev->tg50);
    if (dev->spitzNand) {
        const auto x = dev->spitzTouchX, y = dev->spitzTouchY;
        const bool down = dev->spitzPenDown;
        const auto remoteKey = dev->spitzRemoteKey;
        return [dev, x, y, down, remoteKey] {
            dev->spitzRemoteKey = remoteKey;
            dev->spitzTouchX = x;
            dev->spitzTouchY = y;
            dev->spitzPenDown = down;
            pxaGpioSetState(dev->gpio, spitzPenIrq, !down);
        };
    }
    auto touch=wm9712CaptureInput(dev->wm9712L);auto radio=bcm2035CaptureInput(dev->bcm2035);
    return [touch,radio]{touch();if(radio)radio();};
}

template <typename T>
void deviceLoad(struct Device *dev, T &loader, bool preserveInput) {
    if (dev->spitzNand) {
        spitzNandLoad(dev->spitzNand, loader);
        spitzPcmciaLoad(dev->spitzPcmcia, loader, dev->resetting);
        dev->spitzCodec->Load(loader);
        if (loader.HasChunk(spitzAdcChunk)) {
            auto chunk = loader.GetChunkOrFail(spitzAdcChunk, 0, "Spitz ADC");
            if (!chunk) return;
            LoadChunkHelper helper(*chunk);
            helper.Do(typename decltype(helper)::Pack8() << dev->spitzAdcPhase << dev->spitzAdcSample);
            if (dev->spitzAdcPhase > 2) loader.NotifyError();
        } else {
            dev->spitzAdcPhase = dev->spitzAdcSample = 0;
        }
        dev->spitzTouchSample = 0;
        if (loader.HasChunk(ChunkType::spitzTouch)) {
            auto touch = loader.GetChunkOrFail(ChunkType::spitzTouch, 0, "Spitz touch");
            if (!touch) return;
            LoadChunkHelper touchHelper(*touch);
            touchHelper.Do16(dev->spitzTouchSample);
            if (touch->HasError() || dev->spitzTouchSample > 4095) loader.NotifyError();
        }
        memset(dev->spitzPanelRegisters, 0, sizeof(dev->spitzPanelRegisters));
        dev->spitzPanelWritten = 0;
        if (loader.HasChunk(ChunkType::spitzPanel)) {
            auto panel = loader.GetChunkOrFail(ChunkType::spitzPanel, 0, "Spitz panel");
            if (!panel) return;
            LoadChunkHelper panelHelper(*panel);
            panelHelper.DoBuffer(dev->spitzPanelRegisters, sizeof(dev->spitzPanelRegisters))
                .Do8(dev->spitzPanelWritten);
            for (uint8_t reg : dev->spitzPanelRegisters)
                if (reg > 31) loader.NotifyError();
            if (panel->HasError()) loader.NotifyError();
        }
        if (!dev->resetting) {
            dev->spitzRemoteInserted = false;
            if (loader.HasChunk(ChunkType::spitzRemote)) {
                auto remote = loader.GetChunkOrFail(ChunkType::spitzRemote, 0, "Spitz remote");
                if (!remote) return;
                LoadChunkHelper(*remote).DoBool(dev->spitzRemoteInserted);
                if (remote->HasError()) loader.NotifyError();
            }
        }
        if (!preserveInput || !dev->spitzRemoteInserted) dev->spitzRemoteKey = SpitzRemoteKey::none;
        spitzUpdateRemoteIrq(*dev);
        if (!preserveInput) dev->spitzPenDown = false;
        pxaGpioSetState(dev->gpio, spitzPenIrq, !dev->spitzPenDown);
        // GPIO90 retains the door position. Z1 kept it pulled high.
        if (!dev->resetting) dev->spitzBatteryDoorLocked = pxaGpioInputLevel(dev->gpio, spitzBatteryDoor);
        spitzApplyBattery(*dev);
        return;
    }
    // Sessions from before the address was kept have no chunk, and an unused radio saves
    // zeros; both keep the current address.
    if (deviceHasBluetooth(dev) && !dev->resetting && loader.HasChunk(ChunkType::bluetoothAddress)) {
        auto chunk = loader.GetChunkOrFail(ChunkType::bluetoothAddress, 0, "Bluetooth address");
        if (!chunk) return;
        std::array<uint8_t, 6> address{};
        LoadChunkHelper helper(*chunk);
        helper.DoBuffer(address.data(), address.size());
        if (address != std::array<uint8_t, 6>{}) {
            dev->bluetoothAddress = address;
            deviceApplyBluetoothAddress(dev);
        }
    }
    if (dev->tg50) {
        tg50BoardLoad(dev->tg50, loader, preserveInput);
        deviceApplyBattery(dev);
        return;
    }
    if (dev->type == DeviceType5::deviceTypeTX) {
        auto chunk = loader.GetChunkOrFail(ChunkType::palmTx, 0, "Palm TX board");
        if (!chunk) return;
        LoadChunkHelper helper(*chunk);
        helper.Do32(dev->noRamMarker);
    }
    // The Zire has the TX's PXA270 chunks but no board chunk or NAND; refuse TX and E2 state.
    if (dev->type == DeviceType5::deviceTypeZire72 &&
        (loader.HasChunk(ChunkType::palmTx) || loader.HasChunk(ChunkType::nand))) {
        loader.NotifyError();
        return;
    }
    if(dev->wifi)dev->wifi->Load(loader);
    wm9712Load(dev->wm9712L, loader, preserveInput);
    if (dev->ov9640 && !ov9640Load(dev->ov9640, loader)) return;
    if (dev->qci && !pxaQciLoad(dev->qci, loader)) return;
    if (dev->nand) directNandLoad(dev->nand, loader);
    if (dev->bcm2035) bcm2035Load(dev->bcm2035, loader);
    deviceApplyBattery(dev);
}

template void deviceSave<Savestate<ChunkType>>(Device *dev, Savestate<ChunkType> &savestate);
template void deviceSave<SavestateProbe<ChunkType>>(Device *dev,
                                                    SavestateProbe<ChunkType> &savestate);
template void deviceLoad<SavestateLoader<ChunkType>>(Device *dev,
                                                     SavestateLoader<ChunkType> &loader, bool);

void deviceNetworkPeriodic(Device* dev){
    if(dev->wifi)dev->wifi->Poll();
}

bool deviceSetBluetooth(Device* dev,const std::string& path,std::string& error){
    if(deviceHasBluetooth(dev))deviceSyncBluetoothAddress(dev);
    if(!path.empty() && deviceHasBluetooth(dev) && dev->bluetoothAddress==std::array<uint8_t,6>{}){
        dev->bluetoothAddress=deviceNewBluetoothAddress();deviceApplyBluetoothAddress(dev);
    }
    if(dev->tg50)return tg50BoardBluetooth(dev->tg50)->SetBackend(path,error);
    if(!dev->bcm2035){error="No Bluetooth controller on this board.";return false;}
    return bcm2035SetBackend(dev->bcm2035,path,error);
}
std::string deviceBluetoothState(Device* dev){if(dev->tg50){return tg50BoardBluetooth(dev->tg50)->State();}return bcm2035BackendState(dev->bcm2035);}
void deviceBeginBluetoothCheckpoint(Device* dev){if(dev->tg50){tg50BoardBluetooth(dev->tg50)->BeginCheckpoint();return;}bcm2035BeginCheckpoint(dev->bcm2035);}
void deviceEndBluetoothCheckpoint(Device* dev,bool sleeping){if(dev->tg50){tg50BoardBluetooth(dev->tg50)->EndCheckpoint(sleeping);return;}bcm2035EndCheckpoint(dev->bcm2035,sleeping);}
void deviceServiceBluetoothReset(Device* dev){if(dev->tg50)tg50BoardBluetooth(dev->tg50)->ServiceReset();}
void deviceSuspendBluetooth(Device* dev){if(dev->tg50){tg50BoardBluetooth(dev->tg50)->Suspend();return;}bcm2035Suspend(dev->bcm2035);}
void deviceBluetoothPinsChanged(Device* dev){if(dev->tg50){tg50BoardBluetooth(dev->tg50)->PinsChanged();return;}bcm2035PowerChanged(dev->bcm2035);}
void deviceBluetoothStateReplaced(Device* dev){if(dev->tg50){tg50BoardBluetooth(dev->tg50)->Reset();return;}bcm2035Suspend(dev->bcm2035);}
void deviceStopBluetooth(Device* dev){
    if(dev->tg50){tg50BoardBluetooth(dev->tg50)->Reset();return;}
    if(!dev->bcm2035)return;
    const bool tx=dev->type==deviceTypeTX || dev->type==deviceTypeZire72;
    for(unsigned pin:{tx?17u:32u,tx?83u:80u})pxaGpioSetNotif(dev->gpio,pin,nullptr,nullptr);
    bcm2035Stop(dev->bcm2035);dev->bcm2035=nullptr;
}

void deviceDestroy(Device *dev) {
    if (!dev) return;
    if (dev->spitzNand) {
        pxaSspRemoveClient(dev->spitzSsp, dev);
        pxaI2cDeviceRemove(dev->spitzI2c, dev->spitzCodec);
        delete dev->spitzCodec;
        pxaGpioSetNotif(dev->gpio, spitzAdcSelect, nullptr, nullptr);
        spitzPcmciaDeinit(dev->spitzPcmcia);
        spitzNandDestroy(dev->spitzNand);
    } else if (dev->tg50) {
        tg50BoardDestroy(dev->tg50);
    } else {
        return;
    }
    free(dev);
}

void deviceBeginReset(Device *dev) { dev->resetting = true; if (dev->tg50) tg50BoardBeginReset(dev->tg50); }
void deviceEndReset(Device *dev) {
    dev->resetting = false;
    if (dev->tg50) tg50BoardEndReset(dev->tg50);
    deviceApplyBattery(dev);
}

void deviceAudioAdvance(Device *dev, uint64_t ns) { if (dev->tg50) tg50BoardAudioAdvance(dev->tg50, ns); }

void deviceAudioSleepReset(Device *dev) { if (dev->tg50) tg50BoardAudioSleepReset(dev->tg50); }

bool deviceTg50IrPowerEnabled(Device *dev) {
    return dev && dev->tg50 && tg50BoardIrPowerEnabled(dev->tg50);
}
bool deviceSpitzIrPowerEnabled(const Device* dev) {
    // SCOOP #2 PA11 drives the transceiver's active-high shutdown input.
    constexpr uint16_t irShutdown = 1u << 1;
    return dev && dev->spitzPcmcia && spitzPcmciaGpioDrivenLow(dev->spitzPcmcia, 1, irShutdown);
}

void deviceSetTransportObserver(Device *dev, void (*observer)(void *), void *context) {
    if (dev && dev->tg50) tg50BoardSetTransportObserver(dev->tg50, observer, context);
    if (dev && dev->spitzPcmcia) {
        dev->spitzTransportObserver = observer;
        dev->spitzTransportContext = context;
    }
}

void deviceTg50SetUsbCable(Device *dev, bool connected) {
    if (dev && dev->tg50) tg50BoardSetUsbCable(dev->tg50, connected);
}
void deviceTg50SetAccessoryAux(Device *dev, uint16_t mV) {
    if (dev && dev->tg50) tg50BoardSetAccessoryAux(dev->tg50, mV);
}
void deviceTg50SetConnectorEmpty(Device *dev) {
    if (dev && dev->tg50) tg50BoardSetConnectorEmpty(dev->tg50);
}
uint16_t deviceTg50AccessoryAuxCode(Device *dev) { return dev && dev->tg50 ? tg50BoardAccessoryAuxCode(dev->tg50) : 0; }
bool deviceTg50UsbCableAttached(Device *dev) {
    return dev && dev->tg50 && tg50BoardUsbCableAttached(dev->tg50);
}
bool deviceTg50UsbListening(Device *dev) {
    return dev && dev->tg50 && tg50BoardUsbListening(dev->tg50);
}

MemoryStickController* deviceMemoryStick(Device* d){return d&&d->tg50?tg50BoardMemoryStick(d->tg50):nullptr;}

// The TG50's AK4534 or the WM9712L; the WM9712L ignores these unless its ADC model is on.
void deviceSetInputSource(Device *d, const AudioInputSource *s) {
    if (d->spitzCodec) d->spitzCodec->SetInputSource(s);
    else if (d->tg50) tg50BoardSetInputSource(d->tg50, s);
    else if (d->wm9712L) wm9712LSetInputSource(d->wm9712L, s);
}
void deviceFenceInput(Device *d, bool fence) {
    if (d->spitzCodec) d->spitzCodec->FenceInput(fence);
    else if (d->tg50) tg50BoardFenceInput(d->tg50, fence);
    else if (d->wm9712L) wm9712LFenceInput(d->wm9712L, fence);
    if (d->qci) pxaQciFenceSource(d->qci, fence);
}
void deviceDisarmInput(Device *d) {
    if (d->spitzCodec) d->spitzCodec->SetInputSource(nullptr);
    else if (d->tg50) tg50BoardDisarmInput(d->tg50);
    else if (d->wm9712L) wm9712LDisarmInput(d->wm9712L);
    if (d->qci) pxaQciDisarmSource(d->qci);
}

// The camera source follows the microphone's lifetime: off at start, dropped on sleep, reset,
// a load and leaving the app, fenced in checkpoints.
void deviceSetCameraSource(Device *d, const CameraSource *s) {
    if (d->qci) pxaQciSetSource(d->qci, s);
}
bool deviceHasCamera(Device *d) { return d && d->qci; }
void deviceCameraSleepReset(Device *d) { if (d->qci) pxaQciReset(d->qci); }

AudioInputDemand deviceInputDemand(Device *d) {
    if (d->spitzCodec) {
        uint32_t numerator{}, denominator{};
        pxaI2sGetRate(d->spitzI2s, numerator, denominator);
        return spitzMicDemand(*d, numerator, denominator);
    }
    if (d->tg50) return tg50BoardInputDemand(d->tg50);
    return d->wm9712L ? wm9712LInputDemand(d->wm9712L) : AudioInputDemand{};
}
