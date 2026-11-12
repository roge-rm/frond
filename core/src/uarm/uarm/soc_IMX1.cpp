#include "soc_IMX1.h"

#include <memory>

#include "GunzipContext.h"

#include <chrono>
#include <climits>
#include <cstdio>
#include <iostream>

#include "device_configuration.h"
#include "imx1_bootstrap.h"
#include "sdcard.h"
#include "vSD.h"

namespace {
    constexpr uint64_t ticksPerSecond = ZodiacInput::ticksPerSecond;
    // Host time one Run call may take, so input and frames are served while the guest is busy.
    constexpr auto runHostLimit = std::chrono::milliseconds(12);
    // Every button pin, ports A and D. They read high while released.
    constexpr uint32_t buttonPinsA = 0x780, buttonPinsD = 0x7780;
    // ASSUMPTION "cradle line D22 idles high": the sleep path arms D22 as a low level interrupt with the
    // other wake lines (cradle or cable detect by the ROM's use), so off the cradle it has to read high.
    constexpr uint32_t cradlePinD = 0x00400000;

    // The machine as the input code reaches it. Every change is previewed, then committed.
    struct MachinePort : ZodiacInputPort {
        explicit MachinePort(Imx1Machine* machine) : machine(machine) {}
        bool setControl(unsigned control, bool down) override {
            return imx1MachineSetZodiacControl(machine, Imx1ZodiacControl(control), down, false) == Imx1BootstrapAccess::ok &&
                   imx1MachineSetZodiacControl(machine, Imx1ZodiacControl(control), down, true) == Imx1BootstrapAccess::ok;
        }
        bool releaseControls() override {
            const uint32_t pins[4] = {buttonPinsA, 0, 0, buttonPinsD | cradlePinD};
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
    };
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
    // The state before the first instruction, which Reset loads again.
    if (imx1MachineSaveState(soc->machine, soc->powerOn, "power-on") != Imx1StateStatus::ok) {
        error = "the i.MX1 machine's power-on state could not be kept";
        return nullptr;
    }
    soc->pcm.machine = soc->machine;
    imx1MachineSetPcmOutput(soc->machine, ZodiacPcmSink::word, &soc->pcm);
    soc->displayConfiguration = deviceConfigurationDisplayConfigForDevice(deviceTypeZodiac2, DisplayMode::mode_480x320);
    soc->ramSize = 128u << 20;
    soc->StateChanged();
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
    cardSync = false;
}

void SocIMX1::StateChanged() {
    pcm.restart();
    const auto status = imx1MachineGetStatus(machine);
    halted = status.status == Imx1MachineStatus::terminal || status.status == Imx1MachineStatus::stopped;
    framePending = false;
    redraw = true;
    // The host's keys and pen are up now. What the machine still holds is let go in PumpInput.
    input.machineChanged();
}

// Loads the power-on state as a reset through RESET_IN, which keeps RAM, the clock controller and
// the RTC's time. Every button is released in that state.
// ASSUMPTION "Zodiac reset button": the button on the back is taken to pull the i.MX1's RESET_IN,
// the one reset the manual says keeps the SDRAM (24.6.3.1). The board's wiring is not in the manual
// or the ROM.
void SocIMX1::Reset() {
    SoC::Reset();
    const auto info = imx1MachineLoadState(machine, powerOn.data(), powerOn.size(), imx1StateExternalReset, "power-on");
    if (info.status != Imx1StateStatus::ok)
        std::cerr << "zodiac: reset failed: " << imx1StateStatusName(info.status) << std::endl;
    orientationTracker.Restart(imx1MachineStartups(machine));
    StateChanged();
    // The power-on state has the slot empty; a card still in reads as in from the start.
    if (cardIn) { vsdReset(vsd, uint32_t(sdCardSectorCount())); cardSync = true; SyncCard(); }
}

// The DAL shows its logo with the scan unturned and drawn for landscape; the OS starts in landscape.
unsigned SocIMX1::GuestOrientation() {
    return orientationTracker.Update(imx1MachineScanOrientation(machine), imx1MachineStartups(machine));
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
    // A card in carries on from the state's card section, or starts idle without one. With no card in, a
    // card section waits for OnSdInsert, which a session start calls next; the next Run takes the card out.
    cardSync = true;
    if (cardIn) {
        if (!info.cardResumed) vsdReset(vsd, uint32_t(sdCardSectorCount()));
        SyncCard();
    }
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

uint64_t SocIMX1::GetTime() {
    const auto time = imx1MachineGetStatus(machine).time;
    return uint64_t(__uint128_t(time.ticks) * 1000000000 / ticksPerSecond);
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

void SocIMX1::AnalogStick(float x, float y) { input.stick(zodiacStickOffset(x), zodiacStickOffset(y)); }

void SocIMX1::JamKey(enum KeyId key, uint32_t durationMsec) {
    const int control = zodiacControlFor(key);
    if (control < 0 || halted) return;
    MachinePort port(machine);
    input.jam(port, unsigned(control), durationMsec);
}

// Hands queued host input to the machine. An input the machine refuses just now stays queued.
void SocIMX1::PumpInput() {
    MachinePort port(machine);
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

// Runs the guest up to maxCycles / cyclesPerSecond of its own time, or for a short stretch of host time
// when it can't keep up, and returns the guest time that passed in the same cycle units.
uint64_t SocIMX1::Run(uint64_t maxCycles, uint64_t cyclesPerSecond) {
    if (halted || !cyclesPerSecond) {
        keyEventQueue->Clear();
        penEventQueue->Clear();
        return maxCycles;
    }
    const auto before = imx1MachineGetStatus(machine);
    SyncCard();
    PumpInput();
    const uint64_t wanted = std::max<uint64_t>(1, uint64_t(__uint128_t(maxCycles) * ticksPerSecond / cyclesPerSecond));
    Imx1MachineBounds bounds;
    // Fast runs longer slices: each slice end charges the whole board, and the deadline still ends the call on time.
    const bool fast = imx1MachineFastEngine(machine);
    bounds.retired = fast ? 8000 : 4000;
    bounds.work = fast ? 40000 : 20000;
    bounds.deadline = {before.time.ticks + wanted, 0, 1};
    const auto started = std::chrono::steady_clock::now();
    bool waiting = false;
    for (;;) {
        const auto run = imx1MachineRunBounded(machine, bounds);
        if (run.reason == Imx1MachineYield::terminal || run.reason == Imx1MachineYield::stopped ||
            run.reason == Imx1MachineYield::invalidBounds) {
            halted = true;
            std::cerr << "zodiac: the machine stopped: " << StopText() << ", after "
                      << imx1MachineGetStatus(machine).retired << " instructions" << std::endl;
            break;
        }
        if (run.reason == Imx1MachineYield::waiting) { waiting = true; break; }
        if (run.reason == Imx1MachineYield::deadline) break;
        if (std::chrono::steady_clock::now() - started >= runHostLimit) break;
    }
    const uint64_t elapsed = imx1MachineGetStatus(machine).time.ticks - before.time.ticks;
    // Nothing can wake a waiting guest but input, so host time just passes. Its own clock stands
    // still then, and the input holds are timed on the time asked for. A running guest's holds are
    // timed on its own clock alone, however slowly it runs.
    input.advance(waiting ? std::max(wanted, elapsed) : elapsed);
    if (waiting || halted) return maxCycles;
    const __uint128_t credit = __uint128_t(elapsed) * cyclesPerSecond + creditRemainder;
    creditRemainder = uint64_t(credit % ticksPerSecond);
    return uint64_t(std::min<__uint128_t>(credit / ticksPerSecond, UINT64_MAX));
}
