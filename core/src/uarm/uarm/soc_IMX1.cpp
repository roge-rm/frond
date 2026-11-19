#include "soc_IMX1.h"

#include "syscall_dispatch.h"

#include <memory>

#include "GunzipContext.h"

#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "device_configuration.h"
#include "imx1_bootstrap.h"
#include "HostUartPort.h"
#include "sdcard.h"
#include "vSD.h"
#include "zodiac_battery.h"
#include "zodiac_led.h"

namespace {
    constexpr uint64_t ticksPerSecond = ZodiacInput::ticksPerSecond;
    // The longest slice in guest time, CLK32 ticks (about 1 ms).
    constexpr uint64_t sliceTicks = 32;
    // Host time one Run call may take, so input and frames are served while the guest is busy. Run
    // stops for it only at a slice end, where stopping changes nothing in the machine.
    constexpr auto runHostLimit = std::chrono::milliseconds(12);
    // Every button pin, ports A and D. They read high while released.
    constexpr uint32_t buttonPinsA = 0x780, buttonPinsD = 0x7780;
    // D22 is the cradle's HotSync button, low while pressed (ROM key scan 20084af4 sets keyBitCradle on D22
    // low with the cradle attached; the sleep path arms it as a low level wake interrupt), so it idles high.
    constexpr uint32_t cradlePinD = 0x00400000;
    // B14, the charger input: the DAL's dock status (ROM 20089990) reads it low as external power and
    // charging. zodiacMachineOptions starts it low; ApplyBattery sets it high off the charger.
    constexpr uint32_t chargerPinB = 0x00004000;
    // B16, headphone detect: the DAL (20097d40) reads it high as headphones in, writes YMF768 reg 25 = 0 and
    // turns D27 low and D26 to the headphone bass setting; low (Frond's default) is the speaker.
    constexpr uint32_t headphonePinB = 0x00010000;
    // A11 is the accessory attach detect (high attached, the DAL's level interrupt at 200899d8), and A1 high with
    // accessory ID 0x10 makes it the cradle (20089bbc). Both low with nothing attached.
    constexpr uint32_t accessoryPinsA = 0x00000802;
    // The accessory ID is the graphics chip's aux ADC sample (200876b6, through the ROM's calibration at chip
    // globals +0x18c, which turns 900 into 2085 mV and 0 into 12 mV). Over 1796 mV is no accessory, -124 to
    // 124 mV the cradle. 900 is the sample every Zodiac state was made with.
    // ASSUMPTION "Zodiac cradle ID": the cradle's ID divider reads the bottom of the range, sample 0.
    constexpr uint32_t accessorySampleNone = 900, accessorySampleCradle = 0;

    // The machine as the input code reaches it. Every change is previewed, then committed.
    struct MachinePort : ZodiacInputPort {
        // cradleHeld keeps the release after a load or reset off D22 once the host's cradle button is down again.
        MachinePort(Imx1Machine* machine, bool cradleHeld) : machine(machine), cradleHeld(cradleHeld) {}
        bool setControl(unsigned control, bool down) override {
            return imx1MachineSetZodiacControl(machine, Imx1ZodiacControl(control), down, false) == Imx1BootstrapAccess::ok &&
                   imx1MachineSetZodiacControl(machine, Imx1ZodiacControl(control), down, true) == Imx1BootstrapAccess::ok;
        }
        bool releaseControls() override {
            const uint32_t pins[4] = {buttonPinsA, 0, 0, buttonPinsD | (cradleHeld ? 0 : cradlePinD)};
            return imx1MachineReplaceGpioInputs(machine, pins, pins, false) == Imx1BootstrapAccess::ok &&
                   imx1MachineReplaceGpioInputs(machine, pins, pins, true) == Imx1BootstrapAccess::ok;
        }
        bool replacePen(const Imx1AspPenInputs& pen) {
            return imx1MachineReplacePenInputs(machine, pen, false) == Imx1BootstrapAccess::ok &&
                   imx1MachineReplacePenInputs(machine, pen, true) == Imx1BootstrapAccess::ok;
        }
        bool setPen(bool down, int x, int y) override { return replacePen(zodiacPenInputs(down, x, y)); }
        bool liftPen() override {
            auto pen = imx1MachinePenInputs(machine);
            if (!pen.contactKnown || !pen.contactDown) return true;
            pen.contactDown = false;
            return replacePen(pen);
        }
        bool setStick(int x, int y) override {
            return imx1MachineSetZodiacStick(machine, x, y, false) == Imx1BootstrapAccess::ok &&
                   imx1MachineSetZodiacStick(machine, x, y, true) == Imx1BootstrapAccess::ok;
        }
        Imx1Machine* machine;
        bool cradleHeld;
    };

    // Previews then commits one set of GPIO input pins.
    bool replaceGpioInputs(Imx1Machine* machine, const uint32_t (&values)[4], const uint32_t (&masks)[4]) {
        return imx1MachineReplaceGpioInputs(machine, values, masks, false) == Imx1BootstrapAccess::ok &&
               imx1MachineReplaceGpioInputs(machine, values, masks, true) == Imx1BootstrapAccess::ok;
    }
}  // namespace

static_assert(ZodiacInput::controls == unsigned(Imx1ZodiacControl::count));
static_assert(zodiacControlFor(keyIdHome) == int(Imx1ZodiacControl::home) && zodiacControlFor(keyIdTriggerLeft) == int(Imx1ZodiacControl::triggerLeft) &&
              zodiacControlFor(keyIdTriggerRight) == int(Imx1ZodiacControl::triggerRight) && zodiacControlFor(keyIdPower) == int(Imx1ZodiacControl::power) &&
              zodiacControlFor(keyIdStickPress) == int(Imx1ZodiacControl::stickClick) && zodiacControlFor(keyIdHard2) == int(Imx1ZodiacControl::actionRight) &&
              zodiacControlFor(keyIdHard4) == int(Imx1ZodiacControl::actionUp) && zodiacControlFor(keyIdHard3) == int(Imx1ZodiacControl::actionLeft) &&
              zodiacControlFor(keyIdFunction) == int(Imx1ZodiacControl::function) && zodiacControlFor(keyIdBluetooth) == int(Imx1ZodiacControl::bluetooth) &&
              zodiacControlFor(keyIdHard1) == int(Imx1ZodiacControl::actionDown));

Imx1ProbeOptions zodiacMachineOptions(Imx1I2cBus* eeprom) {
    Imx1ProbeOptions o;
    o.bootstrapPeripherals = true; o.assumeMpllSettled = true; o.assumeSpllSettled = true;
    o.assumeI2cSingleMaster = true; o.i2cBus = eeprom; o.aspPx1HighFromReset = true;
    o.gpioBInputKnown = true; o.gpioBInput = 0x08100000;
    o.gpioDInputKnown = true; o.gpioDInput = buttonPinsD | cradlePinD;
    o.ramMiB = 128; o.cs4Port0InputKnown = true; o.cs4Port0Input = 0;
    o.rtcInitialTimeKnown = true; o.rtcInitialTime = {1, 12, 34, 56};
    o.aspUSampleKnown = true; o.aspUSample = 0x9b00;
    o.graphicsProfile = Imx1GraphicsProfile::virtual5744Compatible8MiB;
    o.gpioAInputMaskedKnown = true; o.gpioAInput = buttonPinsA; o.gpioAInputMask = 0xffffdfff;
    o.graphicsIrqA13 = true; o.graphicsRawAxesKnown = true; o.graphicsRawFirst = 512; o.graphicsRawSecond = 512;
    o.arm920IcacheIndexCompatibility = true; o.lcdcContrastOnly = true; o.cs4IndexedInputs.set(1, 0x0e, 0);
    o.mmcsdEmptySlot = true; o.virtualMpllRetune = true;
    o.graphicsMode2Route = Imx1GraphicsMode2Route::defaultAux;
    o.graphicsRawAuxKnown = true; o.graphicsRawAux = 900;
    o.graphicsRawMode1Known = true; o.graphicsRawMode1 = 712;
    o.graphicsVirtualA4SourceOver = true; o.graphicsVirtualPackedMono = true;
    return o;
}

// UART1 on the connector for HostSerial. The UART takes a character from the host each frame time
// (Imx1Uart::setSource) while the guest's CTS output is low, so Kick has nothing to do.
struct SocIMX1::SerialPort final : HostUartPort {
    explicit SerialPort(SocIMX1& soc) : soc(soc) {}
    SocIMX1& soc;
    Read read{};
    Write write{};
    Active active{};
    Detached detached{};
    void* context{};
    uint32_t disables{0};
    bool wasOn{false};

    Imx1MachineUartLine Line() const { return imx1MachineUartLine(soc.machine, 1); }
    bool Attach(Read r, Write w, Active a, Detached d, void* c) override {
        if (soc.halted) return false;
        if (imx1MachineSetUartPeer(soc.machine, 1, Transmit, Pending, Take, this) != Imx1BootstrapAccess::ok) return false;
        read = r; write = w; active = a; detached = d; context = c;
        wasOn = Line().receiverOn;
        return true;
    }
    void Detach() override {
        imx1MachineSetUartPeer(soc.machine, 1, nullptr, nullptr, nullptr, nullptr);
        if (!soc.halted) imx1MachineSetUartRts(soc.machine, 1, false);
        read = nullptr; write = nullptr; active = nullptr; detached = nullptr; context = nullptr;
    }
    // A load or reset made a new board without the peer: the host attaches again.
    void Replaced() {
        if (!context) return;
        const auto notify = detached;
        void* const host = context;
        Detach();
        if (notify) notify(host);
    }
    bool SetInputs(uint8_t asserted) override {
        return !soc.halted && imx1MachineSetUartRts(soc.machine, 1, asserted & hostUartCts) == Imx1BootstrapAccess::ok;
    }
    bool WantsInput() const override { return Line().ctsLow; }
    bool ReceiverEnabled() const override { return Line().receiverOn; }
    uint32_t DisableCount() const override {
        auto& self = const_cast<SerialPort&>(*this);
        const bool on = Line().receiverOn;
        if (self.wasOn && !on) ++self.disables;
        self.wasOn = on;
        return disables;
    }
    void Kick() override {}
    HostUartMode Mode() const override {
        const auto line = Line();
        return {uint32_t(line.bitsPerSecond), line.format, line.receiverOn, line.enabled};
    }
    static void Transmit(void* c, uint8_t character, bool) {
        auto& self = *static_cast<SerialPort*>(c);
        if (self.write) self.write(character, self.context);
    }
    static bool Pending(void* c) {
        auto& self = *static_cast<SerialPort*>(c);
        return self.active && self.active(self.context) && self.Line().ctsLow;
    }
    static bool Take(void* c, uint8_t& character) {
        auto& self = *static_cast<SerialPort*>(c);
        if (!self.read) return false;
        const auto value = self.read(self.context);
        if (value & ~0xffu) return false;
        character = uint8_t(value);
        return true;
    }
};

// UART2 in IrDA mode for HostInfrared.
// ASSUMPTION "Zodiac IR transceiver": powered while UART2 is enabled with IREN set; the ROM's shutdown
// pin, if it has one, is not modelled. Asleep is the i.MX1 in stop mode.
struct SocIMX1::InfraredPort final : HostInfraredPort {
    explicit InfraredPort(SocIMX1& soc) : soc(soc) {}
    SocIMX1& soc;
    SerialHostHooks hooks{};

    Imx1MachineUartLine Line() const { return imx1MachineUartLine(soc.machine, 2); }
    uint64_t Time() override { return soc.GetTime(); }
    bool SetHooks(const SerialHostHooks* next) override {
        if (!next) {
            hooks = {};
            imx1MachineSetUartPeer(soc.machine, 2, nullptr, nullptr, nullptr, nullptr);
            if (!soc.halted) imx1MachineSetUartRts(soc.machine, 2, false);
            return true;
        }
        if (soc.halted) return false;
        hooks = *next;
        return Wire();
    }
    // The transceiver has no RTS wire: with a peer the transmitter runs whatever UCR2's IRTS says.
    bool Wire() {
        return imx1MachineSetUartPeer(soc.machine, 2, Transmit, Pending, Take, this) == Imx1BootstrapAccess::ok &&
               imx1MachineSetUartRts(soc.machine, 2, true) == Imx1BootstrapAccess::ok;
    }
    void Replaced() {
        if (!hooks.context) return;
        if (hooks.replaced) hooks.replaced(hooks.context);
        if (soc.halted || !Wire()) hooks = {};
    }
    bool PowerEnabled() override { const auto line = Line(); return line.enabled && line.infrared; }
    bool ReceiveEnabled() override { const auto line = Line(); return line.receiverOn && line.infrared; }
    HostUartMode Mode() override {
        const auto line = Line();
        return {uint32_t(line.bitsPerSecond), line.format, line.receiverOn && line.infrared, line.enabled && line.infrared};
    }
    void Kick() override {}
    bool HostIoPaused() override { return false; }
    bool Sleeping() override { return soc.PowerSleeping(); }
    bool KeyboardAttached() override { return false; }
    static void Transmit(void* c, uint8_t character, bool infrared) {
        auto& self = *static_cast<InfraredPort*>(c);
        if (infrared && self.hooks.write) self.hooks.write(character, self.hooks.context);
    }
    static bool Pending(void* c) {
        auto& self = *static_cast<InfraredPort*>(c);
        return self.hooks.pending && self.Line().infrared && self.hooks.pending(self.hooks.context);
    }
    static bool Take(void* c, uint8_t& character) {
        auto& self = *static_cast<InfraredPort*>(c);
        if (!self.hooks.read) return false;
        const auto value = self.hooks.read(self.hooks.context);
        if (value & ~0xffu) return false;
        character = uint8_t(value);
        return true;
    }
};

SocIMX1::~SocIMX1() = default;

HostUartPort* SocIMX1::SerialHostPort() {
    if (!serialPort) serialPort = std::make_unique<SerialPort>(*this);
    return serialPort.get();
}

HostInfraredPort* SocIMX1::InfraredHostPort() {
    if (!infraredPort) infraredPort = std::make_unique<InfraredPort>(*this);
    return infraredPort.get();
}

// ASSUMPTION "Zodiac RTS idle": with nothing on the line the RTS inputs read inactive (high), so a
// send with CTS flow control waits (the ROM's write loop times out) and USR1 reads as on a real port.
void SocIMX1::ApplyUartLines() {
    if (halted) return;
    imx1MachineSetUartRts(machine, 1, false);
    imx1MachineSetUartRts(machine, 2, false);
    if (serialPort) serialPort->Replaced();
    if (infraredPort) infraredPort->Replaced();
}

SocIMX1* SocIMX1::Create(const uint8_t* romData, size_t size, std::string& error) {
    Imx1I2cReadWindow::Image image{};
    if (size > UINT32_MAX || !zodiacEepromIdentity(romData, size, image)) {
        error = "this Zodiac ROM keeps its identity data in a layout Frond doesn't know";
        return nullptr;
    }
    std::unique_ptr<SocIMX1> soc(new SocIMX1());
    soc->rom.assign(romData, romData + size);
    soc->eeprom = std::make_unique<Imx1I2cReadWindow>(image);
    Imx1ProbeResult failure{};
    soc->machine = imx1MachineCreate(soc->rom.data(), uint32_t(size), Imx1ProbeEntry::physicalFlash, false,
                                     zodiacMachineOptions(soc->eeprom.get()), nullptr, nullptr, &failure);
    if (!soc->machine) {
        error = std::string("the i.MX1 machine could not be made: ") + imx1ProbeStopName(failure.stop);
        return nullptr;
    }
    imx1MachineEnableYmf768(soc->machine);
    // Aborts and undefined instructions go to the guest's own handlers, as on a real Zodiac.
    // FROND_ZODIAC_STOP_ON_EXCEPTION stops the machine on them instead, for debugging.
    imx1MachineSetGuestExceptions(soc->machine, !std::getenv("FROND_ZODIAC_STOP_ON_EXCEPTION"));
    // The state before the first instruction, which Reset loads again.
    if (imx1MachineSaveState(soc->machine, soc->powerOn, "power-on") != Imx1StateStatus::ok) {
        error = "the i.MX1 machine's power-on state could not be kept";
        return nullptr;
    }
    // Guest calls need the ROM PACE; without it they stay unavailable.
    paceDiscoverZodiac(&soc->paceSites, zodiacRomBase, soc->rom.data(), soc->rom.size());
    soc->syscallDispatch = initSyscallDispatch(soc.get());
    soc->pcm.machine = soc->machine;
    imx1MachineSetPcmOutput(soc->machine, ZodiacPcmSink::word, &soc->pcm);
    soc->displayConfiguration = deviceConfigurationDisplayConfigForDevice(deviceTypeZodiac2, DisplayMode::mode_480x320);
    soc->ramSize = 128u << 20;
    soc->ApplyBattery();
    soc->StateChanged();
    soc->ApplyUartLines();
    // The owner keeps it for the life of the process, like the machine inside.
    return soc.release();
}

// The largest card the ROM can take: it has no CMD8, so only standard capacity, which vSD gives up to here.
static constexpr size_t zodiacCardSectors = size_t(2048) << 11;

void SocIMX1::OnSdInsert() {
    const size_t sectors = sdCardSectorCount();
    if (sectors > zodiacCardSectors) {
        std::cerr << "zodiac: the card is over 2 GB, which the Zodiac can't use; left out" << std::endl;
        return;
    }
    if (!vsd) vsd = vsdInit(sdCardRead, sdCardWrite, 0);
    vsdReset(vsd, uint32_t(sectors));
    cardIn = true;
    cardSync = true;
    SyncCard();
}

void SocIMX1::OnSdRemount() {
    cardSync = true;
    SyncCard();
}

void SocIMX1::OnSdEject() {
    cardIn = false;
    cardSync = true;
    SyncCard();
    if (vsd) vsdReset(vsd, 0);
}

void SocIMX1::SyncCard() {
    if (!cardSync || halted) return;
    VSD* const card = cardIn ? vsd : nullptr;
    if (imx1MachineSetMmcsdCard(machine, card, false) != Imx1BootstrapAccess::ok ||
        imx1MachineSetMmcsdCard(machine, card, true) != Imx1BootstrapAccess::ok) return;
    imx1MachineSetMmcsdCardId(machine, card ? sdCardGetId() : nullptr);
    cardSync = false;
}

bool SocIMX1::Sd2Insert() {
    auto& store = sdCardStore(1);
    if (card2In) return true;
    if (!store.initialized()) return false;
    const size_t sectors = store.sectorCount();
    if (sectors > zodiacCardSectors) {
        std::cerr << "zodiac: the second card is over 2 GB, which the Zodiac can't use; left out" << std::endl;
        return false;
    }
    if (!vsd2) vsd2 = vsdInit(sdCardStore1Read, sdCardStore1Write, 0);
    vsdReset(vsd2, uint32_t(sectors));
    card2In = true;
    card2Sync = true;
    SyncSecondCard();
    return true;
}

void SocIMX1::Sd2Eject() {
    if (!card2In) return;
    card2In = false;
    card2Sync = true;
    SyncSecondCard();
    if (vsd2) vsdReset(vsd2, 0);
}

void SocIMX1::SyncSecondCard() {
    if (!card2Sync || halted) return;
    VSD* const card = card2In ? vsd2 : nullptr;
    if (imx1MachineSetGraphicsSdCard(machine, card, false) != Imx1BootstrapAccess::ok ||
        imx1MachineSetGraphicsSdCard(machine, card, true) != Imx1BootstrapAccess::ok) return;
    card2Sync = false;
}

void SocIMX1::StateChanged() {
    // A load or reset ends a guest call.
    if (machine) EndGuestCall();
    pcm.restart();
    const auto status = imx1MachineGetStatus(machine);
    halted = status.status == Imx1MachineStatus::terminal || status.status == Imx1MachineStatus::stopped;
    framePending = false;
    redraw = true;
    // A load or reset brings the state's own D22 (high unless saved while held); the host's button is up.
    cradleDown = false;
    hotSyncTicks = 0;
    wasStopped = imx1MachineSystemStopped(machine);
    // The host's keys and pen are up now. What the machine still holds is let go in PumpInput.
    input.machineChanged();
    // A USB host on the bus starts again with the new machine.
    if (usbHostHooks.replaced) usbHostHooks.replaced(usbHostHooks.context);
}

UsbDevicePort SocIMX1::GetUsbPort() {
    using R = UsbPacketResult;
    static const UsbDevicePort::Operations ops{
        [](void* p) { return imx1MachineUsbBusReset(static_cast<SocIMX1*>(p)->machine); },
        [](void*) { return false; },  // no start-of-frame model
        [](void* p) { return imx1MachineUsbState(static_cast<SocIMX1*>(p)->machine).listening; },
        [](void* p) { return imx1MachineUsbState(static_cast<SocIMX1*>(p)->machine).listening; },
        [](void*) { return false; },
        [](void* p) { return imx1MachineUsbState(static_cast<SocIMX1*>(p)->machine).enableCount; },
        [](void* p) { return imx1MachineUsbState(static_cast<SocIMX1*>(p)->machine).address; },
        [](void*, void (*)(void*), void*) {},
        [](void* p, const uint8_t* data) { return imx1MachineUsbSetup(static_cast<SocIMX1*>(p)->machine, data); },
        [](void* p, const uint8_t* data, unsigned size) {
            return imx1MachineUsbOut(static_cast<SocIMX1*>(p)->machine, 0, data, size);
        },
        [](void* p, uint8_t* data, unsigned* size) {
            return imx1MachineUsbIn(static_cast<SocIMX1*>(p)->machine, 0, data, size, true);
        },
        [](void* p, uint8_t ep, const uint8_t* data, unsigned size) {
            ep &= 0x7f;
            return ep ? imx1MachineUsbOut(static_cast<SocIMX1*>(p)->machine, ep, data, size) : R::Unsupported;
        },
        [](void* p, uint8_t ep, uint8_t* data, unsigned* size) {
            ep &= 0x7f;
            return ep ? imx1MachineUsbIn(static_cast<SocIMX1*>(p)->machine, ep, data, size, true) : R::Unsupported;
        },
        [](void* p, uint8_t ep, unsigned* size) {
            ep &= 0x7f;
            return ep ? imx1MachineUsbIn(static_cast<SocIMX1*>(p)->machine, ep, nullptr, size, false) : R::Unsupported;
        },
        [](void* p, uint8_t ep, unsigned) {
            ep &= 0x7f;
            return !ep || imx1MachineUsbOutReady(static_cast<SocIMX1*>(p)->machine, ep);
        }};
    return {this, ops};
}

void SocIMX1::TickUsbHost() {
    if (!usbHostHooks.tick || UsbHostIoPaused()) return;
    if (!usbHostHooks.wake || usbHostHooks.wake(usbHostHooks.context) <= GetTime())
        usbHostHooks.tick(usbHostHooks.context, GetTime());
}

// The host has a tick due (a plug-in, a packet, a retry). Nothing in the machine would wake an idle guest
// for it, so this counts as an event and the guest's clock runs on to the slice end, where the host ticks.
bool SocIMX1::UsbHostEventPending() {
    if (!usbHostHooks.tick || UsbHostIoPaused()) return false;
    return !usbHostHooks.wake || usbHostHooks.wake(usbHostHooks.context) != UINT64_MAX;
}

// Loads the power-on state as a reset through RESET_IN, which keeps RAM, the clock controller and
// the RTC's time. Every button is released in that state.
// ASSUMPTION "Zodiac reset button": the button on the back is taken to pull the i.MX1's RESET_IN,
// the one reset the manual says keeps the SDRAM (24.6.3.1). The board's wiring is not in the manual
// or the ROM.
void SocIMX1::SetClockMultiplier(uint32_t num, uint32_t den) {
    SoC::SetClockMultiplier(num, den);
    // Run calls hand back with every cycle charged, so this only fails while one runs, which it never is here.
    if (machine) imx1MachineSetCpuClockMultiplier(machine, ClockMultiplierNum(), ClockMultiplierDen());
}

void SocIMX1::Reset() {
    SoC::Reset();
    const auto info = imx1MachineLoadState(machine, powerOn.data(), powerOn.size(), imx1StateExternalReset, "power-on");
    if (info.status != Imx1StateStatus::ok)
        std::cerr << "zodiac: reset failed: " << imx1StateStatusName(info.status) << std::endl;
    orientationTracker.Restart(imx1MachineStartups(machine));
    StateChanged();
    // After StateChanged, which works out halted again for the loaded state.
    ApplyBattery();
    ApplyUartLines();
    // The power-on state has the slot empty; a card still in reads as in from the start.
    if (cardIn) { vsdReset(vsd, uint32_t(sdCardSectorCount())); cardSync = true; SyncCard(); }
    if (card2In) vsdReset(vsd2, uint32_t(sdCardStore(1).sectorCount()));
    card2Sync = true;
    SyncSecondCard();
}

void SocIMX1::HardResetHolding(uint32_t holdMs) {
    Reset();
    input.jamAfterRelease(unsigned(zodiacControlFor(keyIdPower)), holdMs);
}

// The DAL shows its logo with the scan unturned and drawn for landscape; the OS starts in landscape.
unsigned SocIMX1::GuestOrientation() {
    const unsigned orientation = orientationTracker.Update(imx1MachineScanOrientation(machine), imx1MachineStartups(machine));
    // SysGetOrientation's numbers: landscape 2 is no turn, portrait 1 one, reverse landscape 4 two, reverse portrait 3 three.
    static constexpr int turnsFor[] = {-1, 1, 0, 3, 2};
    const int turns = orientation < 5 ? turnsFor[orientation] : -1;
    if (turns >= 0 && turns != stickTurns) {
        stickTurns = turns;
        PushStick();
    }
    return orientation;
}

// The YMF768's LED and rumble motor, and the power LED's colour (zodiac_led.h).
SoC::Attention SocIMX1::GuestAttention() {
    const auto outputs = imx1MachineAttention(machine);
    static_assert(zodiacLedGreen == ledGreen && zodiacLedRed == ledRed);
    Attention a;
    a.ledColour = outputs.power;
    a.vibrate = outputs.motor;
    a.led = outputs.led;
    return a;
}

// A stopped machine can't run again, so its state is not worth keeping over a session that can.
bool SocIMX1::Save() {
    if (halted) {
        std::cerr << "zodiac: not saved, the machine has stopped" << std::endl;
        return false;
    }
    return imx1MachineSaveState(machine, saved, "frond") == Imx1StateStatus::ok;
}

bool SocIMX1::LoadState(const uint8_t* data, size_t size, uint32_t flags) {
    const auto info = imx1MachineLoadState(machine, data, size, flags, "frond");
    if (info.status != Imx1StateStatus::ok) {
        std::cerr << "zodiac: saved state refused: " << imx1StateStatusName(info.status);
        if (info.section[0]) std::cerr << " in " << info.section;
        if (info.detail) std::cerr << " (" << info.detail << ")";
        if (info.status == Imx1StateStatus::coreRevisionMismatch)
            std::cerr << ", made by core revision " << info.coreRevision << ", this is " << imx1MachineStateCoreRevision;
        std::cerr << std::endl;
        return false;
    }
    SoC::Reset();
    // A saved machine is taken as past its boot.
    orientationTracker.Loaded(imx1MachineStartups(machine));
    StateChanged();
    ApplyBattery();
    ApplyUartLines();
    // A card in carries on from the state's card section, or starts idle without one. With no card in,
    // a card section puts the card store's card back in when it's the saved card (a session saved with
    // its card in). Another card than the saved one is a swap: it goes or stays out, the slot sees the
    // saved card taken out, and the card goes in later as a new one. A state without a card id takes
    // the store's card as the saved one.
    cardSync = true;
    cardSwapped = false;
    if (cardIn && info.cardSection && info.cardIdKnown &&
        std::strncmp(info.cardId, sdCardGetId(), SD_CARD_ID_MAX_LEN) != 0) {
        // The card in isn't the saved one, though it took on its card section: out as a swap, and
        // in again later as a new card.
        SdEject();
        cardSwapped = sdCardInitialized();
    } else if (cardIn) {
        if (!info.cardResumed) vsdReset(vsd, uint32_t(sdCardSectorCount()));
        SyncCard();
    } else {
        const bool stored = info.cardSection && !cardInserted && sdCardInitialized();
        if (stored && (!info.cardIdKnown || std::strncmp(info.cardId, sdCardGetId(), SD_CARD_ID_MAX_LEN) == 0)) {
            // It takes on the card section, a command or transfer under way included. The id keeps it
            // in through the session start's SdRemount.
            std::strncpy(cardId, sdCardGetId(), sizeof(cardId) - 1);
            cardId[sizeof(cardId) - 1] = '\0';
            SdInsert();
        } else {
            cardSwapped = stored;
            // The slot stays empty, which drops a card section no card took.
            SyncCard();
        }
    }
    // Slot 2's card stays in through the load (imx1MachineLoadState); this puts in or takes out the one asked for.
    // A card that didn't carry on from the state's SD2 section starts idle.
    if (card2In && !info.graphicsSdResumed) vsdReset(vsd2, uint32_t(sdCardStore(1).sectorCount()));
    card2Sync = true;
    SyncSecondCard();
    return true;
}

bool SocIMX1::Load(size_t savestateSize, void* savestateData) {
    // A machine started from the ROM has no state to load.
    if (!savestateSize || !savestateData) return true;
    return LoadState(static_cast<const uint8_t*>(savestateData), savestateSize, 0);
}

bool SocIMX1::LoadBringUpState(const uint8_t* data, size_t size) {
    std::unique_ptr<GunzipContext> gunzip;
    if (size > 2 && data[0] == 0x1f && data[1] == 0x8b) {
        gunzip = std::make_unique<GunzipContext>(data, size);
        while (gunzip->GetState() == static_cast<int>(GunzipContext::State::more)) gunzip->Continue();
        if (gunzip->GetState() != static_cast<int>(GunzipContext::State::done)) {
            std::cerr << "zodiac: saved state refused: bad gzip" << std::endl;
            return false;
        }
        data = static_cast<const uint8_t*>(gunzip->GetUncompressedData());
        size = gunzip->GetUncompressedSize();
    }
    return LoadState(data, size, imx1StateAllowBootInputs);
}

bool SocIMX1::SetBattery(const BatteryPower& value) {
    battery = value;
    ApplyBattery();
    return true;
}

// The level as the ASP U word the DAL's battery read takes (zodiac_battery.h); Maximum is the fixed 0x9b00
// every Zodiac state was made with. Charging pulls B14 low.
bool SocIMX1::ApplyBattery() {
    if (halted) return false;
    if (imx1MachineSetAspUInput(machine, zodiacBatterySample(battery.Percent())) != Imx1BootstrapAccess::ok)
        std::cerr << "zodiac: the battery level could not be set" << std::endl;
    // The charger and the headphones go in separately, so one refused pin doesn't hold back the other.
    uint32_t values[4]{}, masks[4]{};
    masks[1] = chargerPinB;
    values[1] = battery.charging ? 0 : chargerPinB;
    if (!replaceGpioInputs(machine, values, masks)) std::cerr << "zodiac: the charger input could not be set" << std::endl;
    const bool cradleSet = ApplyCradle();
    return ApplyHeadphones() && cradleSet;
}

bool SocIMX1::ApplyHeadphones() {
    if (halted) return false;
    uint32_t values[4]{}, masks[4]{};
    masks[1] = headphonePinB;
    values[1] = headphones ? headphonePinB : 0;
    if (replaceGpioInputs(machine, values, masks)) return true;
    std::cerr << "zodiac: the headphone input could not be set" << std::endl;
    return false;
}

bool SocIMX1::SetHeadphones(bool in) {
    if (halted) return false;
    const bool before = headphones;
    headphones = in;
    if (ApplyHeadphones()) return true;
    headphones = before;
    return false;
}

// Changes the inputs only where they differ, so a machine with no cradle runs exactly as before. Attaching
// sets the ID before A11, detaching drops A11 first, so the ROM never reads the ID of the other state.
bool SocIMX1::ApplyCradle() {
    if (halted) return false;
    const bool in = CradleAttached();
    const uint32_t sample = in ? accessorySampleCradle : accessorySampleNone;
    uint32_t values[4]{}, masks[4]{};
    masks[0] = accessoryPinsA;
    values[0] = in ? accessoryPinsA : 0;
    const bool pinsSet = (imx1MachineGpioInput(machine, 0) & accessoryPinsA) == values[0];
    const bool sampleSet = imx1MachineGraphicsAux(machine) == sample;
    if (pinsSet && sampleSet) return true;
    const auto setSample = [&] {
        return sampleSet || (imx1MachineSetGraphicsAux(machine, sample, false) == Imx1BootstrapAccess::ok &&
                             imx1MachineSetGraphicsAux(machine, sample, true) == Imx1BootstrapAccess::ok);
    };
    const bool ok = in ? setSample() && (pinsSet || replaceGpioInputs(machine, values, masks))
                       : (pinsSet || replaceGpioInputs(machine, values, masks)) && setSample();
    if (!ok) std::cerr << "zodiac: the cradle inputs could not be set" << std::endl;
    return ok;
}

bool SocIMX1::SetCradle(bool attached) { return SetCradleInputs(cradle, attached); }
bool SocIMX1::SetUsbCable(bool attached) { return SetCradleInputs(usbCable, attached); }

bool SocIMX1::SetCradleInputs(bool& setting, bool attached) {
    if (halted) return false;
    const bool before = setting;
    setting = attached;
    if (ApplyCradle()) return true;
    setting = before;
    ApplyCradle();
    return false;
}

// D22 low while held. Without the cradle attached (A11 high and the accessory checks of 20089bae/20089bbc)
// the awake ROM's key scan (20084af4) ignores it; asleep, its low level interrupt wakes the Zodiac.
bool SocIMX1::SetCradleButton(bool down) {
    if (halted) return false;
    uint32_t values[4]{}, masks[4]{};
    masks[3] = cradlePinD;
    values[3] = down ? 0 : cradlePinD;
    if (!replaceGpioInputs(machine, values, masks)) return false;
    cradleDown = down;
    hotSyncTicks = 0;
    return true;
}

bool SocIMX1::PressHotSync() {
    if (halted || cradleDown || !SetCradleButton(true)) return false;
    hotSyncTicks = ticksPerSecond / 2;
    return true;
}

uint64_t SocIMX1::GetTime() {
    const auto time = imx1MachineGetStatus(machine).time;
    return uint64_t(__uint128_t(time.ticks) * 1000000000 / ticksPerSecond);
}

uint64_t SocIMX1::YmfLastReadyReadNs() const {
    const auto time = imx1MachineYmfLastReadyRead(machine);
    return uint64_t((__uint128_t(time.ticks) * time.denominator + time.numerator) * 1000000000 /
                    (__uint128_t(ticksPerSecond) * time.denominator));
}

bool SocIMX1::LcdEnabled() {
    uint32_t width = 0, height = 0;
    return imx1MachinePeekFrame(machine, width, height) != nullptr;
}

uint32_t* SocIMX1::GetPendingFrame(uint32_t& firstDirtyLine, uint32_t& lastDirtyLine) {
    uint32_t width = 0, height = 0, first = 0, last = 0;
    const uint32_t* panel = imx1MachineAcquireFrame(machine, width, height, first, last);
    const bool acquired = panel != nullptr;
    // After a load or a reset the frame the machine already has is shown again.
    if (!panel && redraw) panel = imx1MachinePeekFrame(machine, width, height);
    if (panel && width == zodiacPanelWidth && height == zodiacPanelHeight) {
        std::copy(panel, panel + view.size(), view.begin());
        framePending = true;
    }
    if (acquired) imx1MachineAcknowledgeFrame(machine);
    if (redraw) {
        // The redraw a resize or rotation asked for is done, so the next one is noticed.
        redraw = false;
        ClearFramebufferDirty();
    }
    if (!framePending) return nullptr;
    firstDirtyLine = 0;
    lastDirtyLine = zodiacPanelHeight - 1;
    return view.data();
}

void SocIMX1::AnalogStick(float x, float y) {
    stickHostX = x;
    stickHostY = y;
    PushStick();
}

void SocIMX1::SetStickHostTurns(int turns) {
    turns &= 3;
    if (turns == stickHostTurns) return;
    stickHostTurns = turns;
    PushStick();
}

void SocIMX1::PushStick() {
    float x = stickHostX, y = stickHostY;
    zodiacStickForView(x, y, zodiacStickTotalTurns(stickTurns, stickHostTurns), x, y);
    input.stick(zodiacStickOffset(x), zodiacStickOffset(y));
}

void SocIMX1::JamKey(enum KeyId key, uint32_t durationMsec) {
    const int control = zodiacControlFor(key);
    if (control < 0 || halted) return;
    MachinePort port(machine, cradleDown);
    input.jam(port, unsigned(control), durationMsec);
}

// Hands queued host input to the machine. An input the machine refuses just now stays queued.
void SocIMX1::PumpInput() {
    MachinePort port(machine, cradleDown);
    input.hostTime(uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()));
    input.pump(port);
    while (keyEventQueue->GetSize()) {
        const KeyEvent event = keyEventQueue->Peek();
        const int control = zodiacControlFor(event.key);
        // A hold is measured between the host's own events, however late they are taken.
        input.hostTime(event.hostMsec);
        if (control >= 0 && !input.key(port, unsigned(control), event.keyDown)) break;
        keyEventQueue->Pop();
    }
    while (penEventQueue->GetSize()) {
        const PenEvent event = penEventQueue->Peek();
        input.hostTime(event.hostMsec);
        if (!input.pen(port, event.penDown, event.x, event.y)) break;
        penEventQueue->Pop();
    }
}

std::string SocIMX1::StopText() const {
    if (!halted) return {};
    const auto result = imx1MachineInspect(machine).result;
    char line[200];
    std::snprintf(line, sizeof line, "%s (%s) at pc %08x, address %08x, value %08x", imx1ProbeStopName(result.stop),
                  imx1BootstrapAccessName(result.peripheralStatus), result.trace.pc, result.address, result.value);
    std::string text = line;
    // A refused graphics batch (the ring submit at 16010220) also says what it was.
    const auto refusal = imx1MachineGraphicsRefusal(machine);
    if (refusal.name && result.address == 0x16010220) {
        std::snprintf(line, sizeof line, ", %s %08x", refusal.name, refusal.value);
        text += line;
    }
    return text;
}

// Runs the guest for at least maxCycles / cyclesPerSecond of its own time, or for a short stretch of host
// time when it can't keep up, and returns the guest time that passed in the same cycle units.
// Each slice's bounds come from the machine's own counts where it starts: retired and work up to their next
// whole multiple of the slice size, and the time up to the next whole sliceTicks. A slice starts where the
// last one ended, so every slice end, and the board charge it makes, is set by the guest alone. An instruction,
// a CS5 wait, a PLL stall or a delay skip can carry a slice a little past its bound. Run only stops at a slice
// end, so where the calls are cut and how busy the host is change nothing in the machine. A run can pass the
// time asked for by up to sliceTicks (about 1 ms) and that carry; the time returned counts it.
bool SocIMX1::RunToStop(uint64_t ticks) {
    if (halted) return false;
    PumpInput();
    const auto start = imx1MachineGetStatus(machine);
    auto now = start;
    Imx1MachineBounds bounds;
    for (;;) {
        bounds.retired = 4000;
        bounds.work = 20000;
        bounds.deadline = {(now.time.ticks / sliceTicks + 1) * sliceTicks, 0, 1};
        const auto run = imx1MachineRunBounded(machine, bounds);
        now = run.total;
        if (run.reason == Imx1MachineYield::terminal || run.reason == Imx1MachineYield::stopped ||
            run.reason == Imx1MachineYield::invalidBounds) {
            halted = true;
            std::cerr << "zodiac: the machine stopped in a guest call: " << StopText() << std::endl;
            break;
        }
        if (run.reason == Imx1MachineYield::pcStop || run.reason == Imx1MachineYield::waiting) break;
        if (now.time.ticks - start.time.ticks >= ticks) break;
    }
    input.advance(now.time.ticks - start.time.ticks);
    return !halted && imx1MachineAtPcStop(machine);
}

void SocIMX1::EndGuestCall() {
    imx1MachineClearPcStop(machine);
    guestCall = false;
}

bool SocIMX1::RunToPaceSyscall(uint16_t trap, uint64_t maxCycles, uint64_t cyclesPerSecond) {
    if (!paceSites.syscallSite || halted || !cyclesPerSecond) return false;
    const uint64_t ticks = std::max<uint64_t>(1, uint64_t(__uint128_t(maxCycles) * ticksPerSecond / cyclesPerSecond));
    const uint64_t start = imx1MachineGetStatus(machine).time.ticks;
    if (!imx1MachineSetPcStop(machine, paceSites.syscallSite)) return false;
    guestCall = true;
    for (;;) {
        const uint64_t passed = imx1MachineGetStatus(machine).time.ticks - start;
        if (passed >= ticks || !RunToStop(ticks - passed)) break;
        uint32_t registers[16];
        if (GuestRegisters(registers) && (registers[1] & 0xffff) == trap) return true;
        imx1MachineLeavePcStop(machine);
    }
    EndGuestCall();
    return false;
}

bool SocIMX1::ExecuteInjected(uint64_t maxCycles, uint64_t cyclesPerSecond) {
    if (!guestCall || halted || !cyclesPerSecond) return false;
    const uint64_t ticks = std::max<uint64_t>(1, uint64_t(__uint128_t(maxCycles) * ticksPerSecond / cyclesPerSecond));
    const uint64_t start = imx1MachineGetStatus(machine).time.ticks;
    bool returned = false;
    for (;;) {
        // A hook may make injected calls of its own, which set the stops for themselves.
        const uint32_t stops[] = {zodiacInjectedReturn, paceSites.trap0Site};
        if (!imx1MachineSetPcStops(machine, stops, trap0Hook ? 2 : 1)) break;
        const uint64_t before = imx1MachineGetStatus(machine).time.ticks;
        if (before - start >= ticks) break;
        if (!RunToStop(ticks - (before - start))) {
            // A wait for an interrupt ends the run early; go on while guest time moves.
            if (halted || imx1MachineGetStatus(machine).time.ticks == before) break;
            continue;
        }
        uint32_t registers[16];
        if (!GuestRegisters(registers)) break;
        if (registers[15] == zodiacInjectedReturn) {
            returned = true;
            break;
        }
        if (!trap0Hook()) imx1MachineLeavePcStop(machine);
    }
    // The caller puts the registers back on the syscall site, where the machine stops again.
    if (!halted) imx1MachineSetPcStop(machine, paceSites.syscallSite);
    return returned;
}

uint64_t SocIMX1::Run(uint64_t maxCycles, uint64_t cyclesPerSecond) {
    // A run after a guest call lets the guest go on from the syscall site.
    if (guestCall) EndGuestCall();
    if (halted || !cyclesPerSecond) {
        keyEventQueue->Clear();
        penEventQueue->Clear();
        return maxCycles;
    }
    const auto before = imx1MachineGetStatus(machine);
    SyncCard();
    SyncSecondCard();
    PumpInput();
    const uint64_t wanted = std::max<uint64_t>(1, uint64_t(__uint128_t(maxCycles) * ticksPerSecond / cyclesPerSecond));
    const Imx1ReferenceTime until{before.time.ticks + wanted, 0, 1};
    // Fast runs longer slices: each slice end charges the whole board.
    const bool fast = imx1MachineFastEngine(machine);
    const uint64_t sliceRetired = fast ? 8000 : 4000, sliceWork = fast ? 40000 : 20000;
    Imx1MachineBounds bounds;
    const auto started = std::chrono::steady_clock::now();
    bool waiting = false;
    auto now = before;
    for (;;) {
        bounds.retired = uint32_t(sliceRetired - now.retired % sliceRetired);
        bounds.work = sliceWork - now.work % sliceWork;
        // The time asked for is checked between slices. The time bound keeps slices short in stop mode,
        // where each idle period is a CLK32 tick, so input reaches a sleeping guest promptly.
        bounds.deadline = {(now.time.ticks / sliceTicks + 1) * sliceTicks, 0, 1};
        imx1MachineSetUsbHostPending(machine, UsbHostEventPending());
        const auto run = imx1MachineRunBounded(machine, bounds);
        if (run.reason == Imx1MachineYield::terminal || run.reason == Imx1MachineYield::stopped ||
            run.reason == Imx1MachineYield::invalidBounds) {
            halted = true;
            std::cerr << "zodiac: the machine stopped: " << StopText() << ", after "
                      << imx1MachineGetStatus(machine).retired << " instructions" << std::endl;
            break;
        }
        if (run.reason == Imx1MachineYield::waiting) { waiting = true; break; }
        now = run.total;
        // The USB host runs at slice ends, on the guest's clock.
        TickUsbHost();
        if (now.time.compare(until) >= 0) break;
        if (hostTimeLimit && std::chrono::steady_clock::now() - started >= runHostLimit) break;
    }
    const uint64_t elapsed = imx1MachineGetStatus(machine).time.ticks - before.time.ticks;
    // Nothing can wake a waiting guest but input, so host time just passes. Its own clock stands
    // still then, and the input holds are timed on the time asked for. A running guest's holds are
    // timed on its own clock alone, however slowly it runs.
    input.advance(waiting ? std::max(wanted, elapsed) : elapsed);
    if (hotSyncTicks) {
        const uint64_t passed = waiting ? std::max(wanted, elapsed) : elapsed;
        hotSyncTicks = passed >= hotSyncTicks ? 0 : hotSyncTicks - passed;
        // A release the machine refuses just now is tried again after the next run.
        if (!hotSyncTicks && !SetCradleButton(false)) hotSyncTicks = 1;
    }
    const bool stopped = imx1MachineSystemStopped(machine);
    if (wasStopped && !stopped) ++wakes;
    wasStopped = stopped;
    if (waiting || halted) return maxCycles;
    const __uint128_t credit = __uint128_t(elapsed) * cyclesPerSecond + creditRemainder;
    creditRemainder = uint64_t(credit % ticksPerSecond);
    return uint64_t(std::min<__uint128_t>(credit / ticksPerSecond, UINT64_MAX));
}

// Facts of the 1.1 ROM: the DAL's doze routine, which the idle loop calls in SVC mode with IRQs off, in Thumb, and
// EvtWakeup's Boot table glue (entry 0x2e8, as on the T|X and Zire 72) and its Thumb code, which sets the event
// manager's wake flag and wakes the UI task.
namespace {
constexpr uint32_t zodiacDoze = 0x20094534, zodiacEvtWakeupGlue = 0x200706c0, zodiacEvtWakeup = 0x2005d754;
constexpr uint16_t zodiacDozeCode[] = {0xb570, 0x4c30, 0x2501, 0x4648, 0x6800, 0x6800, 0x4e15, 0x1836};
constexpr uint32_t zodiacEvtWakeupGlueCode[] = {0xe59fc004, 0xe08cc00f, 0xe12fff1c, 0xfffed089};
constexpr uint16_t zodiacEvtWakeupCode[] = {0xb580, 0x4649, 0x6809, 0x6849, 0x483f, 0x1840, 0x6bc1, 0x2001,
                                            0x7008, 0xf017, 0xfd1b, 0xbc80, 0xbc08, 0x4718};

bool romHas(const std::vector<uint8_t>& rom, uint32_t address, const void* code, size_t size) {
    return address >= zodiacRomBase && address - zodiacRomBase + size <= rom.size() &&
           !std::memcmp(rom.data() + (address - zodiacRomBase), code, size);
}
}  // namespace

// Runs to the idle loop's next doze and calls EvtWakeup there in its place, as an interrupt routine would, so a
// UI task waiting with no timeout gets a nilEvent. EvtWakeup returns to the doze's caller. At most half a second
// of guest time; false with nothing changed if the ROM differs or the guest doesn't idle.
bool SocIMX1::WakeIdle() {
    if (halted || guestCall || !romHas(rom, zodiacDoze, zodiacDozeCode, sizeof(zodiacDozeCode)) ||
        !romHas(rom, zodiacEvtWakeupGlue, zodiacEvtWakeupGlueCode, sizeof(zodiacEvtWakeupGlueCode)) ||
        !romHas(rom, zodiacEvtWakeup, zodiacEvtWakeupCode, sizeof(zodiacEvtWakeupCode)) ||
        !imx1MachineSetPcStop(machine, zodiacDoze))
        return false;
    const bool stopped = RunToStop(ticksPerSecond / 2);
    imx1MachineClearPcStop(machine);
    uint32_t registers[16], cpsr, boot = 0, hook = 0;
    // Thumb, SVC mode, IRQs off, with the system r9: its Boot table holds the glue, unless a patch replaced it,
    // which is skipped like the T|X's wake does.
    if (!stopped || !GuestRegisters(registers, &cpsr) || registers[15] != (zodiacDoze | 1) ||
        (cpsr & 0xbf) != 0xb3 || !ReadGuest(registers[9] - 8, &boot, 4) || !ReadGuest(boot + 0x2e8, &hook, 4))
        return false;
    if (hook != zodiacEvtWakeupGlue)
        std::cerr << "zodiac: EvtWakeup is patched, calling its ROM routine" << std::endl;
    return SetGuestRegister(15, zodiacEvtWakeup | 1);
}
