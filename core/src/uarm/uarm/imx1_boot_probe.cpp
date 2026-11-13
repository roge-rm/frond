#include "imx1_boot_probe.h"
#include "imx1_cs5_bus.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include "imx1_graphics_frame.h"
#include "imx1_graphics_commands.h"
#include "imx1_graphics_gpu_gles.h"
#include <array>
#include <vector>

#include "CPU.h"
#include "arm920_state.h"
#include "cpu_diagnostics.h"
#include "machine_state_archive.h"
#include "md5.h"
#include "patch_dispatch.h"
#include "system_state.h"

void imx1NoteStartup(const void* probe);
namespace {
// CPU cycles of retired instructions not charged to the board and the graphics chip yet, all in clock
// mode quietMode, and how many more can wait. No model changes state and no bound is reached within
// them, so one charge of the sum gives the state the single charges would. Both are 0 between slices.
// Only one machine exists at a time. These stay out of Probe, whose size is part of the state layout.
uint64_t deferredCycles = 0, quietCycles = 0;
uint32_t quietMode = 0;
// Fast only: CS5 wait clocks (HCLK) of graphics memory writes the lean path took inside the quiet span, charged
// after deferredCycles. The span covers both, so the order of the two charges changes nothing.
uint64_t deferredHclk = 0;
// Set when an instruction that charged opened a new quiet span itself: the next step still looks at the interrupt
// lines, as it would after charging that instruction alone.
bool linesPending = false;
uint64_t leanRetired = 0;  // instructions the Fast engine ran on the lean path since the machine was made
uint64_t leanTimerReads = 0;  // timer count reads answered on the lean path, for tests
uint64_t jitRetired = 0, jitRuns = 0;
bool fastBlocksDefault = true;  // what new machines start with (imx1SetFastBlocksDefault)  // instructions and runs of the Fast engine's JIT since the machine was made
uint64_t leanCs5Writes = 0;  // graphics memory writes taken on the lean path, for tests
uint64_t leanCs5Reads = 0;  // CS5 register reads answered on the lean path, for tests
uint64_t leanCs5MemoryReads = 0;  // graphics memory reads answered on the lean path, for tests
uint64_t leanBoardReads = 0;  // ASP, GPIO and AITC reads answered on the lean path, for tests
// One lean run's graphics memory writes: whether they can be taken and their wait clocks, worked out at the first
// one, and the wait clocks and budget they took.
struct LeanCs5 {
    bool checked, usable;
    // CS5 register reads: the GPIO and graphics checks, worked out at the first one. A board write clears them.
    bool registerChecked, registerUsable;
    // Graphics memory: memoryReady, worked out at the first access, as only MMIO writes change it.
    bool memoryChecked, memoryUsable;
    uint32_t clocks, cost;
    uint64_t waitClocks, waitCost;
};
LeanCs5 leanCs5{};
// Fast blocks: set while cpuArm920RunJit runs, with its budget; jitHalt when a board write asked it to stop at its
// next block (SLOW_PATH_REASON_RESCHEDULE, cleared after the run).
bool inJit = false, jitHalt = false;
uint32_t jitBudget = 0;
uint64_t jitBoardWrites = 0, jitReads = 0, jitRegisterReads = 0, delaySkips = 0, delaySkipCycles = 0;  // for tests
uint64_t scanPollSkips = 0, scanPollRounds = 0;  // for tests
bool scanPollSkip = true;
// Spans shorter than this are left to the step.
constexpr uint64_t leanMinimumSpan = 8;

struct Probe {
    explicit Probe(const Imx1ProbeOptions& config)
        : options(config), peripherals(config.aspPx1HighFromReset, config.aspUSampleKnown, config.aspUSample), cs4Capture(config.cs4Port0Input, config.cs4IndexedInputs),
          cs5Reset(config.graphicsProfile == Imx1GraphicsProfile::strict ? config.cs5ResetStatusInput : 0),
          cs5Configuration(config.graphicsProfile, config.graphicsVirtualA4SourceOver, config.graphicsVirtualPackedMono),
          graphicsMemory(config.graphicsProfile == Imx1GraphicsProfile::virtual5744Compatible8MiB ?
                         new Imx1GraphicsMemory() : nullptr) {
        if (config.aspPenInputs.contactKnown || config.aspPenInputs.normalKnown || config.aspPenInputs.referenceKnown)
            peripherals.replaceAspPenInputs(config.aspPenInputs);
        peripherals.setLcdcContrastOnly(config.lcdcContrastOnly);
        peripherals.setMmcsdEmptySlot(config.mmcsdEmptySlot);
        configureAcquisition(cs5Configuration, config);
    }
    // The acquisition route and raw inputs from the options. A state load starts its copy the same way.
    static void configureAcquisition(Imx1Cs5Configuration& graphics, const Imx1ProbeOptions& config) {
        graphics.setAcquisitionMode2Route(config.graphicsMode2Route);
        if (config.graphicsRawAuxKnown) graphics.setAcquisitionAuxInput(config.graphicsRawAux);
        if (config.graphicsRawMode1Known) graphics.setAcquisitionMode1Input(config.graphicsRawMode1);
        if (config.graphicsRawAxesKnown) graphics.setAcquisitionInput(config.graphicsRawFirst, config.graphicsRawSecond);
    }
    std::vector<uint8_t> rom;
    // MD5 of rom for the state identity, worked out when first needed.
    mutable bool romDigestKnown = false;
    mutable uint8_t romDigest[16]{};
    uint32_t alias;
    Imx1ProbeResult result{};
    struct Recent { uint64_t step; uint32_t pc, instruction, cpsr, sp, lr, r0, r1, r2, r3; };
    std::vector<Recent> recent;
    uint32_t recentNext = 0, recentCount = 0;
    uint64_t retired = 0, work = 0;
    const Imx1MachineBounds* bounds = nullptr;
    uint64_t sliceWorkStart = 0;
    bool boundedYield = false;
    // Passes through the wait loop. A host count for tests, the skip makes fewer of them than idle periods.
    uint64_t idleAdvances = 0;
    bool boundReached() {
        if (!bounds) return false;
        boundedYield = work - sliceWorkStart >= bounds->work ||
            peripherals.referenceTime().compare(bounds->deadline) >= 0;
        return boundedYield;
    }
    // HCLK periods a waiting CPU can pass in one advance. Every period before the last one
    // changes no peripheral or graphics state and reaches no idle, work or deadline limit.
    uint64_t idleQuietHclk(Imx1PllRatio period, uint64_t idleHclkLimit) const {
        // advanceHclkCycles takes 32 bits.
        uint64_t clocks = std::min<uint64_t>(peripherals.idleQuietHclk(), uint64_t(1) << 30);
        clocks = std::min(clocks, cs5Configuration.idleQuietSteps(period.numerator, period.denominator));
        clocks = std::min(clocks, idleHclkLimit - result.idleHclk);
        if (bounds) {
            clocks = std::min(clocks, bounds->work - (work - sliceWorkStart));
            const auto& now = peripherals.referenceTime();
            if (now.compare(bounds->deadline) >= 0) return 1;
            // Periods until the deadline, rounded up. Without room for the exact product, the whole ticks alone.
            const auto left = bounds->deadline.intervalSince(now);
            const __uint128_t max = ~__uint128_t(0);
            __uint128_t steps;
            if (left.ticks > (max - left.numerator) / left.denominator ||
                (__uint128_t(left.ticks) * left.denominator + left.numerator) > max / period.denominator ||
                left.denominator > max / period.numerator) {
                steps = __uint128_t(left.ticks) * period.denominator / period.numerator;
            } else {
                const __uint128_t value = (__uint128_t(left.ticks) * left.denominator + left.numerator) * period.denominator;
                const __uint128_t divisor = left.denominator * period.numerator;
                steps = value / divisor + (value % divisor != 0);
            }
            if (steps < clocks) clocks = uint64_t(steps);
        }
        if (clocks > 1 && !peripherals.referenceChargeSupported(clocks * period.numerator, period.denominator)) return 1;
        return clocks ? clocks : 1;
    }
    Imx1ProbeObserver observer = nullptr;
    void* observerContext = nullptr;
    bool blPrefix = false;
    uint32_t blNextPc = 0;
    struct InterruptedPrefix { uint32_t pc, cpsr, mode, lr, sp; };
    std::array<InterruptedPrefix, 256> interruptedPrefixes{};
    size_t interruptedPrefixCount = 0;
    bool prefixCapacityExceeded = false;
    bool terminal = false, stopped = false;
    bool failed = false;
    ArmCpu* cpu = nullptr;
    ArmMem* mem = nullptr;
    Imx1ProbeOptions options{};
    Imx1Bootstrap peripherals;
    Imx1Cs4Capture cs4Capture;
    Imx1Cs5Reset cs5Reset;
    Imx1Cs5Configuration cs5Configuration;
    std::unique_ptr<Imx1GraphicsMemory> graphicsMemory;
    Imx1GraphicsFrame graphicsFrame;
    Imx1GraphicsCommands graphicsCommands;
    // The host GPU copy of graphics memory, Fast only and when FROND_ZODIAC_GPU asks for it. Not saved.
    struct GraphicsGpu {
        std::unique_ptr<Imx1GraphicsGpu> gpu;
        bool failed = false;
    } graphicsGpu;
    // Graphics memory as a memory-to-memory DMA reaches it: through the EIM's CS5 set-up like a CPU access, with
    // its wait clocks a beat, in the virtual graphics profile only.
    struct GraphicsDma final : Imx1DmaExternal {
        Probe& p;
        explicit GraphicsDma(Probe& probe) : p(probe) {}
        bool owns(uint32_t address) const override {
            return p.graphicsMemory && p.cs5Configuration.virtualProfile() && Imx1GraphicsMemory::owns(address);
        }
        Imx1BootstrapAccess check(uint32_t address, uint32_t size, bool write) const override {
            const auto transfer = p.peripherals.eimState().cs5Transfer(address, size, write);
            if (transfer == Imx1EimAccess::width) return Imx1BootstrapAccess::width;
            if (transfer != Imx1EimAccess::ok) return Imx1BootstrapAccess::sideEffect;
            if (!p.cs5Configuration.memoryReady(p.peripherals.eimState())) return Imx1BootstrapAccess::sequence;
            return p.graphicsMemory->validate(p.cs5Configuration, p.peripherals.eimState(), address, size);
        }
        void move(uint32_t address, uint32_t size, bool write, uint32_t& value) override {
            const auto status = p.graphicsMemory->access(p.cs5Configuration, p.peripherals.eimState(), address, size,
                                                         write, value);
            assert(status == Imx1BootstrapAccess::ok);
            (void)status;
        }
        bool clocks(uint32_t& perBeat) const override { return p.quietWriteClocks(perBeat); }
    } graphicsDma{*this};
    // graphicsGpu and graphicsDma, left out of the saved layout's size.
    static constexpr size_t unsavedBytes = sizeof(GraphicsGpu) + sizeof(GraphicsDma);
    // Attaches the GPU to graphics memory as it is now, or lets it go outside Fast.
    void attachGraphicsGpu() {
        const auto mode = Imx1GraphicsGpu::modeFromEnvironment();
        if (!fastEngine || !graphicsMemory || mode == Imx1GraphicsGpu::Mode::off) {
            if (graphicsGpu.gpu) graphicsGpu.gpu->detach();
            graphicsGpu.gpu.reset();
            return;
        }
        if (!graphicsGpu.gpu) {
            if (graphicsGpu.failed) return;
            uint32_t banks[2] = {};
            for (unsigned bank = 0; bank < 2; ++bank) graphicsMemory->rawBank(bank, banks[bank]);
            std::string error;
            auto backend = imx1MakeGraphicsGpuBackend(banks, error);
            if (!backend) {
                std::fprintf(stderr, "zodiac gpu: off, %s\n", error.c_str());
                graphicsGpu.failed = true;
                return;
            }
            const bool differential = mode == Imx1GraphicsGpu::Mode::differential;
            std::fprintf(stderr, "zodiac gpu: %s%s\n", backend->renderer().c_str(), differential ? ", differential" : "");
            graphicsGpu.gpu = std::make_unique<Imx1GraphicsGpu>(std::move(backend), differential);
        }
        // Already attached to this memory: nothing to bring back or upload.
        if (graphicsGpu.gpu->attachedMemory() == graphicsMemory.get()) return;
        graphicsGpu.gpu->attach(graphicsMemory.get());
    }
    // What the command decoder said about the batch it last refused. For the stop text, not saved.
    Imx1GraphicsCommands::Refusal graphicsRefusal;
    uint32_t graphicsSampledPin = 32, graphicsSampleOffset = 0;
    uint64_t i2cIssueStep = 0;
    uint32_t i2cIssuePc = 0;
    bool irqRaised = false, fiqRaised = false;
    // The Fast engine: inside a quiet span the lean interpreter runs instead of the guarded step. Not saved.
    // fastJit: Dynarmic blocks run there first (cpuArm920RunJit), falling back to the lean path and the step.
    // fastBlocks is whether Fast may use them (on by default, off for lean-only runs).
    bool fastEngine = false, fastJit = false, fastBlocks = fastBlocksDefault;
    Imx1BootstrapAccess driveGraphicsLine(const Imx1Cs5Configuration& candidate, bool commit) {
        if (!options.graphicsIrqA13) return Imx1BootstrapAccess::ok;
        const auto line = candidate.interruptState().interruptOutput();
        // The explicit virtual pull-up resolves a released INTb to high.
        const uint32_t value = line == Imx1GpioLine::clear ? 0x2000 : 0;
        const uint32_t known = line == Imx1GpioLine::unknown ? 0 : 0x2000;
        return peripherals.driveGpioInput(0, value, known, commit);
    }
    // What follows a committed graphics state.
    Imx1BootstrapAccess graphicsCommitted() {
        if (!cs5Configuration.engineState().ringBytes()) graphicsCommands.reset();
        if (graphicsMemory && cs5Configuration.engineState().completed())
            return graphicsCommands.complete(cs5Configuration,peripherals.eimState(),*graphicsMemory);
        return Imx1BootstrapAccess::ok;
    }
    Imx1BootstrapAccess acceptGraphics(const Imx1Cs5Configuration& candidate, bool commit) {
        const auto status = driveGraphicsLine(candidate, commit);
        if (status != Imx1BootstrapAccess::ok || !commit) return status;
        cs5Configuration = candidate;
        return graphicsCommitted();
    }
    // Moves the graphics chip's time on with advance and accepts the result. Where acceptGraphics can't
    // refuse it, the chip advances in place; otherwise a copy does, so a refusal leaves the chip as it was.
    // A time advance changes the interrupt state only through observeDisplay with a known source, which
    // can set the display bit but never make the line unknown. A known line before it is then accepted
    // when the A13 drive is accepted at both levels, so the in-place drive below can't be refused.
    template <class Advance> Imx1BootstrapAccess advanceGraphics(Advance advance) {
        bool inPlace = !options.graphicsIrqA13;
        if (!inPlace) {
            const auto line = cs5Configuration.interruptState().interruptOutput();
            inPlace = line != Imx1GpioLine::unknown && peripherals.gpioDriveBothAccepted(0, 0x2000);
        }
        if (!inPlace) {
            auto candidate = cs5Configuration;
            advance(candidate);
            return acceptGraphics(candidate, true);
        }
        advance(cs5Configuration);
        if (options.graphicsIrqA13) {
            // The same level as the settled drive changes no pin, and its gate was checked above.
            const auto line = cs5Configuration.interruptState().interruptOutput();
            if (line == Imx1GpioLine::unknown ||
                !peripherals.gpioDriveSettled(0, line == Imx1GpioLine::clear ? 0x2000 : 0, 0x2000)) {
                const auto status = driveGraphicsLine(cs5Configuration, true);
                assert(status == Imx1BootstrapAccess::ok);
                if (status != Imx1BootstrapAccess::ok) return status;
            }
        }
        return graphicsCommitted();
    }
    Imx1BootstrapAccess advanceGraphicsCpu(uint32_t cycles, Imx1PllRatio period) {
        if (!peripherals.referenceChargeSupported(uint64_t(cycles) * period.numerator, period.denominator))
            return Imx1BootstrapAccess::sideEffect;
        // advanceCpuCycles refuses this before it changes anything.
        if (!cs5Configuration.referencePeriodSupported(period)) return Imx1BootstrapAccess::sideEffect;
        return advanceGraphics([&](Imx1Cs5Configuration& c) { c.advanceCpuCycles(cycles, period); });
    }
    Imx1BootstrapAccess graphicsWireStatus = Imx1BootstrapAccess::ok;
    // CPU cycles from now that can be charged later in one piece. Worked out only after the interrupt
    // lines and the clock boundary have been looked at in the state it starts from.
    uint64_t quietCpuCycles(uint32_t mode) const {
        if (options.idleStepped) return 0;
        const auto cpuPeriod = peripherals.cpuReferencePeriod(mode);
        uint64_t cycles = cs5Configuration.quietCpuCycles(cpuPeriod);
        if (!cycles) return 0;
        // The board counts its quiet time in HCLK periods, the last of which is the one with the change.
        // One more is left out for the part of a System PLL clock an asynchronous charge can carry over.
        const uint64_t hclk = std::min<uint64_t>(peripherals.idleQuietHclk(), uint64_t(1) << 30);
        if (hclk < 3) return 0;
        const auto busPeriod = peripherals.busReferencePeriod();
        const __uint128_t board = __uint128_t(hclk - 2) * busPeriod.numerator * cpuPeriod.denominator /
                                  (__uint128_t(busPeriod.denominator) * cpuPeriod.numerator);
        // One charge takes 32 bits of cycles.
        cycles = std::min<uint64_t>(cycles, uint64_t(1) << 24);
        if (board < cycles) cycles = uint64_t(board);
        if (bounds && cycles) {
            const auto& now = peripherals.referenceTime();
            if (now.compare(bounds->deadline) >= 0) return 0;
            // Cycles until the deadline, rounded up; the one that reaches it is charged alone.
            const auto left = bounds->deadline.intervalSince(now);
            const __uint128_t max = ~__uint128_t(0);
            __uint128_t steps;
            if (left.ticks > (max - left.numerator) / left.denominator ||
                (__uint128_t(left.ticks) * left.denominator + left.numerator) > max / cpuPeriod.denominator ||
                left.denominator > max / cpuPeriod.numerator) {
                steps = __uint128_t(left.ticks) * cpuPeriod.denominator / cpuPeriod.numerator;
            } else {
                const __uint128_t value = (__uint128_t(left.ticks) * left.denominator + left.numerator) * cpuPeriod.denominator;
                const __uint128_t divisor = left.denominator * cpuPeriod.numerator;
                steps = value / divisor + (value % divisor != 0);
            }
            if (!steps) return 0;
            if (steps - 1 < cycles) cycles = uint64_t(steps - 1);
        }
        if (cycles && !peripherals.referenceChargeSupported(cycles * cpuPeriod.numerator, cpuPeriod.denominator)) return 0;
        return cycles;
    }
    // Charges the waiting cycles. Anything that reads or changes the board, the graphics chip or the
    // time calls this first. A refusal is kept in graphicsWireStatus for the next clock boundary.
    bool chargeDeferred() {
        quietCycles = 0;
        linesPending = false;
        if (!deferredCycles && !deferredHclk) return true;
        const uint32_t cycles = uint32_t(deferredCycles);
        const uint32_t hclk = uint32_t(deferredHclk);
        deferredCycles = deferredHclk = 0;
        auto status = advanceGraphicsCpu(cycles, peripherals.cpuReferencePeriod(quietMode));
        if (status == Imx1BootstrapAccess::ok) {
            if (graphicsMemory) graphicsFrame.advance(cs5Configuration, *graphicsMemory, peripherals.eimState());
            peripherals.advanceCpuCycles(cycles, quietMode, options.assumeMpllSettled);
            status = peripherals.clockStatus();
        }
        // The wait clocks as cs5Wait charges them, in pieces of at most one transaction.
        if (status == Imx1BootstrapAccess::ok && hclk) {
            const auto bus = peripherals.busReferencePeriod();
            const auto cpuPeriod = peripherals.cpuReferencePeriod(quietMode);
            peripherals.advanceHclkCycles(hclk);
            status = advanceGraphics([&](Imx1Cs5Configuration& c) {
                for (uint32_t left = hclk; left;) {
                    const uint32_t chunk = std::min(left, Imx1Cs5Bus::timeoutClocks);
                    c.advanceBusCycles(chunk, bus, cpuPeriod);
                    left -= chunk;
                }
            });
            if (graphicsMemory) graphicsFrame.advance(cs5Configuration, *graphicsMemory, peripherals.eimState());
            if (status == Imx1BootstrapAccess::ok) status = peripherals.clockStatus();
        }
        if (status == Imx1BootstrapAccess::ok) return true;
        if (graphicsWireStatus == Imx1BootstrapAccess::ok) graphicsWireStatus = status;
        return false;
    }
    bool idleWakePossible() const {
        if (peripherals.idleWakePossible()) return true;
        // Preview the connected display request through GPIO and AITC without driving a pin.
        return options.graphicsIrqA13 && cs5Configuration.clockState().programmed() &&
            cs5Configuration.displayState().lcdEnabled() &&
            (cs5Configuration.interruptState().enableValue() & Imx1GraphicsInterrupts::displayMask) &&
            peripherals.gpioWakePossible(0, 0, 0x2000);
    }
    void updateInterrupts() {
        const auto lines = peripherals.interruptLines();
        const bool irq = lines.irq == Imx1AitcLine::asserted;
        const bool fiq = lines.fiq == Imx1AitcLine::asserted;
        if (irq != irqRaised) { cpuIrq(cpu, false, irq); irqRaised = irq; }
        if (fiq != fiqRaised) { cpuIrq(cpu, true, fiq); fiqRaised = fiq; }
    }
    // A card DMA wrote this RAM page: as a CPU store there, decoded and translated code from it goes.
    static void dmaStored(void* context, uint32_t pa) {
        auto& p = *static_cast<Probe*>(context);
        if (!p.cpu) return;
        if (!p.fastJit) cpuDropTranslatedCode(p.cpu);
        if (p.fastEngine) cpuArm920LeanStored(p.cpu, pa);
    }
    static void i2cEvent(void* context, const Imx1I2cTransfer& transfer, Imx1I2cResponse response) {
        auto& p = *static_cast<Probe*>(context);
        const auto& state = p.peripherals.i2cState();
        p.result.i2cEvent = {p.result.trace.step, p.result.trace.pc, p.i2cIssueStep, p.i2cIssuePc,
                             transfer, response, state.controlValue(), state.statusValue(), state.clockDivider()};
        if (p.options.i2cObserver) p.options.i2cObserver(p.options.i2cContext, p.result.i2cEvent);
    }

    void finishGraphicsBusStall() {
        const auto period = peripherals.cpuReferencePeriod(3);
        const auto units = peripherals.finishSystemPllBusStall();
        if (!units) return;
        ++work;
        graphicsWireStatus = advanceGraphics([&](Imx1Cs5Configuration& c) { c.advanceReference(units, period.denominator); });
    }

    // Wait clocks from now that change no board or graphics state, at most a whole transaction. 0 with the
    // stepped idle loop, before the chip's clocks are programmed or with a clock status already pending.
    uint32_t quietWaitClocks(Imx1PllRatio busPeriod) const {
        if (options.idleStepped || !cs5Configuration.clockState().programmed() ||
            peripherals.clockStatus() != Imx1BootstrapAccess::ok) return 0;
        // The last period of each count is the one with the change.
        uint64_t quiet = std::min<uint64_t>(peripherals.idleQuietHclk(), uint64_t(1) << 30);
        quiet = std::min(quiet, cs5Configuration.idleQuietSteps(busPeriod.numerator, busPeriod.denominator));
        return quiet > 1 ? uint32_t(std::min<uint64_t>(quiet - 1, Imx1Cs5Bus::timeoutClocks)) : 0;
    }
    Imx1BootstrapAccess cs5Wait(uint32_t& elapsed) {
        const auto mode = peripherals.eimState().cs5Mode();
        if (mode == Imx1Cs5Mode::unsupported) return Imx1BootstrapAccess::sequence;
        // The fixed-cycle diagnostic predates mux qualification. External transport
        // uses the virtual GPU connection, independently of GPIO SSR diagnostic levels.
        if (mode != Imx1Cs5Mode::fixed && !peripherals.cs5PinsSelected(true)) return Imx1BootstrapAccess::sequence;
        const auto busPeriod = peripherals.busReferencePeriod();
        const auto cpuPeriod = peripherals.cpuReferencePeriod(cpuArm920ClockMode(cpu));
        if (!cs5Configuration.busPeriodSupported(busPeriod,cpuPeriod)) return Imx1BootstrapAccess::sideEffect;
        if (mode != Imx1Cs5Mode::fixed &&
            (!cs5Configuration.memoryInterfaceReady() || !cs5Configuration.clockState().programmed()))
            return Imx1BootstrapAccess::externalInput;
        if (!peripherals.referenceChargeSupported(uint64_t(Imx1Cs5Bus::timeoutClocks) * busPeriod.numerator, busPeriod.denominator))
            return Imx1BootstrapAccess::sideEffect;
        Imx1Cs5Bus transaction(mode);
        Imx1GraphicsFraction service;
        bool ready = mode == Imx1Cs5Mode::fixed;
        auto timedStatus = Imx1BootstrapAccess::ok;
        auto state = transaction.sample(0,true,mode == Imx1Cs5Mode::dtack ? !ready : ready);
        // Wait clocks inside a quiet span of the board and the chip are charged in one piece. The ready line
        // and the bus state are worked out first, as the chip's clock rate can't change there. The clocks
        // after the span run one at a time below.
        if (state == Imx1Cs5Bus::State::waiting) {
            const uint32_t limit = quietWaitClocks(busPeriod);
            if (limit && acceptGraphics(cs5Configuration, false) == Imx1BootstrapAccess::ok) {
                const auto rate = cs5Configuration.clockState().systemRate();
                uint32_t clocks = 0;
                while (state == Imx1Cs5Bus::State::waiting && clocks < limit) {
                    if (!ready && rate.numerator) {
                        const uint64_t numerator = busPeriod.numerator * rate.numerator;
                        const uint64_t denominator = uint64_t(busPeriod.denominator) * 32768 * rate.denominator;
                        // The clock loop below refuses it at the same clock.
                        if (!service.supports(denominator)) break;
                        ready = service.add(numerator,denominator) != 0;
                    }
                    ++clocks;
                    state = transaction.sample(elapsed + clocks,true,mode == Imx1Cs5Mode::dtack ? !ready : ready);
                }
                if (clocks) {
                    peripherals.advanceHclkCycles(clocks);
                    timedStatus = advanceGraphics([&](Imx1Cs5Configuration& c) { c.advanceBusCycles(clocks,busPeriod,cpuPeriod); });
                    elapsed += clocks;
                    work += clocks;
                    if (timedStatus == Imx1BootstrapAccess::ok) timedStatus = peripherals.clockStatus();
                }
            }
        }
        while (timedStatus == Imx1BootstrapAccess::ok && state == Imx1Cs5Bus::State::waiting) {
            const auto rate = cs5Configuration.clockState().systemRate();
            if (!ready && rate.numerator) {
                const uint64_t numerator = busPeriod.numerator * rate.numerator;
                const uint64_t denominator = uint64_t(busPeriod.denominator) * 32768 * rate.denominator;
                if (!service.supports(denominator)) return Imx1BootstrapAccess::sideEffect;
                ready = service.add(numerator,denominator) != 0;
            }
            peripherals.advanceHclkCycles(1);
            timedStatus = advanceGraphics([&](Imx1Cs5Configuration& c) { c.advanceBusCycles(1,busPeriod,cpuPeriod); });
            ++elapsed;
            ++work;
            if (timedStatus == Imx1BootstrapAccess::ok) timedStatus = peripherals.clockStatus();
            if (timedStatus != Imx1BootstrapAccess::ok) break;
            state = transaction.sample(elapsed,true,mode == Imx1Cs5Mode::dtack ? !ready : ready);
        }
        result.cs5WaitClocks += elapsed;
        if (graphicsMemory) graphicsFrame.advance(cs5Configuration,*graphicsMemory,peripherals.eimState());
        if (timedStatus != Imx1BootstrapAccess::ok) return timedStatus;
        return state == Imx1Cs5Bus::State::complete ? Imx1BootstrapAccess::ok :
            state == Imx1Cs5Bus::State::timeout ? Imx1BootstrapAccess::busTimeout : Imx1BootstrapAccess::externalInput;
    }

    // What clockBoundary stops on, with no change.
    Imx1BootstrapAccess boundaryStatus() const {
        return graphicsWireStatus != Imx1BootstrapAccess::ok ? graphicsWireStatus : cs5Configuration.referencePeriodSupported(peripherals.cpuReferencePeriod(cpuArm920ClockMode(cpu))) ?
            peripherals.clockStatus() : Imx1BootstrapAccess::sideEffect;
    }
    bool clockBoundary() {
        const auto status = boundaryStatus();
        if (status == Imx1BootstrapAccess::ok) return false;
        result.stop = Imx1ProbeStop::unsupportedPeripheral;
        result.peripheralStatus = status;
        if (peripherals.i2cState().clockStatus() != Imx1I2cAccess::ok) {
            result.address = 0x00217010;
            result.size = 4;
            result.value = peripherals.i2cState().pendingTransfer().byte;
        } else {
            // Elapsed events carry no guest MMIO address and deliver no CPU exception.
            result.address = result.size = result.value = 0;
            result.possibleWatchdogTimeoutBoundary = status == Imx1BootstrapAccess::possibleWatchdogTimeout;
            result.elapsedInterruptBoundary = !result.possibleWatchdogTimeoutBoundary;
            if (result.elapsedInterruptBoundary) result.interruptPending = peripherals.interruptPending();
        }
        return true;
    }

    const uint8_t* resolve(uint32_t address, uint32_t size) const {
        for (uint32_t base : {0x10000000u, alias}) {
            // Table 3-1 and section 8.2 limit the bootstrap alias to 1 MiB.
            const uint32_t length = base == 0 && rom.size() > 0x100000 ? 0x100000 : rom.size();
            if (address >= base && uint64_t(address) + size <= uint64_t(base) + length)
                return rom.data() + address - base;
        }
        return nullptr;
    }
    // Pages the Fast engine reads and writes straight from host memory: ROM for reads, SRAM and SDRAM in
    // normal mode, as Probe::access answers them with no time and no side effect. Access counts a data
    // access to RAM in busAccesses, and one to ROM not.
    static uint8_t* leanPage(void* context, uint32_t pa, bool write, bool* counted) {
        auto& p = *static_cast<Probe*>(context);
        *counted = false;
        if (!write)
            if (const auto* rom = p.resolve(pa, 0x1000)) return const_cast<uint8_t*>(rom);
        uint8_t* page = p.options.bootstrapPeripherals ? p.peripherals.plainPage(pa, write) : nullptr;
        *counted = page != nullptr;
        return page;
    }
    // A timer count read on the lean path, answered as access answers it inside the quiet cycles: the
    // instructions the run finished are cycles still to charge, and no charge is needed. A CS5 register read
    // (single loads only) goes to leanRegisterRead.
    static bool leanRead(void* context, uint32_t pa, uint32_t size, uint32_t pc, uint32_t ran, uint32_t left, bool single,
                         uint32_t* value, uint32_t* cost) {
        const bool answered = leanReadAnswer(context, pa, size, pc, ran, left, single, value, cost);
        jitReads += answered && inJit;
        jitRegisterReads += answered && inJit && pa >= 0x16000000 && pa < 0x17000000 && !Imx1GraphicsMemory::owns(pa);
        return answered;
    }
    static bool leanReadAnswer(void* context, uint32_t pa, uint32_t size, uint32_t pc, uint32_t ran, uint32_t left,
                               bool single, uint32_t* value, uint32_t* cost) {
        auto& p = *static_cast<Probe*>(context);
        *cost = 0;
        if (p.graphicsMemory && Imx1GraphicsMemory::owns(pa))
            return single && p.leanGraphicsRead(pa, size, ran, left, *value, *cost);
        if (size != 4) return false;
        if (Imx1Bootstrap::quietReadAddress(pa)) return single && p.leanBoardRead(pa, ran, *value);
        if (!Imx1Bootstrap::timerCountAddress(pa))
            return single && p.leanRegisterRead(pa, pc, ran, left, *value, *cost);
        // Wait clocks still deferred (graphics accesses answered earlier) count after the CPU cycles, as
        // chargeDeferred charges them; the span has to hold their cost too.
        if (quietCycles <= uint64_t(ran) + leanCs5.waitCost || p.failed || p.graphicsWireStatus != Imx1BootstrapAccess::ok ||
            !p.peripherals.timerCountAfter(pa, size, deferredCycles + ran, quietMode, *value,
                                           deferredHclk + leanCs5.waitClocks)) return false;
        ++leanTimerReads;
        return true;
    }
    // Wait clocks of a CS5 access now, as cs5Wait works them out inside a quiet span, with nothing charged. False
    // where cs5Wait would refuse, run past one transaction or advance a clock at a time.
    bool quietWriteClocks(uint32_t& clocks) const {
        const auto mode = peripherals.eimState().cs5Mode();
        if (mode == Imx1Cs5Mode::unsupported || (mode != Imx1Cs5Mode::fixed && !peripherals.cs5PinsSelected(true)))
            return false;
        const auto busPeriod = peripherals.busReferencePeriod();
        const auto cpuPeriod = peripherals.cpuReferencePeriod(quietMode);
        if (!cs5Configuration.busPeriodSupported(busPeriod, cpuPeriod) ||
            (mode != Imx1Cs5Mode::fixed &&
             (!cs5Configuration.memoryInterfaceReady() || !cs5Configuration.clockState().programmed())) ||
            !peripherals.referenceChargeSupported(uint64_t(Imx1Cs5Bus::timeoutClocks) * busPeriod.numerator,
                                                  busPeriod.denominator))
            return false;
        const auto rate = cs5Configuration.clockState().systemRate();
        // The count below depends only on the mode, the bus period and the system rate; the last one is kept.
        static thread_local struct { Imx1Cs5Mode mode; Imx1PllRatio bus; uint64_t numerator, denominator; uint32_t clocks; bool ok, known; } last{};
        if (last.known && last.mode == mode && last.bus.numerator == busPeriod.numerator &&
            last.bus.denominator == busPeriod.denominator && last.numerator == rate.numerator &&
            last.denominator == rate.denominator) {
            clocks = last.clocks;
            return last.ok;
        }
        last = {mode, busPeriod, rate.numerator, rate.denominator, 0, false, false};
        Imx1Cs5Bus transaction(mode);
        Imx1GraphicsFraction service;
        bool ready = mode == Imx1Cs5Mode::fixed;
        auto state = transaction.sample(0, true, mode == Imx1Cs5Mode::dtack ? !ready : ready);
        clocks = 0;
        while (state == Imx1Cs5Bus::State::waiting) {
            if (!ready && rate.numerator) {
                const uint64_t numerator = busPeriod.numerator * rate.numerator;
                const uint64_t denominator = uint64_t(busPeriod.denominator) * 32768 * rate.denominator;
                if (!service.supports(denominator)) { last.known = true; return false; }
                ready = service.add(numerator, denominator) != 0;
            }
            ++clocks;
            state = transaction.sample(clocks, true, mode == Imx1Cs5Mode::dtack ? !ready : ready);
        }
        last.clocks = clocks;
        last.ok = state == Imx1Cs5Bus::State::complete && clocks;
        last.known = true;
        return last.ok;
    }
    // Whether this lean run can answer CS5 accesses, worked out at its first one, with the wait clocks and the cost
    // of each. The span has to hold the instructions and the wait clocks together, counted in CPU cycles rounded up
    // and one more, which also covers the work the clocks add.
    __attribute__((noinline)) bool leanCs5Ready() {
        if (leanCs5.checked) return leanCs5.usable;
        leanCs5.checked = true;
        uint32_t clocks = 0;
        // With a frame scan due, the step's charge scans the frame before the access, as in Exact. A System PLL bus
        // stall is finished by the step's next access.
        leanCs5.usable = boundaryStatus() == Imx1BootstrapAccess::ok && !peripherals.systemPllBusStallPending() &&
                         quietWriteClocks(clocks) && acceptGraphics(cs5Configuration, false) == Imx1BootstrapAccess::ok &&
                         !graphicsFrame.scanDue(cs5Configuration, peripherals.eimState());
        if (leanCs5.usable) {
            const auto bus = peripherals.busReferencePeriod();
            const auto cpuPeriod = peripherals.cpuReferencePeriod(quietMode);
            const __uint128_t time = __uint128_t(clocks) * bus.numerator * cpuPeriod.denominator;
            const __uint128_t unit = __uint128_t(bus.denominator) * cpuPeriod.numerator;
            const uint64_t cycles = uint64_t(time / unit + (time % unit != 0)) + 1;
            leanCs5.clocks = clocks;
            leanCs5.cost = uint32_t(std::max<uint64_t>(cycles, clocks));
        }
        return leanCs5.usable;
    }
    bool leanMemoryReady() {
        if (!leanCs5.memoryChecked) {
            leanCs5.memoryChecked = true;
            leanCs5.memoryUsable = cs5Configuration.memoryReady(peripherals.eimState());
        }
        return leanCs5.memoryUsable;
    }
    // The GPIO side of a CS5 register read: the acquisition source the port B pins select and the settled A13
    // drive. Neither the pins nor the graphics state change in a run except through a board write, which clears
    // leanCs5, so the first read of a run works them out for the rest.
    __attribute__((noinline)) bool leanRegisterReady() {
        if (leanCs5.registerChecked) return leanCs5.registerUsable;
        leanCs5.registerChecked = true;
        leanCs5.registerUsable = false;
        const bool stick = peripherals.gpioOutputPattern(1,0,0,0x00400000);
        const bool aux = peripherals.gpioOutputPattern(1,0,0x00400000,0);
        const bool first = peripherals.gpioOutputPattern(1,0x05000000,0x08000000,0x02000000);
        const bool second = peripherals.gpioOutputPattern(1,0x0a000000,0x04000000,0x01000000);
        if (cs5Configuration.acquisitionState().source() != (aux ? 2u : stick && first ? 0u : stick && second ? 1u : 3u))
            return false;
        if (options.graphicsIrqA13) {
            const auto line = cs5Configuration.interruptState().interruptOutput();
            const uint32_t level = line == Imx1GpioLine::clear ? 0x2000 : 0;
            if (line == Imx1GpioLine::unknown || !peripherals.gpioDriveSettled(0, level, 0x2000) ||
                !peripherals.gpioDriveAccepted(0, level, 0x2000)) return false;
        }
        leanCs5.registerUsable = true;
        return true;
    }
    // A CS5 register read on the lean path that changes nothing: what access commits is the same configuration
    // (the same acquisition source, the same settled A13 drive, no finished command to complete), so only the
    // value and the wait clocks are left, the clocks deferred as for graphics memory writes. The scan status is
    // worked out at the time the read would see after every cycle and clock still deferred, its own included.
    __attribute__((noinline)) bool leanRegisterRead(uint32_t pa, uint32_t pc, uint32_t ran, uint32_t left, uint32_t& value, uint32_t& cost) {
        if (pa < 0x16000000 || pa >= 0x17000000 || !graphicsMemory || !cs5Configuration.virtualProfile() ||
            Imx1GraphicsMemory::owns(pa) || options.cs5ConfigurationObserver || failed || quietCycles <= ran ||
            cs5Configuration.engineState().completed())
            return false;
        if (!leanCs5Ready() || left < leanCs5.cost) return false;
        const bool user = (cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_USR;
        if (peripherals.eimState().cs5Transfer(pa, 4, false, user) != Imx1EimAccess::ok || !leanRegisterReady())
            return false;
        uint64_t ticks = 0;
        if (!cs5Configuration.ticksAfter(deferredCycles + ran, peripherals.cpuReferencePeriod(quietMode),
                                         deferredHclk + leanCs5.waitClocks + leanCs5.clocks, peripherals.busReferencePeriod(),
                                         ticks) ||
            !cs5Configuration.quietRegisterRead(peripherals.eimState(), pc, pa, ticks, value))
            return false;
        leanCs5.waitClocks += leanCs5.clocks;
        leanCs5.waitCost += leanCs5.cost;
        ++leanCs5Reads;
        cost = leanCs5.cost;
        return true;
    }
    // A graphics memory write on the lean path, answered as access answers it: the write now, its wait clocks
    // deferred with the CPU cycles.
    static bool leanWrite(void* context, uint32_t pa, uint32_t size, uint32_t value, uint32_t ran, uint32_t left,
                          uint32_t* cost) {
        auto& p = *static_cast<Probe*>(context);
        if (inJit && jitBoardWriteAddress(pa)) return size == 4 && p.jitBoardWrite(pa, value);
        if (!p.graphicsMemory || !p.cs5Configuration.virtualProfile() || !Imx1GraphicsMemory::owns(pa) ||
            p.options.cs5ConfigurationObserver || p.failed || quietCycles <= ran)
            return false;
        if (!p.leanCs5Ready() || left < leanCs5.cost) return false;
        const bool user = (cpuGetRegExternal(p.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_USR;
        uint32_t datum = value;
        if (p.peripherals.eimState().cs5Transfer(pa, size, true, user) != Imx1EimAccess::ok || !p.leanMemoryReady() ||
            !p.graphicsMemory->accessReady(p.cs5Configuration, pa, size, true, datum))
            return false;
        leanCs5.waitClocks += leanCs5.clocks;
        leanCs5.waitCost += leanCs5.cost;
        ++leanCs5Writes;
        *cost = leanCs5.cost;
        return true;
    }
    // Writes Fast blocks take in generated code: GPIO IMR and ISR, AITC NIMASK, INTENNUM and INTDISNUM, timer status,
    // the watchdog service register and DMA DISR (which only clears status, so the PCM DMA pages stay as they are).
    static bool jitBoardWriteAddress(uint32_t pa) {
        const uint32_t offset = pa & 0xff;
        if ((pa & ~0x3ffu) == 0x0021c000) return offset == 0x30 || offset == 0x34;
        if ((pa & ~0xffu) == 0x00223000) return offset == 0x04 || offset == 0x08 || offset == 0x0c;
        return pa == 0x00202014 || pa == 0x00203014 || pa == 0x00201004 || pa == 0x00209004;
    }
    // A board write from generated code, taken as the step takes it after charging the cycles before the run. The
    // run's own cycles are charged at its end, so the write lands up to one run early. The interrupt lines follow
    // at once, and the run stops at its next block if the new quiet span ends before its budget would.
    __attribute__((noinline)) bool jitBoardWrite(uint32_t pa, uint32_t value) {
        if (failed || graphicsWireStatus != Imx1BootstrapAccess::ok || options.busObserver) return false;
        deferredHclk += leanCs5.waitClocks;
        result.cs5WaitClocks += leanCs5.waitClocks;
        work += leanCs5.waitClocks;
        leanCs5 = {};
        if (!chargeDeferred()) return false;
        finishGraphicsBusStall();
        if (graphicsWireStatus != Imx1BootstrapAccess::ok) return false;
        uint32_t datum = value;
        if (peripherals.access(pa, 4, true, datum, cpuArm920ClockMode(cpu)) != Imx1BootstrapAccess::ok) return false;
        ++result.busAccesses;
        ++jitBoardWrites;
        updateInterrupts();
        quietCycles = boundaryStatus() == Imx1BootstrapAccess::ok ? quietCpuCycles(quietMode) : 0;
        if (quietCycles + cpuArm920JitMaxUnchecked(cpu) < jitBudget) {
            jitHalt = true;
            cpuSetSlowPath(cpu, SLOW_PATH_REASON_RESCHEDULE);
        }
        return true;
    }
    // Fast blocks: the ROM's timer delay loop (ldr rT, [rB, #16]; sub rT, rT, rS; cmp rT, #n; bcc back, as at
    // 2009f678) waits for timer count - rS to reach n. When the CPU is in one on the timer count, the cycles up to the
    // first count that ends it, within the quiet span, pass at once without running the loop.
    void skipDelay() {
        const uint32_t* r = cpuGetRegisters(cpu);
        if (cpuIsThumb(cpu) || quietCycles < leanMinimumSpan) return;
        uint32_t w[4], pa;
        uint32_t start = r[15];
        if (!cpuArm920PeekWord(cpu, start, &w[0], &pa)) return;
        // pc is on one of the loop's four words; find the load.
        for (unsigned back = 0; back < 4 && (w[0] & 0xfff00fffu) != 0xe5900010u; ++back) {
            start -= 4;
            if (back == 3 || !cpuArm920PeekWord(cpu, start, &w[0], &pa)) return;
        }
        for (unsigned i = 1; i < 4; ++i)
            if (!cpuArm920PeekWord(cpu, start + 4 * i, &w[i], &pa)) return;
        const unsigned t = (w[0] >> 12) & 15, b = (w[0] >> 16) & 15, sreg = w[1] & 15;
        if (t == 15 || b == 15 || sreg == 15 || sreg == t || t == b ||
            (w[1] & 0xfffffff0u) != (0xe0400000u | (t << 16) | (t << 12)) ||
            (w[2] & 0xffff0f00u) != (0xe3500000u | (t << 16)) || w[3] != 0x3afffffbu) return;
        uint32_t timer;
        if (!cpuArm920PeekWord(cpu, r[b] + 16, nullptr, &timer) || !Imx1Bootstrap::timerCountAddress(timer) ||
            failed || graphicsWireStatus != Imx1BootstrapAccess::ok) return;
        const uint32_t base = r[sreg], wait = w[2] & 0xff;
        auto done = [&](uint64_t cycles, bool& known) {
            uint32_t count;
            // Wait clocks still deferred count after the cycles, as chargeDeferred charges them.
            known = peripherals.timerCountAfter(timer, 4, deferredCycles + cycles, quietMode, count, deferredHclk);
            return known && count - base >= wait;
        };
        bool known;
        if (done(0, known) || !known) return;
        // The last cycle of the span stays for the loop to see; a wait past the span passes all the rest of it.
        uint64_t low = 0, high = quietCycles - 1;
        const bool reached = done(high, known);
        if (!known) return;
        while (reached && high - low > 1) {
            const uint64_t mid = low + (high - low) / 2;
            if (done(mid, known)) high = mid; else if (known) low = mid; else return;
        }
        deferredCycles += high;
        quietCycles -= high;
        work += high;
        ++delaySkips;
        delaySkipCycles += high;
    }
    // Fast blocks: the ROM's vertical wait polls the scan status, ldr r1, [r0, #0x54c]; tst r1, #0x400 and beq back at
    // 200adb08 (for the status rows), bne back at 200adb14 (for their end). At a JIT run's start on one of the loads,
    // after a round that read the value it waits on, the rounds that would read it again pass at once, taken as the
    // JIT takes them: 3 instructions and cycles each, the read's wait clocks deferred and its cost to the span. The
    // value is that of the display region at the first round, and the region only grows, so the first round in
    // another region (or not answered) is found by bisection. That round and the last few of the span run in the JIT.
    // Only where the JIT would run: a pending interrupt or anything else for the step is taken at the next
    // instruction. The load must succeed in the current mode, as the JIT's would. hard is the most work the slice
    // has left. Returns the instructions passed.
    uint64_t skipScanPoll(uint64_t hard) {
        const uint32_t* r = cpuGetRegisters(cpu);
        const uint32_t pc = r[15];
        if ((pc != 0x200adb08 && pc != 0x200adb14) || !scanPollSkip || cpuIsThumb(cpu) || !cpuArm920JitCanRun(cpu))
            return 0;
        const bool rows = pc == 0x200adb08;
        const uint32_t want = rows ? 0 : 0x400;
        // The round before left r1 = want, N and C clear (the rotated immediate) and Z as the branch loops on.
        const uint32_t cpsr = cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR);
        if (r[1] != want || (cpsr & 0xe0000000u) != (rows ? 0x40000000u : 0)) return 0;
        uint32_t w, pa;
        if (!cpuArm920PeekWord(cpu, pc, &w, &pa) || w != 0xe590154cu || !cpuArm920PeekWord(cpu, pc + 4, &w, &pa) ||
            w != 0xe3110e40u || !cpuArm920PeekWord(cpu, pc + 8, &w, &pa) || w != (rows ? 0x0afffffcu : 0x1afffffcu) ||
            ((r[0] + 0x54c) & 3) || !cpuArm920PeekWord(cpu, r[0] + 0x54c, nullptr, &pa) || pa != 0x1601054c)
            return 0;
        // What leanRegisterRead needs, none of which changes inside the span.
        if (!graphicsMemory || !cs5Configuration.virtualProfile() || options.cs5ConfigurationObserver || failed ||
            cs5Configuration.engineState().completed() || !leanCs5Ready())
            return 0;
        const bool user = (cpsr & 31) == ARM_SR_MODE_USR;
        if (peripherals.eimState().cs5Transfer(pa, 4, false, user) != Imx1EimAccess::ok || !leanRegisterReady()) return 0;
        const uint64_t clocks = leanCs5.clocks, round = 3 + uint64_t(leanCs5.cost);
        // The span keeps room for the JIT's last rounds; the slice for the work the rounds add.
        const uint64_t keep = leanMinimumSpan + 4 * round;
        if (quietCycles <= keep) return 0;
        const uint64_t most = std::min((quietCycles - keep) / round, hard / (2 * (3 + clocks) + 2));
        if (!most) return 0;
        const auto cpuPeriod = peripherals.cpuReferencePeriod(quietMode);
        const auto busPeriod = peripherals.busReferencePeriod();
        unsigned first = 0;
        // Round i reads after 3 i cycles and i + 1 reads' wait clocks.
        auto same = [&](uint64_t i, unsigned* region) {
            uint64_t ticks;
            uint32_t value;
            unsigned at;
            if (!cs5Configuration.ticksAfter(deferredCycles + 3 * i, cpuPeriod, deferredHclk + (i + 1) * clocks, busPeriod,
                                             ticks) ||
                !cs5Configuration.quietScanStatus(peripherals.eimState(), pc, ticks, value, at))
                return false;
            if (region) { *region = at; return value == want; }
            return at == first;
        };
        if (!same(0, &first)) return 0;
        uint64_t low = 0, high = most;  // round low reads the same, round high is the first that may not
        if (same(most, nullptr)) low = most;
        while (high - low > 1) {
            const uint64_t mid = low + (high - low) / 2;
            if (same(mid, nullptr)) low = mid; else high = mid;
        }
        // Rounds 0 to low read want; they pass, round low + 1 on runs.
        const uint64_t n = low + 1 >= most ? most : low + 1;
        deferredCycles += 3 * n;
        quietCycles -= n * round;
        deferredHclk += n * clocks;
        result.cs5WaitClocks += n * clocks;
        work += n * (3 + clocks);
        retired += 3 * n;
        ++scanPollSkips;
        scanPollRounds += n;
        return 3 * n;
    }
    // A board register read on the lean path (quietReadAddress): inside the quiet span it reads what the step would
    // after charging, so it needs no charge.
    __attribute__((noinline)) bool leanBoardRead(uint32_t pa, uint32_t ran, uint32_t& value) {
        if (quietCycles <= ran || failed || graphicsWireStatus != Imx1BootstrapAccess::ok) return false;
        if (peripherals.access(pa, 4, false, value, cpuArm920ClockMode(cpu)) != Imx1BootstrapAccess::ok) return false;
        ++leanBoardReads;
        return true;
    }
    // A graphics memory read on the lean path, as leanWrite takes a write: inside the quiet span no batch completes,
    // so the memory holds what the step would read, and only the wait clocks are left, deferred as for writes.
    __attribute__((noinline)) bool leanGraphicsRead(uint32_t pa, uint32_t size, uint32_t ran, uint32_t left,
                                                    uint32_t& value, uint32_t& cost) {
        if (!cs5Configuration.virtualProfile() || options.cs5ConfigurationObserver || failed || quietCycles <= ran)
            return false;
        if (!leanCs5Ready() || left < leanCs5.cost) return false;
        const bool user = (cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_USR;
        if (peripherals.eimState().cs5Transfer(pa, size, false, user) != Imx1EimAccess::ok || !leanMemoryReady() ||
            !graphicsMemory->accessReady(cs5Configuration, pa, size, false, value))
            return false;
        leanCs5.waitClocks += leanCs5.clocks;
        leanCs5.waitCost += leanCs5.cost;
        ++leanCs5MemoryReads;
        cost = leanCs5.cost;
        return true;
    }
    static bool cacheMemoryCheck(void* context, uint32_t address, uint32_t size, bool write) {
        auto& p = *static_cast<Probe*>(context);
        if (!write && p.resolve(address, size)) return true;
        return p.options.bootstrapPeripherals && p.peripherals.cacheMemoryRange(address, size, write);
    }
    static bool access(void* context, uint32_t address, uint_fast8_t size, bool write, void* data) {
        auto& p = *static_cast<Probe*>(context);
        // A register write can change which pages are plain (SDRAM modes, the PCM DMA buffer).
        if (p.fastEngine && write && Imx1Bootstrap::plainPagesWrite(address)) cpuArm920LeanFlush(p.cpu);
        // ROM and RAM hold no time. Everything else sees the board at this instruction's own time, and so
        // does the PCM DMA when its buffer is written.
        // A timer count read inside the quiet cycles is worked out without charging them.
        uint32_t timerCount = 0;
        bool timerCountKnown = false;
        if ((deferredCycles || deferredHclk || quietCycles) &&
            !(Imx1Bootstrap::isRamAddress(address) && !(write && p.peripherals.pcmDmaReads(address, size))) &&
            !(!write && p.resolve(address, size))) {
            timerCountKnown = !write && quietCycles && !deferredHclk && !p.failed && !memIsInstructionFetch(p.mem) &&
                p.peripherals.timerCountAfter(address, size, deferredCycles, quietMode, timerCount);
            if (!timerCountKnown) p.chargeDeferred();
        }
        // Further data accesses within one instruction also wait for the nominal bus clock.
        if (!p.failed && p.options.bootstrapPeripherals && !memIsInstructionFetch(p.mem))
            p.finishGraphicsBusStall();
        if (p.graphicsWireStatus != Imx1BootstrapAccess::ok) {
            p.clockBoundary();
            p.failed = true;
            return false;
        }
        if (!write) {
            if (const auto* source = p.resolve(address, size)) {
                std::memcpy(data, source, size);
                return true;
            }
        }
        if (!write && memIsInstructionFetch(p.mem)) {
            uint32_t word;
            if (!p.options.bootstrapPeripherals ||
                !p.peripherals.instructionRead(address, size, word)) return false;
            std::memcpy(data, &word, size);
            return true;
        }
        // A JIT fault can retry through the interpreter. Keep the first bus failure.
        if (p.failed) return false;
        uint32_t value = 0;
        if (write && size <= 4) std::memcpy(&value, data, size);
        const auto previousIssueStep = p.i2cIssueStep;
        const auto previousIssuePc = p.i2cIssuePc;
        if (address == 0x00217010 || (write && address == 0x00217008)) {
            p.i2cIssueStep = p.result.trace.step;
            p.i2cIssuePc = p.result.trace.pc;
        }
        const bool diagnosticCs4 = p.options.bootstrapPeripherals && p.options.cs4Port0InputKnown &&
                                   address >= 0x15000000 && address < 0x16000000;
        const bool diagnosticCs5 = p.options.bootstrapPeripherals && (p.options.cs5ResetStatusInputKnown || p.cs5Configuration.virtualProfile()) &&
                                        address >= 0x16000000 && address < 0x17000000;
        const bool diagnosticCs5Reset = diagnosticCs5 && address == 0x16000018;
        const bool diagnosticCs5Configuration = diagnosticCs5 && !diagnosticCs5Reset;
        const bool graphicsMemoryAccess = diagnosticCs5Configuration && p.graphicsMemory &&
                                          Imx1GraphicsMemory::owns(address);
        const uint32_t sampledPin = p.result.trace.pc == 0x200b9cd4 &&
            uint64_t(p.graphicsSampleOffset) + 0x16010000 == address ? p.graphicsSampledPin : 32;
        uint32_t waitClocks = 0;
        auto deviceAccess = [&](bool preview, uint32_t& datum) {
            if (graphicsMemoryAccess) return preview ?
                p.graphicsMemory->validate(p.cs5Configuration,p.peripherals.eimState(),address,size) :
                p.graphicsMemory->access(p.cs5Configuration,p.peripherals.eimState(),address,size,write,datum);
            if (diagnosticCs5Reset) {
                auto candidate = p.cs5Reset;
                const auto status = candidate.access(p.peripherals.eimState(),p.result.trace.pc,address,size,write,datum,
                                                       p.cs5Configuration.virtualProfile());
                if (status != Imx1BootstrapAccess::ok) return status;
                if (write && (datum & 8) && p.cs5Configuration.virtualProfile()) {
                    auto graphicsNext = p.cs5Configuration;
                    graphicsNext.reset();
                    const auto gate = p.acceptGraphics(graphicsNext, !preview);
                    if (gate != Imx1BootstrapAccess::ok) return gate;
                }
                if (!preview) p.cs5Reset = candidate;
                return status;
            }
            auto candidate = p.cs5Configuration;
            // Alternate-board mode 2 requires the native B22 output selector as well as the axis mux.
            const bool stick = p.peripherals.gpioOutputPattern(1,0,0,0x00400000);
            const bool aux = p.peripherals.gpioOutputPattern(1,0,0x00400000,0);
            const bool first = p.peripherals.gpioOutputPattern(1,0x05000000,0x08000000,0x02000000);
            const bool second = p.peripherals.gpioOutputPattern(1,0x0a000000,0x04000000,0x01000000);
            candidate.selectAcquisitionSource(aux ? 2 : stick && first ? 0 : stick && second ? 1 : 3);
            const bool player = candidate.virtualProfile() && Imx1Cs5Configuration::playerPc(p.result.trace.pc);
            // A player's reads of the engine words it saves (ati4200.c's RingBegin).
            if (player && !write && ((address >= 0x16011000 && address < 0x16012000) ||
                                     address == 0x16010c38 || address == 0x16010c3c)) {
                if (size != 4) return Imx1BootstrapAccess::width;
                if (!candidate.memoryReady(p.peripherals.eimState()) || !candidate.engineState().emptyRing())
                    return Imx1BootstrapAccess::sequence;
                return p.graphicsCommands.playerRead(address, datum);
            }
            const auto status = candidate.access(p.peripherals.eimState(),p.result.trace.pc,address,size,write,datum,sampledPin);
            if (status == Imx1BootstrapAccess::ok && write && address == 0x16010220 &&
                (p.result.trace.pc == 0x200a9ed4 || p.result.trace.pc == 0x200b0200 ||
                 p.result.trace.pc == 0x200ac300 || p.result.trace.pc == 0x200aa504 ||
                 p.result.trace.pc == 0x200a9a9c || p.result.trace.pc == 0x200b1568 ||
                 p.result.trace.pc == 0x200b1790 || Imx1GraphicsEngine::otherProducer(p.result.trace.pc) || player) &&
                p.graphicsMemory) {
                auto commands = p.graphicsCommands;
                const auto prepared = commands.prepareCopy(candidate,p.peripherals.eimState(),*p.graphicsMemory);
                if (prepared != Imx1BootstrapAccess::ok) {
                    p.graphicsRefusal = commands.refusal();
                    return prepared;
                }
                candidate.startEngine(commands.duration());
                const auto accepted = p.acceptGraphics(candidate,!preview);
                if (accepted == Imx1BootstrapAccess::ok && !preview) p.graphicsCommands = std::move(commands);
                return accepted;
            }
            return status == Imx1BootstrapAccess::ok ? p.acceptGraphics(candidate, !preview) : status;
        };
        Imx1BootstrapAccess status;
        if (diagnosticCs5 && p.cs5Configuration.virtualProfile()) {
            uint32_t previewValue = value;
            const auto transfer = p.peripherals.eimState().cs5Transfer(address,size,write,
                (p.result.trace.cpsr & 31) == ARM_SR_MODE_USR);
            status = transfer == Imx1EimAccess::width ? Imx1BootstrapAccess::width :
                transfer != Imx1EimAccess::ok ? Imx1BootstrapAccess::sideEffect : deviceAccess(true,previewValue);
            if (status == Imx1BootstrapAccess::ok) status = p.cs5Wait(waitClocks);
            // Recreate the candidate after elapsed events, retaining current clocks,
            // display, memory-controller progress and read-side-effect state.
            if (status == Imx1BootstrapAccess::ok) status = deviceAccess(false,value);
        } else {
            status = graphicsMemoryAccess ? p.graphicsMemory->access(p.cs5Configuration, p.peripherals.eimState(),
                address, size, write, value) : diagnosticCs5Configuration ? p.cs5Configuration.access(p.peripherals.eimState(),
                p.result.trace.pc, address, size, write, value, sampledPin) : diagnosticCs5Reset ? p.cs5Reset.access(p.peripherals.eimState(),
                p.result.trace.pc, address, size, write, value) : diagnosticCs4 ? (p.peripherals.ymf768Enabled() ? p.peripherals.ymf768Access(address, size, write, value) :
                p.cs4Capture.access(p.peripherals.eimState(), address, size, write, value)) :
                timerCountKnown ? (value = timerCount, Imx1BootstrapAccess::ok) :
                p.options.bootstrapPeripherals ? p.peripherals.access(address, size, write, value, cpuArm920ClockMode(p.cpu)) : Imx1BootstrapAccess::unmapped;
        }
        if (diagnosticCs5Reset && p.options.cs5ResetObserver)
            p.options.cs5ResetObserver(p.options.cs5ResetContext, {p.result.trace.step, p.result.trace.pc,
                address, uint32_t(size), value, write, p.cs5Reset.phaseValue(), status});
        const uint32_t knownMask = diagnosticCs5Configuration && !write && size == 4 && sampledPin < 16 &&
            (address == 0x160104c0 || address == 0x16010530) && status == Imx1BootstrapAccess::ok ?
            1u << sampledPin : 0xffffffffu;
        if (diagnosticCs5Configuration && p.options.cs5ConfigurationObserver)
            p.options.cs5ConfigurationObserver(p.options.cs5ConfigurationContext, {p.result.trace.step, p.result.trace.pc,
                address, uint32_t(size), value, write, p.cs5Configuration.defaultsKnown(), status, knownMask});
        if (diagnosticCs4 && p.options.cs4Observer)
            p.options.cs4Observer(p.options.cs4Context, {p.result.trace.step, p.result.trace.pc,
                address, uint32_t(size), value, write, p.cs4Capture.selectedIndex(), p.cs4Capture.observedBank(), status});
        if (status != Imx1BootstrapAccess::ok) {
            p.i2cIssueStep = previousIssueStep;
            p.i2cIssuePc = previousIssuePc;
        }
        ++p.result.busAccesses;
        if (write && address == 0x00205034 && (p.result.trace.pc & 0x00ffffff) == 0x3b8 && status == Imx1BootstrapAccess::ok)
            imx1NoteStartup(&p);
        if (p.options.busObserver)
            p.options.busObserver(p.options.busContext, {p.result.trace.step, p.result.trace.pc,
                                   address, uint32_t(size), value, write, status, diagnosticCs4, diagnosticCs5Reset, diagnosticCs5Configuration, knownMask, waitClocks});
        if (status == Imx1BootstrapAccess::ok || status == Imx1BootstrapAccess::command) {
            // Probe RAM has no ArmRam write watches. Retain guest I-cache words.
            if (write && status == Imx1BootstrapAccess::ok &&
                Imx1Bootstrap::isRamAddress(address)) {
                // Fast blocks drop only the code translated from this page (cpuArm920LeanStored).
                if (!p.fastJit) cpuDropTranslatedCode(p.cpu);
                if (p.fastEngine) cpuArm920LeanStored(p.cpu, address);
            }
            if (!write) std::memcpy(data, &value, size);
            return true;
        }
        if (!p.failed) {
            p.failed = true;
            p.result.stop = write ? Imx1ProbeStop::unsupportedWrite : Imx1ProbeStop::unsupportedRead;
            if (status != Imx1BootstrapAccess::unmapped) p.result.stop = Imx1ProbeStop::unsupportedPeripheral;
            p.result.peripheralStatus = status;
            p.result.address = address;
            p.result.size = size;
            if (write && size <= 4) std::memcpy(&p.result.value, data, size);
        }
        return false;
    }
};

bool condition(uint32_t instruction, uint32_t cpsr) {
    const bool n = cpsr & ARM_SR_N, z = cpsr & ARM_SR_Z;
    const bool c = cpsr & ARM_SR_C, v = cpsr & ARM_SR_V;
    switch (instruction >> 28) {
        case 0: return z; case 1: return !z;
        case 2: return c; case 3: return !c;
        case 4: return n; case 5: return !n;
        case 6: return v; case 7: return !v;
        case 8: return c && !z; case 9: return !c || z;
        case 10: return n == v; case 11: return n != v;
        case 12: return !z && n == v; case 13: return z || n != v;
        case 14: return true; default: return false;
    }
}

bool softwareInterrupt(uint32_t i, bool thumb) {
    return thumb ? (i & 0xff00) == 0xdf00 : (i & 0x0f000000) == 0x0f000000;
}
bool armMode(uint32_t mode) {
    return mode == 0x10 || mode == 0x11 || mode == 0x12 || mode == 0x13 ||
           mode == 0x17 || mode == 0x1b || mode == 0x1f;
}
bool bootstrapInstruction(uint32_t i, const uint32_t* registers, uint32_t cpsr, uint32_t spsr) {
    if (i >> 28 == 15) return false;
    if (softwareInterrupt(i, false)) return true;
    if ((i & 0x0ffffff0) == 0x012fff10) return true;  // BX
    if ((i & 0x0e000000) == 0x0a000000) return true;  // B, BL
    if ((i & 0x0fbf0fff) == 0x010f0000) return true;  // MRS
    if ((i & 0x0fb0fff0) == 0x0120f000 || (i & 0x0fb0f000) == 0x0320f000) return true;  // MSR
    if ((i & 0x0f000f10) == 0x0e000f10) return true;  // CP15, strict bootstrap handler
    if ((i & 0x0e000000) == 0x08000000) {
        const unsigned base = (i >> 16) & 15;
        const bool load = i & 0x00100000;
        if (i & 0x00400000) {
            const unsigned mode = cpsr & 31;
            if (!armMode(mode) || mode == 0x10 || mode == 0x1f || base == 15) return false;
            if (load && (i & 0x8000)) {
                // LDM exception return uses current registers, then restores CPSR.
                return armMode(spsr & 31) && !(spsr & ~0xf00000ffu) &&
                       (!(i & 0x00200000) || !(i & (1u << base)));
            }
            // User-bank LDM/STM require no PC and no writeback.
            return (i & 0x7fff) && !(i & 0x00208000);
        }
        return (i & 0xffff) &&
               base != 15 && (!(i & 0x00200000) || !(i & (1u << base)));
    }
    if ((i & 0x0c000000) == 0x04000000) {
        const bool pc = ((i >> 12) & 15) == 15;
        if (pc && !(i & 0x00100000)) {
            const bool pre = i & 0x01000000, immediate = !(i & 0x02000000);
            const bool writeback = !pre || (i & 0x00200000);
            // Word STR PC uses the ARM920T pipeline value; STRT remains unqualified.
            if ((i & 0x00400000) || (!pre && (i & 0x00200000)) ||
                (writeback && ((i >> 16) & 15) == 15) || (!immediate && (i & 15) == 15)) return false;
        }
        return (!pc || !(i & 0x00400000)) && !((i & 0x02000010) == 0x02000010);
    }
    if ((i & 0x0c000000) != 0) return false;
    if ((i & 0x0fb00ff0) == 0x01000090) {
        // SWP and SWPB: the read and the write in one step, so nothing comes between them. Rd may be Rm; the
        // forms ARM DDI0100I A4.1.108 makes unpredictable (PC anywhere, Rn equal to Rd or Rm) stay out.
        const unsigned base = (i >> 16) & 15, dest = (i >> 12) & 15, rm = i & 15;
        return base != 15 && dest != 15 && rm != 15 && base != dest && base != rm;
    }
    if ((i & 0x0fc000f0) == 0x00000090) {
        const unsigned dest = (i >> 16) & 15, add = (i >> 12) & 15;
        const unsigned rs = (i >> 8) & 15, rm = i & 15;
        // ARMv4 MUL/MLA: the CPU model preserves C/V and sets N/Z when S is set. Rd may be Rm: ARMv4 calls it
        // unpredictable, but the ARM920T gives the ordinary product (ASSUMPTION "ARM920T multiply with Rd == Rm":
        // Xploids runs MULS r0,r0,r1 at 30955e68; ARMv6 drops the restriction, and both engines compute it so).
        return dest != 15 && rs != 15 && rm != 15 &&
               ((i & 0x00200000) ? add != 15 : add == 0);
    }
    if ((i & 0x0f8000f0) == 0x00800090) {
        const unsigned hi = (i >> 16) & 15, lo = (i >> 12) & 15;
        const unsigned rs = (i >> 8) & 15, rm = i & 15;
        // ARMv4 long multiply: retain the older Rm/destination overlap restriction.
        return hi != 15 && lo != 15 && rs != 15 && rm != 15 && hi != lo &&
               hi != rm && lo != rm;
    }
    if ((i & 0x0e000090) == 0x00000090 && (i & 0x60)) {
        const bool load = i & 0x00100000, pre = i & 0x01000000;
        const bool immediate = i & 0x00400000, writeback = !pre || (i & 0x00200000);
        const unsigned base = (i >> 16) & 15, dest = (i >> 12) & 15, rm = i & 15;
        // ARMv4 halfword/signed-byte transfers, excluding doubleword and unpredictable forms.
        if ((!load && (i & 0x60) != 0x20) || dest == 15 ||
            (!pre && (i & 0x00200000)) ||
            (writeback && (base == 15 || base == dest || (!immediate && base == rm))) ||
            (!immediate && ((i & 0xf00) || rm == 15))) return false;
        const uint32_t offset = immediate ? ((i >> 4) & 0xf0) | (i & 15) : registers[rm];
        uint32_t address = registers[base] + (base == 15 ? 8 : 0);
        if (pre) address += (i & 0x00800000) ? offset : -offset;
        return !(i & 0x20) || !(address & 1);
    }
    if (((i >> 12) & 15) == 15) {
        // DDI0100I A2.6/A4.1.35: unshifted MOVS pc,Rm restores the saved context.
        if ((i & 0x0ffffff0) == 0x01b0f000 && (i & 15) != 15)
            return armMode(cpsr & 31) && (cpsr & 31) != 0x10 && (cpsr & 31) != 0x1f &&
                   armMode(spsr & 31) && !(spsr & ~0xf00000ffu) &&
                   !(registers[i & 15] & ((spsr & ARM_SR_T) ? 1u : 3u));
        // DDI0100I A2.6.8/9: IRQ/FIQ return subtracts four from the banked link.
        if ((i & 0x0fffffff) == 0x025ef004)
            return armMode(cpsr & 31) && (cpsr & 31) != 0x10 && (cpsr & 31) != 0x1f &&
                   armMode(spsr & 31) && !(spsr & ~0xf00000ffu) &&
                   !((registers[14] - 4) & ((spsr & ARM_SR_T) ? 1u : 3u));
        if ((i & 0x0fff0ff0) == 0x01a00000) return true; // MOV pc, Rm
        // DDI0100I A4.1.3/A4.1.106/A5.1: S=0 ADD/SUB with immediate shifts.
        if ((i & 0x0ff00010) == 0x00800000 || (i & 0x0ff00010) == 0x00400000) {
            const unsigned rn = (i >> 16) & 15, rm = i & 15;
            const unsigned amount = (i >> 7) & 31, type = (i >> 5) & 3;
            uint32_t operand = registers[rm] + (rm == 15 ? 8u : 0u);
            switch (type) {
                case 0: operand <<= amount; break;
                case 1: operand = amount ? operand >> amount : 0; break;
                case 2:
                    operand = amount ? (operand >> amount) |
                        ((operand & 0x80000000u) ? (~0u << (32 - amount)) : 0u) :
                        ((operand & 0x80000000u) ? ~0u : 0u);
                    break;
                case 3:
                    operand = amount ? (operand >> amount) | (operand << (32 - amount)) :
                        (operand >> 1) | ((cpsr & ARM_SR_C) ? 0x80000000u : 0u);
                    break;
            }
            const uint32_t base = registers[rn] + (rn == 15 ? 8u : 0u);
            const uint32_t target = (i & 0x00400000) ? base - operand : base + operand;
            return !(target & 3);
        }
        const unsigned shift = ((i >> 8) & 15) * 2;
        const uint32_t imm = i & 255;
        const uint32_t operand = shift ? (imm >> shift) | (imm << (32 - shift)) : imm;
        // Immediate ADD/SUB linkage preserves flags and the current ARM state.
        if ((i & 0x0ff00000) == 0x02800000 || (i & 0x0ff00000) == 0x02400000) {
            const unsigned source = (i >> 16) & 15;
            const uint32_t base = registers[source] + (source == 15 ? 8u : 0u);
            const uint32_t target = (i & 0x00400000) ? base - operand : base + operand;
            return !(target & 3);
        }
        return (i & 0x0ff00000) == 0x03a00000 && !(operand & 3);
    }
    const unsigned opcode = (i >> 21) & 15;
    if (opcode >= 8 && opcode <= 11 && !(i & 0x00100000)) return false;
    // ARMv4 data processing with immediate or ordinary register shifts.
    return (i & 0x02000000) || !(i & 0x10) || !(i & 0x80);
}
// ARM DDI0100I A6/A7: ordinary ARMv4T forms, with aligned memory operands.
bool bootstrapThumb(uint16_t i, const uint32_t* r, bool blPrefix) {
    if (i < 0x4000) return true;  // Shifts, add/subtract and immediate arithmetic.
    if ((i & 0xfc00) == 0x4000) {
        // DDI0100I A7.1.45: equal encoded MUL operands are unpredictable before ARMv6.
        return ((i >> 6) & 15) != 13 || (i & 7) != ((i >> 3) & 7);
    }
    if ((i & 0xfc00) == 0x4400) {
        const unsigned op = (i >> 8) & 3, source = (i >> 3) & 15;
        const unsigned dest = (i & 7) | ((i >> 4) & 8);
        if (op == 3) {
            // BX, excluding BLX and SBZ bits. BX PC at a halfword address goes to the ARM word below PC+4: the
            // ARM linker's Thumb to ARM veneers put BX PC there with the ARM branch in the next word (Tony Hawk 4,
            // 3083531e). ASSUMPTION "Thumb BX PC aligns down": ARMv4T leaves it unpredictable; ARM7TDMI-class
            // cores ignore address bit 1 in ARM state, and the veneers only work that way. Another register
            // with bit 1 set and bit 0 clear stays refused.
            const uint32_t target = r[source] + (source == 15 ? 4 : 0);
            return !(i & 0x87) && ((target & 3) != 2 || source == 15);
        }
        // ADD, CMP and MOV with both H bits clear run on two low registers, ADD and MOV without touching the
        // flags (ASSUMPTION "ARM920T Thumb low ADD, CMP and MOV": ARMv4T calls them unpredictable; H1/H2 are taken
        // as the registers' top bits, as ARMv6 later defines MOV and ADD. Tony Hawk 4 runs ADD r0,r1 at 3083cf96).
        if (op == 1 && (source == 15 || dest == 15)) return false;
        // ADD (4) preserves Thumb state and ignores target bit 0.
        // Dynarmic rejects PC/PC; that alias remains unqualified here.
        return op != 0 || dest != 15 || source != 15;
    }
    if ((i & 0xf800) == 0x4800) return true; // PC-relative word load.
    if ((i & 0xf000) == 0x5000) {
        const unsigned op = (i >> 9) & 7;
        const uint32_t address = r[(i >> 3) & 7] + r[(i >> 6) & 7];
        if (op == 0 || op == 4) return !(address & 3);
        if (op == 1 || op == 5 || op == 7) return !(address & 1);
        return true;
    }
    if ((i & 0xe000) == 0x6000) return (i & 0x1000) || !(r[(i >> 3) & 7] & 3);
    if ((i & 0xf000) == 0x8000) return !(r[(i >> 3) & 7] & 1);
    if ((i & 0xf000) == 0x9000) return !(r[13] & 3);
    if ((i & 0xf000) == 0xa000 || (i & 0xff00) == 0xb000) return true;
    if ((i & 0xf600) == 0xb400) return (i & 0x1ff) && !(r[13] & 3); // PUSH/POP.
    if ((i & 0xf000) == 0xc000) {
        const unsigned base = (i >> 8) & 7;
        // Thumb LDM suppresses writeback when the base itself is loaded.
        return (i & 0xff) && ((i & 0x0800) || !(i & (1u << base))) && !(r[base] & 3);
    }
    if (softwareInterrupt(i, true)) return true;
    if ((i & 0xf000) == 0xd000) return (i & 0xf00) < 0xe00; // Undefined and host SWI stay gated.
    if ((i & 0xf800) == 0xe000 || (i & 0xf800) == 0xf000) return true; // B, BL prefix.
    return (i & 0xf800) == 0xf800 && blPrefix; // BL suffix needs its preceding halfword.
}
// Board settings that come from the options and never change afterwards. A state load builds
// its board the same way and holds the saved copies of these to it.
void configureBoard(Imx1Bootstrap& board, const Imx1ProbeOptions& options) {
    board.setLcdcContrastOnly(options.lcdcContrastOnly);
    board.setMmcsdEmptySlot(options.mmcsdEmptySlot);
    if (options.bootstrapPeripherals) board.connectCpuInterrupts();
    board.setI2cScenario(options.assumeI2cSingleMaster);
    board.setMcuRetuneScenario(options.virtualMpllRetune);
    board.setSystemPllScenario(options.assumeSpllSettled);
    board.setRamMiB(options.ramMiB);
}
}  // namespace


// startups: times the guest has run the ROM's first register write (LCDC.RMCR from 3b8, whichever alias it
// runs at), which any restart goes through and sleep and wake don't. Outside the saved state.
struct Imx1Machine { Probe* probe; uint32_t startups; };
namespace { Imx1Machine* activeMachine = nullptr; }
void imx1NoteStartup(const void* probe) {
    if (activeMachine && activeMachine->probe == probe) ++activeMachine->startups;
}

Imx1Machine* imx1MachineCreate(const uint8_t* rom, uint32_t size, Imx1ProbeEntry entry,
                             bool fast, const Imx1ProbeOptions& config,
                             Imx1ProbeObserver observer, void* context, Imx1ProbeResult* error) {
    const auto* options = &config;
    if (activeMachine) {
        if (error) { *error = {}; error->stop = Imx1ProbeStop::unsupportedPeripheral;
                     error->peripheralStatus = Imx1BootstrapAccess::sequence; }
        return nullptr;
    }
    if (options && (options->idleHclkBudget > imx1ProbeMaximumIdleHclk ||
        !imx1ProbeGpioEventValid(*options, UINT32_MAX) || ((options->lcdcContrastOnly || options->mmcsdEmptySlot) && !options->bootstrapPeripherals) ||
        (options->virtualMpllRetune && (!options->bootstrapPeripherals || !options->assumeMpllSettled)) ||
        !imx1GraphicsProfileValid(options->graphicsProfile) ||
        (!options->cs4IndexedInputs.empty() && (!options->bootstrapPeripherals || !options->cs4Port0InputKnown)) ||
        ((options->graphicsVirtualA4SourceOver || options->graphicsVirtualPackedMono) && (!options->bootstrapPeripherals ||
         options->graphicsProfile != Imx1GraphicsProfile::virtual5744Compatible8MiB)) ||
        (!imx1GraphicsMode2RouteValid(options->graphicsMode2Route)) ||
        ((options->graphicsMode2Route != Imx1GraphicsMode2Route::unknown || options->graphicsRawAuxKnown) &&
         (!options->bootstrapPeripherals || options->graphicsProfile != Imx1GraphicsProfile::virtual5744Compatible8MiB)) ||
        (options->graphicsRawAuxKnown && options->graphicsRawAux > 1023) ||
        (options->graphicsRawMode1Known && (!options->bootstrapPeripherals ||
         options->graphicsProfile != Imx1GraphicsProfile::virtual5744Compatible8MiB || options->graphicsRawMode1 > 1023)) ||
        (options->graphicsRawAxesKnown && (!options->bootstrapPeripherals ||
         options->graphicsProfile != Imx1GraphicsProfile::virtual5744Compatible8MiB ||
         options->graphicsRawFirst > 1023 || options->graphicsRawSecond > 1023)) ||
        (options->graphicsProfile != Imx1GraphicsProfile::strict &&
         (!options->bootstrapPeripherals || options->cs5ResetStatusInputKnown)) ||
        (options->gpioAInputMaskedKnown && (!options->bootstrapPeripherals || options->gpioAInputKnown ||
          !options->gpioAInputMask || (options->gpioAInput & ~options->gpioAInputMask))) ||
        (options->graphicsIrqA13 && (!options->bootstrapPeripherals ||
          options->graphicsProfile != Imx1GraphicsProfile::virtual5744Compatible8MiB || options->gpioAInputKnown ||
          (options->gpioAInputMaskedKnown && (options->gpioAInputMask & 0x2000)))))) {
        Imx1ProbeResult result{};
        result.stop = Imx1ProbeStop::unsupportedPeripheral;
        result.peripheralStatus = Imx1BootstrapAccess::sequence;
        if (error) *error = result;
        return nullptr;
    }
    if (!cpuSetFastEngine(fast)) {
        if (error) { *error = {}; error->stop = Imx1ProbeStop::engineUnavailable; }
        return nullptr;
    }
    leanRetired = 0;
    leanTimerReads = 0;
    jitRetired = 0;
    jitRuns = 0;
    leanCs5Writes = 0;
    leanCs5Reads = 0;
    leanCs5MemoryReads = 0;
    leanBoardReads = 0;
    deferredHclk = 0;
    // The core has process lifetime, as in the existing standalone CPU diagnostics.
    auto* p = new Probe(options ? *options : Imx1ProbeOptions{});
    if (p->options.diagnosticSnapshot) p->recent.resize(4096);
    if (p->options.rtcInitialTimeKnown && !p->peripherals.setRtcInitialTime(p->options.rtcInitialTime)) {
        p->result.stop = Imx1ProbeStop::unsupportedPeripheral;
        p->result.peripheralStatus = Imx1BootstrapAccess::externalInput;
        if (error) *error = p->result;
        delete p;
        return nullptr;
    }
    if (p->options.gpioAInputKnown) p->peripherals.setGpioInput(0, p->options.gpioAInput);
    if (p->options.gpioAInputMaskedKnown)
        p->peripherals.setGpioInputMasked(0, p->options.gpioAInput, p->options.gpioAInputMask);
    if (p->options.graphicsIrqA13) {
        if (!p->peripherals.reserveGpioInput(0, 0x2000) ||
            p->acceptGraphics(p->cs5Configuration, true) != Imx1BootstrapAccess::ok) {
            p->result.stop = Imx1ProbeStop::unsupportedPeripheral;
            p->result.peripheralStatus = Imx1BootstrapAccess::sequence;
            if (error) *error = p->result;
            delete p;
            return nullptr;
        }
    }
    if (p->options.gpioBInputKnown) p->peripherals.setGpioInput(1, p->options.gpioBInput);
    if (p->options.gpioDInputKnown) p->peripherals.setGpioDInput(p->options.gpioDInput);
    configureBoard(p->peripherals, p->options);
    p->peripherals.setI2cBus(p->options.i2cBus);
    p->peripherals.setI2cObserver(Probe::i2cEvent, p);
    p->peripherals.setDmaStoreHook(Probe::dmaStored, p);
    p->peripherals.setDmaExternal(&p->graphicsDma);
    if (rom && size) p->rom.assign(rom, rom + size);
    p->alias = entry == Imx1ProbeEntry::resetAlias ? 0 :
               entry == Imx1ProbeEntry::linkedAlias ? 0x20000000 : 0x10000000;
    auto* mem = memInit();
    p->mem = mem;
    memRegionAdd(mem, 0, 0xffffffff, Probe::access, p);
    auto* patches = initPatchDispatch();
    auto* pace = new PacePatch{};
    p->cpu = cpuInit(p->alias, mem, ARM_MEMORY_SYSTEM_MMU, false, false, -1, 0, 0,
                     patches, pace, createSystemState(), ArmCpuModel::arm920t, true);
    patchDispatchSetCpu(patches, p->cpu);
    if (p->options.bootstrapPeripherals) cpuArm920EnableWait(p->cpu);
    cpuArm920SetMemoryCheck(p->cpu, Probe::cacheMemoryCheck, p);
    cpuArm920SetIcacheIndexCompatibility(p->cpu, p->options.arm920IcacheIndexCompatibility);
    p->result.stop = Imx1ProbeStop::budget;
    p->observer = observer;
    p->observerContext = context;
    activeMachine = new Imx1Machine{p, 0};
    return activeMachine;
}

Imx1GraphicsRefusal imx1MachineGraphicsRefusal(const Imx1Machine* machine) {
    return {machine->probe->graphicsRefusal.what, machine->probe->graphicsRefusal.value};
}

Imx1MachineRun imx1MachineInspect(Imx1Machine* machine) {
    auto* p = machine->probe;
    if (p->stopped) return {Imx1MachineStatus::stopped, p->retired, p->result};
    if (p->options.bootstrapPeripherals) p->result.interruptPending = p->peripherals.interruptPending();
    if (p->options.bootstrapPeripherals) p->result.pcm = p->peripherals.pcmState();
    std::memcpy(p->result.registers, cpuGetRegisters(p->cpu), sizeof(p->result.registers));
    p->result.cpsr = cpuGetRegExternal(p->cpu, ARM_REG_NUM_CPSR);
    p->result.spsr = cpuGetRegExternal(p->cpu, ARM_REG_NUM_SPSR);
    p->result.graphicsLcdEnabled = p->graphicsFrame.lcdEnabled();
    p->result.graphicsFrameWidth = p->result.graphicsFrameHeight = p->result.graphicsFrameHash = 0;
    if (const auto* frame = p->graphicsFrame.peekFrame()) {
        p->result.graphicsFrameWidth = p->graphicsFrame.width();
        p->result.graphicsFrameHeight = p->graphicsFrame.height();
        uint32_t hash = 2166136261u;
        for (uint32_t i = 0; i < p->result.graphicsFrameWidth * p->result.graphicsFrameHeight; ++i)
            for (unsigned lane = 0; lane < 4; ++lane) hash = (hash ^ uint8_t(frame[i] >> (lane * 8))) * 16777619u;
        p->result.graphicsFrameHash = hash;
    }
    cpuJitExecutionCounts(p->cpu, &p->result.jitBlocks, &p->result.jitInstructions);
    return {p->terminal ? Imx1MachineStatus::terminal :
            p->result.stop == Imx1ProbeStop::idleNoWake ? Imx1MachineStatus::waiting :
            Imx1MachineStatus::yielded, p->retired, p->result};
}

// One guarded step: the probe guard, interrupts, one retired instruction and its charge. Returns false
// where the slice ends (a stop, a bound or a refused charge).
static bool runStep(Probe* p, uint64_t idleHclkLimit) {
    if (p->boundReached()) return false;
    const uint64_t step = p->retired;
    if (!p->result.gpioEventApplied && p->options.gpioEventAfterInstructions && step == p->options.gpioEventAfterInstructions) {
        if (!p->chargeDeferred()) { p->clockBoundary(); return false; }
        const auto status = p->peripherals.transitionGpioInputs(p->options.gpioEventValues, p->options.gpioEventMasks);
        if (status != Imx1BootstrapAccess::ok) {
            p->result.stop = Imx1ProbeStop::unsupportedPeripheral;
            p->result.peripheralStatus = status;
            p->result.trace = {step, cpuGetRegisters(p->cpu)[15], 0,
                cpuGetRegExternal(p->cpu, ARM_REG_NUM_CPSR), 0, cpuIsThumb(p->cpu) ? 2u : 4u};
            return false;
        }
        p->result.gpioEventApplied = true;
    }
    if (p->options.bootstrapPeripherals &&
        (cpuArm920Waiting(p->cpu) || !p->peripherals.mcuEnabled() || p->peripherals.mcuWakePending() ||
         p->peripherals.systemStopped())) {
        if (!p->chargeDeferred()) { p->clockBoundary(); return false; }
        bool wake = p->peripherals.wakeMcuFromInterrupt();
        // The first period of a wait always runs alone, so every check below has seen the state the skip starts from.
        bool settled = false;
        // In stop mode the CPU's clock is off wherever it is, in a wait or not.
        while ((cpuArm920Waiting(p->cpu) || p->peripherals.cpuClockHeld(cpuArm920ClockMode(p->cpu))) &&
               (!wake || p->peripherals.mcuWakePending())) {
            if (p->boundReached()) break;
            if (!wake && !p->idleWakePossible()) {
                p->result.stop = Imx1ProbeStop::idleNoWake;
                break;
            }
            if (p->result.idleHclk >= idleHclkLimit) { p->result.stop = Imx1ProbeStop::idleBudget; break; }
            // One HCLK preserves event ordering, including requests already high at entry.
            // A skip passes the quiet periods in one advance and then runs the last one the same way.
            const auto period = p->peripherals.busReferencePeriod();
            const uint64_t clocks = settled ? p->idleQuietHclk(period, idleHclkLimit) : 1;
            if (!p->peripherals.referenceChargeSupported(period.numerator, period.denominator)) {
                p->result.stop = Imx1ProbeStop::unsupportedPeripheral;
                p->result.peripheralStatus = Imx1BootstrapAccess::sideEffect;
                break;
            }
            if (clocks > 1) p->peripherals.advanceHclkCycles(uint32_t(clocks - 1));
            p->peripherals.advanceHclkCycles(1);
            if (!p->cs5Configuration.referencePeriodSupported(period)) {
                p->result.stop = Imx1ProbeStop::unsupportedPeripheral;
                p->result.peripheralStatus = Imx1BootstrapAccess::sideEffect;
                break;
            }
            const auto status = p->advanceGraphics([&](Imx1Cs5Configuration& c) {
                if (clocks > 1) c.advanceReference((clocks - 1) * period.numerator, period.denominator);
                c.advanceReference(period.numerator, period.denominator);
            });
            if (p->graphicsMemory) p->graphicsFrame.advance(p->cs5Configuration, *p->graphicsMemory, p->peripherals.eimState());
            p->result.idleHclk += clocks;
            p->work += clocks;
            ++p->idleAdvances;
            if (status != Imx1BootstrapAccess::ok) {
                p->result.stop = Imx1ProbeStop::unsupportedPeripheral;
                p->result.peripheralStatus = status;
                break;
            }
            if (p->clockBoundary()) break;
            wake = p->peripherals.wakeMcuFromInterrupt() || wake;
            settled = !p->options.idleStepped;
        }
        if (p->boundedYield || p->result.stop != Imx1ProbeStop::budget) return false;
        if (wake) cpuArm920EndWait(p->cpu);
    }
    // While cycles can wait nothing has changed since these were last looked at.
    const bool quiet = quietCycles != 0 && !linesPending;
    linesPending = false;
    // Conservative bus stall: jump to the reference event without retiring instructions.
    if (p->options.bootstrapPeripherals && !quiet) {
        p->finishGraphicsBusStall();
        if (p->clockBoundary()) return false;
    }
    if (p->boundReached()) return false;
    ++p->work;
    if (p->options.bootstrapPeripherals) {
        if (!quiet) {
            p->updateInterrupts();
            if (!deferredCycles && !deferredHclk) {
                quietMode = cpuArm920ClockMode(p->cpu);
                quietCycles = p->quietCpuCycles(quietMode);
            }
        }
        const auto pc = cpuGetRegisters(p->cpu)[15];
        const auto cpsr = cpuGetRegExternal(p->cpu, ARM_REG_NUM_CPSR);
        const auto lr = cpuGetRegisters(p->cpu)[14];
        const auto sp = cpuGetRegisters(p->cpu)[13];
        if (cpuProcessPendingInterrupts(p->cpu)) {
            const auto vector = cpuGetRegisters(p->cpu)[15];
            const auto mode = cpuGetRegExternal(p->cpu, ARM_REG_NUM_CPSR) & 31;
            if (p->blPrefix) {
                bool known = false;
                for (size_t n = 0; n < p->interruptedPrefixCount; ++n) {
                    const auto& saved = p->interruptedPrefixes[n];
                    known |= saved.pc == pc && saved.cpsr == cpsr && saved.mode == mode && saved.lr == lr && saved.sp == sp;
                }
                if (!known) {
                    if (p->interruptedPrefixCount == p->interruptedPrefixes.size()) p->prefixCapacityExceeded = true;
                    else p->interruptedPrefixes[p->interruptedPrefixCount++] = {pc, cpsr, mode, lr, sp};
                }
            }
            p->blPrefix = false;
            const bool fiq = mode == ARM_SR_MODE_FIQ;
            if (fiq) ++p->result.fiqEntries; else ++p->result.irqEntries;
            if (p->options.interruptObserver) p->options.interruptObserver(p->options.interruptContext,
                {step, pc, cpsr, vector, cpuGetRegisters(p->cpu)[14], fiq, p->peripherals.interruptPending()});
        }
    }
    auto& t = p->result.trace;
    t = {step, cpuGetRegisters(p->cpu)[15], 0,
         cpuGetRegExternal(p->cpu, ARM_REG_NUM_CPSR), 0, cpuIsThumb(p->cpu) ? 2u : 4u};
    uint_fast8_t fsr = 0;
    if (!cpuFetchInstruction(p->cpu, t.pc, &t.instruction, &fsr)) {
        if (!p->failed) {
            p->result.stop = Imx1ProbeStop::exception;
            p->result.address = t.pc;
            p->result.value = fsr;
            p->result.size = t.instructionSize;
        }
        return false;
    }
    if (t.instructionSize == 4 && (t.instruction & 0x0f100f10) == 0x0e000f10) {
        const unsigned source = (t.instruction >> 12) & 15;
        t.operand = source == 15 ? t.pc + 8 : cpuGetRegisters(p->cpu)[source];
    }
    if (p->observer) p->observer(p->observerContext, t);
    const bool thumb = t.instructionSize == 2;
    if (p->prefixCapacityExceeded || (p->blPrefix && (!thumb || t.pc != p->blNextPc || (t.instruction & 0xf800) != 0xf800)) ||
        (thumb ? !bootstrapThumb(t.instruction, cpuGetRegisters(p->cpu), p->blPrefix && t.pc == p->blNextPc) :
         t.instruction >> 28 == 15 ||
         (condition(t.instruction, t.cpsr) && !bootstrapInstruction(t.instruction, cpuGetRegisters(p->cpu), t.cpsr,
                                                                        cpuGetRegExternal(p->cpu, ARM_REG_NUM_SPSR))))) {
        p->result.stop = Imx1ProbeStop::unsupportedInstruction;
        return false;
    }
    // Validate the prospective clock before the CP15 instruction can change state.
    if (!thumb && p->options.bootstrapPeripherals && condition(t.instruction, t.cpsr) &&
        (t.instruction & 0x0fff0fff) == 0x0e010f10 &&
        !p->peripherals.cpuClockModeSupported(t.operand >> 30, p->options.assumeMpllSettled)) {
        p->result.stop = Imx1ProbeStop::unsupportedCp15;
        return false;
    }
    const auto exceptions = cpuExceptionSequence(p->cpu);
    // Each probe call retires one instruction. Capture its entry operands;
    // a Fast memory callback need not spill its current register values.
    p->graphicsSampledPin = 32;
    p->graphicsSampleOffset = 0;
    if (!thumb && t.pc == 0x200b9cd4 && t.instruction == 0xe791100c) {
        const auto* registers = cpuGetRegisters(p->cpu);
        if (registers[0] < 16 && registers[7] == 1 && (registers[12] == 0x4c0 || registers[12] == 0x530)) {
            p->graphicsSampledPin = registers[0];
            p->graphicsSampleOffset = registers[12];
        }
    }
    Probe::Recent recent{};
    if (p->options.diagnosticSnapshot) {
        const auto* r=cpuGetRegisters(p->cpu);
        recent={t.step,t.pc,t.instruction,t.cpsr,r[13],r[14],r[0],r[1],r[2],r[3]};
    }
    const auto cycles = cpuCycle<ARM_MEMORY_SYSTEM_MMU, false>(p->cpu, 1);
    if (p->failed) return false;
    const auto dataGuard = cpuArm920DataGuard(p->cpu);
    if (dataGuard.active) {
        p->result.stop = Imx1ProbeStop::unsupportedDataCache;
        p->result.address = dataGuard.address;
        p->result.size = dataGuard.size;
        p->result.value = dataGuard.write;
        return false;
    }
    const bool softwareEntry = softwareInterrupt(t.instruction, thumb) &&
                               (thumb || condition(t.instruction, t.cpsr));
    if (cpuExceptionSequence(p->cpu) != exceptions &&
        !(softwareEntry && cpuExceptionSequence(p->cpu) == exceptions + 1 &&
          (cpuGetRegisters(p->cpu)[15] == 8 || cpuGetRegisters(p->cpu)[15] == 0xffff0008) &&
          cpuGetRegisters(p->cpu)[14] == t.pc + t.instructionSize &&
          cpuGetRegExternal(p->cpu, ARM_REG_NUM_SPSR) == t.cpsr &&
          cpuGetRegExternal(p->cpu, ARM_REG_NUM_CPSR) == ((t.cpsr & ~0xbfu) | 0x93))) {
        p->result.stop = !thumb && (t.instruction & 0x0f000f10) == 0x0e000f10 ?
                         Imx1ProbeStop::unsupportedCp15 : Imx1ProbeStop::exception;
        return false;
    }
    if (p->options.diagnosticSnapshot) {
        p->recent[p->recentNext]=recent;
        p->recentNext=(p->recentNext+1)%4096;
        if (p->recentCount<4096) ++p->recentCount;
    }
    p->blPrefix = thumb && (t.instruction & 0xf800) == 0xf000;
    p->blNextPc = t.pc + 2;
    const bool contextReturn = !thumb && condition(t.instruction, t.cpsr) &&
        (((t.instruction & 0x0ffffff0) == 0x01b0f000) ||
         ((t.instruction & 0x0fffffff) == 0x025ef004) ||
         ((t.instruction & 0x0e508000) == 0x08508000));
    // A scheduler can resume another task before returning to the interrupted BL suffix.
    if (contextReturn) {
        const auto* registers = cpuGetRegisters(p->cpu);
        const auto cpsr = cpuGetRegExternal(p->cpu, ARM_REG_NUM_CPSR);
        for (size_t n = p->interruptedPrefixCount; n > 0; --n) {
            const auto& saved = p->interruptedPrefixes[n - 1];
            if ((t.cpsr & 31) == saved.mode && registers[15] == saved.pc &&
                cpsr == saved.cpsr && registers[14] == saved.lr && registers[13] == saved.sp) {
                p->blPrefix = true;
                p->blNextPc = saved.pc;
                p->interruptedPrefixes[n - 1] = p->interruptedPrefixes[--p->interruptedPrefixCount];
                break;
            }
        }
    }
    ++p->retired;
    if (p->options.bootstrapPeripherals && quietCycles && cycles <= quietCycles &&
        cpuArm920ClockMode(p->cpu) == quietMode) {
        deferredCycles += cycles;
        quietCycles -= cycles;
    } else if (p->options.bootstrapPeripherals) {
        if (!p->chargeDeferred()) { p->clockBoundary(); return false; }
        // The write that turned the CPU's own clock off ends there. The time after it passes in the wait loop.
        if (p->peripherals.cpuClockHeld(cpuArm920ClockMode(p->cpu))) return true;
        // An access charged the span so far. When a new span starts here, in the same clock mode and with no bus
        // stall to finish, this instruction's cycles wait in it, which gives the state their own charge would.
        // With a clock boundary pending they are charged below first, so the stop comes with them charged.
        if (cpuArm920ClockMode(p->cpu) == quietMode && p->graphicsWireStatus == Imx1BootstrapAccess::ok &&
            !p->peripherals.systemPllBusStallPending()) {
            const uint64_t span = p->quietCpuCycles(quietMode);
            if (span > cycles && p->boundaryStatus() == Imx1BootstrapAccess::ok) {
                deferredCycles = cycles;
                quietCycles = span - cycles;
                linesPending = true;
                return true;
            }
        }
        const auto graphicsStatus = p->advanceGraphicsCpu(cycles, p->peripherals.cpuReferencePeriod(cpuArm920ClockMode(p->cpu)));
        if (graphicsStatus != Imx1BootstrapAccess::ok) {
            p->result.stop = Imx1ProbeStop::unsupportedPeripheral;
            p->result.peripheralStatus = graphicsStatus;
            return false;
        }
        if (p->graphicsMemory) p->graphicsFrame.advance(p->cs5Configuration, *p->graphicsMemory, p->peripherals.eimState());
        p->peripherals.advanceCpuCycles(cycles, cpuArm920ClockMode(p->cpu), p->options.assumeMpllSettled);
        if (p->clockBoundary()) return false;
    }
    return true;
}

static void runSlice(Imx1Machine* machine, uint32_t retiredBudget, uint64_t idleHclkLimit) {
    auto* p = machine->probe;
    if (p->stopped || p->terminal || !retiredBudget) return;
    if (retiredBudget > imx1ProbeMaximumSteps || UINT64_MAX - p->retired < retiredBudget) {
        p->result.stop = Imx1ProbeStop::unsupportedPeripheral;
        p->result.peripheralStatus = Imx1BootstrapAccess::sequence;
        p->terminal = true;
        return;
    }
    p->result.stop = Imx1ProbeStop::budget;
    if (!p->fastEngine || !p->options.bootstrapPeripherals || p->observer || p->options.diagnosticSnapshot ||
        p->options.busObserver) {
        for (uint32_t slice = 0; slice < retiredBudget; ++slice)
            if (!runStep(p, idleHclkLimit)) break;
    } else {
        // Fast: after each step, the rest of a quiet span runs in the JIT and on the lean path. Their instructions
        // are charged one cycle each through the deferred charge, and they stop before anything the step has
        // to see (MMIO, CP15, faults, a guard refusal), which the next step runs. The JIT may pass the span by
        // up to its block length (the charge then comes a little late); the slice, the bounds and the GPIO
        // event it never passes. The last instruction before the slice's retired or work limit is left to the
        // step, so the slice always ends on a step and result.trace is the last instruction run.
        const uint32_t jitSlack = p->fastJit ? cpuArm920JitMaxUnchecked(p->cpu) : 0;
        for (uint32_t slice = 0; slice < retiredBudget;) {
            if (!runStep(p, idleHclkLimit)) break;
            ++slice;
            for (;;) {
                if (quietCycles < leanMinimumSpan || linesPending || p->blPrefix || slice + 1 >= retiredBudget ||
                    cpuArm920ClockMode(p->cpu) != quietMode) break;
                uint64_t hard = retiredBudget - slice - 1;
                if (p->bounds) {
                    const uint64_t used = p->work - p->sliceWorkStart;
                    hard = used + 1 >= p->bounds->work ? 0 : std::min(hard, p->bounds->work - used - 1);
                }
                if (!p->result.gpioEventApplied && p->options.gpioEventAfterInstructions > p->retired)
                    hard = std::min(hard, p->options.gpioEventAfterInstructions - p->retired);
                if (!hard) break;
                uint32_t leanCount = UINT32_MAX;
                // An exception return to an interrupted Thumb BL suffix goes through the step.
                if (p->fastJit && jitSlack && hard >= jitSlack && !p->interruptedPrefixCount) {
                    if (const uint64_t passed = p->skipScanPoll(hard - jitSlack)) {
                        slice += uint32_t(passed);
                        leanCs5 = {};
                        continue;
                    }
                    int stop;
                    jitBudget = uint32_t(std::min(quietCycles, hard - jitSlack + 1));
                    leanCs5 = {};
                    inJit = true;
                    // The slice limits instructions, the quiet span instructions and wait clocks together: the
                    // run's graphics accesses are checked against the span alone.
                    const uint32_t ran = cpuArm920RunJit(p->cpu, jitBudget, &stop,
                                                         uint32_t(std::min<uint64_t>(quietCycles, UINT32_MAX)));
                    inJit = false;
                    if (jitHalt) {
                        jitHalt = false;
                        if (!cpuArm920Waiting(p->cpu)) cpuClearSlowPath(p->cpu, SLOW_PATH_REASON_RESCHEDULE);
                    }
                    // Graphics accesses the run answered, as after a lean run.
                    deferredHclk += leanCs5.waitClocks;
                    p->result.cs5WaitClocks += leanCs5.waitClocks;
                    p->work += leanCs5.waitClocks;
                    quietCycles = leanCs5.waitCost < quietCycles ? quietCycles - leanCs5.waitCost : 0;
                    leanCs5 = {};
                    ++jitRuns;
                    if (ran) {
                        deferredCycles += ran;
                        quietCycles = ran < quietCycles ? quietCycles - ran : 0;
                        p->retired += ran;
                        jitRetired += ran;
                        p->work += ran;
                        slice += ran;
                        p->blPrefix = false;  // a Thumb BL pair runs whole in the JIT
                    }
                    if (stop == arm920JitBudget) {
                        p->skipDelay();
                        if (ran) continue;
                        break;
                    }
                    // A halt (an ARM/Thumb switch, a board write's reschedule, a pending interrupt) ends the run at a
                    // block end. After a run that did something, the JIT and the lean path try again: both refuse with
                    // anything but a switch or a reschedule pending, which then goes to the step.
                    if (stop == arm920JitHalt) {
                        if (ran) continue;
                        break;
                    }
                    // The instruction the JIT left goes to the lean path, then the JIT again. Code not compiled
                    // yet, or a JIT that keeps stopping straight away, runs on the lean path for a while.
                    leanCount = stop == arm920JitCold || ran < 4 ? 32 : 1;
                }
                if (quietCycles < leanMinimumSpan || slice + 1 >= retiredBudget) break;
                uint64_t budget = std::min<uint64_t>(quietCycles, retiredBudget - slice - 1);
                if (p->bounds) {
                    const uint64_t used = p->work - p->sliceWorkStart;
                    budget = used + 1 >= p->bounds->work ? 0 : std::min(budget, p->bounds->work - used - 1);
                }
                if (!p->result.gpioEventApplied && p->options.gpioEventAfterInstructions > p->retired)
                    budget = std::min(budget, p->options.gpioEventAfterInstructions - p->retired);
                if (!budget) break;
                uint32_t blNext;
                uint64_t busAccesses;
                leanCs5 = {};
                const uint32_t ran = cpuArm920RunLeanFor(p->cpu, uint32_t(budget), leanCount, &blNext, &busAccesses);
                p->result.busAccesses += busAccesses;
                // Graphics memory writes add their wait clocks to the work and the deferred charge, and their
                // cost to the span used, as cs5Wait would.
                deferredHclk += leanCs5.waitClocks;
                p->result.cs5WaitClocks += leanCs5.waitClocks;
                p->work += leanCs5.waitClocks;
                quietCycles -= leanCs5.waitCost;
                leanCs5 = {};
                if (!ran) break;
                deferredCycles += ran;
                quietCycles -= ran;
                p->retired += ran;
                leanRetired += ran;
                p->work += ran;
                slice += ran;
                p->blPrefix = blNext != ~0u;
                if (p->blPrefix) p->blNextPc = blNext;
                if (!p->fastJit) break;
            }
        }
    }
    // The slice ends with every cycle charged, so the state, the time and host input see the machine whole.
    if (!p->chargeDeferred() && p->result.stop == Imx1ProbeStop::budget) p->clockBoundary();
    p->terminal = p->result.stop != Imx1ProbeStop::budget &&
                  p->result.stop != Imx1ProbeStop::idleBudget && p->result.stop != Imx1ProbeStop::idleNoWake;
}

Imx1MachineRun imx1MachineRunSlice(Imx1Machine* machine, uint32_t retiredBudget, uint64_t idleHclkLimit) {
    runSlice(machine, retiredBudget, idleHclkLimit);
    return imx1MachineInspect(machine);
}

bool imx1MachineSetFastEngine(Imx1Machine* machine, bool fast) {
    auto* p = machine->probe;
    if (p->bounds || p->stopped) return false;
    p->fastEngine = fast;
    cpuArm920SetLean(p->cpu, fast ? Probe::leanPage : nullptr, fast ? bootstrapInstruction : nullptr, p);
    cpuArm920SetLeanRead(p->cpu, fast ? Probe::leanRead : nullptr);
    cpuArm920SetLeanWrite(p->cpu, fast ? Probe::leanWrite : nullptr);
    // The register reads the graphics model answers by pc that Fast blocks take in generated code: the identity
    // read in the register read helper, the scan status reads and the engine's ring pointer polls.
    static constexpr uint32_t accessPcs[] = {0x200871dc, 0x200adb08, 0x200adb14, 0x200ad7b0,
                                             0x200b8c64, 0x200b8c70, 0x200b8cb0, 0x200b8fcc, 0x200b9030};
    cpuArm920SetJitAccessPcs(p->cpu, accessPcs, sizeof accessPcs / sizeof accessPcs[0]);
    p->fastJit = fast && p->fastBlocks && p->options.bootstrapPeripherals && cpuArm920SetJit(p->cpu, true);
    if (!p->fastJit) cpuArm920SetJit(p->cpu, false);
    // Fast keeps no cache contents: dirty data goes to memory first.
    if (fast) cpuArm920LeanReset(p->cpu);
    p->attachGraphicsGpu();
    return true;
}

bool imx1MachineFastEngine(const Imx1Machine* machine) { return machine->probe->fastEngine; }

uint64_t imx1MachineLeanRetired(const Imx1Machine*) { return leanRetired; }

uint64_t imx1MachineJitRetired(const Imx1Machine*) { return jitRetired; }

uint64_t imx1MachineJitRuns(const Imx1Machine*) { return jitRuns; }
uint64_t imx1MachineJitReads(const Imx1Machine*) { return jitReads; }
uint64_t imx1MachineJitRegisterReads(const Imx1Machine*) { return jitRegisterReads; }
uint64_t imx1MachineJitBoardWrites(const Imx1Machine*) { return jitBoardWrites; }
uint64_t imx1MachineDelaySkips(const Imx1Machine*) { return delaySkips; }
uint64_t imx1MachineDelaySkipCycles(const Imx1Machine*) { return delaySkipCycles; }
uint64_t imx1MachineScanPollSkips(const Imx1Machine*) { return scanPollSkips; }
uint64_t imx1MachineScanPollRounds(const Imx1Machine*) { return scanPollRounds; }
void imx1SetScanPollSkip(bool skip) { scanPollSkip = skip; }

void imx1SetFastBlocksDefault(bool blocks) { fastBlocksDefault = blocks; }

bool imx1MachineSetFastBlocks(Imx1Machine* machine, bool blocks) {
    auto* p = machine->probe;
    if (p->bounds || p->stopped) return false;
    p->fastBlocks = blocks;
    return !p->fastEngine || imx1MachineSetFastEngine(machine, true);
}

uint64_t imx1MachineLeanTimerReads(const Imx1Machine*) { return leanTimerReads; }

uint64_t imx1MachineLeanCs5Writes(const Imx1Machine*) { return leanCs5Writes; }

uint64_t imx1MachineLeanCs5Reads(const Imx1Machine*) { return leanCs5Reads; }
uint64_t imx1MachineLeanCs5MemoryReads(const Imx1Machine*) { return leanCs5MemoryReads; }
uint64_t imx1MachineLeanBoardReads(const Imx1Machine*) { return leanBoardReads; }

uint64_t imx1MachineMemoryHash(const Imx1Machine* machine) {
    const auto* p = machine->probe;
    uint64_t h = p->peripherals.memoryHash(0xcbf29ce484222325ull);
    for (unsigned bank = 0; p->graphicsMemory && bank < 2; ++bank) {
        uint32_t size = 0;
        const uint8_t* data = p->graphicsMemory->diagnosticData(bank, size);
        for (uint32_t i = 0; i < size; ++i) h = (h ^ data[i]) * 0x100000001b3ull;
    }
    return h;
}

uint64_t imx1MachineLeanCodeDrops(const Imx1Machine* machine) { return cpuArm920LeanCodeDrops(machine->probe->cpu); }

uint16_t imx1MachinePcmLastTransmitted(const Imx1Machine* machine) {
    return machine->probe->peripherals.pcmLastTransmittedWord();
}

void imx1MachineEnableYmf768(Imx1Machine* machine) { machine->probe->peripherals.enableYmf768(); }

void imx1MachineSetPcmOutput(Imx1Machine* machine, void (*hook)(void*, uint16_t, unsigned), void* context) {
    machine->probe->peripherals.setPcmOutput(hook, context);
}

Imx1MachineAudio imx1MachineAudio(const Imx1Machine* machine) {
    const auto& board = machine->probe->peripherals;
    Imx1MachineAudio a;
    board.pcmFrameRate(a.rateNumerator, a.rateDenominator);
    a.pwmKnown = board.pwmDuty(a.pwmHigh, a.pwmPeriod);
    a.gpioDDirection = board.gpioRegister(3, 0);
    a.gpioDInUse = board.gpioRegister(3, 8);
    a.gpioDData = board.gpioRegister(3, 7);
    a.gpioDOutputSelect = board.gpioRegister(3, 2);
    return a;
}

Imx1MachineCheapStatus imx1MachineGetStatus(const Imx1Machine* machine) {
    const auto* p = machine->probe;
    return {p->stopped ? Imx1MachineStatus::stopped : p->terminal ? Imx1MachineStatus::terminal :
            p->result.stop == Imx1ProbeStop::idleNoWake ? Imx1MachineStatus::waiting : Imx1MachineStatus::yielded,
            p->result.stop, p->retired, p->work, p->result.idleHclk, p->result.cs5WaitClocks,
            p->peripherals.referenceTime(), p->stopped ? p->result.registers[15] : cpuGetRegisters(p->cpu)[15]};
}

Imx1MachineBoundedRun imx1MachineRunBounded(Imx1Machine* machine, const Imx1MachineBounds& bounds) {
    auto* p = machine->probe;
    const auto before = imx1MachineGetStatus(machine);
    Imx1MachineBoundedRun run{};
    run.total = before; run.reason = Imx1MachineYield::invalidBounds;
    if (p->stopped) { run.reason = Imx1MachineYield::stopped; return run; }
    if (p->terminal) { run.reason = Imx1MachineYield::terminal; return run; }
    // Reserve room for the largest atomic wait/instruction, without wrapping counters.
    if (!p->options.bootstrapPeripherals || !bounds.deadline.valid() ||
        bounds.retired > imx1ProbeMaximumSteps || UINT64_MAX - p->retired < bounds.retired ||
        UINT64_MAX - p->work < bounds.work || UINT64_MAX - p->work - bounds.work < UINT32_MAX)
        return run;
    if (!bounds.retired) { run.reason = Imx1MachineYield::retired; return run; }
    if (before.time.compare(bounds.deadline) >= 0) { run.reason = Imx1MachineYield::deadline; return run; }
    if (!bounds.work) { run.reason = Imx1MachineYield::work; return run; }
    p->bounds = &bounds; p->sliceWorkStart = p->work; p->boundedYield = false;
    runSlice(machine, bounds.retired, UINT64_MAX);
    p->bounds = nullptr; p->boundedYield = false;
    run.total = imx1MachineGetStatus(machine);
    run.retiredDelta = run.total.retired - before.retired;
    run.workDelta = run.total.work - before.work;
    run.elapsed = run.total.time.since(before.time);
    if (run.workDelta > bounds.work) run.workOvershoot = run.workDelta - bounds.work;
    if (run.total.time.compare(bounds.deadline) > 0) run.timeOvershoot = run.total.time.intervalSince(bounds.deadline);
    run.reason = p->terminal ? Imx1MachineYield::terminal :
        run.total.status == Imx1MachineStatus::waiting ? Imx1MachineYield::waiting :
        run.total.time.compare(bounds.deadline) >= 0 ? Imx1MachineYield::deadline :
        run.workDelta >= bounds.work ? Imx1MachineYield::work : Imx1MachineYield::retired;
    return run;
}

uint64_t imx1MachineIdleAdvances(const Imx1Machine* machine) { return machine->probe->idleAdvances; }
const uint32_t* imx1MachinePeekFrame(const Imx1Machine* machine, uint32_t& width, uint32_t& height) {
    const auto& frame = machine->probe->graphicsFrame;
    width = frame.width(); height = frame.height();
    return frame.peekFrame();
}
const uint32_t* imx1MachineAcquireFrame(Imx1Machine* machine, uint32_t& width, uint32_t& height,
                                      uint32_t& first, uint32_t& last) {
    auto& frame = machine->probe->graphicsFrame;
    width = frame.width(); height = frame.height();
    return frame.getPendingFrame(first, last);
}
void imx1MachineAcknowledgeFrame(Imx1Machine* machine) { machine->probe->graphicsFrame.acknowledgeFrame(); }
uint32_t imx1MachineStartups(const Imx1Machine* machine) { return machine ? machine->startups : 0; }
unsigned imx1MachineScanOrientation(const Imx1Machine* machine) {
    if (!machine) return 0;
    const auto& display = machine->probe->cs5Configuration.displayState();
    if (!display.lcdEnabled()) return 0;
    // 0x414 bits 3 and 4 are the scan rotation 200ae8e4 sets from the OS's degrees: 0 for 0, 1 for 90,
    // 3 for 180 and 2 for 270. TwScreen's landscape (SysGetOrientation 2) is 270.
    static const unsigned orientations[] = {1, 4, 2, 3};
    return orientations[(display.activeWord(0x414) >> 3) & 3];
}
Imx1BootstrapAccess imx1MachineReplacePenInputs(Imx1Machine* machine, const Imx1AspPenInputs& inputs, bool commit) {
    auto* p = machine->probe;
    if (p->stopped || p->terminal || !p->options.bootstrapPeripherals) return Imx1BootstrapAccess::sequence;
    return p->peripherals.replaceAspPenInputs(inputs, commit);
}
Imx1AspPenInputs imx1MachinePenInputs(const Imx1Machine* machine) {
    return machine && machine->probe->options.bootstrapPeripherals ? machine->probe->peripherals.aspPenInputs()
                                                                   : Imx1AspPenInputs{};
}

Imx1BootstrapAccess imx1MachineReplaceGpioInputs(Imx1Machine* machine, const uint32_t (&values)[4],
                                               const uint32_t (&masks)[4], bool commit) {
    if (!machine) return Imx1BootstrapAccess::sequence;
    auto* p = machine->probe;
    if (p->stopped || p->terminal || !p->options.bootstrapPeripherals) return Imx1BootstrapAccess::sequence;
    return p->peripherals.transitionGpioInputs(values, masks, commit);
}
Imx1BootstrapAccess imx1MachineSetMmcsdCard(Imx1Machine* machine, VSD* card, bool commit) {
    if (!machine) return Imx1BootstrapAccess::sequence;
    auto* p = machine->probe;
    if (p->stopped || p->terminal || !p->options.bootstrapPeripherals || !p->options.mmcsdEmptySlot)
        return Imx1BootstrapAccess::sequence;
    uint32_t values[4]{}, masks[4]{};
    masks[1] = imx1MmcsdCardDetectPin;
    values[1] = card ? 0 : imx1MmcsdCardDetectPin;
    const auto status = p->peripherals.transitionGpioInputs(values, masks, false);
    if (status != Imx1BootstrapAccess::ok || !commit) return status;
    p->peripherals.attachMmcsdCard(card);
    return p->peripherals.transitionGpioInputs(values, masks, true);
}
VSD* imx1MachineMmcsdCard(const Imx1Machine* machine) {
    return machine ? machine->probe->peripherals.mmcsdCard() : nullptr;
}
Imx1BootstrapAccess imx1MachineReplaceGraphicsAxes(Imx1Machine* machine, uint32_t first, uint32_t second, bool commit) {
    if (!machine) return Imx1BootstrapAccess::sequence;
    auto* p = machine->probe;
    if (p->stopped || p->terminal || !p->options.bootstrapPeripherals ||
        p->options.graphicsProfile != Imx1GraphicsProfile::virtual5744Compatible8MiB)
        return Imx1BootstrapAccess::sequence;
    if (first > 1023 || second > 1023) return Imx1BootstrapAccess::sideEffect;
    if (commit) p->cs5Configuration.setAcquisitionInput(first, second);
    return Imx1BootstrapAccess::ok;
}

Imx1BootstrapAccess imx1MachineSetZodiacControl(Imx1Machine* machine, Imx1ZodiacControl control, bool pressed, bool commit) {
    if (unsigned(control) >= unsigned(Imx1ZodiacControl::count)) return Imx1BootstrapAccess::sideEffect;
    const auto& pin = imx1ZodiacControlPins[unsigned(control)];
    uint32_t values[4]{}, masks[4]{};
    masks[pin.port] = 1u << pin.pin;
    values[pin.port] = pressed ? 0 : masks[pin.port];
    return imx1MachineReplaceGpioInputs(machine, values, masks, commit);
}
Imx1BootstrapAccess imx1MachineSetZodiacStick(Imx1Machine* machine, int x, int y, bool commit) {
    if (x < -imx1ZodiacStickTravel || x > imx1ZodiacStickTravel || y < -imx1ZodiacStickTravel || y > imx1ZodiacStickTravel)
        return machine ? Imx1BootstrapAccess::sideEffect : Imx1BootstrapAccess::sequence;
    return imx1MachineReplaceGraphicsAxes(machine, uint32_t(512 - x), uint32_t(512 + y), commit);
}
namespace {
void emitDiagnostic(Probe* p, const ArmDiagnosticSink& sink, bool legacy) {
        if (legacy) {
            const uint32_t end[] = {uint32_t(p->result.stop),uint32_t(p->result.trace.step),p->result.trace.pc,
                uint32_t(p->result.idleHclk),uint32_t(p->result.idleHclk >> 32),
                uint32_t(p->result.busAccesses),uint32_t(p->result.busAccesses >> 32),p->result.graphicsFrameHash};
            sink.emit("stop",end,8);
        }
        cpuEmitArm920Diagnostic(p->cpu,sink);
        p->peripherals.emitDiagnosticMemory(sink);
        if (p->graphicsMemory) {
            for (unsigned bank=0;bank<2;++bank) {
                uint32_t bytes=0;
                const auto* data=p->graphicsMemory->diagnosticData(bank,bytes);
                const auto& windows=p->cs5Configuration.memoryWindows();
                const auto window=bank ? windows.external() : windows.internal();
                const uint32_t map[] = {bank,window.first,window.last,uint32_t(window.known),bytes};
                sink.emit("gpu-memory-map",map,5);
                if (sink.bytes) sink.bytes(sink.context,bank ? "gpu-external" : "gpu-internal",data,bytes);
            }
            const auto& display=p->cs5Configuration.displayState();
            for (uint32_t offset=0x410;offset<=0x59c;offset+=4) {
                const uint32_t row[] = {offset,display.stagedWord(offset),display.activeWord(offset)};
                sink.emit("display-word",row,3);
            }
            const uint32_t state[] = {uint32_t(display.lcdEnabled()),uint32_t(display.latchPending()),
                uint32_t(display.revision()),uint32_t(display.revision() >> 32),
                uint32_t(display.frameNumber()),uint32_t(display.frameNumber() >> 32),
                uint32_t(display.scanPhase()),uint32_t(display.scanPhase() >> 32)};
            sink.emit("display-state",state,8);
        }
        const uint32_t romMap[] = {0x10000000,uint32_t(p->rom.size()),p->alias,
            p->alias == 0 ? uint32_t(p->rom.size() > 0x100000 ? 0x100000 : p->rom.size()) : uint32_t(p->rom.size())};
        sink.emit("rom-map",romMap,4);
        for (uint32_t i=0;legacy && i<p->recentCount;++i) {
            const auto& r=p->recent[(p->recentNext+4096-p->recentCount+i)%4096];
            const uint32_t row[] = {uint32_t(r.step),r.pc,r.instruction,r.cpsr,r.sp,r.lr,r.r0,r.r1,r.r2,r.r3};
            sink.emit("recent",row,10);
        }
}
}

void imx1MachineEmitDiagnostic(Imx1Machine* machine, const Imx1MachineDiagnosticSink& sink) {
    auto* p = machine->probe;
    if (!sink.counters) return;
    imx1MachineInspect(machine);
    const uint64_t schema[] = {2};
    sink.counters(sink.context, "machine-schema", schema, 1);
    emitDiagnostic(p, sink.architecture, false);
    const uint64_t end[] = {uint64_t(p->result.stop), p->result.trace.step, p->result.trace.pc,
        p->retired, p->result.idleHclk, p->result.busAccesses, p->result.cs5WaitClocks,
        p->result.irqEntries, p->result.fiqEntries, p->result.jitBlocks, p->result.jitInstructions};
    sink.counters(sink.context, "machine-stop", end, 11);
    for (uint32_t i = 0; i < p->recentCount; ++i) {
        const auto& r = p->recent[(p->recentNext + 4096 - p->recentCount + i) % 4096];
        const uint64_t row[] = {r.step,r.pc,r.instruction,r.cpsr,r.sp,r.lr,r.r0,r.r1,r.r2,r.r3};
        sink.counters(sink.context, "machine-recent", row, 10);
    }
}

void imx1MachineStop(Imx1Machine* machine) {
    auto* p = machine->probe;
    if (p->stopped) return;
    p->peripherals.setI2cObserver(nullptr, nullptr);
    p->peripherals.setI2cBus(nullptr);
    p->options = {};
    p->observer = nullptr; p->observerContext = nullptr;
    p->stopped = true;
    // The core lives on, but nothing runs it again: give back the JIT's memory.
    cpuArm920DropJit(p->cpu);
    p->fastJit = false;
    // And the GPU, with its pages brought back first.
    if (p->graphicsGpu.gpu) p->graphicsGpu.gpu->detach();
    p->graphicsGpu.gpu.reset();
    if (activeMachine == machine) activeMachine = nullptr;
}

Imx1ProbeResult imx1BootProbe(const uint8_t* rom, uint32_t size, Imx1ProbeEntry entry,
                            uint32_t steps, Imx1ProbeObserver observer, void* context, bool fast,
                            const Imx1ProbeOptions* options) {
    Imx1ProbeResult error{};
    if (!steps || steps > imx1ProbeMaximumSteps ||
        (options && (options->idleHclkBudget > imx1ProbeMaximumIdleHclk || !imx1ProbeGpioEventValid(*options, steps)))) {
        error.stop = Imx1ProbeStop::unsupportedPeripheral;
        error.peripheralStatus = Imx1BootstrapAccess::sequence;
        return error;
    }
    auto* machine = imx1MachineCreate(rom, size, entry, fast, options ? *options : Imx1ProbeOptions{}, observer, context, &error);
    if (!machine) return error;
    auto* p = machine->probe;
    const auto result = imx1MachineRunSlice(machine, steps, p->options.idleHclkBudget).result;
    if (p->options.frameObserver) {
        uint32_t first = 0, last = 0;
        if (const auto* frame = p->graphicsFrame.getPendingFrame(first, last))
            p->options.frameObserver(p->options.frameContext, p->result.graphicsFrameWidth, p->result.graphicsFrameHeight, frame);
    }
    if (p->options.diagnosticSnapshot) emitDiagnostic(p, *p->options.diagnosticSnapshot, true);
    imx1MachineStop(machine);
    return result;
}

const char* imx1ProbeStopName(Imx1ProbeStop stop) {
    switch (stop) {
        case Imx1ProbeStop::idleBudget: return "idle-clock-budget";
        case Imx1ProbeStop::idleNoWake: return "idle-no-known-wake";
        case Imx1ProbeStop::budget: return "instruction-budget";
        case Imx1ProbeStop::unsupportedDataCache: return "unsupported-data-cache";
        case Imx1ProbeStop::unsupportedInstruction: return "unsupported-instruction";
        case Imx1ProbeStop::unsupportedCp15: return "unsupported-cp15";
        case Imx1ProbeStop::unsupportedRead: return "unsupported-read";
        case Imx1ProbeStop::unsupportedWrite: return "unsupported-write";
        case Imx1ProbeStop::exception: return "cpu-exception";
        case Imx1ProbeStop::engineUnavailable: return "engine-unavailable";
        case Imx1ProbeStop::unsupportedPeripheral: return "unsupported-peripheral";
    }
    return "unknown";
}

// Machine state. The format is in machine_state_archive.h, the content lists are the state()
// members of the models, Arm920SavedState and ProbeState below.
namespace {
constexpr uint32_t stateFlagFast = 1, stateFlagRing = 2, stateFlagTerminal = 4;
constexpr uint32_t stateIdentityVersion = 1, stateMemoryVersion = 1, stateRingVersion = 1, stateJitVersion = 1;
constexpr uint32_t stateGraphicsMemoryVersion = 1, stateBusVersion = 1;
constexpr uint32_t stateRingEntries = 4096;
constexpr uint32_t stateMaxNoteBytes = 4096;

// What the run loop keeps in Probe between two calls, and the part of the result that Inspect
// doesn't work out again. bounds, sliceWorkStart and boundedYield are only set inside a call.
// The WFI loop's wake and settled flags are worked out again on every entry, and idleAdvances
// is a host count that differs between the skip and the stepped loop.
struct ProbeState {
    static constexpr uint32_t stateVersion = 1;
    uint64_t retired = 0, work = 0;
    bool blPrefix = false;
    uint32_t blNextPc = 0;
    std::array<Probe::InterruptedPrefix, 256> interruptedPrefixes{};
    uint32_t interruptedPrefixCount = 0;
    bool prefixCapacityExceeded = false, terminal = false, failed = false;
    uint32_t graphicsSampledPin = 32, graphicsSampleOffset = 0;
    uint64_t i2cIssueStep = 0;
    uint32_t i2cIssuePc = 0;
    bool irqRaised = false, fiqRaised = false;
    Imx1BootstrapAccess graphicsWireStatus = Imx1BootstrapAccess::ok;
    Imx1ProbeResult result{};

    void capture(const Probe& p) {
        retired = p.retired; work = p.work;
        blPrefix = p.blPrefix; blNextPc = p.blNextPc;
        interruptedPrefixes = p.interruptedPrefixes;
        interruptedPrefixCount = uint32_t(p.interruptedPrefixCount);
        prefixCapacityExceeded = p.prefixCapacityExceeded; terminal = p.terminal; failed = p.failed;
        graphicsSampledPin = p.graphicsSampledPin; graphicsSampleOffset = p.graphicsSampleOffset;
        i2cIssueStep = p.i2cIssueStep; i2cIssuePc = p.i2cIssuePc;
        irqRaised = p.irqRaised; fiqRaised = p.fiqRaised;
        graphicsWireStatus = p.graphicsWireStatus;
        result = p.result;
    }
    void apply(Probe& p) const {
        p.retired = retired; p.work = work;
        p.blPrefix = blPrefix; p.blNextPc = blNextPc;
        p.interruptedPrefixes = interruptedPrefixes;
        p.interruptedPrefixCount = interruptedPrefixCount;
        p.prefixCapacityExceeded = prefixCapacityExceeded; p.terminal = terminal; p.failed = failed;
        p.graphicsSampledPin = graphicsSampledPin; p.graphicsSampleOffset = graphicsSampleOffset;
        p.i2cIssueStep = i2cIssueStep; p.i2cIssuePc = i2cIssuePc;
        p.irqRaised = irqRaised; p.fiqRaised = fiqRaised;
        p.graphicsWireStatus = graphicsWireStatus;
        p.result = result;
    }
    template <class A> void state(A& a) {
        a.Do64(retired).Do64(work).DoBool(blPrefix).Do32(blNextPc).Do32(interruptedPrefixCount);
        a.Require(interruptedPrefixCount <= interruptedPrefixes.size(), "interrupted prefix count");
        // Slots past the count are never read.
        for (uint32_t n = 0; a.ok() && n < interruptedPrefixCount; ++n) {
            auto& s = interruptedPrefixes[n];
            a.Do32(s.pc).Do32(s.cpsr).Do32(s.mode).Do32(s.lr).Do32(s.sp);
        }
        a.DoBool(prefixCapacityExceeded).DoBool(terminal).DoBool(failed);
        a.Do32(graphicsSampledPin).Do32(graphicsSampleOffset).Do64(i2cIssueStep).Do32(i2cIssuePc);
        a.DoBool(irqRaised).DoBool(fiqRaised).DoEnum(graphicsWireStatus, 12);
        // registers, cpsr, spsr, the frame fields, pcm and the JIT counters come from Inspect.
        auto& r = result;
        a.DoEnum(r.stop, 11).Do64(r.trace.step).Do32(r.trace.pc).Do32(r.trace.instruction).Do32(r.trace.cpsr);
        a.Do32(r.trace.operand).Do32(r.trace.instructionSize).Do32(r.address).Do32(r.value).Do32(r.size);
        a.Do64(r.busAccesses).Do64(r.cs5WaitClocks).Do64(r.idleHclk).Do64(r.irqEntries).Do64(r.fiqEntries);
        a.DoEnum(r.peripheralStatus, 12);
        auto& e = r.i2cEvent;
        a.Do64(e.step).Do32(e.pc).Do64(e.issueStep).Do32(e.issuePc);
        a.DoEnum(e.transfer.phase, 6).Do8(e.transfer.address).Do8(e.transfer.byte);
        a.DoBool(e.transfer.read).DoBool(e.transfer.acknowledge).DoEnum(e.response, 4);
        a.Do32(e.control).Do32(e.status).Do32(e.divider);
        a.DoBool(r.gpioEventApplied).DoBool(r.elapsedInterruptBoundary).DoBool(r.possibleWatchdogTimeoutBoundary);
        a.Do64(r.interruptPending.normal).Do64(r.interruptPending.fast);
        a.Do64(r.interruptPending.unknownNormal).Do64(r.interruptPending.unknownFast);
        const bool resumable = r.stop == Imx1ProbeStop::budget || r.stop == Imx1ProbeStop::idleBudget ||
                               r.stop == Imx1ProbeStop::idleNoWake;
        a.Require(terminal == !resumable, "terminal stop");
    }
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1ProbeOptions) == 1352, "update stateOptionFields, stateIdentityVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Probe) - Imx1Bootstrap::unsavedBytes - Probe::unsavedBytes == 26080, "update ProbeState, its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1ProbeResult) == 352, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Arm920SavedState) == 43328, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Bootstrap) - Imx1Bootstrap::unsavedBytes == 2344, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Aitc) == 80, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Cgm) == 128, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Gpio) == 416, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Eim) == 56, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Timer) == 28, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Sysctrl) == 12, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Reset) == 4, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Bta) == 4, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Uart) == 264, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Mmcsd) - Imx1Mmcsd::unsavedBytes == 96, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1I2c) == 64, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Asp) == 100, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Pwm) == 64, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Watchdog) == 40, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Rtc) == 48, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Cs4Capture) == 1040, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Cs5Reset) == 8, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Cs5Configuration) == 5136, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsGpio) == 36, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsWindows) == 32, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsLookup) == 3080, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsController) == 32, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsEngine) == 96, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsDisplay) == 1504, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsInterrupts) == 12, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsAcquisition) == 80, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsSd) == 80, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsClocks) == 104, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsCommands) == 10352, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsMemory) - Imx1GraphicsMemory::unsavedBytes == 248, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsFrame) == 136, "update its stateVersion and the size in stateLayoutFingerprint");
#endif

// The fingerprint hashes each section's version and a fixed size word per type: the x86-64 libstdc++ sizes
// they had when the layout was set. Sections are written field by field, so the fingerprint must be the same
// on every host (libc++ on Android gives Probe and Imx1GraphicsCommands other sizes). On x86-64 the asserts below
// catch a size change, which needs a stateVersion bump and the new size here.
constexpr uint64_t stateLayoutFingerprint() {
    const uint64_t words[] = {
        stateFourcc("IDNT"), stateIdentityVersion, 1352,
        stateFourcc("PROB"), ProbeState::stateVersion, 26080, 352,
        stateFourcc("CPU "), Arm920SavedState::stateVersion, 43328,
        stateFourcc("BOOT"), Imx1Bootstrap::stateVersion, 2344,
        Imx1Aitc::stateVersion, 80, Imx1Cgm::stateVersion, 128,
        Imx1Gpio::stateVersion, 416, Imx1Eim::stateVersion, 56,
        Imx1Timer::stateVersion, 28, Imx1Sysctrl::stateVersion, 12,
        Imx1Reset::stateVersion, 4, Imx1Bta::stateVersion, 4,
        Imx1Uart::stateVersion, 264,
        Imx1Mmcsd::stateVersion, 96, Imx1I2c::stateVersion, 64,
        Imx1Asp::stateVersion, 100, Imx1Pwm::stateVersion, 64,
        Imx1Watchdog::stateVersion, 40, Imx1Rtc::stateVersion, 48,
        stateFourcc("CS4 "), Imx1Cs4Capture::stateVersion, 1040,
        stateFourcc("CS5R"), Imx1Cs5Reset::stateVersion, 8,
        stateFourcc("CS5C"), Imx1Cs5Configuration::stateVersion, 5136,
        Imx1GraphicsGpio::stateVersion, 36,
        Imx1GraphicsWindows::stateVersion, 32,
        Imx1GraphicsLookup::stateVersion, 3080,
        Imx1GraphicsController::stateVersion, 32,
        Imx1GraphicsEngine::stateVersion, 96,
        Imx1GraphicsDisplay::stateVersion, 1504,
        Imx1GraphicsInterrupts::stateVersion, 12,
        Imx1GraphicsAcquisition::stateVersion, 80,
        Imx1GraphicsSd::stateVersion, 80,
        Imx1GraphicsClocks::stateVersion, 104,
        stateFourcc("GCMD"), Imx1GraphicsCommands::stateVersion, 10352,
        stateFourcc("GMEM"), Imx1GraphicsMemory::stateVersion, 248, stateGraphicsMemoryVersion,
        stateFourcc("FRAM"), Imx1GraphicsFrame::stateVersion, 136,
        stateFourcc("I2CB"), stateBusVersion,
        stateMemoryVersion, stateRingVersion, stateJitVersion};
    uint64_t hash = 1469598103934665603ull;
    for (const auto word : words) hash = stateLayoutMix(hash, word);
    return hash;
}
static_assert(stateLayoutFingerprint() == 13343462981849839175ull, "the state layout changed: update this value");

// Every option that shapes the machine, by name, so a mismatch can say which one.
// Left out: observers and contexts, the diagnostic and frame sinks, the I2C bus (it has its own
// identity), idleHclkBudget (a run limit) and idleStepped (both loops give the same state).
struct StateOptionField { const char* name; uint64_t value; };
std::vector<StateOptionField> stateOptionFields(const Imx1ProbeOptions& o, uint32_t alias) {
    uint64_t indexed = 1469598103934665603ull;
    for (int bank = 0; bank < 2; ++bank)
        for (int index = 0; index < 256; ++index)
            indexed = stateLayoutMix(indexed, uint64_t(int64_t(o.cs4IndexedInputs.sample(bank, index))));
    const auto& pen = o.aspPenInputs;
    return {
        {"entry", alias},
        {"graphicsProfile", uint64_t(o.graphicsProfile)},
        {"cs5ResetStatusInputKnown", o.cs5ResetStatusInputKnown}, {"cs5ResetStatusInput", o.cs5ResetStatusInput},
        {"rtcInitialTimeKnown", o.rtcInitialTimeKnown}, {"rtcInitialTime.day", o.rtcInitialTime.day},
        {"rtcInitialTime.hour", o.rtcInitialTime.hour}, {"rtcInitialTime.minute", o.rtcInitialTime.minute},
        {"rtcInitialTime.second", o.rtcInitialTime.second},
        {"cs4Port0InputKnown", o.cs4Port0InputKnown}, {"cs4Port0Input", o.cs4Port0Input},
        {"cs4IndexedInputs", indexed},
        {"aspPx1HighFromReset", o.aspPx1HighFromReset},
        {"aspPenInputs.contactKnown", pen.contactKnown}, {"aspPenInputs.contactDown", pen.contactDown},
        {"aspPenInputs.normalKnown", pen.normalKnown}, {"aspPenInputs.referenceKnown", pen.referenceKnown},
        {"aspPenInputs.normal[0]", pen.normal[0]}, {"aspPenInputs.normal[1]", pen.normal[1]},
        {"aspPenInputs.normal[2]", pen.normal[2]}, {"aspPenInputs.normal[3]", pen.normal[3]},
        {"aspPenInputs.reference[0]", pen.reference[0]}, {"aspPenInputs.reference[1]", pen.reference[1]},
        {"aspPenInputs.reference[2]", pen.reference[2]},
        {"aspUSampleKnown", o.aspUSampleKnown}, {"aspUSample", o.aspUSample},
        {"assumeI2cSingleMaster", o.assumeI2cSingleMaster},
        {"bootstrapPeripherals", o.bootstrapPeripherals},
        {"arm920IcacheIndexCompatibility", o.arm920IcacheIndexCompatibility},
        {"lcdcContrastOnly", o.lcdcContrastOnly}, {"mmcsdEmptySlot", o.mmcsdEmptySlot},
        {"assumeMpllSettled", o.assumeMpllSettled}, {"virtualMpllRetune", o.virtualMpllRetune},
        {"assumeSpllSettled", o.assumeSpllSettled}, {"graphicsIrqA13", o.graphicsIrqA13},
        {"graphicsVirtualA4SourceOver", o.graphicsVirtualA4SourceOver},
        {"graphicsVirtualPackedMono", o.graphicsVirtualPackedMono},
        {"graphicsMode2Route", uint64_t(o.graphicsMode2Route)},
        {"graphicsRawAuxKnown", o.graphicsRawAuxKnown}, {"graphicsRawAux", o.graphicsRawAux},
        {"graphicsRawMode1Known", o.graphicsRawMode1Known}, {"graphicsRawMode1", o.graphicsRawMode1},
        {"graphicsRawAxesKnown", o.graphicsRawAxesKnown}, {"graphicsRawFirst", o.graphicsRawFirst},
        {"graphicsRawSecond", o.graphicsRawSecond},
        {"gpioEventAfterInstructions", o.gpioEventAfterInstructions},
        {"gpioEventValues[0]", o.gpioEventValues[0]}, {"gpioEventValues[1]", o.gpioEventValues[1]},
        {"gpioEventValues[2]", o.gpioEventValues[2]}, {"gpioEventValues[3]", o.gpioEventValues[3]},
        {"gpioEventMasks[0]", o.gpioEventMasks[0]}, {"gpioEventMasks[1]", o.gpioEventMasks[1]},
        {"gpioEventMasks[2]", o.gpioEventMasks[2]}, {"gpioEventMasks[3]", o.gpioEventMasks[3]},
        {"gpioAInputMaskedKnown", o.gpioAInputMaskedKnown}, {"gpioAInputMask", o.gpioAInputMask},
        {"gpioAInputKnown", o.gpioAInputKnown}, {"gpioAInput", o.gpioAInput},
        {"gpioBInputKnown", o.gpioBInputKnown}, {"gpioBInput", o.gpioBInput},
        {"gpioDInputKnown", o.gpioDInputKnown}, {"gpioDInput", o.gpioDInput},
        {"ramMiB", o.ramMiB},
    };
}

const uint8_t* stateRomDigest(const Probe& p) {
    if (!p.romDigestKnown) {
        MD5Context md5;
        md5Init(&md5);
        if (!p.rom.empty()) md5Update(&md5, const_cast<uint8_t*>(p.rom.data()), p.rom.size());
        md5Finalize(&md5);
        std::memcpy(p.romDigest, md5.digest, 16);
        p.romDigestKnown = true;
    }
    return p.romDigest;
}

// 0 without a bus. An attached bus that has no identity can't be saved.
uint32_t stateBusIdentity(const Probe& p) { return p.options.i2cBus ? p.options.i2cBus->stateIdentity() : 0; }
bool stateConfigurationSupported(const Probe& p) { return !p.options.i2cBus || stateBusIdentity(p); }

template <class A> void stateRecent(A& a, std::vector<Probe::Recent>& recent, uint32_t& next, uint32_t& count) {
    a.Do32(next).Do32(count);
    a.Require(count <= stateRingEntries && next < stateRingEntries && (count == stateRingEntries || next == count),
              "recent ring");
    if (!a.ok()) return;
    if (recent.size() < stateRingEntries) recent.resize(stateRingEntries);
    // Filled from slot 0, so the first count slots are the used ones.
    for (uint32_t n = 0; a.ok() && n < count; ++n) {
        auto& r = recent[n];
        a.Do64(r.step).Do32(r.pc).Do32(r.instruction).Do32(r.cpsr).Do32(r.sp).Do32(r.lr);
        a.Do32(r.r0).Do32(r.r1).Do32(r.r2).Do32(r.r3);
    }
}

void stateText(StateWriter& a, const char* text) {
    uint32_t length = text ? uint32_t(std::min<size_t>(std::strlen(text), stateMaxNoteBytes)) : 0;
    a.Do32(length);
    if (length) a.DoBuffer(const_cast<char*>(text), length);
}
bool stateText(StateReader& a, std::string& text, uint32_t limit) {
    uint32_t length = 0;
    a.Do32(length).Require(length <= limit, "text length");
    if (!a.ok()) return false;
    text.resize(length);
    if (length) a.DoBuffer(text.data(), length);
    return a.ok();
}

void setSection(Imx1StateInfo& info, uint32_t fourcc) {
    for (unsigned n = 0; n < 4; ++n) info.section[n] = char(fourcc >> (n * 8));
    info.section[4] = 0;
}

// Everything a load decodes before it touches the machine.
struct StagedState {
    std::unique_ptr<Arm920SavedState> cpu;
    std::unique_ptr<Imx1Bootstrap> board;
    std::unique_ptr<Imx1Cs4Capture> cs4;
    std::unique_ptr<Imx1Cs5Reset> cs5Reset;
    std::unique_ptr<Imx1Cs5Configuration> graphics;
    std::unique_ptr<Imx1GraphicsCommands> commands;
    std::unique_ptr<Imx1GraphicsMemory> graphicsMemory;
    std::unique_ptr<Imx1GraphicsFrame> frame;
    ProbeState probe;
    // The bus section, checked against the bus and still inside the caller's bytes.
    const uint8_t* bus = nullptr;
    size_t busBytes = 0;
    bool ring = false, jit = false;
    // An older state without a YMF section keeps B15 as it was saved.
    bool ymfSection = false;
    std::vector<Probe::Recent> recent;
    uint32_t recentNext = 0, recentCount = 0;
    uint64_t jitBlocks = 0, jitInstructions = 0;
};

Imx1StateInfo decodeState(const Probe& p, const uint8_t* data, size_t size, uint32_t flags,
                          const char* buildNote, StagedState& staged) {
    using S = Imx1StateStatus;
    Imx1StateInfo info;
    const auto fail = [&](S status, uint32_t fourcc, const char* detail) {
        info.status = status;
        if (fourcc) setSection(info, fourcc);
        info.detail = detail;
        return info;
    };
    if (p.stopped || p.bounds) return fail(S::notAtBoundary, 0, nullptr);
    if (!stateConfigurationSupported(p)) return fail(S::unsupportedConfiguration, 0, nullptr);
    // A load replaces the pixels a caller still holds.
    if (p.graphicsFrame.frameAcquired()) return fail(S::notAtBoundary, 0, "frame acquired");
    if (!data) return fail(S::truncated, 0, nullptr);
    StateContainerReader file;
    const auto opened = file.open(data, size);
    info.formatVersion = file.formatVersion;
    info.coreRevision = file.coreRevision;
    switch (opened) {
        case StateContainerError::ok: break;
        case StateContainerError::truncated: return fail(S::truncated, 0, nullptr);
        case StateContainerError::badMagic: return fail(S::badMagic, 0, nullptr);
        case StateContainerError::badFormatVersion: return fail(S::badFormatVersion, 0, nullptr);
        case StateContainerError::corruptHeader: return fail(S::corruptSection, stateFourcc("HEAD"), "header or table");
        case StateContainerError::corruptSection: return fail(S::corruptSection, file.corrupt()->fourcc, "crc");
    }
    if (file.coreRevision != imx1MachineStateCoreRevision) {
        if (!(flags & imx1StateAllowCoreRevision)) return fail(S::coreRevisionMismatch, 0, nullptr);
        info.coreRevisionDiffers = true;
    }
    if (file.layoutFingerprint != stateLayoutFingerprint()) return fail(S::layoutMismatch, 0, "layout fingerprint");
    if (file.flags & ~(stateFlagFast | stateFlagRing | stateFlagTerminal)) return fail(S::invalidValue, 0, "flags");

    const StateSection* section = nullptr;
    const auto open = [&](const char (&name)[5], uint32_t version, bool required, StateReader& reader) {
        section = file.find(stateFourcc(name));
        if (!section) {
            if (required) fail(S::missingSection, stateFourcc(name), nullptr);
            return false;
        }
        if (section->version != version) { fail(S::layoutMismatch, section->fourcc, "section version"); return false; }
        reader = file.reader(*section);
        return true;
    };
    // A decode failure is a fixed field that differs from this machine's, or a bad value.
    const auto finish = [&](const StateReader& reader) {
        if (reader.complete()) return true;
        fail(reader.fixedMismatch() ? S::optionsMismatch : S::invalidValue, section->fourcc,
             reader.ok() ? "trailing bytes" : reader.detail());
        return false;
    };
    StateReader a(nullptr, 0);

    if (!open("IDNT", stateIdentityVersion, true, a)) return info;
    {
        uint64_t romBytes = 0;
        uint8_t digest[16]{};
        uint32_t fieldCount = 0, busIdentity = 0;
        a.Do64(romBytes).DoBuffer(digest, 16);
        if (a.ok() && (romBytes != p.rom.size() || std::memcmp(digest, stateRomDigest(p), 16)))
            return fail(S::romMismatch, section->fourcc, "rom");
        const auto fields = stateOptionFields(p.options, p.alias);
        a.Do32(fieldCount);
        if (a.ok() && fieldCount != fields.size()) return fail(S::optionsMismatch, section->fourcc, "option count");
        for (const auto& field : fields) {
            std::string name;
            uint64_t value = 0;
            if (!stateText(a, name, 64)) break;
            a.Do64(value);
            const bool bootInput = !std::strncmp(field.name, "gpioEvent", 9) || !std::strcmp(field.name, "gpioAInput") ||
                                   !std::strcmp(field.name, "gpioDInput");
            if (a.ok() && (name != field.name ||
                           (value != field.value && !(bootInput && (flags & imx1StateAllowBootInputs)))))
                return fail(S::optionsMismatch, section->fourcc, field.name);
        }
        a.Do32(busIdentity);
        if (a.ok() && busIdentity != stateBusIdentity(p)) return fail(S::busMismatch, section->fourcc, "bus identity");
        std::string note;
        stateText(a, note, stateMaxNoteBytes);
        if (!finish(a)) return info;
        info.buildNoteDiffers = note != (buildNote ? buildNote : "");
    }

    if (!open("PROB", ProbeState::stateVersion, true, a)) return info;
    staged.probe.state(a);
    if (!finish(a)) return info;
    info.retired = staged.probe.retired;
    if (staged.probe.terminal != bool(file.flags & stateFlagTerminal))
        return fail(S::invalidValue, section->fourcc, "terminal flag");

    // The live state supplies the fixed fields the saved ones are held to.
    staged.cpu = std::make_unique<Arm920SavedState>();
    if (!cpuArm920CaptureState(p.cpu, *staged.cpu)) return fail(S::unsupportedConfiguration, 0, "cpu");
    if (!open("CPU ", Arm920SavedState::stateVersion, true, a)) return info;
    staged.cpu->stateCpu(a);
    if (!finish(a)) return info;
    if (!cpuArm920StateFits(p.cpu, *staged.cpu) || (staged.cpu->waitingIrqs != 0) != staged.probe.irqRaised ||
        (staged.cpu->waitingFiqs != 0) != staged.probe.fiqRaised)
        return fail(S::invalidValue, section->fourcc, "interrupt lines");
    if (!open("ICAC", Arm920SavedState::stateVersion, true, a)) return info;
    staged.cpu->stateIcache(a);
    if (!finish(a)) return info;
    if (!open("DCAC", Arm920SavedState::stateVersion, true, a)) return info;
    staged.cpu->stateDcache(a);
    if (!finish(a)) return info;
    if (!open("TLB ", Arm920SavedState::stateVersion, true, a)) return info;
    staged.cpu->stateTlb(a);
    if (!finish(a)) return info;

    staged.board = std::make_unique<Imx1Bootstrap>(p.options.aspPx1HighFromReset, p.options.aspUSampleKnown,
                                                   p.options.aspUSample);
    configureBoard(*staged.board, p.options);
    if (!open("BOOT", Imx1Bootstrap::stateVersion, true, a)) return info;
    staged.board->state(a);
    if (!finish(a)) return info;
    // The card section, only in states saved with a card in. It waits in the board for the next attach.
    if (open("SDCD", Imx1Bootstrap::cardStateVersion, false, a)) {
        staged.board->stateCard(a);
        if (!finish(a)) return info;
    } else if (info.status != S::ok) return info;
    // The memory DMA section, only in states saved with a memory-to-memory transfer under way.
    if (open("MDMA", Imx1Bootstrap::memoryDmaStateVersion, false, a)) {
        staged.board->stateMemoryDma(a);
        if (!finish(a)) return info;
    } else if (info.status != S::ok) return info;
    // The YMF768 section, in states saved with the chip on. Without it the chip starts reset and GPIO B15
    // stays at the state's level, so an older session runs as it did until the ROM next touches the chip.
    staged.ymfSection = open("YMF ", Imx1Bootstrap::ymf768StateVersion, false, a);
    if (staged.ymfSection) {
        staged.board->stateYmf768(a);
        if (!finish(a)) return info;
    } else if (info.status != S::ok) return info;
    if (!open("SRAM", stateMemoryVersion, true, a)) return info;
    staged.board->stateSram(a);
    if (!finish(a)) return info;
    for (unsigned bank = 0; bank < 2; ++bank) {
        if (!open(bank ? "SDR1" : "SDR0", stateMemoryVersion, true, a)) return info;
        staged.board->stateSdram(a, bank);
        if (!finish(a)) return info;
    }

    // Graphics, built from the options the way Probe builds its own.
    staged.cs4 = std::make_unique<Imx1Cs4Capture>(p.options.cs4Port0Input, p.options.cs4IndexedInputs);
    if (!open("CS4 ", Imx1Cs4Capture::stateVersion, true, a)) return info;
    staged.cs4->state(a);
    if (!finish(a)) return info;
    staged.cs5Reset = std::make_unique<Imx1Cs5Reset>(
        p.options.graphicsProfile == Imx1GraphicsProfile::strict ? p.options.cs5ResetStatusInput : 0);
    if (!open("CS5R", Imx1Cs5Reset::stateVersion, true, a)) return info;
    staged.cs5Reset->state(a);
    if (!finish(a)) return info;
    staged.graphics = std::make_unique<Imx1Cs5Configuration>(p.options.graphicsProfile, p.options.graphicsVirtualA4SourceOver,
                                                             p.options.graphicsVirtualPackedMono);
    Probe::configureAcquisition(*staged.graphics, p.options);
    if (!open("CS5C", Imx1Cs5Configuration::stateVersion, true, a)) return info;
    staged.graphics->state(a);
    if (!finish(a)) return info;
    staged.commands = std::make_unique<Imx1GraphicsCommands>();
    if (!open("GCMD", Imx1GraphicsCommands::stateVersion, true, a)) return info;
    staged.commands->state(a);
    if (!finish(a)) return info;
    if (p.graphicsMemory) {
        staged.graphicsMemory = std::make_unique<Imx1GraphicsMemory>();
        if (!open("GMEM", Imx1GraphicsMemory::stateVersion, true, a)) return info;
        staged.graphicsMemory->state(a);
        if (!finish(a)) return info;
        for (unsigned bank = 0; bank < 2; ++bank) {
            if (!open(bank ? "GEXT" : "GINT", stateGraphicsMemoryVersion, true, a)) return info;
            staged.graphicsMemory->stateBytes(a, bank);
            if (!finish(a)) return info;
        }
    }
    staged.frame = std::make_unique<Imx1GraphicsFrame>();
    if (!open("FRAM", Imx1GraphicsFrame::stateVersion, true, a)) return info;
    staged.frame->state(a);
    if (!finish(a)) return info;
    if (p.options.i2cBus) {
        if (!open("I2CB", stateBusVersion, true, a)) return info;
        if (!p.options.i2cBus->loadState(a, false))
            return fail(S::invalidValue, section->fourcc, a.detail() ? a.detail() : "bus state");
        staged.bus = data + section->offset;
        staged.busBytes = size_t(section->size);
    } else if (file.find(stateFourcc("I2CB"))) {
        return fail(S::invalidValue, stateFourcc("I2CB"), "no bus");
    }

    // The ring and the JIT counters are restored where this machine has them.
    info.status = S::ok;
    if (open("RECN", stateRingVersion, false, a)) {
        if (!(file.flags & stateFlagRing)) return fail(S::invalidValue, section->fourcc, "ring flag");
        stateRecent(a, staged.recent, staged.recentNext, staged.recentCount);
        if (!finish(a)) return info;
        staged.ring = true;
    } else if (info.status != S::ok) return info;
    else if (file.flags & stateFlagRing) return fail(S::missingSection, stateFourcc("RECN"), nullptr);
    if (open("JITC", stateJitVersion, false, a)) {
        if (!(file.flags & stateFlagFast)) return fail(S::invalidValue, section->fourcc, "engine flag");
        a.Do64(staged.jitBlocks).Do64(staged.jitInstructions);
        if (!finish(a)) return info;
        staged.jit = true;
    } else if (info.status != S::ok) return info;
    else if (file.flags & stateFlagFast) return fail(S::missingSection, stateFourcc("JITC"), nullptr);
    uint64_t blocks = 0, instructions = 0;
    info.engineChanged = cpuJitExecutionCounts(p.cpu, &blocks, &instructions) != staged.jit;
    return info;
}
}  // namespace

Imx1StateStatus imx1MachineSaveState(Imx1Machine* machine, std::vector<uint8_t>& out, const char* buildNote) {
    using S = Imx1StateStatus;
    out.clear();
    if (!machine) return S::notAtBoundary;
    auto* p = machine->probe;
    if (p->stopped || p->bounds) return S::notAtBoundary;
    if (!stateConfigurationSupported(*p)) return S::unsupportedConfiguration;
    // The caller still holds this frame's pixels. It acknowledges first.
    if (p->graphicsFrame.frameAcquired()) return S::notAtBoundary;
    auto cpu = std::make_unique<Arm920SavedState>();
    if (!cpuArm920CaptureState(p->cpu, *cpu)) return S::unsupportedConfiguration;
    // The loaded machine starts without translated code, so this one carries on without it too.
    cpuDropTranslatedCode(p->cpu);
    uint64_t jitBlocks = 0, jitInstructions = 0;
    const bool jit = cpuJitExecutionCounts(p->cpu, &jitBlocks, &jitInstructions);
    const bool ring = !p->recent.empty();
    // A load ends with Inspect, which reads the pending lines again. A bounded run doesn't, so do it here.
    if (p->options.bootstrapPeripherals) p->result.interruptPending = p->peripherals.interruptPending();
    ProbeState probe;
    probe.capture(*p);

    StateContainerWriter file(out, imx1MachineStateCoreRevision, stateLayoutFingerprint(),
                              (jit ? stateFlagFast : 0) | (ring ? stateFlagRing : 0) | (p->terminal ? stateFlagTerminal : 0));
    {
        auto a = file.begin(stateFourcc("IDNT"), stateIdentityVersion);
        uint64_t romBytes = p->rom.size();
        a.Do64(romBytes).DoBuffer(const_cast<uint8_t*>(stateRomDigest(*p)), 16);
        const auto fields = stateOptionFields(p->options, p->alias);
        uint32_t fieldCount = uint32_t(fields.size()), busIdentity = stateBusIdentity(*p);
        a.Do32(fieldCount);
        for (const auto& field : fields) {
            uint64_t value = field.value;
            stateText(a, field.name);
            a.Do64(value);
        }
        a.Do32(busIdentity);
        stateText(a, buildNote);
        file.end();
    }
    { auto a = file.begin(stateFourcc("PROB"), ProbeState::stateVersion); probe.state(a); file.end(); }
    { auto a = file.begin(stateFourcc("CPU "), Arm920SavedState::stateVersion); cpu->stateCpu(a); file.end(); }
    { auto a = file.begin(stateFourcc("ICAC"), Arm920SavedState::stateVersion); cpu->stateIcache(a); file.end(); }
    { auto a = file.begin(stateFourcc("DCAC"), Arm920SavedState::stateVersion); cpu->stateDcache(a); file.end(); }
    { auto a = file.begin(stateFourcc("TLB "), Arm920SavedState::stateVersion); cpu->stateTlb(a); file.end(); }
    { auto a = file.begin(stateFourcc("BOOT"), Imx1Bootstrap::stateVersion); p->peripherals.state(a); file.end(); }
    if (p->peripherals.mmcsdCard()) {
        auto a = file.begin(stateFourcc("SDCD"), Imx1Bootstrap::cardStateVersion);
        p->peripherals.stateCard(a);
        file.end();
    }
    if (p->peripherals.memoryDmaBusy()) {
        auto a = file.begin(stateFourcc("MDMA"), Imx1Bootstrap::memoryDmaStateVersion);
        p->peripherals.stateMemoryDma(a);
        file.end();
    }
    if (p->peripherals.ymf768Enabled()) {
        auto a = file.begin(stateFourcc("YMF "), Imx1Bootstrap::ymf768StateVersion);
        p->peripherals.stateYmf768(a);
        file.end();
    }
    { auto a = file.begin(stateFourcc("SRAM"), stateMemoryVersion); p->peripherals.stateSram(a); file.end(); }
    for (unsigned bank = 0; bank < 2; ++bank) {
        auto a = file.begin(stateFourcc(bank ? "SDR1" : "SDR0"), stateMemoryVersion);
        p->peripherals.stateSdram(a, bank);
        file.end();
    }
    { auto a = file.begin(stateFourcc("CS4 "), Imx1Cs4Capture::stateVersion); p->cs4Capture.state(a); file.end(); }
    { auto a = file.begin(stateFourcc("CS5R"), Imx1Cs5Reset::stateVersion); p->cs5Reset.state(a); file.end(); }
    { auto a = file.begin(stateFourcc("CS5C"), Imx1Cs5Configuration::stateVersion); p->cs5Configuration.state(a); file.end(); }
    { auto a = file.begin(stateFourcc("GCMD"), Imx1GraphicsCommands::stateVersion); p->graphicsCommands.state(a); file.end(); }
    if (p->graphicsMemory) {
        { auto a = file.begin(stateFourcc("GMEM"), Imx1GraphicsMemory::stateVersion); p->graphicsMemory->state(a); file.end(); }
        for (unsigned bank = 0; bank < 2; ++bank) {
            auto a = file.begin(stateFourcc(bank ? "GEXT" : "GINT"), stateGraphicsMemoryVersion);
            p->graphicsMemory->stateBytes(a, bank);
            file.end();
        }
    }
    { auto a = file.begin(stateFourcc("FRAM"), Imx1GraphicsFrame::stateVersion); p->graphicsFrame.state(a); file.end(); }
    if (p->options.i2cBus) {
        auto a = file.begin(stateFourcc("I2CB"), stateBusVersion);
        p->options.i2cBus->saveState(a);
        file.end();
    }
    if (ring) {
        auto a = file.begin(stateFourcc("RECN"), stateRingVersion);
        stateRecent(a, p->recent, p->recentNext, p->recentCount);
        file.end();
    }
    if (jit) {
        auto a = file.begin(stateFourcc("JITC"), stateJitVersion);
        a.Do64(jitBlocks).Do64(jitInstructions);
        file.end();
    }
    file.finish();
    return S::ok;
}

Imx1StateInfo imx1MachineCheckState(const Imx1Machine* machine, const uint8_t* data, size_t size,
                                    uint32_t flags, const char* buildNote) {
    if (!machine) { Imx1StateInfo info; info.status = Imx1StateStatus::notAtBoundary; return info; }
    StagedState staged;
    return decodeState(*machine->probe, data, size, flags, buildNote, staged);
}

Imx1StateInfo imx1MachineLoadState(Imx1Machine* machine, const uint8_t* data, size_t size,
                                   uint32_t flags, const char* buildNote) {
    if (!machine) { Imx1StateInfo info; info.status = Imx1StateStatus::notAtBoundary; return info; }
    auto* p = machine->probe;
    StagedState staged;
    auto info = decodeState(*p, data, size, flags, buildNote, staged);
    if (info.status != Imx1StateStatus::ok) return info;
    // Nothing from here on can fail. No cpuIrq call, so the restored lines make no new edge.
    cpuArm920ApplyState(p->cpu, *staged.cpu);
    if (flags & imx1StateExternalReset) staged.board->takeExternalReset(p->peripherals);
    else if (flags & imx1StateKeepMemory) staged.board->takeMemory(p->peripherals);
    // The card stays in the slot. It carries on from the state's card section if there is one, else it
    // starts again with no transfer.
    VSD* const card = p->peripherals.mmcsdCard();
    staged.board->takePcmOutput(p->peripherals);
    if (p->peripherals.ymf768Enabled()) staged.board->enableYmf768(staged.ymfSection);
    p->peripherals = std::move(*staged.board);
    p->peripherals.setI2cBus(p->options.i2cBus);
    p->peripherals.setI2cObserver(Probe::i2cEvent, p);
    p->peripherals.setDmaStoreHook(Probe::dmaStored, p);
    p->peripherals.setDmaExternal(&p->graphicsDma);
    info.cardResumed = card && p->peripherals.attachMmcsdCard(card);
    p->cs4Capture = *staged.cs4;
    p->cs5Reset = *staged.cs5Reset;
    p->cs5Configuration = std::move(*staged.graphics);
    p->graphicsCommands = std::move(*staged.commands);
    if (p->graphicsMemory) p->graphicsMemory.swap(staged.graphicsMemory);
    if (p->graphicsGpu.gpu) p->graphicsGpu.gpu->attach(p->graphicsMemory.get());
    p->graphicsFrame.replace(*staged.frame);
    if (p->options.i2cBus) {
        // Checked in decodeState, so this takes it.
        StateReader bus(staged.bus, staged.busBytes);
        p->options.i2cBus->loadState(bus, true);
    }
    staged.probe.apply(*p);
    if (!p->recent.empty()) {
        if (staged.ring) { p->recent.swap(staged.recent); p->recentNext = staged.recentNext; p->recentCount = staged.recentCount; }
        else p->recentNext = p->recentCount = 0;
    }
    // After the CPU state, so the JIT takes its mapping and permission context from it.
    cpuDropTranslatedCode(p->cpu);
    cpuJitSetExecutionCounts(p->cpu, staged.jit ? staged.jitBlocks : 0, staged.jit ? staged.jitInstructions : 0);
    // Fast starts from the loaded state with its caches cleaned and emptied, over the loaded memory.
    if (p->fastEngine) cpuArm920LeanReset(p->cpu);
    imx1MachineInspect(machine);
    return info;
}

const char* imx1StateStatusName(Imx1StateStatus status) {
    switch (status) {
        case Imx1StateStatus::ok: return "ok";
        case Imx1StateStatus::notAtBoundary: return "not-at-boundary";
        case Imx1StateStatus::unsupportedConfiguration: return "unsupported-configuration";
        case Imx1StateStatus::truncated: return "truncated";
        case Imx1StateStatus::badMagic: return "bad-magic";
        case Imx1StateStatus::badFormatVersion: return "bad-format-version";
        case Imx1StateStatus::coreRevisionMismatch: return "core-revision-mismatch";
        case Imx1StateStatus::layoutMismatch: return "layout-mismatch";
        case Imx1StateStatus::corruptSection: return "corrupt-section";
        case Imx1StateStatus::missingSection: return "missing-section";
        case Imx1StateStatus::romMismatch: return "rom-mismatch";
        case Imx1StateStatus::optionsMismatch: return "options-mismatch";
        case Imx1StateStatus::busMismatch: return "bus-mismatch";
        case Imx1StateStatus::invalidValue: return "invalid-value";
    }
    return "unknown";
}
