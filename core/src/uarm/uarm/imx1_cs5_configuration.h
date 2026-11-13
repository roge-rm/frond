#ifndef FROND_IMX1_CS5_CONFIGURATION_H
#define FROND_IMX1_CS5_CONFIGURATION_H

#include "imx1_bootstrap.h"
#include "imx1_graphics_gpio.h"
#include "imx1_graphics_windows.h"
#include "imx1_graphics_lookup.h"
#include "imx1_graphics_controller.h"
#include "imx1_graphics_engine.h"
#include "imx1_graphics_display.h"
#include "imx1_graphics_interrupts.h"
#include "imx1_graphics_acquisition.h"
#include "imx1_graphics_sd.h"

enum class Imx1GraphicsProfile { strict, virtual5744Compatible8MiB };
inline bool imx1GraphicsProfileValid(Imx1GraphicsProfile profile) {
    return profile == Imx1GraphicsProfile::strict || profile == Imx1GraphicsProfile::virtual5744Compatible8MiB;
}

// Configured virtual aperture for the native W4200 configuration helper.
class Imx1Cs5Configuration {
   public:
    static constexpr uint32_t virtualIdentity = 0x57441002;
    static constexpr uint32_t virtualRevision = 0;
    static constexpr uint32_t virtualExternalBytes = 8 * 1024 * 1024;
    static constexpr uint32_t virtualInternalBytes = 0x60000;
    static constexpr uint32_t virtualStartupOptionNibble = 0;
    explicit Imx1Cs5Configuration(Imx1GraphicsProfile profile = Imx1GraphicsProfile::strict,
                                   bool virtualA4SourceOver = false, bool virtualPackedMono = false)
        : profile(profile), virtualA4SourceOver(virtualA4SourceOver), virtualPackedMono(virtualPackedMono) {}
    bool a4SourceOverEnabled() const { return virtualProfile() && virtualA4SourceOver; }
    bool packedMonoEnabled() const { return virtualProfile() && virtualPackedMono; }
    bool virtualProfile() const { return profile == Imx1GraphicsProfile::virtual5744Compatible8MiB; }
    void reset() {
        selected = programmed = false;
        defaults = interfaceMode = 0;
        interfaceControl = interfaceIo = readControl = writeControl = busTiming = memoryIo = busArbitration = 0;
        gpio.reset();
        windows.reset();
        lookup.reset();
        controller.reset();
        engine.reset();
        display.reset();
        interrupts.reset();
        acquisition.reset();
        sd.reset();
        clocks.reset();
        timeFraction = {};
        waitCpuFraction = {};
    }
    // The profile and its two policies come from the options.
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        uint32_t fixedProfile = uint32_t(profile);
        a.Fixed32(fixedProfile).FixedBool(virtualA4SourceOver).FixedBool(virtualPackedMono);
        gpio.state(a); windows.state(a); lookup.state(a); controller.state(a); engine.state(a);
        display.state(a); interrupts.state(a); acquisition.state(a); sd.state(a); clocks.state(a);
        timeFraction.state(a);
        waitCpuFraction.state(a);
        a.Do32(busArbitration).Do32(interfaceControl).Do32(interfaceIo).Do32(readControl).Do32(writeControl);
        a.Do32(busTiming).Do32(memoryIo).DoBool(selected).DoBool(programmed).Do8(defaults).Do8(interfaceMode);
    }
    bool referencePeriodSupported(Imx1PllRatio period) const {
        return !clocks.programmed() || timeFraction.supports(uint64_t(period.denominator) * 64);
    }
    bool advanceCpuCycles(uint32_t cycles, Imx1PllRatio period = {64, 390625}) {
        if (!referencePeriodSupported(period)) return false;
        controller.advanceCpuCycles(cycles);
        if (!clocks.programmed()) { advanceDisplay(cycles); return true; }
        while (cycles) {
            const uint32_t chunk = std::min(cycles, 1000000u);
            advanceGraphicsTicks(timeFraction.add(uint64_t(chunk) * period.numerator * 390625,
                                                  uint64_t(period.denominator) * 64));
            cycles -= chunk;
        }
        return true;
    }
    void advanceReference(uint64_t numerator, uint64_t denominator) {
        if (!clocks.programmed()) return;
        assert(timeFraction.supports(denominator * 64));
        while (numerator) {
            const uint64_t chunk = std::min<uint64_t>(numerator, 1000000);
            advanceGraphicsTicks(timeFraction.add(chunk * 390625, denominator * 64));
            numerator -= chunk;
        }
    }
    // Reference steps of numerator/denominator until any graphics model changes state.
    // 1 when one is due now or the busy level hasn't reached the clocks yet.
    uint64_t idleQuietSteps(uint64_t numerator, uint64_t denominator) const {
        if (!clocks.programmed()) return UINT64_MAX;
        if (!timeFraction.supports(denominator * 64) || clocks.gpuBusyValue() != !engine.emptyRing()) return 1;
        uint64_t ticks = clocks.nextEvent() ? clocks.nextEvent() : UINT64_MAX;
        ticks = std::min(ticks, engine.idleQuietTicks());
        if (acquisition.deadlineRemaining()) ticks = std::min(ticks, acquisition.deadlineRemaining());
        ticks = std::min(ticks, sd.idleQuietTicks());
        ticks = std::min(ticks, display.idleQuietTicks());
        if (ticks == UINT64_MAX) return UINT64_MAX;
        return imx1StepsUntil(timeFraction.phase, timeFraction.denominator, numerator * 390625, denominator * 64, ticks);
    }
    // CPU cycles of this period that advanceCpuCycles can take in one call or in any split with the same
    // result, as no graphics model changes state within them. 0 when a change is due or can't be worked out.
    uint64_t quietCpuCycles(Imx1PllRatio period) const {
        if (!clocks.programmed() || controller.delayRunning()) return 0;
        const uint64_t steps = idleQuietSteps(period.numerator, period.denominator);
        return steps == UINT64_MAX ? UINT64_MAX : steps - 1;
    }
    // Graphics ticks the next cycles CPU cycles and then busClocks bus clocks give, charged as advanceCpuCycles
    // and advanceBusCycles (in pieces of at most one transaction) charge them, with nothing changed.
    bool ticksAfter(uint64_t cycles, Imx1PllRatio cpu, uint64_t busClocks, Imx1PllRatio bus, uint64_t& ticks) const {
        if (!clocks.programmed() || !timeFraction.supports(uint64_t(cpu.denominator) * 64) ||
            !timeFraction.supports(uint64_t(bus.denominator) * 64)) return false;
        auto fraction = timeFraction;
        ticks = 0;
        // Once the fraction's denominator holds the period's, each part goes in one add (addWhole).
        if (fraction.addWhole(cycles, uint64_t(cpu.numerator) * 390625, uint64_t(cpu.denominator) * 64, ticks)) cycles = 0;
        while (cycles) {
            const uint64_t chunk = std::min<uint64_t>(cycles, 1000000);
            if (!fraction.supports(uint64_t(cpu.denominator) * 64)) return false;
            ticks += fraction.add(chunk * cpu.numerator * 390625, uint64_t(cpu.denominator) * 64);
            cycles -= chunk;
        }
        if (fraction.addWhole(busClocks, uint64_t(bus.numerator) * 390625, uint64_t(bus.denominator) * 64, ticks))
            busClocks = 0;
        while (busClocks) {
            const uint64_t piece = std::min<uint64_t>(busClocks, 1022);
            for (uint64_t numerator = piece * bus.numerator; numerator;) {
                const uint64_t chunk = std::min<uint64_t>(numerator, 1000000);
                if (!fraction.supports(uint64_t(bus.denominator) * 64)) return false;
                ticks += fraction.add(chunk * 390625, uint64_t(bus.denominator) * 64);
                numerator -= chunk;
            }
            busClocks -= piece;
        }
        return true;
    }
    // A register read that changes nothing, as access answers it after ticks more of graphics time with no
    // change due: the scan status (54c), the acquisition registers but the FIFO and the engine's ring pointer
    // polls. False for anything else and wherever access would refuse it.
    bool quietRegisterRead(const Imx1Eim& eim, uint32_t pc, uint32_t address, uint64_t ticks, uint32_t& value) const {
        if (!imx1GraphicsProfileValid(profile) || !virtualProfile() || Imx1GraphicsSd::owns(address)) return false;
        if (address == 0x16010000 && pc == 0x200871dc) return quietIdentityRead(eim, value);
        if (Imx1GraphicsAcquisition::owns(address))
            return memoryReady(eim) && clocks.programmed() && acquisition.peek(address, value);
        // No model before the engine in access owns these, and the board range at 20086952-20086a60 has none of them.
        if (Imx1GraphicsEngine::owns(address))
            return !(pc >= 0x20086952 && pc <= 0x20086a60) && memoryReady(eim) && engine.quietRead(pc, address, value);
        if (address != 0x1601054c || Imx1GraphicsInterrupts::owns(address) || Imx1GraphicsClocks::owns(address))
            return false;
        const bool scanStatus = (pc == 0x200adb08 || pc == 0x200adb14) && !engine.completed();
        if (!memoryReady(eim) || (!engine.emptyRing() && !scanStatus)) return false;
        return display.scanStatusAfter(pc, ticks, value);
    }
    // The scan status read by the vertical wait (200adb08 or 200adb14) as quietRegisterRead answers it after ticks
    // more, with the display's region (Imx1GraphicsDisplay::scanStatusAfter), which only grows with ticks.
    bool quietScanStatus(const Imx1Eim& eim, uint32_t pc, uint64_t ticks, uint32_t& value, unsigned& region) const {
        constexpr uint32_t address = 0x1601054c;
        if (!imx1GraphicsProfileValid(profile) || !virtualProfile() || Imx1GraphicsSd::owns(address) ||
            Imx1GraphicsAcquisition::owns(address) || Imx1GraphicsEngine::owns(address) ||
            Imx1GraphicsInterrupts::owns(address) || Imx1GraphicsClocks::owns(address) ||
            (pc != 0x200adb08 && pc != 0x200adb14) || engine.completed() || !memoryReady(eim))
            return false;
        return display.scanStatusAfter(pc, ticks, value, &region);
    }
    // The identity read in the register read helper (200871dc) as access answers it, changing nothing. No other
    // model owns 0x16010000, so access reaches its identity case.
    bool quietIdentityRead(const Imx1Eim& eim, uint32_t& value) const {
        constexpr uint32_t address = 0x16010000;
        if (Imx1GraphicsAcquisition::owns(address) || Imx1GraphicsInterrupts::owns(address) ||
            Imx1GraphicsClocks::owns(address) || Imx1GraphicsDisplay::owns(address) || Imx1GraphicsEngine::owns(address) ||
            Imx1GraphicsController::owns(address) || Imx1GraphicsLookup::owns(address) || Imx1GraphicsWindows::owns(address) ||
            Imx1GraphicsGpio::owns(address))
            return false;
        if (!deviceConfigured(eim) || !controller.configured() || !memoryReady(eim) || !clocks.programmed() || !programmed)
            return false;
        value = virtualIdentity;
        return true;
    }
    bool busPeriodSupported(Imx1PllRatio bus, Imx1PllRatio cpu) const {
        return referencePeriodSupported(bus) && waitCpuFraction.supports(uint64_t(bus.denominator) * cpu.numerator);
    }
    void advanceBusCycles(uint32_t cycles, Imx1PllRatio bus, Imx1PllRatio cpu) {
        assert(cycles <= 1022 && busPeriodSupported(bus,cpu));
        const auto elapsed = waitCpuFraction.add(uint64_t(cycles) * bus.numerator * cpu.denominator,
                                                 uint64_t(bus.denominator) * cpu.numerator);
        controller.advanceCpuCycles(elapsed);
        if (!clocks.programmed()) advanceDisplay(elapsed);
        else advanceReference(uint64_t(cycles) * bus.numerator,bus.denominator);
    }
    bool deviceConfigured(const Imx1Eim& eim) const {
        return virtualProfile() ? eim.cs5DeviceConfigured() : eim.configurationMatches(5,0x3e00,0x11111e01);
    }
    bool setAcquisitionMode2Route(Imx1GraphicsMode2Route route) { return acquisition.setMode2Route(route); }
    bool setAcquisitionAuxInput(uint32_t sample) { return acquisition.setAuxInput(sample); }
    void selectAcquisitionSource(uint32_t source) { acquisition.selectSource(source); }
    bool setAcquisitionInput(uint32_t first, uint32_t second) { return acquisition.setInput(first, second); }
    bool setAcquisitionMode1Input(uint32_t sample) { return acquisition.setMode1Input(sample); }
    const Imx1GraphicsAcquisition& acquisitionState() const { return acquisition; }
    const Imx1GraphicsSd& sdState() const { return sd; }
    const Imx1GraphicsClocks& clockState() const { return clocks; }
    Imx1GraphicsLookup::Entry lookupEntry(unsigned index) const { return lookup.entry(index); }
    bool memoryInterfaceReady() const { return programmed && interfaceMode == 0x11 && (interfaceControl & ~0xe00u) == 0x0c000000; }
    bool memoryReady(const Imx1Eim& eim) const {
        return virtualProfile() && memoryInterfaceReady() && controller.ready() && memoryIo == 0x7bbb7444 &&
               deviceConfigured(eim);
    }
    const Imx1GraphicsWindows& memoryWindows() const { return windows; }
    const Imx1GraphicsEngine& engineState() const { return engine; }
    void startEngine(uint64_t ticks) { engine.start(ticks); }
    void completeEngine(uint32_t option) { engine.complete(option); }
    // An overlay word (4d8, 43c-450) a player's ring packet sets, when the engine reaches it.
    Imx1BootstrapAccess ringDisplayWord(uint32_t offset, uint32_t value) {
        return display.playerAccess(windows, 0x16010000 + offset, true, value);
    }
    const Imx1GraphicsDisplay& displayState() const { return display; }
    const Imx1GraphicsInterrupts& interruptState() const { return interrupts; }
    Imx1GraphicsGpio::Bank gpioBank(unsigned index) const { return gpio.bank(index); }
    bool defaultsKnown() const { return programmed; }
    uint8_t defaultsValue() const { return defaults; }
    bool apertureSelected() const { return selected; }
    // The OS maps SDRAM at 40000000-473fffff for applications and their data. The ROM runs at 20000000 and the
    // exception vectors at 0, so only code in that RAM drives the chip as a player.
    static bool playerPc(uint32_t pc) { return pc >= 0x40000000 && pc < 0x47400000; }
    Imx1BootstrapAccess access(const Imx1Eim& eim, uint32_t pc, uint32_t address,
                              uint32_t size, bool write, uint32_t& value, uint32_t sampledPin = 32) {
        if (!imx1GraphicsProfileValid(profile)) return Imx1BootstrapAccess::sequence;
        if (virtualProfile() && Imx1GraphicsSd::owns(address)) {
            if (!memoryReady(eim) || !clocks.programmed()) return Imx1BootstrapAccess::sequence;
            const auto window = windows.aperture2();
            if (address >= 0x16200000 && (!window.known || address - 0x16000000 < window.first ||
                uint64_t(address - 0x16000000) + size > uint64_t(window.last) + 1)) return Imx1BootstrapAccess::sequence;
            return sd.access(pc,address,size,write,value);
        }
        if (virtualProfile() && Imx1GraphicsAcquisition::owns(address)) {
            if (!memoryReady(eim) || !clocks.programmed()) return Imx1BootstrapAccess::sequence;
            return acquisition.access(pc, address, size, write, value);
        }
        if (virtualProfile() && Imx1GraphicsInterrupts::owns(address)) {
            if (!deviceConfigured(eim) || !memoryInterfaceReady()) return Imx1BootstrapAccess::sequence;
            return interrupts.access(pc, address, size, write, value, sd.cardEventsKnown());
        }
        // The chip interrupt set-up on the way up from sleep (20086e0c) does the same 018 update as the boot's.
        const bool wakeInterface = address == 0x16010018 && (pc == 0x20086e10 || pc == 0x20086e1a);
        if (virtualProfile() && ((pc >= 0x20086952 && pc <= 0x20086a60 && !Imx1GraphicsDisplay::owns(address)) || address == 0x16010184 || wakeInterface)) {
            if (!deviceConfigured(eim)) return Imx1BootstrapAccess::sequence;
            return boardAccess(pc,address,size,write,value);
        }
        // Player code in RAM, such as TCPMP driving the chip itself (its code runs at 40xxxxxx).
        if (virtualProfile() && playerPc(pc)) {
            if (Imx1GraphicsDisplay::playerWord(address) || Imx1GraphicsEngine::playerWord(address) ||
                address == 0x16010098) {
                if (size != 4) return Imx1BootstrapAccess::width;
                if (!memoryReady(eim)) return Imx1BootstrapAccess::sequence;
            }
            if (Imx1GraphicsDisplay::playerWord(address)) return display.playerAccess(windows, address, write, value);
            if (Imx1GraphicsEngine::playerWord(address)) return engine.playerAccess(address, write, value);
            if (address == 0x16010098) {
                if (!engine.emptyRing()) return Imx1BootstrapAccess::sequence;
                const auto status = clocks.playerAccess(address, write, value);
                if (status == Imx1BootstrapAccess::ok && write)
                    display.setClockSources(clocks.pixelRate(), clocks.systemRate());
                return status;
            }
        }
        if (virtualProfile() && Imx1GraphicsClocks::owns(address)) {
            if (size != 4) return Imx1BootstrapAccess::width;
            if (!memoryReady(eim) || !engine.emptyRing()) return Imx1BootstrapAccess::sequence;
            auto candidate=clocks;
            const auto status = candidate.access(pc, address, size, write, value);
            if (status != Imx1BootstrapAccess::ok || !write) return status;
            if (!display.clockSourcesSupported(candidate.pixelConfiguredRate())) return Imx1BootstrapAccess::sideEffect;
            clocks=candidate;
            display.setClockSources(clocks.pixelRate(), clocks.systemRate());
            return status;
        }
        if (virtualProfile() && Imx1GraphicsDisplay::owns(address)) {
            if (size != 4) return Imx1BootstrapAccess::width;
            // Everything here needs an empty ring but the ROM's vertical wait (200adaf0), whose two reads of the
            // scan status (54c) run with any batch in the ring. Its callers are the screen unlock (2008b2ac),
            // the application wait service dal[288] (20099a08) and the display switch (veneer 200ce4f4 from
            // 20088180 and 2008821a), and none of them reads an engine register first.
            // ASSUMPTION "scan status while drawing": 54c follows the scan phase alone, whatever the engine does.
            const bool scanStatus = !write && (address & 0xffff) == 0x054c && (pc == 0x200adb08 || pc == 0x200adb14) &&
                !engine.completed();
            // The panel pulse word (4f0, the backlight level: read by 20086cf8, written by 200869c4) is outside
            // the drawing engine, and games change the level with drawing queued.
            const bool panelPulse = (address & 0xffff) == 0x04f0;
            if (!memoryReady(eim) || (!engine.emptyRing() && !scanStatus && !panelPulse)) return Imx1BootstrapAccess::sequence;
            return display.access(windows, pc, address, size, write, value);
        }
        if (virtualProfile() && Imx1GraphicsEngine::owns(address)) {
            if (size != 4) return Imx1BootstrapAccess::width;
            if (!memoryReady(eim)) return Imx1BootstrapAccess::sequence;
            return engine.access(windows, pc, address, size, write, value);
        }
        if (virtualProfile() && Imx1GraphicsController::owns(address)) {
            if (size != 4) return Imx1BootstrapAccess::width;
            if (!memoryInterfaceReady() || !deviceConfigured(eim))
                return Imx1BootstrapAccess::sequence;
            return controller.access(pc, address, size, write, value);
        }
        if (virtualProfile() && Imx1GraphicsLookup::owns(address)) {
            if (size != 4) return Imx1BootstrapAccess::width;
            if (!programmed || !deviceConfigured(eim))
                return Imx1BootstrapAccess::sequence;
            return lookup.access(pc, address, size, write, value);
        }
        if (virtualProfile() && Imx1GraphicsWindows::owns(address)) {
            if (size != 4) return Imx1BootstrapAccess::width;
            if (!programmed || !deviceConfigured(eim))
                return Imx1BootstrapAccess::sequence;
            return windows.access(pc, address, size, write, value);
        }
        if (virtualProfile() && Imx1GraphicsGpio::owns(address)) {
            if (size != 4) return Imx1BootstrapAccess::width;
            if (!programmed || !deviceConfigured(eim))
                return Imx1BootstrapAccess::sequence;
            if (!write && pc == 0x200b9cd4 && sampledPin < 16 &&
                (address == 0x160104c0 || address == 0x16010530)) {
                const unsigned bank = address == 0x16010530;
                const auto state = gpio.bank(bank);
                const uint32_t bit = 1u << sampledPin;
                if (state.padKnown() & bit) value = state.padValue() & bit;
                else if (!bank && state.controlKnown && state.dataKnown && display.lcdEnabled() && (bit & 0x1e00))
                    value = (virtualStartupOptionNibble << 9) & bit;
                else return Imx1BootstrapAccess::externalInput;
                // The qualified helper observes this pin only. Other input lanes
                // carry no sample semantics and the probe reports the known mask.
                return Imx1BootstrapAccess::ok;
            }
            return gpio.access(pc, address, size, write, value);
        }
        if (virtualProfile() && (address == 0x16010020 || address == 0x16010024 ||
                                 address == 0x16010028 || address == 0x16010018 || address == 0x16010908 || address == 0x1601019c))
            return interfaceAccess(eim, pc, address, size, write, value);
        // ASSUMPTION "graphics USB function off": b30 reads 0 and takes 0. The ROM's display off code reads it
        // and writes it back with bit 0 clear (200c5348, 200c537c) and tests nothing. Its reset value and its
        // other bits are not known, and a write that turns the USB function on (bits 0 and 10) stays a stop.
        if (virtualProfile() && address == 0x16010b30) {
            if (size != 4) return Imx1BootstrapAccess::width;
            if (!programmed || !deviceConfigured(eim)) return Imx1BootstrapAccess::sequence;
            if (write && value) return Imx1BootstrapAccess::sideEffect;
            value = 0;
            return Imx1BootstrapAccess::ok;
        }
        if (virtualProfile() && address == 0x16000014) {
            if (size != 1) return Imx1BootstrapAccess::width;
            if (!programmed || !deviceConfigured(eim))
                return Imx1BootstrapAccess::sequence;
            if (!write || pc != 0x200c50dc) return Imx1BootstrapAccess::externalInput;
            if (value != 0x11) return Imx1BootstrapAccess::sideEffect;
            interfaceMode = value;
            return Imx1BootstrapAccess::ok;
        }
        if (address != 0x16000010 && address != 0x1600001c &&
            address != 0x16010000 && address != 0x16010004)
            return Imx1BootstrapAccess::unmapped;
        if (size != (address >= 0x16010000 ? 4u : 1u)) return Imx1BootstrapAccess::width;
        if (!deviceConfigured(eim)) return Imx1BootstrapAccess::sequence;
        if (virtualProfile() && address == 0x16000010 && pc == (write ? 0x200c111cu : 0x200c1118u)) {
            if (!programmed) return Imx1BootstrapAccess::sequence;
            if (write && value != 1) return Imx1BootstrapAccess::sideEffect;
            value = 1;
            return Imx1BootstrapAccess::ok;
        }
        if (address == 0x16000010) {
            if (write || (pc != 0x200bab4c && pc != 0x200bab80))
                return Imx1BootstrapAccess::externalInput;
            value = 1;
            selected = true;
            return Imx1BootstrapAccess::ok;
        }
        if (address == 0x1600001c) {
            if (!write || pc != 0x200babf8) return Imx1BootstrapAccess::externalInput;
            if (!selected) return Imx1BootstrapAccess::sequence;
            if (value != 0x0e) return Imx1BootstrapAccess::sideEffect;
            defaults = value;
            programmed = true;
            return Imx1BootstrapAccess::ok;
        }
        const bool controllerIdentity = address == 0x16010000 && controller.configured() &&
            (pc == 0x200c4174 || pc == 0x200c41a8 || pc == 0x200c41e4 || pc == 0x200c4230 ||
             pc == 0x200c424c || pc == 0x200c42d0 || pc == 0x200c42f4 || pc == 0x200c4314 ||
             pc == 0x200c4330 || pc == 0x200c4344 || pc == 0x200c435c || pc == 0x200c4374 ||
             (pc == 0x200871dc && memoryReady(eim) && clocks.programmed()) ||
             pc == 0x20084650 || pc == 0x20084658 || pc == 0x20084660 || pc == 0x20084664 ||
             (memoryReady(eim) && clocks.programmed() &&
              (pc == 0x204d26c6 || pc == 0x204d26ce || pc == 0x204d26da || pc == 0x204d270a ||
               pc == 0x204d218e || pc == 0x204d22f4 || pc == 0x204d2314 || pc == 0x204d231c ||
               pc == 0x204d3652 || pc == 0x204d2742 || pc == 0x204d247e || pc == 0x204d2522 ||
               pc == 0x204d2534 || pc == 0x204d2574 || pc == 0x204d2586 || pc == 0x204d277a ||
               pc == 0x204d22ae)));
        if (!virtualProfile() || write || (!controllerIdentity &&
            pc != (address == 0x16010000 ? 0x200bac00u : 0x200bac28u)))
            return Imx1BootstrapAccess::externalInput;
        if (!programmed) return Imx1BootstrapAccess::sequence;
        // Explicit virtual ABI choices, with no physical W4200 identity claim.
        value = address == 0x16010000 ? virtualIdentity : virtualRevision;
        return Imx1BootstrapAccess::ok;
    }

   private:
    void advanceDisplay(uint64_t ticks) {
        if (!clocks.programmed() || clocks.systemRate().numerator) engine.advance(ticks);
        acquisition.advance(ticks);
        sd.advance(ticks);
        const auto before = display.verticalBlankEvents();
        display.advanceCpuCycles(ticks);
        interrupts.observeDisplay(true, display.verticalBlankEvents() != before);
    }
    void advanceGraphicsTicks(uint64_t ticks) {
        clocks.setGpuBusy(!engine.emptyRing());
        while (ticks) {
            uint64_t step = std::min<uint64_t>(ticks, 1000000);
            if (clocks.nextEvent()) step = std::min(step, clocks.nextEvent());
            advanceDisplay(step);
            clocks.advance(step);
            ticks -= step;
            display.setClockSources(clocks.pixelRate(), clocks.systemRate());
        }
    }
    Imx1BootstrapAccess boardAccess(uint32_t pc, uint32_t address, uint32_t size, bool write, uint32_t& value) {
        if (size != 4) return Imx1BootstrapAccess::width;
        if (!memoryInterfaceReady()) return Imx1BootstrapAccess::sequence;
        struct Operation { uint32_t pc, address, clear, set; bool write; };
        constexpr Operation operations[] = {
            {0x20086a00,0x16010024,0,0,false},{0x20086a08,0x16010024,0,0x20000,true},{0x20086a0a,0x16010024,0,0,false},
            {0x20086a10,0x16010028,0,0,false},{0x20086a1a,0x16010028,0x200,0,true},{0x20086a1c,0x16010028,0,0,false},
            {0x20086a22,0x16010018,0,0,false},{0x20086a2c,0x16010018,0,0x8000200,true},{0x20086a2e,0x16010018,0,0,false},
            {0x20086a34,0x16010184,0,0,false},{0x20086a3c,0x16010184,0xffffffff,0xf7,true},{0x20086a3e,0x16010184,0,0,false},
            {0x20086a44,0x16010018,0,0,false},{0x20086a52,0x16010018,0x100000,0x400,true},{0x20086a54,0x16010018,0,0,false},
            {0x20086a5a,0x16010020,0,0,false},{0x20086a5e,0x16010020,0,0x100000,true},{0x20086a60,0x16010020,0,0,false},
            {0x20086974,0x16010018,0,0,false},
            {0x20086982,0x16010018,0x100000,0x400,true},
            {0x20086e10,0x16010018,0,0,false},{0x20086e1a,0x16010018,0x100000,0x400,true}};
        const Operation* op = nullptr;
        for (const auto& candidate : operations) if (candidate.pc == pc && candidate.address == address && candidate.write == write) {op=&candidate;break;}
        if (!op) return Imx1BootstrapAccess::externalInput;
        uint32_t* state = address == 0x16010018 ? &interfaceControl : address == 0x16010020 ? &interfaceIo :
            address == 0x16010024 ? &readControl : address == 0x16010028 ? &writeControl :
            &busArbitration;
        if (write) {
            if (value != ((*state & ~op->clear) | op->set)) return Imx1BootstrapAccess::sideEffect;
            *state=value;
        } else value=*state;
        return Imx1BootstrapAccess::ok;
    }
    Imx1BootstrapAccess interfaceAccess(const Imx1Eim& eim, uint32_t pc, uint32_t address,
                                       uint32_t size, bool write, uint32_t& value) {
        if (size != 4) return Imx1BootstrapAccess::width;
        if (!programmed || !deviceConfigured(eim))
            return Imx1BootstrapAccess::sequence;
        uint32_t* state = nullptr;
        uint32_t readPc = 0, writePc = 0, mask = 0;
        switch (address) {
            case 0x1601019c:
                if (pc == 0x200c6888 && write) {
                    const auto status = controller.configureIo(value);
                    if (status == Imx1BootstrapAccess::ok) memoryIo = value;
                    return status;
                }
                state = &memoryIo;
                if (pc == 0x200c6790 || pc == 0x200c67c4) {
                    readPc = 0x200c6790; writePc = 0x200c67c4; mask = 0x100f100f;
                } else if (pc == 0x200c67e8 || pc == 0x200c681c) {
                    readPc = 0x200c67e8; writePc = 0x200c681c; mask = 0x20f020f0;
                } else {
                    readPc = 0x200c6840; writePc = 0x200c6874; mask = 0x4f004f00;
                }
                break;
            case 0x16010908:
                state = &busTiming;
                if (pc == 0x200c241c || pc == 0x200c2450) {
                    readPc = 0x200c241c; writePc = 0x200c2450; mask = 0x3ff00000;
                } else if (pc == 0x200c2468 || pc == 0x200c249c) {
                    readPc = 0x200c2468; writePc = 0x200c249c; mask = 0xffc00;
                } else {
                    readPc = 0x200c24b4; writePc = 0x200c24e8; mask = 0x3ff;
                }
                break;
            case 0x16010020: state = &interfaceIo; readPc = 0x200c2500; writePc = 0x200c2534; mask = 0x3ff; break;
            case 0x16010028: state = &writeControl; readPc = 0x200c508c; writePc = 0x200c5098; mask = 0x380; break;
            case 0x16010024: state = &readControl; readPc = 0x200c50a0; writePc = 0x200c50a8; mask = 0x20000; break;
            case 0x16010018: state = &interfaceControl; readPc = 0x200c50b0; writePc = 0x200c50bc; mask = 0xc000800; break;
            default: return Imx1BootstrapAccess::unmapped;
        }
        if (pc != (write ? writePc : readPc)) return Imx1BootstrapAccess::externalInput;
        if (write) {
            if ((value ^ *state) & ~mask) return Imx1BootstrapAccess::sideEffect;
            *state = value;
        } else value = *state;
        return Imx1BootstrapAccess::ok;
    }
    Imx1GraphicsProfile profile;
    bool virtualA4SourceOver = false, virtualPackedMono = false;
    Imx1GraphicsGpio gpio;
    Imx1GraphicsWindows windows;
    Imx1GraphicsLookup lookup;
    Imx1GraphicsController controller;
    Imx1GraphicsEngine engine;
    Imx1GraphicsDisplay display;
    Imx1GraphicsInterrupts interrupts;
    Imx1GraphicsAcquisition acquisition;
    Imx1GraphicsSd sd;
    Imx1GraphicsClocks clocks;
    Imx1GraphicsFraction timeFraction, waitCpuFraction;
    // These interface configuration words reset to zero in the virtual profile.
    uint32_t busArbitration = 0;
    uint32_t interfaceControl = 0, interfaceIo = 0, readControl = 0, writeControl = 0, busTiming = 0, memoryIo = 0;
    bool selected = false, programmed = false;
    uint8_t defaults = 0, interfaceMode = 0;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1Cs5Configuration) == 5136, "update state() and stateVersion");
#endif

struct Imx1ProbeCs5ConfigurationEvent {
    uint64_t step;
    uint32_t pc, address, size, value;
    bool write, defaultsKnown;
    Imx1BootstrapAccess status;
    uint32_t knownMask = 0xffffffff;
};
using Imx1ProbeCs5ConfigurationObserver = void (*)(void*, const Imx1ProbeCs5ConfigurationEvent&);

#endif
