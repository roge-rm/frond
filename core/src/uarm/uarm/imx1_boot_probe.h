#ifndef FROND_IMX1_BOOT_PROBE_H
#define FROND_IMX1_BOOT_PROBE_H

#include "arm_diagnostic.h"
#include <cstddef>
#include <cstdint>
#include <vector>
#include "imx1_bootstrap.h"
#include "imx1_cs4_capture.h"
#include "imx1_cs5_reset.h"
#include "imx1_cs5_configuration.h"

inline constexpr uint32_t imx1ProbeMaximumSteps = 1000000000;
inline constexpr uint32_t imx1ProbeMaximumIdleHclk = 512000000;

// The physical flash mapping is observed in the updater. Aliases are explicit experiments.
enum class Imx1ProbeEntry { physicalFlash, resetAlias, linkedAlias };
enum class Imx1ProbeStop {
    budget, unsupportedInstruction, unsupportedCp15, unsupportedRead, unsupportedWrite, exception, engineUnavailable,
    unsupportedPeripheral, unsupportedDataCache, idleBudget, idleNoWake
};
struct Imx1ProbeTrace {
    uint64_t step;
    uint32_t pc, instruction, cpsr, operand;
    uint32_t instructionSize;
};
struct Imx1ProbeAccess {
    uint64_t step;
    uint32_t pc, address, size, value;
    bool write;
    Imx1BootstrapAccess status;
    bool diagnosticCs4 = false;
    bool diagnosticCs5Reset = false;
    bool diagnosticCs5Configuration = false;
    uint32_t knownMask = 0xffffffff;
    uint32_t cs5WaitClocks = 0;
};
using Imx1ProbeBusObserver = void (*)(void*, const Imx1ProbeAccess&);
struct Imx1ProbeI2cEvent {
    uint64_t step;
    uint32_t pc;
    uint64_t issueStep;
    uint32_t issuePc;
    Imx1I2cTransfer transfer;
    Imx1I2cResponse response;
    uint32_t control, status, divider;
};
using Imx1ProbeI2cObserver = void (*)(void*, const Imx1ProbeI2cEvent&);
struct Imx1ProbeInterrupt {
    uint64_t step;
    uint32_t interruptedPc, cpsr, vector, link;
    bool fiq;
    Imx1AitcPending pending;
};
using Imx1ProbeInterruptObserver = void (*)(void*, const Imx1ProbeInterrupt&);
struct Imx1ProbeOptions {
    // Private end-stop state and the last 4096 retired instruction entry contexts.
    const ArmDiagnosticSink* diagnosticSnapshot = nullptr;
    // Borrowed final ABGR8888 frame, valid only during the callback.
    void (*frameObserver)(void*, uint32_t, uint32_t, const uint32_t*) = nullptr;
    void* frameContext = nullptr;
    Imx1GraphicsProfile graphicsProfile = Imx1GraphicsProfile::strict;
    Imx1ProbeInterruptObserver interruptObserver = nullptr;
    void* interruptContext = nullptr;
    bool cs5ResetStatusInputKnown = false;
    uint8_t cs5ResetStatusInput = 0;
    Imx1ProbeCs5ResetObserver cs5ResetObserver = nullptr;
    void* cs5ResetContext = nullptr;
    Imx1ProbeCs5ConfigurationObserver cs5ConfigurationObserver = nullptr;
    void* cs5ConfigurationContext = nullptr;
    bool rtcInitialTimeKnown = false;
    Imx1RtcTime rtcInitialTime{};
    bool cs4Port0InputKnown = false;
    uint8_t cs4Port0Input = 0;
    Imx1Cs4IndexedInputs cs4IndexedInputs;
    Imx1ProbeCs4Observer cs4Observer = nullptr;
    void* cs4Context = nullptr;
    bool aspPx1HighFromReset = false;
    Imx1AspPenInputs aspPenInputs{};
    bool aspUSampleKnown = false;
    uint16_t aspUSample = 0;
    bool assumeI2cSingleMaster = false;
    Imx1I2cBus* i2cBus = nullptr;
    Imx1ProbeI2cObserver i2cObserver = nullptr;
    void* i2cContext = nullptr;
    // Diagnostic elapsed HCLK limit per probe run, independent of the instruction budget.
    uint32_t idleHclkBudget = 16000000;
    // Advance a waiting CPU one HCLK at a time. Off, it goes straight to the next event, with the same results.
    bool idleStepped = false;
    bool bootstrapPeripherals = false;
    bool arm920IcacheIndexCompatibility = false;
    bool lcdcContrastOnly = false;
    bool mmcsdEmptySlot = false;
    bool assumeMpllSettled = false;
    bool virtualMpllRetune = false;
    bool assumeSpllSettled = false;
    bool graphicsIrqA13 = false;
    bool graphicsVirtualA4SourceOver = false;
    bool graphicsVirtualPackedMono = false;
    Imx1GraphicsMode2Route graphicsMode2Route = Imx1GraphicsMode2Route::unknown;
    bool graphicsRawAuxKnown = false;
    uint32_t graphicsRawAux = 0;
    bool graphicsRawMode1Known = false;
    uint32_t graphicsRawMode1 = 0;
    bool graphicsRawAxesKnown = false;
    uint32_t graphicsRawFirst = 0, graphicsRawSecond = 0;
    // One event before instruction N, after exactly N successful instructions; idle HCLK does not count.
    uint32_t gpioEventAfterInstructions = 0;
    uint32_t gpioEventValues[4]{}, gpioEventMasks[4]{};
    bool gpioAInputMaskedKnown = false;
    uint32_t gpioAInputMask = 0;
    bool gpioAInputKnown = false;
    uint32_t gpioAInput = 0;
    bool gpioBInputKnown = false;
    uint32_t gpioBInput = 0;
    bool gpioDInputKnown = false;
    uint32_t gpioDInput = 0;
    uint32_t ramMiB = 0;
    Imx1ProbeBusObserver busObserver = nullptr;
    void* busContext = nullptr;
};
inline bool imx1ProbeGpioEventValid(const Imx1ProbeOptions& options, uint32_t steps) {
    const uint32_t fixed[4] = {
        options.gpioAInputKnown ? 0xffffffff : options.gpioAInputMaskedKnown ? options.gpioAInputMask : 0,
        options.gpioBInputKnown ? 0xffffffff : 0, 0, options.gpioDInputKnown ? 0xffffffff : 0};
    bool any = false;
    for (unsigned port = 0; port < 4; ++port) {
        const auto mask = options.gpioEventMasks[port], value = options.gpioEventValues[port];
        if ((value & ~mask) || (mask & ~fixed[port])) return false;
        any = any || mask;
    }
    return any ? options.bootstrapPeripherals && options.gpioEventAfterInstructions > 0 &&
                 options.gpioEventAfterInstructions < steps : options.gpioEventAfterInstructions == 0;
}
struct Imx1ProbeResult {
    Imx1ProbeStop stop;
    Imx1ProbeTrace trace;
    // unsupportedDataCache carries the direction in value: 0 read, 1 write.
    uint32_t address, value, size;
    uint32_t registers[16], cpsr, spsr;
    uint64_t jitBlocks, jitInstructions;
    uint64_t busAccesses;
    uint64_t cs5WaitClocks = 0;
    uint64_t idleHclk = 0;
    uint32_t graphicsFrameWidth = 0, graphicsFrameHeight = 0, graphicsFrameHash = 0;
    bool graphicsLcdEnabled = false;
    uint64_t irqEntries = 0, fiqEntries = 0;
    Imx1PcmState pcm{};
    Imx1BootstrapAccess peripheralStatus;
    Imx1ProbeI2cEvent i2cEvent{};
    bool gpioEventApplied = false;
    bool elapsedInterruptBoundary = false;
    bool possibleWatchdogTimeoutBoundary = false;
    Imx1AitcPending interruptPending{};
};
using Imx1ProbeObserver = void (*)(void*, const Imx1ProbeTrace&);
// Execution-thread API. One active machine per process, without another core runner.
// CPU/memory allocations remain until process exit; stop detaches borrowed callbacks.
// No reset, portable state or general recreate/cleanup support. Callbacks cannot re-enter.
// ROM/options are copied. Observer contexts and I2C bus must outlive stop.
struct Imx1Machine;
enum class Imx1MachineStatus { yielded, waiting, terminal, stopped };
struct Imx1MachineRun {
    Imx1MachineStatus status;
    uint64_t retiredInstructions;
    Imx1ProbeResult result;
};
Imx1Machine* imx1MachineCreate(const uint8_t* rom, uint32_t size, Imx1ProbeEntry entry,
                             bool fast, const Imx1ProbeOptions& options,
                             Imx1ProbeObserver observer = nullptr, void* context = nullptr,
                             Imx1ProbeResult* error = nullptr);
// Retired-instruction slices include atomic bus waits and WFI wake. The idle limit
// is cumulative HCLK, independently of the retired budget. Zero retires nothing.
Imx1MachineRun imx1MachineRunSlice(Imx1Machine*, uint32_t retiredBudget, uint64_t idleHclkLimit);
Imx1MachineRun imx1MachineInspect(Imx1Machine*);
// The graphics command decoder's name and value for the batch it last refused, such as the ratio of a
// stretch blit it does not draw. name is null when it gave none.
struct Imx1GraphicsRefusal { const char* name; uint32_t value; };
Imx1GraphicsRefusal imx1MachineGraphicsRefusal(const Imx1Machine*);
// Bounded calls require bootstrap clocks. Deadline is absolute CLK32 time.
// Work counts guarded instruction attempts, idle/CS5 HCLK and nonzero PLL stalls.
// An instruction (including its CS5 waits) and a PLL stall are atomic and may overshoot.
struct Imx1MachineBounds {
    uint32_t retired = 0;
    Imx1ReferenceTime deadline{};
    uint64_t work = 0;
};
enum class Imx1MachineYield { retired, deadline, work, waiting, terminal, stopped, invalidBounds };
struct Imx1MachineCheapStatus {
    Imx1MachineStatus status;
    Imx1ProbeStop stop;
    uint64_t retired, work, idleHclk, cs5WaitClocks;
    Imx1ReferenceTime time;
    uint32_t pc;
};
struct Imx1MachineBoundedRun {
    Imx1MachineCheapStatus total;
    Imx1MachineYield reason;
    uint64_t retiredDelta = 0, workDelta = 0;
    Imx1ReferenceTime elapsed;
    // Magnitudes beyond the requested limits, from indivisible architectural work.
    uint64_t workOvershoot = 0;
    Imx1ReferenceInterval timeOvershoot;
};
// Reads CPU/counters only, without acquiring, hashing or copying framebuffer pixels.
Imx1MachineCheapStatus imx1MachineGetStatus(const Imx1Machine*);
// The engine between runs. Fast runs quiet spans on the lean interpreter, with no I- or D-cache contents:
// switching to it cleans the data cache to memory and empties both. False inside a run or after a stop.
bool imx1MachineSetFastEngine(Imx1Machine*, bool fast);
bool imx1MachineFastEngine(const Imx1Machine*);
// Whether Fast runs Dynarmic blocks before the lean path (on by default). Off gives the lean-only Fast engine,
// for tests and measuring. False inside bounds or after a stop.
bool imx1MachineSetFastBlocks(Imx1Machine*, bool blocks);
// The same for machines made after this call.
void imx1SetFastBlocksDefault(bool blocks);
// Instructions and runs of the Fast engine's JIT since the machine was made, for tests.
uint64_t imx1MachineJitRetired(const Imx1Machine*);
uint64_t imx1MachineJitRuns(const Imx1Machine*);
// Fast blocks, since the process started: MMIO reads (CS5 register reads among them) and board writes answered in
// generated code, and the timer delay loops passed at once with the cycles they skipped.
uint64_t imx1MachineJitReads(const Imx1Machine*);
uint64_t imx1MachineJitRegisterReads(const Imx1Machine*);
uint64_t imx1MachineJitBoardWrites(const Imx1Machine*);
uint64_t imx1MachineDelaySkips(const Imx1Machine*);
uint64_t imx1MachineDelaySkipCycles(const Imx1Machine*);
// The vertical wait's scan status polls Fast blocks passed at once, since the process started: the times and the
// rounds. imx1SetScanPollSkip(false) turns the skip off, for tests.
uint64_t imx1MachineScanPollSkips(const Imx1Machine*);
uint64_t imx1MachineScanPollRounds(const Imx1Machine*);
void imx1SetScanPollSkip(bool skip);
// Instructions the Fast engine ran on its lean path since the machine was made.
uint64_t imx1MachineLeanRetired(const Imx1Machine*);
// Timer count reads the lean path answered, including any in an instruction the step then ran again.
uint64_t imx1MachineLeanTimerReads(const Imx1Machine*);
// Graphics memory writes the Fast engine took on its lean path, wait clocks deferred.
uint64_t imx1MachineLeanCs5Writes(const Imx1Machine*);
// CS5 register reads the Fast engine answered on its lean path, wait clocks deferred.
uint64_t imx1MachineLeanCs5Reads(const Imx1Machine*);
// Graphics memory reads the Fast engine answered on its lean path, wait clocks deferred.
uint64_t imx1MachineLeanCs5MemoryReads(const Imx1Machine*);
// ASP status, GPIO and AITC reads the Fast engine answered on its lean path.
uint64_t imx1MachineLeanBoardReads(const Imx1Machine*);
// A hash of SRAM, SDRAM and graphics memory as they are in memory (cache contents left out), for tests.
uint64_t imx1MachineMemoryHash(const Imx1Machine*);
// Times the Fast engine dropped its decoded code for a store to a page it came from.
uint64_t imx1MachineLeanCodeDrops(const Imx1Machine*);
// The last PCM sample SSI1 shifted out, for tests.
uint16_t imx1MachinePcmLastTransmitted(const Imx1Machine*);
// Sends each 16-bit word SSI1 finishes and its slot (0 the frame sync slot) to hook, on the thread that runs
// the machine, in guest time order. Kept through state loads; null stops it.
void imx1MachineSetPcmOutput(Imx1Machine*, void (*hook)(void*, uint16_t word, unsigned slot), void* context);
// Puts the Zodiac's YMF768 on CS4 in place of the diagnostic recorder: its registers, its FM output mixed into
// the PCM output's words and its interrupt on GPIO B15. Call before the first instruction. Saved states carry it
// in an optional section; a state without one loads with the chip reset.
void imx1MachineEnableYmf768(Imx1Machine*);
// What the board's audio output looks like just now: SSI1's frame rate (frames a second as a fraction, zero
// while it isn't sending), the PWM duty last written and port D's direction, use, data and OCR2 registers.
// Safe to call from the PCM output hook; apart from the System PLL's ratio these change only at CPU writes.
struct Imx1MachineAudio {
    uint64_t rateNumerator = 0, rateDenominator = 0;
    bool pwmKnown = false;
    uint32_t pwmHigh = 0, pwmPeriod = 0;
    uint32_t gpioDDirection = 0, gpioDInUse = 0, gpioDData = 0, gpioDOutputSelect = 0;
};
Imx1MachineAudio imx1MachineAudio(const Imx1Machine*);
Imx1MachineBoundedRun imx1MachineRunBounded(Imx1Machine*, const Imx1MachineBounds&);
// Passes the wait loop has made since create, for tests. With the idle skip one pass covers
// many idle HCLK periods. It is a host count and no part of the machine's state.
uint64_t imx1MachineIdleAdvances(const Imx1Machine*);

// Borrowed pixels last until the next execution call. Peek does not acquire/ack.
const uint32_t* imx1MachinePeekFrame(const Imx1Machine*, uint32_t& width, uint32_t& height);
const uint32_t* imx1MachineAcquireFrame(Imx1Machine*, uint32_t& width, uint32_t& height,
                                      uint32_t& firstRow, uint32_t& lastRow);
void imx1MachineAcknowledgeFrame(Imx1Machine*);
// How the display scans the guest's frame onto the panel, which follows the OS's orientation:
// 1 portrait (as wired), 2 landscape, 3 reverse portrait, 4 reverse landscape, the SysGetOrientation
// numbers. 0 while the display is off or not set up.
unsigned imx1MachineScanOrientation(const Imx1Machine*);
// How many times the guest has started up from the ROM's entry, by its first register write. Counts a guest
// reset and a power-on, not sleep and wake.
uint32_t imx1MachineStartups(const Imx1Machine*);
Imx1BootstrapAccess imx1MachineReplacePenInputs(Imx1Machine*, const Imx1AspPenInputs&, bool commit = true);
// The pen inputs the machine has now, as the options or the last replace left them.
Imx1AspPenInputs imx1MachinePenInputs(const Imx1Machine*);
// Execution-thread raw physical inputs. Preview validates without changing guest state.
// Masks must stay within fixed known baselines; reserved/driven GPIO lanes are excluded.
Imx1BootstrapAccess imx1MachineReplaceGpioInputs(Imx1Machine*, const uint32_t (&values)[4],
                                               const uint32_t (&masks)[4], bool commit = true);
// The card in the internal MMCSD slot (slot 1), or null to take it out, with its card detect pin, GPIO B20,
// low while a card is in (ROM 204c5a5c). The guest sees the pin change and mounts or drops the card itself.
// Needs the virtual slot (mmcsdEmptySlot). A state saved with a card in keeps the card's own state and the
// transfer under way in its SDCD section; the card image itself stays outside. Setting the card already in
// changes nothing.
inline constexpr uint32_t imx1MmcsdCardDetectPin = 1u << 20;
Imx1BootstrapAccess imx1MachineSetMmcsdCard(Imx1Machine*, VSD* card, bool commit = true);
VSD* imx1MachineMmcsdCard(const Imx1Machine*);
Imx1BootstrapAccess imx1MachineReplaceGraphicsAxes(Imx1Machine*, uint32_t first, uint32_t second,
                                                 bool commit = true);
// The Zodiac 2's controls. Each is one GPIO input that reads low while pressed (ROM key scan 20084af4).
// The ROM proves the pins of Home, power, the stick click and the pins and key codes of the triggers and action
// buttons. ASSUMPTION "Zodiac button sides": which trigger is left and where each action button sits.
// ASSUMPTION "Zodiac Function and Bluetooth pins": D12 and D13 by elimination, the ROM has no key code for them.
// Not wired: D22 (cradle or HotSync, active high) and B23 (the scan's other stick click input).
enum class Imx1ZodiacControl {
    home, triggerLeft, triggerRight, power, stickClick, actionRight, actionUp, actionLeft, function, bluetooth,
    actionDown, count
};
struct Imx1ZodiacControlPin { const char* name; uint8_t port, pin; }; // port 0 is A, 3 is D
inline constexpr Imx1ZodiacControlPin imx1ZodiacControlPins[] = {
    {"home", 0, 7}, {"trigger-left", 0, 8}, {"trigger-right", 0, 9}, {"power", 0, 10}, {"stick-click", 3, 7},
    {"action-right", 3, 8}, {"action-up", 3, 9}, {"action-left", 3, 10}, {"function", 3, 12}, {"bluetooth", 3, 13},
    {"action-down", 3, 14}};
static_assert(sizeof imx1ZodiacControlPins / sizeof *imx1ZodiacControlPins == size_t(Imx1ZodiacControl::count));
Imx1BootstrapAccess imx1MachineSetZodiacControl(Imx1Machine*, Imx1ZodiacControl, bool pressed, bool commit = true);
// Stick offsets from rest, right and down positive, -511..511. This is a fixed virtual stick: rest is raw
// 512/512 and full travel the ends of the 10 bit range, which the ROM's default calibration (centre 0,
// scale 1024) turns into its full +-32767. The raw words are first = 512 - x and second = 512 + y.
inline constexpr int imx1ZodiacStickTravel = 511;
Imx1BootstrapAccess imx1MachineSetZodiacStick(Imx1Machine*, int x, int y, bool commit = true);
// v2 requires counters; absolute counters/recent rows are u64, memory stays u32.
// A configured diagnosticSnapshot retains recent entries, without automatic emission.
struct Imx1MachineDiagnosticSink {
    ArmDiagnosticSink architecture;
    void (*counters)(void*, const char*, const uint64_t*, uint32_t) = nullptr;
    void* context = nullptr;
};
void imx1MachineEmitDiagnostic(Imx1Machine*, const Imx1MachineDiagnosticSink&);

// Machine state, private to this build family. Save and load only between run calls.
// A state loads into a machine made from the same ROM, entry and options, fresh or already run,
// and that machine then continues the way the saved one did. Observers, contexts, the idle
// budget and idleStepped may differ. A failed check or load leaves the machine as it was.
// Saving drops translated code, so the saved and the loaded machine also agree on jitBlocks.
// Bump the revision in any patch that changes guest-visible i.MX1 or ARM920 behaviour or a state() list.
inline constexpr uint32_t imx1MachineStateCoreRevision = 760;
enum class Imx1StateStatus {
    ok, notAtBoundary, unsupportedConfiguration, truncated, badMagic, badFormatVersion, coreRevisionMismatch,
    layoutMismatch, corruptSection, missingSection, romMismatch, optionsMismatch, busMismatch, invalidValue
};
// Load a state from another core revision anyway; the result says coreRevisionDiffers.
inline constexpr uint32_t imx1StateAllowCoreRevision = 1;
// Load everything but the SDRAM and SRAM bytes, which stay as the machine has them. The power-on state
// loaded this way is a warm reset: the CPU, caches, board and graphics chip start again and RAM is kept.
inline constexpr uint32_t imx1StateKeepMemory = 2;
// Load a state whose machine was made with other GPIO A and D input levels or another scheduled GPIO
// event. Those options only say how the pins stood at reset; the levels a state continues with are
// in the state. A state saved before its event was due never gets it on a machine without one.
inline constexpr uint32_t imx1StateAllowBootInputs = 4;
// Load the power-on state as a reset through RESET_IN: RAM is kept as with imx1StateKeepMemory, and
// so are the clock controller, the SDRAM controller and the RTC's time, and the reset source
// register reads EXR (Imx1Bootstrap::takeExternalReset has the manual's sections).
inline constexpr uint32_t imx1StateExternalReset = 8;
struct Imx1StateInfo {
    Imx1StateStatus status = Imx1StateStatus::ok;
    uint32_t formatVersion = 0, coreRevision = 0;
    uint64_t retired = 0;
    // The section and the field or check a failure names, when there is one.
    char section[5]{};
    const char* detail = nullptr;
    // Saved by the other engine: the JIT counters start from zero.
    bool engineChanged = false;
    bool coreRevisionDiffers = false, buildNoteDiffers = false;
    // Load only: the card in the slot took on the state's card section. Without one, or for a card of
    // another size, the slot starts again with no transfer and the caller resets the card. With no card
    // in, the section waits for the next imx1MachineSetMmcsdCard.
    bool cardResumed = false;
};
// An attached I2C bus is saved through its stateIdentity, saveState and loadState; one with identity 0
// returns unsupportedConfiguration, and a state from another bus busMismatch. A frame from AcquireFrame is
// acknowledged first, or save and load return notAtBoundary. A load ends the pixels PeekFrame lent.
// buildNote is free text kept in the state.
Imx1StateStatus imx1MachineSaveState(Imx1Machine*, std::vector<uint8_t>& out, const char* buildNote = nullptr);
// Checks everything a load checks, without changing the machine.
Imx1StateInfo imx1MachineCheckState(const Imx1Machine*, const uint8_t* data, size_t size,
                                    uint32_t flags = 0, const char* buildNote = nullptr);
Imx1StateInfo imx1MachineLoadState(Imx1Machine*, const uint8_t* data, size_t size,
                                   uint32_t flags = 0, const char* buildNote = nullptr);
const char* imx1StateStatusName(Imx1StateStatus status);
void imx1MachineStop(Imx1Machine*);
// One process-owned CPU with optional bootstrap registers/RAM and qualified interrupt inputs.
Imx1ProbeResult imx1BootProbe(const uint8_t* rom, uint32_t size, Imx1ProbeEntry entry,
                            uint32_t steps, Imx1ProbeObserver observer = nullptr, void* context = nullptr,
                            bool fast = false, const Imx1ProbeOptions* options = nullptr);
const char* imx1ProbeStopName(Imx1ProbeStop stop);

#endif
