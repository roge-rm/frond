#ifndef FROND_IMX1_BOOT_PROBE_H
#define FROND_IMX1_BOOT_PROBE_H

#include "arm_diagnostic.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
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
    // False when the fetch at pc aborted: no instruction was fetched and instruction is 0. Sits in the padding,
    // so the struct's size doesn't change, and isn't saved in states (a load sets it true).
    bool fetched = true;
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
enum class Imx1MachineYield { retired, deadline, work, waiting, terminal, stopped, invalidBounds, pcStop, codeHook };
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
// Faster speed between runs: the CPU's FCLK at num / den times the MCU PLL's (Imx1Bootstrap::setCpuClockMultiplier).
// 1 / 1 is Original. Not saved in the state; a state load keeps the one set. False inside a run.
bool imx1MachineSetCpuClockMultiplier(Imx1Machine*, uint32_t num, uint32_t den);
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
// MMCSD and card DMA writes Fast blocks took in generated code (jitCardWrite).
uint64_t imx1MachineJitCardWrites(const Imx1Machine*);
// The card driver's STATUS wait rounds Fast blocks passed at once, since the process started: the times and the
// rounds. On unless FROND_CARD_POLL_SKIP=0; imx1SetCardPollSkip(false) turns it off, for tests.
uint64_t imx1MachineCardPollSkips(const Imx1Machine*);
uint64_t imx1MachineCardPollRounds(const Imx1Machine*);
void imx1SetCardPollSkip(bool skip);
// Fast's card lookahead: quiet spans run past MMCSD events that only change STATUS (a clock stop's end, a masked
// command end), and STATUS reads on the lean path are worked out at their own time. On unless
// FROND_CARD_LOOKAHEAD=0; imx1SetCardLookahead(false) turns it off from the next slice, for tests.
// imx1MachineLeanStatusReads counts those reads since the process started.
void imx1SetCardLookahead(bool on);
uint64_t imx1MachineLeanStatusReads(const Imx1Machine*);
// Of jitCardWrites, those the lookahead took with nothing charged (Imx1Bootstrap::cardRegisterLatch).
uint64_t imx1MachineJitCardLatches(const Imx1Machine*);
// MMCSD and DMA register reads past a card event the lookahead ran over, charged in generated code.
uint64_t imx1MachineJitChargedReads(const Imx1Machine*);
// For tests: every such read with the lookahead on is charged first, as one past a card event is.
void imx1SetChargedCardReads(bool on);
// Fast's slot 2 reads: the W4200 SD block's registers and data port and the identity read its driver's waits make,
// answered in generated code. On unless FROND_SLOT2_LEAN=0; imx1SetSlot2Lean(false) turns it off from the next slice,
// for tests. imx1MachineLeanSlot2Reads counts those reads since the process started.
void imx1SetSlot2Lean(bool on);
uint64_t imx1MachineLeanSlot2Reads(const Imx1Machine*);
uint64_t imx1MachineLeanSlot2Writes(const Imx1Machine*);
uint64_t imx1MachineJitSlot2Accesses(const Imx1Machine*);
// Fast blocks: slot 2's status wait rounds passed at once (skipSlot2Poll) since the process started; the skip is on
// unless FROND_SLOT2_POLL_SKIP=0, and imx1SetSlot2PollSkip(false) turns it off, for tests.
uint64_t imx1MachineSlot2PollRounds(const Imx1Machine*);
void imx1SetSlot2PollSkip(bool skip);
// Ring pointer writes Fast blocks took in generated code with the run's cycles charged first (jitChargedWrite).
uint64_t imx1MachineJitChargedWrites(const Imx1Machine*);
uint64_t imx1MachineDelaySkips(const Imx1Machine*);
// Of those, the ones at the start of a run a delay loop's timer read ended (the ROM's loops at 2009f678 and 2008492c).
uint64_t imx1MachineDelayHaltSkips(const Imx1Machine*);
uint64_t imx1MachineDelaySkipCycles(const Imx1Machine*);
// The vertical wait's scan status polls Fast blocks passed at once, since the process started: the times and the
// rounds. imx1SetScanPollSkip(false) turns the skip off, for tests.
uint64_t imx1MachineScanPollSkips(const Imx1Machine*);
uint64_t imx1MachineScanPollRounds(const Imx1Machine*);
// Fast runs a vertical wait read ended so the next run could skip the polls (scanHalted).
uint64_t imx1MachineScanHalts(const Imx1Machine*);
void imx1SetScanPollSkip(bool skip);
// UART status polls (ldr, tst, branch back) Fast blocks and Exact steps passed at once, since the process started:
// the times and the rounds. imx1SetUartPollSkip(false) turns the skip off, for tests.
uint64_t imx1MachineUartPollSkips(const Imx1Machine*);
uint64_t imx1MachineUartPollRounds(const Imx1Machine*);
void imx1SetUartPollSkip(bool skip);
// The pen routine's ASP FIFO wait rounds Fast blocks passed at once, since the process started: the times, the rounds
// and their CPU cycles. On unless FROND_ASP_POLL_SKIP=0; imx1SetAspPollSkip(false) turns it off, for tests.
uint64_t imx1MachineAspPollSkips(const Imx1Machine*);
uint64_t imx1MachineAspPollRounds(const Imx1Machine*);
uint64_t imx1MachineAspPollCycles(const Imx1Machine*);
void imx1SetAspPollSkip(bool skip);
// Xploids' draw retry rounds Fast blocks passed at once, since the process started: the times and the rounds. On
// unless FROND_SURFACE_WAIT_SKIP=0; imx1SetSurfaceWaitSkip(false) turns it off, for tests.
uint64_t imx1MachineSurfaceWaitSkips(const Imx1Machine*);
uint64_t imx1MachineSurfaceWaitRounds(const Imx1Machine*);
void imx1SetSurfaceWaitSkip(bool skip);
// Fast spans that ran past queued graphics batches' completions, and the lean accesses that refused for them, since
// the process started. On unless FROND_ZODIAC_CROSS=0; imx1SetCrossCompletions(false) turns it off, for tests.
uint64_t imx1MachineCrossSpans(const Imx1Machine*);
uint64_t imx1MachineCrossRefusals(const Imx1Machine*);
void imx1SetCrossCompletions(bool on);
// Ring submits Fast took with the board left behind the chip, since the process started. On unless
// FROND_ZODIAC_SUBMIT_LAG=0; imx1SetSubmitLag sets it, for tests.
uint64_t imx1MachineLagSubmits(const Imx1Machine*);
uint64_t imx1MachineLagBoundaries(const Imx1Machine*);
// Ring submits that left the chip behind too (FROND_ZODIAC_DEFER_SUBMIT), for traces.
uint64_t imx1MachineDeferredSubmits(const Imx1Machine*);
uint64_t imx1MachineCrossEnds(const Imx1Machine*);
void imx1SetSubmitLag(bool on);
// Whether data and prefetch aborts and undefined instructions go to this machine's guest vectors, as on the
// ARM920T. Off (the default), the machine stops on them. CP15 refusals still stop. Not saved in states.
// Execution thread, between runs; false inside a bounded run or after stop.
bool imx1MachineSetGuestExceptions(Imx1Machine* machine, bool on);
bool imx1MachineGuestExceptions(const Imx1Machine* machine);
// The graphics FIFO wait's rounds Fast blocks passed at once, since the process started: the times and the rounds.
// imx1SetFifoWaitSkip(false) turns the skip off, for tests.
uint64_t imx1MachineFifoWaitSkips(const Imx1Machine*);
uint64_t imx1MachineFifoWaitRounds(const Imx1Machine*);
void imx1SetFifoWaitSkip(bool skip);
// CPU reads of the YMF768's ready register (bank 1 38) since the machine was made or loaded, for tests.
uint64_t imx1MachineYmfReadyReads(const Imx1Machine*);
// The board time of the last of those reads, for tests (not saved).
Imx1ReferenceTime imx1MachineYmfLastReadyRead(const Imx1Machine*);
// The YMF768's engine samples made so far and how many of them sounded (host only, not saved).
void imx1MachineYmfSamples(const Imx1Machine*, uint64_t* samples, uint64_t* sounding);
// SSI1 words sent again from an empty FIFO since the process started (every machine, not saved), for tools.
uint64_t imx1MachinePcmUnderrunWords(const Imx1Machine*);
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
// The YMF768's selected bank, index and registers and the graphics chip's clock registers as text, for comparing
// where two runs end.
std::string imx1MachineChipSummary(const Imx1Machine*);
// A hash of SRAM, SDRAM and graphics memory as they are in memory (cache contents left out), for tests.
uint64_t imx1MachineMemoryHash(const Imx1Machine*);
// Times the Fast engine dropped its decoded code for a store to a page it came from.
uint64_t imx1MachineLeanCodeDrops(const Imx1Machine*);
// The last PCM sample SSI1 shifted out, for tests.
uint16_t imx1MachinePcmLastTransmitted(const Imx1Machine*);
// Sends each 16-bit word SSI1 finishes and its slot (0 the frame sync slot) to hook, on the thread that runs
// the machine, in guest time order. Kept through state loads; null stops it.
void imx1MachineSetPcmOutput(Imx1Machine*, void (*hook)(void*, uint16_t word, unsigned slot), void* context);
// Leaves the YMF768's FM out of the PCM output's words (the chip still runs and mixes as before, so nothing the
// guest sees or saves changes). Kept through state loads; off at first.
void imx1MachineSetFmOutputMuted(Imx1Machine*, bool muted);
// Puts the Zodiac's YMF768 on CS4 in place of the diagnostic recorder: its registers, its FM output mixed into
// the PCM output's words and its interrupt on GPIO B15. Call before the first instruction. Saved states carry it
// in an optional section; a state without one loads with the chip reset.
void imx1MachineEnableYmf768(Imx1Machine*);
// What the board's audio output looks like just now: SSI1's frame rate (frames a second as a fraction, zero
// while it isn't sending) and port D's direction, use, data and OCR2 registers.
// Safe to call from the PCM output hook; apart from the System PLL's ratio these change only at CPU writes.
struct Imx1MachineAudio {
    uint64_t rateNumerator = 0, rateDenominator = 0;
    uint32_t gpioDDirection = 0, gpioDInUse = 0, gpioDData = 0, gpioDOutputSelect = 0;
};
Imx1MachineAudio imx1MachineAudio(const Imx1Machine*);
Imx1MachineBoundedRun imx1MachineRunBounded(Imx1Machine*, const Imx1MachineBounds&);
// Passes the wait loop has made since create, for tests. With the idle skip one pass covers
// many idle HCLK periods. It is a host count and no part of the machine's state.
uint64_t imx1MachineIdleAdvances(const Imx1Machine*);
// Guest calls, between runs only. Pc stops: while set, bounded runs go instruction by instruction through the
// step (no JIT or lean spans) and end before the instruction at any of them, in either state, with
// Imx1MachineYield::pcStop. Setting replaces the ones before. Leave lets the run that starts on one run that
// instruction first. A machine with none set runs as before. Not saved; a load or reset keeps them.
inline constexpr unsigned imx1MachineMaximumPcStops = 4;
bool imx1MachineSetPcStops(Imx1Machine*, const uint32_t* pcs, unsigned count);
bool imx1MachineSetPcStop(Imx1Machine*, uint32_t pc);
void imx1MachineClearPcStop(Imx1Machine*);
void imx1MachineLeavePcStop(Imx1Machine*);
bool imx1MachineAtPcStop(const Imx1Machine*);
// The pc stops as they are, to put back after a nested call that sets its own (a Bluetooth delivery in a guest
// call), with leave and hit as they were.
struct Imx1MachinePcStops {
    bool armed = false, leave = false, hit = false;
    unsigned count = 0;
    uint32_t pcs[imx1MachineMaximumPcStops]{};
};
Imx1MachinePcStops imx1MachineSavePcStops(const Imx1Machine*);
void imx1MachineRestorePcStops(Imx1Machine*, const Imx1MachinePcStops&);
// Code hooks, between runs only: bounded runs end before the instruction at any of pcs, in either state, with
// Imx1MachineYield::codeHook, in Exact and Fast alike; unlike pc stops they leave Fast its lean path and JIT, which
// stop short of the hooks. Leave lets the next run run the instruction at the hook first. Setting replaces the
// ones before (none: count 0). Not saved; a load or reset keeps them. None set runs as before.
inline constexpr unsigned imx1MachineMaximumCodeHooks = 32;
bool imx1MachineSetCodeHooks(Imx1Machine*, const uint32_t* pcs, unsigned count);
void imx1MachineLeaveCodeHook(Imx1Machine*);
bool imx1MachineAtCodeHook(const Imx1Machine*);
// The current mode's r0 to r15 (pc with bit 0 set in Thumb) and the CPSR. Setting pc keeps the current state;
// register 16 is the CPSR, of which only the flags and the interrupt masks are set.
bool imx1MachineRegisters(const Imx1Machine*, uint32_t (&registers)[16], uint32_t* cpsr = nullptr);
bool imx1MachineSetRegister(Imx1Machine*, unsigned reg, uint32_t value);
// Guest virtual memory as the CPU's own loads and stores see it, caches included. RAM only (ROM for reads):
// false with nothing done for anything else, or an unmapped page.
bool imx1MachineAccessMemory(Imx1Machine*, uint32_t address, void* data, uint32_t size, bool write);

// Borrowed pixels last until the next execution call. Peek does not acquire/ack.
const uint32_t* imx1MachinePeekFrame(const Imx1Machine*, uint32_t& width, uint32_t& height);
const uint32_t* imx1MachineAcquireFrame(Imx1Machine*, uint32_t& width, uint32_t& height,
                                      uint32_t& firstRow, uint32_t& lastRow);
void imx1MachineAcknowledgeFrame(Imx1Machine*);
// How the display scans the guest's frame onto the panel, which follows the OS's orientation:
// 1 portrait (as wired), 2 landscape, 3 reverse portrait, 4 reverse landscape, the SysGetOrientation
// numbers. 0 while the display is off or not set up.
unsigned imx1MachineScanOrientation(const Imx1Machine*);
// The YMF768's LED and rumble motor outputs as the guest last set them, false while the chip is off. Reads
// only; for the frontends.
// The YMF768's LED and motor bits, and the power LED's colour bits (zodiac_led.h). Host side only: reads
// latched registers, with no time or side effect.
struct Imx1MachineAttention { bool led = false, motor = false; uint8_t power = 0; };
Imx1MachineAttention imx1MachineAttention(const Imx1Machine*);
// How many times the guest has started up from the ROM's entry, by its first register write. Counts a guest
// reset and a power-on, not sleep and wake.
uint32_t imx1MachineStartups(const Imx1Machine*);
Imx1BootstrapAccess imx1MachineReplacePenInputs(Imx1Machine*, const Imx1AspPenInputs&, bool commit = true);
// The pen inputs the machine has now, as the options or the last replace left them.
Imx1AspPenInputs imx1MachinePenInputs(const Imx1Machine*);
// The ASP's U input word (the Zodiac's battery), for the next U conversion. Not saved with the state: a load
// or a fresh machine starts from aspUSample again. Needs aspUSampleKnown.
Imx1BootstrapAccess imx1MachineSetAspUInput(Imx1Machine*, uint16_t word);
uint16_t imx1MachineAspUInput(const Imx1Machine*);
// A GPIO port's input sample (0 A to 3 D), as the options or the last replace left it.
// Whether the system clocks are in stop mode (the guest asleep).
bool imx1MachineSystemStopped(const Imx1Machine*);
uint32_t imx1MachineGpioInput(const Imx1Machine*, unsigned port);
// UART1 (unit 1, the connector's serial port) or UART2 (unit 2, infrared) as a host peer sees it.
struct Imx1MachineUartLine {
    bool enabled{false};      // UARTEN
    bool receiverOn{false};   // takes characters now
    bool infrared{false};     // IREN
    bool ctsLow{false};       // the CTS output: the guest takes more
    bool rtsLow{false};       // the RTS input as set
    uint32_t format{0};       // UCR2 WS, STPB, PROE and PREN
    uint64_t bitsPerSecond{0};
};
Imx1MachineUartLine imx1MachineUartLine(const Imx1Machine*, unsigned unit);
// Between runs: the RTS input level (low is active), and a peer (Imx1Uart::setPeer and setSource; all
// null detaches). Refused on a stopped machine.
Imx1BootstrapAccess imx1MachineSetUartRts(Imx1Machine*, unsigned unit, bool low);
Imx1BootstrapAccess imx1MachineSetUartPeer(Imx1Machine*, unsigned unit, Imx1UartTransmit transmit,
                                          Imx1UartPending pending, Imx1UartTake take, void* context);
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
// The id of the card in the slot, saved with its card section so a load can tell it's the same card.
// Null or empty for none.
constexpr size_t imx1CardIdBytes = 32;
void imx1MachineSetMmcsdCardId(Imx1Machine*, const char* id);
// The card in the W4200's own SD slot (slot 2, Zodiac only), or null to take it out. The slot's card detect
// (16010910 bit 2) follows it, and with the driver's card events on an insert or remove raises the chip's SD
// interrupt. Needs the virtual graphics profile. A state saved once the slot has had a card keeps the slot's
// registers, the transfer under way and the card's own state in its SD2 section. Setting the card already in
// changes nothing.
Imx1BootstrapAccess imx1MachineSetGraphicsSdCard(Imx1Machine*, VSD* card, bool commit = true);
void imx1MachineSetGraphicsSdWriteProtect(Imx1Machine*, bool on);
VSD* imx1MachineGraphicsSdCard(const Imx1Machine*);
Imx1BootstrapAccess imx1MachineReplaceGraphicsAxes(Imx1Machine*, uint32_t first, uint32_t second,
                                                 bool commit = true);
// The graphics chip's aux ADC input (mode 2 on the default route), 0-1023, saved with the state like the axes.
// On the Zodiac it's the accessory ID line the DAL's 200876b6 reads; the options' graphicsRawAux starts it.
Imx1BootstrapAccess imx1MachineSetGraphicsAux(Imx1Machine*, uint32_t sample, bool commit = true);
uint32_t imx1MachineGraphicsAux(const Imx1Machine*);
// The graphics chip's mode 1 ADC input, 0-1023, saved with the state. On the Zodiac it's the battery the DAL's
// 20087382 reads; the options' graphicsRawMode1 starts it.
Imx1BootstrapAccess imx1MachineSetGraphicsMode1(Imx1Machine*, uint32_t sample);
uint32_t imx1MachineGraphicsMode1(const Imx1Machine*);
// The W4200's USB function from the host side (Imx1GraphicsUsb), between runs. Each change also drives the
// chip's interrupt line. A stopped machine or a refused line change answers Timeout (false for the reset).
struct Imx1UsbFunctionState { bool listening = false; uint32_t enableCount = 0; uint8_t address = 0; };
Imx1UsbFunctionState imx1MachineUsbState(const Imx1Machine*);
bool imx1MachineUsbBusReset(Imx1Machine*);
UsbPacketResult imx1MachineUsbSetup(Imx1Machine*, const uint8_t* data);
// Endpoint 0 is the control endpoint; take false only peeks at an IN packet and changes nothing.
UsbPacketResult imx1MachineUsbOut(Imx1Machine*, uint8_t endpoint, const uint8_t* data, unsigned size);
UsbPacketResult imx1MachineUsbIn(Imx1Machine*, uint8_t endpoint, uint8_t* data, unsigned* size, bool take);
bool imx1MachineUsbOutReady(const Imx1Machine*, uint8_t endpoint);
// The USB host has a tick due: a waiting guest's clock runs on to the bound instead of waiting.
void imx1MachineSetUsbHostPending(Imx1Machine*, bool pending);
// The RTC moved on by whole seconds, as with the device asleep that long (Imx1Rtc::forwardSeconds): for a session
// opened again. False when the RTC has no known time or isn't running.
bool imx1MachineForwardRtc(Imx1Machine*, uint64_t seconds);
// The Zodiac 2's controls. Each is one GPIO input that reads low while pressed (ROM key scan 20084af4).
// The ROM proves the pins of Home, power, the stick click and the pins and key codes of the triggers and action
// buttons. The same scan reads A8 as TriggerA (the SDK's left trigger) and A9 as TriggerB (right). The action buttons go A top, then clockwise: the key scan reads A on D9, B on D8, C on D14 and D on D10,
// and in landscape the rotation at 20084d26 turns them one place, so top is D10, right D9, bottom D8, left D14.
// The scan reads D12 as key bit 0x8000 and D13 as 0x01000000. D12 is Function: the ROM's stick calibration
// ("press and hold the function button") goes on only for it. D13 is Bluetooth: a press in the launcher shows
// "Discoverable for 3 minutes".
// Not wired: D22 (the cradle's HotSync button, active low, idle high) and B23 (the scan's other stick click input).
enum class Imx1ZodiacControl {
    home, triggerLeft, triggerRight, power, stickClick, actionRight, actionUp, actionLeft, function, bluetooth,
    actionDown, count
};
struct Imx1ZodiacControlPin { const char* name; uint8_t port, pin; }; // port 0 is A, 3 is D
inline constexpr Imx1ZodiacControlPin imx1ZodiacControlPins[] = {
    {"home", 0, 7}, {"trigger-left", 0, 8}, {"trigger-right", 0, 9}, {"power", 0, 10}, {"stick-click", 3, 7},
    {"action-right", 3, 9}, {"action-up", 3, 10}, {"action-left", 3, 14}, {"function", 3, 12}, {"bluetooth", 3, 13},
    {"action-down", 3, 8}};
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
// The app refuses a state from another core revision (imx1StateAllowCoreRevision aside), so a bump stops every
// saved session from loading. Bump it only when an old state can't carry on correctly at all. A state() list
// change is caught by the layout fingerprint and section versions; behaviour changes leave the revision as is.
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
    // The state has a card section: it was saved with a card in the slot.
    bool cardSection = false;
    // The saved card's id, when the state has it.
    bool cardIdKnown = false;
    char cardId[imx1CardIdBytes + 1]{};
    // Slot 2: the state has an SD2 section, and (load only) the card in slot 2 took on its card state.
    bool graphicsSdSection = false, graphicsSdResumed = false;
    // The Bluetooth controller address in HCI byte order. Zero means the state has no address.
    std::array<uint8_t, 6> bluetoothAddress{};
};
// An attached I2C bus is saved through its stateIdentity, saveState and loadState; one with identity 0
// returns unsupportedConfiguration, and a state from another bus busMismatch. A frame from AcquireFrame is
// acknowledged first, or save and load return notAtBoundary. A load ends the pixels PeekFrame lent.
// buildNote is free text kept in the state.
Imx1StateStatus imx1MachineSaveState(Imx1Machine*, std::vector<uint8_t>& out, const char* buildNote = nullptr,
                                     std::array<uint8_t, 6> bluetoothAddress = {});
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
