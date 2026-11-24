#include "imx1_boot_probe.h"
#include "imx1_cs5_bus.h"
#include "xscale_cost.h"

#include <algorithm>
#include <exception>
#include <cassert>
#include <cstring>
#include <optional>
#include <memory>
#include <string>
#include "twglue_log.h"
#include "zodiac_led.h"
#include "imx1_graphics_frame.h"
#include "imx1_graphics_commands.h"
#include "imx1_graphics_gpu_gles.h"
#include <array>
#include <vector>

#include "CPU.h"
#include "MMU.h"
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
// Fast: how many CPU cycles past the end of quietCycles the board and the bounds stay quiet, where the span ends on a
// graphics change (a lagged ring submit can run on to the new batch's). Set with each new span, 0 where unknown;
// the span's use takes from both alike.
uint64_t boardSlack = 0;
// Fast only: CS5 wait clocks (HCLK) of graphics memory writes the lean path took inside the quiet span, charged
// after deferredCycles. The span covers both, so the order of the two charges changes nothing.
uint64_t deferredHclk = 0;
// Fast: the part of deferredCycles and deferredHclk from before a deferred ring submit (jitChargedWrite), charged to
// the chip first and as one piece, as the submit's own charge would have (chargePre). No graphics model changes state
// within it, so the submits' pieces add up to one.
uint64_t preCycles = 0, preHclk = 0;
// Set while access takes a deferred submit: the chip stays behind, so access doesn't charge.
bool deferSubmitAccess = false;
// Set when an instruction that charged opened a new quiet span itself: the next step still looks at the interrupt
// lines, as it would after charging that instruction alone.
bool linesPending = false;
// Exact: the last three instructions the step retired in this slice, at step % 3, for skipUartPollStepped. plain is
// a step that did one unit of work, took no interrupt or exception and waited no clocks; bus is its board accesses,
// address the last one's.
struct SteppedInstruction {
    uint64_t step;
    uint32_t pc, instruction, cycles, bus, address;
    bool plain;
};
SteppedInstruction stepped[3]{};
uint32_t lastBusAddress = 0;
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
    // Writes to the ring wrap view, the producers' batch words: worked out at the first one the slow path takes.
    // A word at pa in [ringLow, ringLow + ringSpan) goes to bank storage at ringOffset + ((pa - ringWrap) & ringMask).
    bool ringChecked, ringUsable;
    unsigned ringBank;
    uint32_t ringLow, ringSpan, ringWrap, ringMask, ringOffset;
};
LeanCs5 leanCs5{};
// Fast blocks: set while cpuArm920RunJit runs, with its budget; jitHalt when a board write asked it to stop at its
// next block (SLOW_PATH_REASON_RESCHEDULE, cleared after the run).
bool inJit = false, jitHalt = false;
// Fast blocks: a timer count read by a delay loop's load (delayLoopPc) ends the run, and the next run starts with
// skipDelay (delayHalted). delayHaltOff stops that until the next step once a skip found nothing to pass. After a
// skip that passed cycles (delayPassed), the next run's first such read doesn't end it: it sees the wait over.
bool delayHalted = false, delayHaltOff = false, delayPassed = false;
// The ROM's delay loop loads: ARM at 2009f678, Thumb at 2008492c. Both are marked access pcs.
static bool delayLoopPc(uint32_t pc) { return pc == 0x2009f678 || pc == 0x2008492c; }
uint32_t jitBudget = 0;
uint64_t jitBoardWrites = 0, jitReads = 0, jitRegisterReads = 0, delaySkips = 0, delaySkipCycles = 0, delayHaltSkips = 0;  // for tests
uint64_t scanPollSkips = 0, scanPollRounds = 0, fifoWaitSkips = 0, fifoWaitRounds = 0;  // for tests
uint64_t uartPollSkips = 0, uartPollRounds = 0;  // for tests
uint64_t jitChargedWrites = 0;  // for tests
uint64_t jitCardWrites = 0;  // for tests
// Fast blocks: set by jitChargedWrite around its access when the write's wait clocks pass with no change. access then
// commits the write at once, with no preview and no cs5Wait, and jitChargedWrite defers the clocks (deferredHclk).
bool leanSubmit = false;
// A graphics-only stretch of a JIT run (lagged ring submits, submitLag) can leave the board behind the chip until
// the next board access. 0 between slices.
uint32_t pendingBoardCycles = 0, pendingBoardHclk = 0;
bool scanPollSkip = true, fifoWaitSkip = true, uartPollSkip = true;
// Fast blocks: the pen routine's ASP FIFO wait (skipAspPoll). FROND_ASP_POLL_SKIP=0 turns it off, for measuring. A
// FIFO_PTR read by the loop's load that sees the loop go on ends the JIT run, so the next run starts on the load
// (aspHalted); aspHaltOff stops that until the next step once a skip after such a halt passed nothing.
bool aspPollSkip = [] {
    const char* v = std::getenv("FROND_ASP_POLL_SKIP");
    return !(v && *v && !std::strcmp(v, "0"));
}();
bool aspHalted = false, aspHaltOff = false;
// Fast blocks: Xploids' draw retry (skipSurfaceWait). FROND_SURFACE_WAIT_SKIP=0 turns it off, for measuring.
bool surfaceWaitSkip = [] {
    const char* v = std::getenv("FROND_SURFACE_WAIT_SKIP");
    return !(v && *v && !std::strcmp(v, "0"));
}();
uint64_t surfaceWaitSkips = 0, surfaceWaitRounds = 0;  // for tests and traces
// Xploids' draw retry (ARM, game code, at 30952890 in the z15 runs, found by its words): it calls the DAL surface
// routine at 2009bc3c through [[sl + 0x772d8] + 1496] (glue 2021444c), which returns 0x7115 while the surface's busy
// bit ([surface + 44] bit 0, surface = [r6 + 12]) is set, then SysTaskDelay(0) through [[sl + 0x772d8] + 644] (glue
// 20214294), and loops. It keeps no count and reads no time; only a transfer's completion interrupt (20098fd8) clears
// the bit. SysTaskDelay(0) goes through KALTaskDelay (Thumb, 200a8bba) to SWI 30 at 200a8bca, which returns at once
// for 0. The SWI's exit (20083516, 2008357e) switches tasks only when the kernel's chosen ready task [P + 0x21c0]
// isn't the current one [P + 0x21c4] and neither lock byte (P + 0x21d0, 0x21d1) is set, P = [[[r9]] + 0x4a3c]. So
// with that task the current one and no interrupt raised, a round from the SWI vector back to it changes nothing
// but time until the next board event. The SWI ends the JIT run there (cpuArm920SetJitSwiHalt); one round is run
// in the JIT and on the lean path and measured, and when it comes back to the state it started from the rounds up
// to the span's end pass at once.
constexpr uint32_t surfaceWaitSwiReturn = 0x200a8bcc;
// From the loop's head to its literals; the call to SysTaskDelay returns to word 18.
constexpr uint32_t surfaceWaitLoop[] = {0xe596000c, 0xe28d1000, 0xe1a02005, 0xe59f3050, 0xe7b3400a, 0xe59435d8,
                                        0xe1a0e00f, 0xe12fff13, 0xe59f1040, 0xe1500001, 0x1a000007, 0xe3a04001,
                                        0xe3a00000, 0xe59f1028, 0xe7b1200a, 0xe5921284, 0xe1a0e00f, 0xe12fff11,
                                        0xea000000, 0xe3a04000, 0xe3540000, 0x1affffe9, 0xe28dd010, 0xe8bd4070,
                                        0xe12fff1e, 0x000772d8, 0x00007115};
constexpr uint32_t surfaceWaitLoopReturn = 18;
// The DAL routine's entry up to its busy return (2009bc3c-2009bc84), and its literals at 2009bebc (0x7115) and
// 2009bec8 (the surface's tag).
constexpr uint32_t surfaceDalAt = 0x2009bc3c;
constexpr uint32_t surfaceDalCode[] = {0xe92d42f0, 0xe5999000, 0xe24dd018, 0xe5999000, 0xe1b04000, 0xe1a07002,
                                       0x0a000003, 0xe5940000, 0xe59f2264, 0xe1500002, 0x0a000003, 0xe59f025c,
                                       0xe28dd018, 0xe8bd42f0, 0xe12fff1e, 0xe594002c, 0xe3100001, 0x159f0234,
                                       0x1afffff8};
// KALTaskDelay (200a8bba-200a8bd4): r1 = r0 < 0 ? -1 : r0; swi 30; returns r0 when 0.
constexpr uint32_t surfaceKalAt = 0x200a8bba;
constexpr uint16_t surfaceKalCode[] = {0xb5b0, 0x1c05, 0x2100, 0x43c9, 0x2d00, 0xdb00, 0x1c29, 0x1c08,
                                       0xdf1e, 0x1c04, 0xd102, 0xbcb0, 0xbc08, 0x4718};
// The round being measured: the state at its start, the retired count then and the cost model clocks and
// instructions the JIT and the lean path ran since. Anything else (a step, a span boundary, another skip) ends it.
// Host only, not saved.
struct SurfaceWait {
    bool armed = false;
    uint32_t state[20]{};
    uint64_t retiredAt = 0, cycles = 0, instructions = 0;
} surfaceWait;
// Fast blocks: the ROM card driver's STATUS waits (skipCardPoll). FROND_CARD_POLL_SKIP=0 turns them off, for
// measuring. A STATUS read by either wait's last load that sees its loop go on ends the JIT run, so the next run
// starts at the loop's head (cardHalted); cardHaltOff stops that until the next step or quiet span once a skip after
// such a halt passed nothing.
bool cardPollSkip = [] {
    const char* v = std::getenv("FROND_CARD_POLL_SKIP");
    return !(v && *v && !std::strcmp(v, "0"));
}();
bool cardHalted = false, cardHaltOff = false;
uint64_t cardPollSkips = 0, cardPollRounds = 0;  // for tests and traces
// Fast: the card lookahead (Imx1Bootstrap::setMmcsdLookahead), set as the engine is. FROND_CARD_LOOKAHEAD=0 turns
// it off, for measuring. leanStatusReads counts the STATUS reads the lean path worked out past the board's time.
bool cardLookahead = [] {
    const char* v = std::getenv("FROND_CARD_LOOKAHEAD");
    return !(v && *v && !std::strcmp(v, "0"));
}();
uint64_t leanStatusReads = 0, jitCardLatches = 0, jitChargedReads = 0;  // for tests
// Every lookahead MMCSD and DMA register read in generated code charged first, as one past a card event is; for tests.
bool chargeCardReads = false;
// Fast: slot 2's SD block reads on the lean path (leanSlot2Read), for plain Fast runs only, set each slice in runSlice.
// FROND_SLOT2_LEAN=0 turns it off, for measuring. leanSlot2Reads counts those reads since the process started.
bool slot2Lean = [] {
    const char* v = std::getenv("FROND_SLOT2_LEAN");
    return !(v && *v && !std::strcmp(v, "0"));
}();
bool slot2LeanOn = false;
uint64_t leanSlot2Reads = 0, leanSlot2Writes = 0, jitSlot2Accesses = 0;  // for tests
// Fast blocks: slot 2's status waits (skipSlot2Poll), with slot2Lean. A status read on the lean path that sees the
// wait go on ends the JIT run (slot2Halted); slot2HaltOff stops that until the next step or charged access once a
// skip after such a halt passed nothing. FROND_SLOT2_POLL_SKIP=0 turns the skip off, for measuring.
bool slot2PollSkip = [] {
    const char* v = std::getenv("FROND_SLOT2_POLL_SKIP");
    return !(v && *v && !std::strcmp(v, "0"));
}();
bool slot2Halted = false, slot2HaltOff = false;
uint64_t slot2PollSkips = 0, slot2PollRounds = 0;  // for tests
// Fast blocks: a quiet span can run past queued graphics batches' completions to the last one's (spanCrosses), where
// the run reads nothing those completions change: the ring and status registers, graphics memory and the option word.
// The fills, the read pointer and the option word are then applied at the span's charge, in order, and the batches'
// timing is the same (the engine works through its ticks whether or not a completion has been taken). An access that
// could see them (leanGraphicsRead, leanRegisterRead but the scan status, a graphics memory write outside the ring
// view, the FIFO wait skip) is refused, which ends the run for the step to take after a charge, and stops crossing
// (crossBlocked) until the next ring submit. FROND_ZODIAC_CROSS=0 turns it off, for measuring.
bool crossCompletions = [] {
    const char* v = std::getenv("FROND_ZODIAC_CROSS");
    return !(v && *v && !std::strcmp(v, "0"));
}();
// A crossing span also runs past the last queued batch's completion where the engine going idle changes no rate
// (Imx1Cs5Configuration::idleQuietStepsThrough's pastLast), to the next board, clock or display event or submit.
// FROND_ZODIAC_CROSS_END=0 turns that part off, for measuring.
bool crossEnd = [] {
    const char* v = std::getenv("FROND_ZODIAC_CROSS_END");
    return !(v && *v && !std::strcmp(v, "0"));
}();
bool spanCrosses = false, crossBlocked = false;
// Fast blocks: a lagged ring submit keeps the run's lean CS5 checks (jitChargedWrite). FROND_ZODIAC_KEEP_CHECKS=0
// turns that off, for measuring.
bool keepLeanChecks = [] {
    const char* v = std::getenv("FROND_ZODIAC_KEEP_CHECKS");
    return !(v && *v && !std::strcmp(v, "0"));
}();
// Fast blocks: a ring submit inside the quiet span leaves the board behind the chip (pendingBoardCycles) and works out
// only the graphics side of the new span, capped at the rest of the old one and its boardSlack, the board's quiet time
// and the bounds (submitSpan). A span boundary on a graphics change does the same (spanBoundary). The board catches up
// at the next access, lean read, full charge, poll or delay skip, lean run or the JIT loop's end. A submit or boundary
// that changes the chip's interrupt output takes the full path. FROND_ZODIAC_SUBMIT_LAG=0 turns both off.
bool submitLag = [] {
    const char* v = std::getenv("FROND_ZODIAC_SUBMIT_LAG");
    return !(v && *v && !std::strcmp(v, "0"));
}();
uint64_t lagSubmits = 0, lagBoundaries = 0, crossEnds = 0;  // for tests and traces
// Fast blocks: a lagged ring submit where the queue is busy and no graphics model changes state between the chip's
// last charge and the submit leaves the chip behind too. The batch goes on the queue's end, which nothing reads before
// its front batch completes, and the span runs on to its old end. The cycles before the submit are charged later as
// their own piece (preCycles), so the result is the same as charging them at the submit.
// FROND_ZODIAC_DEFER_SUBMIT=0 turns it off.
bool deferSubmits = [] {
    const char* v = std::getenv("FROND_ZODIAC_DEFER_SUBMIT");
    return !(v && *v && !std::strcmp(v, "0"));
}();
uint64_t deferredSubmits = 0;  // for tests and traces
uint64_t crossSpans = 0, crossRefusals = 0;  // for tests and traces
uint64_t aspPollSkips = 0, aspPollRounds = 0, aspPollCycles = 0;  // for tests and traces
// The loop at 20096d92 (Thumb): ldr r3, [r0, #52] (FIFO_PTR); lsls r3, #28; lsrs r3, #28; cmp r3, #3; bcc back.
// It waits for the write pointer to reach 3, the calibration round's three samples. Its load is a marked access pc.
constexpr uint32_t aspPollPc = 0x20096d92;
constexpr uint16_t aspPollCode[] = {0x6b43, 0x071b, 0x0f1b, 0x2b03, 0xd3fa};
// The card driver's two STATUS waits (Thumb, in the function at 204c5df6), r4 the MMCSD base. Each round calls the
// HAL globals getter (bl 204cfe98: bx pc to an ARM b 204cf878, which jumps through [r9 - 4] to the glue at 200cd354,
// which goes on to the Thumb getter at 200884bc) and reads the system tick count at [[getter + 16] + 28], which only
// the tick interrupt moves, then STATUS. The command wait (204c5e2c) gives up once more than 100 ticks have passed
// since r5 (r7 is 0x80000000 - r5, for a count that wrapped) and loops while STATUS has none of r6's bits. The data
// wait (204c5ea0), with r7 0 while the clock runs, also calls the HAL timer read (bl 204cfec0, the same way through
// [r9 - 4] + 948 to 20084982, a read of timer 1's count it doesn't use), reads STATUS for CLK_RUN, then the ticks,
// and once more than 500 have passed since r5 STATUS again, and loops while STATUS has none of DATA_TRANS_DONE,
// WRITE_OP_DONE and the CRC and time-out bits (0x80d). Their last loads (204c5e40, 204c5f0c) are marked access pcs.
constexpr uint32_t cardWaitPc = 0x204c5e2c, cardWaitLoadPc = 0x204c5e40;
constexpr uint16_t cardWaitCode[] = {0xf00a, 0xf834, 0x6900, 0x69c0, 0x1b41, 0x42a8, 0xd200, 0x1839,
                                     0x2964, 0xd80c, 0x6860, 0x4030, 0xd0f2};
// The span a halt at either load leaves at least, for the skip to pass a few rounds after keeping its last ones.
constexpr uint64_t cardHaltSpan = 1024;
constexpr uint32_t cardDataPc = 0x204c5ea0, cardDataLoadPc = 0x204c5f0c;
constexpr uint16_t cardDataCode[] = {
    0xf00a, 0xf80e, 0x2f00, 0xd011, 0x6861, 0x05c9, 0xd501, 0x2700, 0xe013, 0x1b81, 0x42b0, 0xd201, 0x1c71, 0x1a41,
    0x20ff, 0x30f5, 0x4281, 0xd90a, 0x2006, 0xf7ff, 0xff7d, 0xe006, 0x6860, 0x05c0, 0xd403, 0x2701, 0xf009, 0xfff4,
    0x1c06, 0xf009, 0xffdd, 0x6900, 0x69c0, 0x1b41, 0x42a8, 0xd203, 0x2101, 0x07c9, 0x1b49, 0x1809, 0x20ff, 0x30f5,
    0x4281, 0xd909, 0x6860, 0x05c0, 0xd406, 0xf009, 0xffcb, 0x6900, 0x69c5, 0x2006, 0xf7ff, 0xff5c, 0x6860, 0x49ab,
    0x4008, 0xd0c5};
// The slot 2 (W4200 SD block) driver's loads that Fast blocks answer in generated code (leanSlot2Read), all Thumb: the
// data port read in its 128-word block copy (204d3730), the status2 load of its block wait (204d23b8) and the status1
// load of its transfer wait (204d23fe), and the chip identity read each round of both waits makes.
constexpr uint32_t slot2DataLoadPc = 0x204d3736, slot2Status2LoadPc = 0x204d23c2, slot2Status1LoadPc = 0x204d2408;
// Slot 2's driver's status waits (Thumb). The status 2 wait (23c2-23ee) reads status 2 (r1 + 28) into [sp], reads the
// chip identity ([r4]) and loops while bits 15, 6 and 8 are all clear; a run halted at its load goes on at 23d8 or
// at the loop's head. The status 1 wait (2408-2416) reads status 1 (r2 + 24) into r4 and [sp], reads the identity
// ([r1]) and loops while bit 2 is clear. Neither has a time-out.
constexpr uint32_t slot2Status2Round = 0x204d23d8;
constexpr uint16_t slot2Status2Code[] = {0x8b8a, 0xab00, 0x801a, 0x6822, 0x881a, 0x0bd2, 0x2a01, 0xd102,
                                         0x4867, 0x3807, 0xe00b, 0xab00, 0x881a, 0x0652, 0xd003, 0x4668,
                                         0xf7ff, 0xffa1, 0xe003, 0xab00, 0x881a, 0x05d2, 0xd5e8};
// A round from 23d8 back to it, as indices into slot2Status2Code.
constexpr uint8_t slot2Status2Run[] = {11, 12, 13, 14, 19, 20, 21, 22, 0, 1, 2, 3, 4, 5, 6, 7};
constexpr uint16_t slot2Status1Code[] = {0x8b14, 0xab00, 0x801c, 0x680b, 0xab00, 0x881b, 0x075b, 0xd5f7};
constexpr uint32_t slot2IdentityPc = 0x204d23c8, slot2Identity1Pc = 0x204d240e;
constexpr uint32_t cardDataMaskAt = 0x204c61bc;  // the 0x80d the data wait's ldr r1, [pc, #684] loads
// The data wait's halfwords (indexes into cardDataCode) a round runs while the clock runs (r7 0, CLK_RUN set): the
// timer call, the CLK_RUN check, the getter call and tick read, then the wrap's four when the ticks are below r5,
// then the 500 compare, then the second STATUS read's three when more than 500 have passed, then the last load.
constexpr uint8_t cardDataRound[] = {0, 1, 2, 3, 22, 23, 24, 29, 30, 31, 32, 33, 34, 35};
constexpr uint8_t cardDataWrap[] = {36, 37, 38, 39};
constexpr uint8_t cardDataCompare[] = {40, 41, 42, 43};
constexpr uint8_t cardDataLate[] = {44, 45, 46};
constexpr uint8_t cardDataEnd[] = {54, 55, 56, 57};
constexpr uint16_t cardVeneerCode[] = {0x4778, 0x46c0};  // 204cfe98 and 204cfec0: bx pc
constexpr uint32_t cardGetterVeneer = 0xeafffe75, cardTimerVeneer = 0xeafffeab;  // 204cfe9c, 204cfec4: b
constexpr uint32_t cardGetterJump[] = {0xe519c004, 0xe59cf000};  // 204cf878: ldr r12, [r9, #-4]; ldr pc, [r12]
constexpr uint32_t cardTimerJump[] = {0xe519c004, 0xe59cf3b4};   // 204cf978: ldr r12, [r9, #-4]; ldr pc, [r12, #948]
constexpr uint32_t cardGetterGlueAt = 0x200cd354, cardTimerGlueAt = 0x200cdf24;
constexpr uint32_t cardGetterGlue[] = {0xe59fc004, 0xe08cc00f, 0xe12fff1c, 0xfffbb15d};  // r12 = 200884bd; bx r12
constexpr uint32_t cardTimerGlue[] = {0xe59fc004, 0xe08cc00f, 0xe12fff1c, 0xfffb6a53};   // r12 = 20084983; bx r12
constexpr uint32_t cardGetterAt = 0x200884bc;  // r1 = [[r9]]; r0 = r1 + 0x4a28; bx lr (and its literal)
constexpr uint16_t cardGetterCode[] = {0x4649, 0x6809, 0x6809, 0x4801, 0x1840, 0x4770, 0x4a28, 0x0000};
constexpr uint32_t cardTimerAt = 0x20084982;  // r0 = [0x80002000 + 16]; bx lr (and its literal)
constexpr uint16_t cardTimerCode[] = {0x4801, 0x6900, 0x4770, 0x2000, 0x8000};
// Fast blocks: a vertical wait read (200adb08, 200adb14) that saw the value its loop waits on ends the JIT run, so
// the next run starts on the load and skipScanPoll can pass the rounds (scanHalted). scanHaltOff stops that until
// the next step once a skip after such a halt passed nothing.
bool scanHalted = false, scanHaltOff = false;
uint64_t scanHalts = 0;  // for tests
// The shortest timer count, in CPU cycles, for which skipFifoWait passes FIFO wait rounds. A round's work in the
// JIT, its 50-odd instructions and the pointer read with its CS5 wait clocks, ends about 55 cycles into a count in
// Origo (200 cycles each). The skip also wants the current round to have read its count in the count's first half,
// which measures that work, wait clocks included.
constexpr uint64_t fifoRoundMost = 128;
// Spans shorter than this are left to the step.
constexpr uint64_t leanMinimumSpan = 8;
// The last timer count worked out, kept for the cycles that read the same while the board doesn't change: a
// polling loop reads the count many times between two of its ticks.
struct TimerCountMemo {
    const Imx1Bootstrap* board;
    uint64_t changes, hclk, from, until;
    uint32_t address, mode, value;
};
TimerCountMemo timerCountMemo{};
// Imx1Bootstrap::timerCountAfter, through timerCountMemo.
bool timerCountAfter(const Imx1Bootstrap& board, uint32_t address, uint32_t size, uint64_t cycles, uint32_t mode,
                     uint32_t& value, uint64_t hclk = 0) {
    auto& m = timerCountMemo;
    if (m.board == &board && m.changes == board.changeCount() && m.address == address && size == 4 && m.mode == mode &&
        m.hclk == hclk && cycles >= m.from && cycles <= m.until) {
        value = m.value;
        return true;
    }
    uint64_t until;
    if (!board.timerCountAfter(address, size, cycles, mode, value, hclk, &until)) return false;
    m = {&board, board.changeCount(), hclk, cycles, until, address, mode, value};
    return true;
}

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
    // Guest calls: while armed, runs go step by step and end before the instruction at any of pcs. leave lets the
    // first instruction of a run that starts on one go; hit says the last run ended on one. Host side, not saved.
    struct PcStop {
        bool armed = false, leave = false, hit = false;
        unsigned count = 0;
        uint32_t pcs[imx1MachineMaximumPcStops]{};
        bool at(uint32_t pc) const {
            for (unsigned i = 0; i < count; ++i) if (pcs[i] == pc) return true;
            return false;
        }
    } pcStop;
    // Code hooks: runs end before the instruction at any of pcs, in Exact and Fast alike (the lean path and the JIT
    // leave them to the step), with Imx1MachineYield::codeHook. leave and hit as for pcStop. Host side, not saved.
    struct CodeHooks {
        bool leave = false, hit = false;
        unsigned count = 0;
        uint32_t pcs[imx1MachineMaximumCodeHooks]{};
        bool at(uint32_t pc) const {
            for (unsigned i = 0; i < count; ++i) if (pcs[i] == pc) return true;
            return false;
        }
    } codeHooks;
    // quietWriteClocks' last count, with what it depends on. Host side, not saved.
    struct QuietWriteMemo {
        Imx1Cs5Mode mode;
        Imx1PllRatio bus;
        uint64_t numerator, denominator;
        uint32_t clocks;
        bool ok, known;
    };
    mutable QuietWriteMemo quietWriteMemo{};
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
    // The USB host has work due (imx1MachineSetUsbHostPending). Host side, not saved.
    bool usbHostPending = false;
    bool blPrefix = false;
    uint32_t blNextPc = 0;
    struct InterruptedPrefix { uint32_t pc, cpsr, mode, lr, sp; };
    std::array<InterruptedPrefix, 256> interruptedPrefixes{};
    size_t interruptedPrefixCount = 0;
    bool prefixCapacityExceeded = false;
    bool terminal = false, stopped = false;
    bool failed = false;
    ArmCpu* cpu = nullptr;
    const Arm920DataGuard* dataGuard = nullptr;  // the CPU's data cache guard (cpuArm920DataGuardAt)
    ArmMem* mem = nullptr;
    Imx1ProbeOptions options{};
    Imx1Bootstrap peripherals;
    // The id of the card in the slot, saved with the card section.
    std::string mmcsdCardId;
    Imx1Cs4Capture cs4Capture;
    Imx1Cs5Reset cs5Reset;
    Imx1Cs5Configuration cs5Configuration;
    // The fill worker thread (Fast only, FROND_ZODIAC_GFX_THREAD). Declared before graphicsMemory, which lets go of
    // it when it goes. Not saved.
    std::unique_ptr<Imx1GraphicsWorker> graphicsWorker;
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
    // graphicsGpu, graphicsDma, graphicsUndo and the lean read rates, left out of the saved layout's size, and
    // mmcsdCardId, saved in a section of its own, slot 2's card part, saved in its SD2 section, the pc stop, the code
    // hooks, the quietWriteClocks memo and the data guard pointer.
    static constexpr size_t unsavedBytes = sizeof(GraphicsGpu) + sizeof(GraphicsDma) + sizeof(std::string) +
                                           sizeof(Imx1GraphicsCommands::Undo) + 3 * sizeof(Imx1GraphicsRate) +
                                           Imx1GraphicsSd::unsavedBytes + sizeof(PcStop) + sizeof(CodeHooks) +
                                           sizeof(QuietWriteMemo) + Imx1GraphicsCommands::unsavedBytes +
                                           sizeof(std::unique_ptr<Imx1GraphicsWorker>) + sizeof(const Arm920DataGuard*);
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
        // The GPU's residency and the fill worker don't go together.
        graphicsWorker.reset();
        graphicsGpu.gpu->attach(graphicsMemory.get());
    }
    // Attaches the fill worker to graphics memory in Fast without a host GPU, or stops it. Its destructor drains
    // first, so leaving Fast or switching it off leaves every fill done and no thread behind.
    void attachGraphicsWorker() {
        const bool on = fastEngine && graphicsMemory && !graphicsMemory->gpuResidency() &&
                        Imx1GraphicsCommands::directFills && Imx1GraphicsWorker::enabledByEnvironment();
        if (!on) {
            graphicsWorker.reset();
            return;
        }
        if (!graphicsWorker) {
            // A host that can't start the thread keeps the fills here.
            try {
                graphicsWorker = std::make_unique<Imx1GraphicsWorker>();
            } catch (const std::exception&) {
                return;
            }
        }
        graphicsWorker->attach(graphicsMemory.get());
    }
    // What the command decoder said about the batch it last refused. For the stop text, not saved.
    Imx1GraphicsCommands::Refusal graphicsRefusal;
    // What a graphics submission keeps to undo its batch (Imx1GraphicsCommands::keep). Not saved.
    Imx1GraphicsCommands::Undo graphicsUndo;
    uint32_t graphicsSampledPin = 32, graphicsSampleOffset = 0;
    uint64_t i2cIssueStep = 0;
    uint32_t i2cIssuePc = 0;
    bool irqRaised = false, fiqRaised = false;
    // The Fast engine: inside a quiet span the lean interpreter runs instead of the guarded step. Not saved.
    // fastJit: Dynarmic blocks run there first (cpuArm920RunJit), falling back to the lean path and the step.
    // fastBlocks is whether Fast may use them (on by default, off for lean-only runs).
    bool fastEngine = false, fastJit = false, fastBlocks = fastBlocksDefault;
    // Data and prefetch aborts and undefined instructions go to the guest's vectors, as on the ARM920T. Off, the
    // machine stops on them (cpu-exception, unsupported-instruction). Off for the probe and its tests; SocIMX1
    // turns it on unless FROND_ZODIAC_STOP_ON_EXCEPTION is set (imx1MachineSetGuestExceptions). Not saved.
    bool guestExceptions = false;
    Imx1BootstrapAccess driveGraphicsLine(const Imx1Cs5Configuration& candidate, bool commit) {
        if (!options.graphicsIrqA13) return Imx1BootstrapAccess::ok;
        const auto line = candidate.interruptState().interruptOutput();
        // The explicit virtual pull-up resolves a released INTb to high.
        const uint32_t value = line == Imx1GpioLine::clear ? 0x2000 : 0;
        const uint32_t known = line == Imx1GpioLine::unknown ? 0 : 0x2000;
        // A board left behind by a lean submit catches up first, so the pin changes at the board's own time.
        if (commit && (pendingBoardCycles || pendingBoardHclk) &&
            (!known || !peripherals.gpioDriveSettled(0, value, known)))
            flushSubmitBoard();
        return peripherals.driveGpioInput(0, value, known, commit);
    }
    // What follows a committed graphics state.
    Imx1BootstrapAccess graphicsCommitted() {
        // Slot 2's card work runs once, on the configuration that stays.
        cs5Configuration.runSdCardWork();
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
    // withGraphics false leaves the graphics models out, for spanBoundary.
    // cross: Fast's span may run past queued batches' completions (crossCompletions); sets spanCrosses for the result.
    // boardSide, where given, gets the cycles the board and the bounds alone stay quiet for (at least the result).
    uint64_t quietCpuCycles(uint32_t mode, bool withGraphics = true, bool cross = false,
                            uint64_t* boardSide = nullptr) const {
        if (cross) spanCrosses = false;
        if (boardSide) *boardSide = 0;
        if (options.idleStepped) return 0;
        const auto cpuPeriod = peripherals.cpuReferencePeriod(mode);
        uint64_t front = UINT64_MAX;
        cross = cross && withGraphics && crossAllowed();
        uint64_t cycles = !withGraphics ? UINT64_MAX : cross ? cs5Configuration.quietCpuCyclesThrough(cpuPeriod, front, crossEnd) :
                                                                cs5Configuration.quietCpuCycles(cpuPeriod);
        if (!cycles) return 0;
        if (cross && cycles > front) {
            const uint64_t through = quietCpuCycles(mode, false);
            const uint64_t bounded = std::min(cycles, through);
            if (boardSide) *boardSide = through;
            if (bounded > front) {
                spanCrosses = true;
                ++crossSpans;
                return bounded;
            }
            return std::min(front, through);
        }
        // The board counts its quiet time in HCLK periods, the last of which is the one with the change.
        // One more is left out for the part of a System PLL clock an asynchronous charge can carry over.
        const uint64_t hclk = std::min<uint64_t>(peripherals.idleQuietHclk(), uint64_t(1) << 30);
        if (hclk < 3) return 0;
        const auto busPeriod = peripherals.busReferencePeriod();
        // Synchronous CPU cycles use the HCLK period. With separate clocks, keep the same
        // rounded-down bound and use wider products only when needed.
        __uint128_t board = hclk - 2;
        if (busPeriod.numerator != cpuPeriod.numerator || busPeriod.denominator != cpuPeriod.denominator) {
            uint64_t busUnits, scaled, divisor;
            if (!__builtin_mul_overflow(hclk - 2, busPeriod.numerator, &busUnits) &&
                !__builtin_mul_overflow(busUnits, cpuPeriod.denominator, &scaled) &&
                !__builtin_mul_overflow(busPeriod.denominator, cpuPeriod.numerator, &divisor))
                board = scaled / divisor;
            else
                board = __uint128_t(hclk - 2) * busPeriod.numerator * cpuPeriod.denominator /
                        (__uint128_t(busPeriod.denominator) * cpuPeriod.numerator);
        }
        // One charge takes 32 bits of cycles.
        cycles = std::min<uint64_t>(cycles, uint64_t(1) << 24);
        uint64_t side = std::min<uint64_t>(uint64_t(1) << 24, board < UINT64_MAX ? uint64_t(board) : UINT64_MAX);
        if (board < cycles) cycles = uint64_t(board);
        if (bounds && cycles) {
            const auto& now = peripherals.referenceTime();
            if (now.compare(bounds->deadline) >= 0) return 0;
            // Cycles until the deadline, rounded up; the one that reaches it is charged alone.
            const auto left = bounds->deadline.intervalSince(now);
            __uint128_t value, divisor, steps;
            if (__builtin_mul_overflow(left.ticks, left.denominator, &value) ||
                __builtin_add_overflow(value, left.numerator, &value) ||
                __builtin_mul_overflow(value, cpuPeriod.denominator, &value) ||
                __builtin_mul_overflow(left.denominator, cpuPeriod.numerator, &divisor)) {
                steps = __uint128_t(left.ticks) * cpuPeriod.denominator / cpuPeriod.numerator;
            } else if (value <= UINT64_MAX && divisor <= UINT64_MAX) {
                const uint64_t scaled = uint64_t(value), period = uint64_t(divisor);
                steps = scaled / period + (scaled % period != 0);
            } else {
                steps = value / divisor + (value % divisor != 0);
            }
            if (!steps) return 0;
            if (steps - 1 < cycles) cycles = uint64_t(steps - 1);
            if (steps - 1 < side) side = uint64_t(steps - 1);
        }
        if (cycles && !peripherals.referenceChargeSupported(cycles * cpuPeriod.numerator, cpuPeriod.denominator)) return 0;
        if (boardSide) {
            if (side > cycles && !peripherals.referenceChargeSupported(side * cpuPeriod.numerator, cpuPeriod.denominator))
                side = cycles;
            *boardSide = cycles ? side : 0;
        }
        return cycles;
    }
    // The span after a lagged submit: the graphics side only, at most cap, the rest of the span the submit was in.
    uint64_t submitSpan(uint64_t cap) const {
        spanCrosses = false;
        const auto period = peripherals.cpuReferencePeriod(quietMode);
        uint64_t front = UINT64_MAX;
        const bool cross = crossAllowed();
        const uint64_t cycles = cross ? cs5Configuration.quietCpuCyclesThrough(period, front, crossEnd) :
                                        cs5Configuration.quietCpuCycles(period);
        if (cross && cycles > front && std::min(cycles, cap) > front) {
            spanCrosses = true;
            ++crossSpans;
            return std::min(cycles, cap);
        }
        return std::min({cycles, front, cap});
    }
    bool crossAllowed() const {
        if (!crossCompletions || crossBlocked || !fastEngine || !fastJit || !graphicsMemory ||
            graphicsMemory->gpuResidency() || options.cs5ConfigurationObserver || options.busObserver ||
            !cs5Configuration.virtualProfile())
            return false;
        // A player's batches carry overlay words for the display; those complete one at a time.
        return cs5Configuration.engineState().otherSubmission() != Imx1GraphicsEngine::player;
    }
    // Refuses a lean access while the span runs past completions, and stops crossing until the next submit.
    bool crossRefused() const {
        if (!spanCrosses) return false;
        crossBlocked = true;
        ++crossRefusals;
        return true;
    }
    // The chip's charge of the cycles and wait clocks before deferred submits (preCycles), with the board kept back,
    // as the first submit's charge would have done it. A refusal is kept in graphicsWireStatus.
    bool chargePre() {
        if (!preCycles && !preHclk) return true;
        const uint32_t cycles = uint32_t(std::exchange(preCycles, 0)), hclk = uint32_t(std::exchange(preHclk, 0));
        deferredCycles -= cycles;
        deferredHclk -= hclk;
        const auto status = chargeCycles(cycles, hclk, true, peripherals.cpuReferencePeriod(quietMode));
        if (status == Imx1BootstrapAccess::ok) return true;
        if (graphicsWireStatus == Imx1BootstrapAccess::ok) graphicsWireStatus = status;
        return false;
    }
    // Charges the waiting cycles. Anything that reads or changes the board, the graphics chip or the
    // time calls this first. A refusal is kept in graphicsWireStatus for the next clock boundary.
    void flushSubmitBoard() {
        chargePre();
        if (!pendingBoardCycles && !pendingBoardHclk) return;
        peripherals.advanceCpuThenHclk(std::exchange(pendingBoardCycles, 0), quietMode,
                                       options.assumeMpllSettled, std::exchange(pendingBoardHclk, 0));
        const auto status = peripherals.clockStatus();
        if (status != Imx1BootstrapAccess::ok && graphicsWireStatus == Imx1BootstrapAccess::ok)
            graphicsWireStatus = status;
    }
    void chargeBoardAfterGraphics(uint32_t cycles, uint32_t hclk, bool keepBoard) {
        if (keepBoard) {
            pendingBoardCycles += cycles;
            pendingBoardHclk += hclk;
        } else {
            peripherals.advanceCpuThenHclk(cycles, quietMode, options.assumeMpllSettled, hclk);
        }
    }
    bool chargeDeferred(bool keepBoard = false) {
        const bool preCharged = chargePre();
        if (!keepBoard) {
            flushSubmitBoard();
            if (graphicsWireStatus != Imx1BootstrapAccess::ok) return false;
        }
        quietCycles = boardSlack = 0;
        const bool crossed = std::exchange(spanCrosses, false);
        linesPending = false;
        if (!deferredCycles && !deferredHclk) return preCharged;
        uint32_t cycles = uint32_t(deferredCycles);
        const uint32_t hclk = uint32_t(deferredHclk);
        deferredCycles = deferredHclk = 0;
        const auto cpuPeriod = peripherals.cpuReferencePeriod(quietMode);
        Imx1BootstrapAccess status = Imx1BootstrapAccess::ok;
        if (crossed && crossEnd) {
            // A span past the last queued batch's completion (crossEnd): the cycles up to it go first, with the
            // completions, and the busy level is taken at its time, as a span boundary there would.
            const uint64_t last = cs5Configuration.cpuStepsThroughLast(cpuPeriod);
            if (last && last < cycles) {
                status = chargeCycles(uint32_t(last), 0, keepBoard, cpuPeriod);
                if (status == Imx1BootstrapAccess::ok) {
                    cs5Configuration.takeBusyLevel();
                    cycles -= uint32_t(last);
                    ++crossEnds;
                }
            }
        }
        if (status == Imx1BootstrapAccess::ok) status = chargeCycles(cycles, hclk, keepBoard, cpuPeriod);
        if (status == Imx1BootstrapAccess::ok) return preCharged;
        if (graphicsWireStatus == Imx1BootstrapAccess::ok) graphicsWireStatus = status;
        return false;
    }
    // chargeDeferred's charge of cycles and then hclk wait clocks: the chip, then the board (kept back with keepBoard).
    Imx1BootstrapAccess chargeCycles(uint32_t cycles, uint32_t hclk, bool keepBoard, Imx1PllRatio cpuPeriod) {
        Imx1BootstrapAccess status;
        if (!hclk) {
            status = advanceGraphicsCpu(cycles, cpuPeriod);
            if (status == Imx1BootstrapAccess::ok) {
                if (graphicsMemory) graphicsFrame.advance(cs5Configuration, *graphicsMemory, peripherals.eimState());
                chargeBoardAfterGraphics(cycles, 0, keepBoard);
                if (!keepBoard) status = peripherals.clockStatus();
            }
        } else {
            // Wait clocks come only from Fast's lean path and JIT, inside a quiet span: it takes the cycles and then
            // the wait clocks (as cs5Wait charges them, in pieces of at most one transaction) in one advance, and
            // then the board takes both. The JIT can pass the span by up to a block, so a batch that completes in
            // the cycles part has its completion work after the wait clocks.
            const auto bus = peripherals.busReferencePeriod();
            if (!peripherals.referenceChargeSupported(uint64_t(cycles) * cpuPeriod.numerator, cpuPeriod.denominator) ||
                !cs5Configuration.referencePeriodSupported(cpuPeriod))
                status = Imx1BootstrapAccess::sideEffect;
            else
                status = advanceGraphics([&](Imx1Cs5Configuration& c) {
                    c.advanceCpuCycles(cycles, cpuPeriod);
                    for (uint32_t left = hclk; left;) {
                        const uint32_t chunk = std::min(left, Imx1Cs5Bus::timeoutClocks);
                        c.advanceBusCycles(chunk, bus, cpuPeriod);
                        left -= chunk;
                    }
                });
            if (status == Imx1BootstrapAccess::ok) {
                if (graphicsMemory) graphicsFrame.advance(cs5Configuration, *graphicsMemory, peripherals.eimState());
                chargeBoardAfterGraphics(cycles, hclk, keepBoard);
                if (!keepBoard) status = peripherals.clockStatus();
            }
        }
        return status;
    }
    // Fast: the end of a quiet span in the JIT loop, taken without a step. The step there would charge the span, look
    // at the clock boundary and the interrupt lines and start a new span, which this does without an instruction.
    // Where the span ended on a graphics change due in the next CPU cycle (a batch's completion, mostly), that one
    // cycle passes with no instruction, as a one cycle stall, which the step's two instructions cost more than on the
    // host. 1 when a new span can run, 0 for the step to go on (with every cycle charged), -1 when the slice ends.
    // A span a step started after an access (linesPending) only needs the lines, as the step would look at them.
    int spanBoundary() {
        const uint32_t mode = cpuArm920ClockMode(cpu);
        if (mode != quietMode || cpuArm920Waiting(cpu) || !peripherals.mcuEnabled() || peripherals.mcuWakePending() ||
            peripherals.systemStopped() || graphicsWireStatus != Imx1BootstrapAccess::ok ||
            peripherals.systemPllBusStallPending() || boundReached() ||
            (codeHooks.count && codeHooks.at(cpuGetRegisters(cpu)[15] & ~1u)))
            return 0;
        if (linesPending) {
            flushSubmitBoard();
            if (clockBoundary()) return -1;
            updateInterrupts();
            linesPending = false;
            if (quietCycles >= leanMinimumSpan) return 1;
        }
        const auto period = peripherals.cpuReferencePeriod(mode);
        // The board stays quiet for quietCycles + boardSlack more cycles: within them a span that ends on a graphics
        // change charges only the chip, leaves the board behind (pendingBoardCycles) and works out the graphics side
        // of the next span, as a lagged submit does. Anything the board sees catches it up first (flushSubmitBoard).
        // A change of the chip's interrupt output and a short span take the full path.
        // The deferred cycles were taken off quietCycles as the runs added them, so only the stall cycle is new.
        const uint64_t boardLeft = quietCycles + boardSlack;
        if (submitLag && boardLeft > leanMinimumSpan &&
            uint64_t(pendingBoardCycles) + deferredCycles < UINT32_MAX &&
            uint64_t(pendingBoardHclk) + deferredHclk <= UINT32_MAX) {
            // The stall cycle as below; the board is quiet past it.
            uint64_t stall = 0;
            if (!quietCycles && !deferredHclk && cs5Configuration.quietCpuCycles(period) == deferredCycles &&
                (deferredCycles || cs5Configuration.changeDue(period))) {
                ++deferredCycles;
                stall = 1;
            }
            const auto lineBefore = cs5Configuration.interruptState().interruptOutput();
            if (!chargeDeferred(true)) { flushSubmitBoard(); clockBoundary(); return -1; }
            cs5Configuration.takeBusyLevel();
            if (graphicsWireStatus == Imx1BootstrapAccess::ok &&
                cs5Configuration.interruptState().interruptOutput() == lineBefore) {
                const uint64_t cap = boardLeft - stall;
                const uint64_t span = submitSpan(cap);
                if (span >= leanMinimumSpan) {
                    quietCycles = span;
                    boardSlack = cap - span;
                    ++lagBoundaries;
                    return 1;
                }
            }
            flushSubmitBoard();
        } else {
            flushSubmitBoard();
            // The span ran out just before a graphics change, with no wait clocks to charge after the cycles: the
            // stall cycle goes in the same charge, as nothing else changes in it.
            if (!quietCycles && !deferredHclk && cs5Configuration.quietCpuCycles(period) == deferredCycles &&
                (deferredCycles || cs5Configuration.changeDue(period)) && quietCpuCycles(mode, false) > deferredCycles)
                ++deferredCycles;
            if (!chargeDeferred()) { clockBoundary(); return -1; }
            // A batch the charge completed leaves its busy level for the next advance, which takes it first: it is
            // taken now, as jitChargedWrite takes a new batch's, so the span can run on.
            cs5Configuration.takeBusyLevel();
        }
        if (clockBoundary()) return -1;
        updateInterrupts();
        uint64_t side = 0;
        uint64_t span = quietCpuCycles(mode, true, true, &side);
        if (span < leanMinimumSpan && !cs5Configuration.quietCpuCycles(period) && cs5Configuration.changeDue(period) &&
            quietCpuCycles(mode, false)) {
            // The graphics change is due in the next cycle and nothing else is: it passes as the step would charge it.
            const auto status = advanceGraphicsCpu(1, period);
            if (status != Imx1BootstrapAccess::ok) {
                result.stop = Imx1ProbeStop::unsupportedPeripheral;
                result.peripheralStatus = status;
                return -1;
            }
            if (graphicsMemory) graphicsFrame.advance(cs5Configuration, *graphicsMemory, peripherals.eimState());
            peripherals.advanceCpuCycles(1, mode, options.assumeMpllSettled);
            if (clockBoundary()) return -1;
            updateInterrupts();
            span = quietCpuCycles(mode, true, true, &side);
        }
        quietCycles = span;
        boardSlack = side > span ? side - span : 0;
        return span >= leanMinimumSpan ? 1 : 0;
    }
    bool idleWakePossible() const {
        if (usbHostPending || peripherals.idleWakePossible()) return true;
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
    // ROM, SRAM and SDRAM in normal mode read with no time and no side effect, for the TwGlue log. Stays within
    // one 4 KiB page.
    static bool peekPhysical(void* context, uint32_t pa, void* data, uint32_t size) {
        auto& p = *static_cast<Probe*>(context);
        if (const auto* rom = p.resolve(pa, size)) {
            std::memcpy(data, rom, size);
            return true;
        }
        if ((pa & 0xfff) + size > 0x1000 || !p.options.bootstrapPeripherals) return false;
        const uint8_t* page = p.peripherals.plainPage(pa & ~0xfffu, false);
        if (!page) return false;
        std::memcpy(data, page + (pa & 0xfff), size);
        return true;
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
        if (pendingBoardCycles || pendingBoardHclk) p.flushSubmitBoard();
        *cost = 0;
        if (inJit && (pa & ~1u) == 0x15000000) return single && size == 1 && p.jitYmfAccess(pa, false, *value, ran, pc);
        if (p.graphicsMemory && Imx1GraphicsMemory::owns(pa))
            return single && p.leanGraphicsRead(pa, size, ran, left, *value, *cost);
        if (slot2LeanOn && (Imx1GraphicsSd::owns(pa) || pa == 0x16010000) && pc >= 0x204d2140 && pc < 0x204d3b20)
        {
            if (!single) return false;
            if (p.leanSlot2Read(pa, size, pc, ran, left, *value, *cost)) {
                // A status wait's load that sees the wait go on ends the run, for skipSlot2Poll.
                if (inJit && slot2PollSkip && !slot2HaltOff && !jitHalt &&
                    ((pc == slot2Status2LoadPc && !(*value & 0x8140)) || (pc == slot2Status1LoadPc && !(*value & 4))) &&
                    quietCycles > uint64_t(ran) + cardHaltSpan) {
                    jitHalt = slot2Halted = true;
                    cpuSetSlowPath(p.cpu, SLOW_PATH_REASON_RESCHEDULE);
                }
                return true;
            }
            return inJit && p.jitSlot2Access(pa, size, false, *value, ran, pc);
        }
        if (size != 4) return false;
        if (Imx1Bootstrap::quietReadAddress(pa)) {
            if (!single || (!(pa == 0x00214004 && p.leanStatusRead(ran, *value)) && !p.leanBoardRead(pa, ran, *value)))
                return false;
            // The ASP FIFO wait's load seeing the loop go on ends the run, for skipAspPoll.
            if (inJit && pa == 0x00215034 && (*value & 15) < 3 && aspPollSkip && !aspHaltOff && !jitHalt &&
                p.atAspPoll(pc)) {
                jitHalt = aspHalted = true;
                cpuSetSlowPath(p.cpu, SLOW_PATH_REASON_RESCHEDULE);
            }
            // So does a card driver STATUS wait's last load that sees its loop go on, with room left in the span
            // for skipCardPoll to pass rounds.
            if (inJit && pa == 0x00214004 && cardPollSkip && !cardHaltOff && !jitHalt &&
                ((pc == cardWaitLoadPc && !(*value & 0x2022)) ||
                 (pc == cardDataLoadPc && !(*value & 0x80d) && (*value & 0x100))) &&
                quietCycles > uint64_t(ran) + cardHaltSpan) {
                jitHalt = cardHalted = true;
                cpuSetSlowPath(p.cpu, SLOW_PATH_REASON_RESCHEDULE);
            }
            return true;
        }
        if (!Imx1Bootstrap::timerCountAddress(pa))
            return single && p.leanRegisterRead(pa, pc, ran, left, *value, *cost);
        // Wait clocks still deferred (graphics accesses answered earlier) count after the CPU cycles, as
        // chargeDeferred charges them; the span has to hold their cost too.
        if (quietCycles <= uint64_t(ran) + leanCs5.waitCost || p.failed || p.graphicsWireStatus != Imx1BootstrapAccess::ok ||
            !timerCountAfter(p.peripherals, pa, size, deferredCycles + ran, quietMode, *value,
                                           deferredHclk + leanCs5.waitClocks)) return false;
        ++leanTimerReads;
        if (inJit && delayLoopPc(pc) && !delayHaltOff && !jitHalt) {
            if (delayPassed) {
                delayPassed = false;
            } else {
                jitHalt = delayHalted = true;
                cpuSetSlowPath(p.cpu, SLOW_PATH_REASON_RESCHEDULE);
            }
        }
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
        auto& last = quietWriteMemo;
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
    // CPU cycles, rounded up and one more, that hold this many wait clocks and the work they add, as leanCs5Ready
    // counts them.
    uint64_t leanClockCost(uint32_t clocks) const {
        const auto bus = peripherals.busReferencePeriod();
        const auto cpuPeriod = peripherals.cpuReferencePeriod(quietMode);
        const __uint128_t time = __uint128_t(clocks) * bus.numerator * cpuPeriod.denominator;
        const __uint128_t unit = __uint128_t(bus.denominator) * cpuPeriod.numerator;
        const uint64_t cycles = uint64_t(time / unit + (time % unit != 0)) + 1;
        return std::max<uint64_t>(cycles, clocks);
    }
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
    // The graphics time and pixel rates of lean CS5 register reads (Imx1GraphicsRate), kept across reads, not saved.
    Imx1GraphicsRate leanCpuRate, leanBusRate, leanScanRate;
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
        // The scan status follows the display's timing alone, which a completion doesn't change.
        if (pa != 0x1601054c && crossRefused()) return false;
        if (!leanCs5Ready() || left < leanCs5.cost) return false;
        const bool user = (cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_USR;
        if (peripherals.eimState().cs5Transfer(pa, 4, false, user) != Imx1EimAccess::ok || !leanRegisterReady())
            return false;
        uint64_t ticks = 0;
        const auto cpuPeriod = peripherals.cpuReferencePeriod(quietMode);
        const auto busPeriod = peripherals.busReferencePeriod();
        const uint64_t cycles = deferredCycles + ran, clocks = deferredHclk + leanCs5.waitClocks + leanCs5.clocks;
        if (!cs5Configuration.ticksAfterQuick(cycles, cpuPeriod, clocks, busPeriod, leanCpuRate, leanBusRate, ticks) &&
            !cs5Configuration.ticksAfter(cycles, cpuPeriod, clocks, busPeriod, ticks))
            return false;
        if (!cs5Configuration.quietRegisterRead(peripherals.eimState(), pc, pa, ticks, value, &leanScanRate)) return false;
        if (inJit && scanPollSkip && !scanHaltOff && !jitHalt && (pc == 0x200adb08 || pc == 0x200adb14) &&
            value == (pc == 0x200adb08 ? 0u : 0x400u)) {
            jitHalt = scanHalted = true;
            ++scanHalts;
            cpuSetSlowPath(cpu, SLOW_PATH_REASON_RESCHEDULE);
        }
        leanCs5.waitClocks += leanCs5.clocks;
        leanCs5.waitCost += leanCs5.cost;
        ++leanCs5Reads;
        cost = leanCs5.cost;
        return true;
    }
    // Fast: a read of slot 2's SD block from its driver, or the chip identity read the driver makes each status poll
    // round, from generated code (slot2Lean, marked access pcs). As leanRegisterRead: inside the quiet span no SD
    // event is due, so the block reads as the step would after charging, and only the wait clocks are left. A data
    // port read takes its word from the block (Imx1GraphicsSd::quietCardRead); every check comes before it.
    __attribute__((noinline)) bool leanSlot2Read(uint32_t pa, uint32_t size, uint32_t pc, uint32_t ran, uint32_t left,
                                                 uint32_t& value, uint32_t& cost) {
        if (!graphicsMemory || !cs5Configuration.virtualProfile() || options.cs5ConfigurationObserver ||
            options.busObserver || failed || quietCycles <= ran || cs5Configuration.engineState().completed() ||
            crossRefused())
            return false;
        if (!leanCs5Ready() || left < leanCs5.cost) return false;
        const bool user = (cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_USR;
        if (peripherals.eimState().cs5Transfer(pa, size, false, user) != Imx1EimAccess::ok || !leanRegisterReady() ||
            !cs5Configuration.quietSdRead(peripherals.eimState(), pc, pa, size, value))
            return false;
        leanCs5.waitClocks += leanCs5.clocks;
        leanCs5.waitCost += leanCs5.cost;
        ++leanCs5Reads;
        ++leanSlot2Reads;
        cost = leanCs5.cost;
        return true;
    }
    // Fast: slot 2's data port write from its driver in generated code, but a block's last word, as leanSlot2Read
    // takes a read.
    __attribute__((noinline)) bool leanSlot2Write(uint32_t pa, uint32_t size, uint32_t value, uint32_t pc, uint32_t ran,
                                                  uint32_t left, uint32_t& cost) {
        if (pa != 0x16202060 || !graphicsMemory || !cs5Configuration.virtualProfile() ||
            options.cs5ConfigurationObserver || options.busObserver || failed || quietCycles <= ran ||
            cs5Configuration.engineState().completed() || crossRefused())
            return false;
        if (!leanCs5Ready() || left < leanCs5.cost) return false;
        const bool user = (cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_USR;
        if (peripherals.eimState().cs5Transfer(pa, size, true, user) != Imx1EimAccess::ok || !leanRegisterReady() ||
            !cs5Configuration.quietSdWrite(peripherals.eimState(), pc, pa, size, value))
            return false;
        leanCs5.waitClocks += leanCs5.clocks;
        leanCs5.waitCost += leanCs5.cost;
        ++leanCs5Writes;
        ++leanSlot2Writes;
        cost = leanCs5.cost;
        return true;
    }
    // Fast: any other access of slot 2's driver to its block or the identity register from generated code. The run's
    // cycles are charged first, as jitChargedRead charges them, then access takes it as the step does, with pc as the
    // instruction's and its wait clocks, and the span is worked out again.
    __attribute__((noinline)) bool jitSlot2Access(uint32_t pa, uint32_t size, bool write, uint32_t& value, uint32_t ran,
                                                  uint32_t pc) {
        if (failed || graphicsWireStatus != Imx1BootstrapAccess::ok || options.busObserver ||
            options.cs5ConfigurationObserver || !pc)
            return false;
        deferredHclk += leanCs5.waitClocks;
        result.cs5WaitClocks += leanCs5.waitClocks;
        work += leanCs5.waitClocks;
        leanCs5 = {};
        deferredCycles += ran;
        cpuArm920JitCharge(cpu, ran, 0);
        if (!chargeDeferred()) return false;
        finishGraphicsBusStall();
        if (graphicsWireStatus != Imx1BootstrapAccess::ok) return false;
        auto& t = result.trace;
        const auto saved = t;
        t.pc = pc;
        t.cpsr = cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR);
        graphicsSampledPin = 32;
        graphicsSampleOffset = 0;
        uint32_t datum = value;
        const bool ok = access(this, pa, uint_fast8_t(size), write, &datum);
        t = saved;
        if (!ok) return false;
        if (!write) value = size == 2 ? datum & 0xffff : datum;
        ++jitSlot2Accesses;
        slot2HaltOff = false;
        respan();
        return true;
    }
    // A graphics memory write on the lean path, answered as access answers it: the write now, its wait clocks
    // deferred with the CPU cycles.
    static bool leanWrite(void* context, uint32_t pa, uint32_t size, uint32_t value, uint32_t ran, uint32_t left,
                          uint32_t* cost) {
        auto& p = *static_cast<Probe*>(context);
        if (inJit && jitBoardWriteAddress(pa)) return size == 4 && p.jitBoardWrite(pa, value);
        if (inJit && jitCardWriteAddress(pa, value)) return size == 4 && p.jitCardWrite(pa, value, ran);
        if (inJit && (pa & ~1u) == 0x15000000) return size == 1 && p.jitYmfAccess(pa, true, value, ran, cpuArm920TakeJitAccessPc(p.cpu));
        if (inJit && slot2LeanOn && Imx1GraphicsSd::owns(pa)) {
            const uint32_t pc = cpuArm920TakeJitAccessPc(p.cpu);
            if (pc < 0x204d2140 || pc >= 0x204d3b20) return false;
            return p.leanSlot2Write(pa, size, value, pc, ran, left, *cost) || p.jitSlot2Access(pa, size, true, value, ran, pc);
        }
        // The graphics ring pointer writes of the ROM's keyed copy and copy helper producers (Origo submits about 190000
        // a second in a mission briefing) and of the blend (200a9788, about 4000 a second in Origo's missions).
        // All are unconditional ARM stores (str r0, [r1, #0x220]); a marked pc must be, since its slot is set even when the condition fails.
        if (pa == 0x16010220 && inJit) {
            const uint32_t pc = cpuArm920TakeJitAccessPc(p.cpu);
            return size == 4 && (pc == 0x200aa504 || pc == 0x200a9ed4 || pc == 0x200a9788) && p.graphicsMemory &&
                   p.cs5Configuration.virtualProfile() && !p.options.cs5ConfigurationObserver &&
                   p.jitChargedWrite(pa, 4, value, ran, pc);
        }
        // A ring word the first write of the run checked the view for: the same checks pass, so it goes straight to
        // the bank storage.
        if (leanCs5.ringUsable && pa - leanCs5.ringLow < leanCs5.ringSpan && size == 4 && !(pa & 3)) {
            if (p.failed || quietCycles <= ran || left < leanCs5.cost) return false;
            p.graphicsMemory->writeWord(leanCs5.ringBank, leanCs5.ringOffset + ((pa - leanCs5.ringWrap) & leanCs5.ringMask),
                                        value);
            leanCs5.waitClocks += leanCs5.clocks;
            leanCs5.waitCost += leanCs5.cost;
            ++leanCs5Writes;
            *cost = leanCs5.cost;
            return true;
        }
        if (!p.graphicsMemory || !p.cs5Configuration.virtualProfile() || !Imx1GraphicsMemory::owns(pa) ||
            p.options.cs5ConfigurationObserver || p.failed || quietCycles <= ran)
            return false;
        if (!p.leanCs5Ready() || left < leanCs5.cost) return false;
        // Past a completion only ring words: the batches were decoded at their submits, and the producer writes only
        // words a read pointer read before showed free, which stay free.
        if (spanCrosses) {
            if (!leanCs5.ringChecked && size == 4 && pa >= 0x16080000 && pa < 0x160c0000) p.leanRingCheck();
            if (!(leanCs5.ringUsable && pa - leanCs5.ringLow < leanCs5.ringSpan && size == 4 && !(pa & 3)) &&
                p.crossRefused())
                return false;
        }
        const bool user = (cpuGetRegExternal(p.cpu, ARM_REG_NUM_CPSR) & 31) == ARM_SR_MODE_USR;
        uint32_t datum = value;
        if (p.peripherals.eimState().cs5Transfer(pa, size, true, user) != Imx1EimAccess::ok || !p.leanMemoryReady() ||
            !p.graphicsMemory->accessReady(p.cs5Configuration, pa, size, true, datum))
            return false;
        if (!leanCs5.ringChecked && size == 4 && pa >= 0x16080000 && pa < 0x160c0000) p.leanRingCheck();
        leanCs5.waitClocks += leanCs5.clocks;
        leanCs5.waitCost += leanCs5.cost;
        ++leanCs5Writes;
        *cost = leanCs5.cost;
        return true;
    }
    // The ring wrap view for leanWrite, after a ring word passed its checks: usable where every word of the view
    // passes them the same way, as no state they read changes until leanCs5 is cleared. The view's words all map into
    // the ring, which has to lie whole in one bank's mapped addresses, with no host GPU. The EIM check doesn't depend
    // on the address, so both modes passing it covers a mode change in the run.
    __attribute__((noinline)) void leanRingCheck() {
        leanCs5.ringChecked = true;
        const auto wrap = cs5Configuration.memoryWindows().wrap();
        const auto& engine = cs5Configuration.engineState();
        const uint32_t bytes = engine.ringBytes(), ring = engine.ringAddress();
        const uint64_t ringEnd = uint64_t(ring) + bytes;
        const auto& eim = peripherals.eimState();
        unsigned bank = 0;
        uint32_t offset = 0;
        if (!wrap.known || !bytes || (bytes & (bytes - 1)) || (ring & 3) || graphicsMemory->gpuResidency() ||
            !((ring >= 0x100000 && ringEnd <= 0x160000) || (ring >= 0x800000 && ringEnd <= 0x1000000)) ||
            !graphicsMemory->contains(cs5Configuration, eim, ring, bytes, &bank, &offset) ||
            eim.cs5Transfer(0x16080000, 4, true, false) != Imx1EimAccess::ok ||
            eim.cs5Transfer(0x16080000, 4, true, true) != Imx1EimAccess::ok)
            return;
        // Words whose four bytes lie in both the wrap range and the view's addresses.
        const uint64_t low = std::max<uint64_t>(0x16080000, 0x16000000ull + wrap.first);
        const uint64_t end = std::min<uint64_t>(0x160c0000, 0x16000000ull + wrap.last + 1);
        if (end < low + 4) return;
        leanCs5.ringUsable = true;
        leanCs5.ringBank = bank;
        leanCs5.ringLow = uint32_t(low);
        leanCs5.ringSpan = uint32_t(end - 4 - low + 1);
        leanCs5.ringWrap = 0x16000000u + wrap.first;
        leanCs5.ringMask = bytes - 1;
        leanCs5.ringOffset = offset;
    }
    // Writes Fast blocks take in generated code: GPIO IMR and ISR, AITC NIMASK, INTENNUM and INTDISNUM, timer status,
    // the watchdog service register and DMA DISR (which only clears status, so the PCM DMA pages stay as they are).
    static bool jitBoardWriteAddress(uint32_t pa) {
        const uint32_t offset = pa & 0xff;
        if ((pa & ~0x3ffu) == 0x0021c000) return offset == 0x30 || offset == 0x34;
        if ((pa & ~0xffu) == 0x00223000) return offset == 0x04 || offset == 0x08 || offset == 0x0c;
        return pa == 0x00202014 || pa == 0x00203014 || pa == 0x00201004 || pa == 0x00209004;
    }
    // A write from generated code that needs the board at its own time: the cycles before the run and the run's
    // finished blocks (ran) are charged first, then access takes it as the step does with pc as the instruction's.
    // The block the write is in is charged at the run's end, so the write lands up to one block early. The quiet
    // span is worked out again, and the run stops at its next block if the span now ends before its budget.
    // jitChargedWrite's lagged submit with the chip left behind as well (deferSubmits), where its queue is busy, no
    // graphics model changes state in the cycles and wait clocks it hasn't been charged (as quietCpuCycles counts
    // them, the clocks rounded up as leanClockCost does) and the rest of the span holds the write's clocks. The
    // batch is decoded and queued at the chip's last charge: the front batch is still running there and here, so
    // the queue, the read pointer and the busy level are the same at both times, and a ring submit changes nothing
    // else (engineSubmitWrite). 1 when taken, -1 when access refused it (as jitChargedWrite's refusal, the cycles
    // before it charged later in their own piece), 0 with nothing changed where it doesn't apply.
    int deferSubmit(const LeanCs5& checks, uint64_t remaining, uint64_t slackBefore, uint32_t pa, uint32_t size,
                     uint32_t value, uint32_t pc, Imx1GpioLine lineBefore) {
        if (failed || !graphicsMemory || graphicsMemory->gpuResidency() || peripherals.systemPllBusStallPending() ||
            cs5Configuration.engineState().idleQuietTicks() == UINT64_MAX || cs5Configuration.engineState().emptyRing() ||
            deferredHclk > UINT32_MAX)
            return 0;
        const uint64_t cost = checks.clocks ? leanClockCost(checks.clocks) : 0;
        if (remaining <= cost + leanMinimumSpan) return 0;
        const uint64_t behind = deferredCycles + (deferredHclk ? leanClockCost(uint32_t(deferredHclk)) : 0);
        if (cs5Configuration.quietCpuCycles(peripherals.cpuReferencePeriod(quietMode)) <= behind + 1) return 0;
        preCycles = deferredCycles;
        preHclk = deferredHclk;
        auto& t = result.trace;
        const auto saved = t;
        t.pc = pc;
        t.cpsr = cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR);
        graphicsSampledPin = 32;
        graphicsSampleOffset = 0;
        uint32_t datum = value;
        leanSubmit = deferSubmitAccess = true;
        const bool ok = access(this, pa, uint_fast8_t(size), true, &datum);
        leanSubmit = deferSubmitAccess = false;
        t = saved;
        // A refusal is the same at the chip's last charge as here.
        if (!ok) return -1;
        ++jitChargedWrites;
        ++deferredSubmits;
        crossBlocked = false;
        cs5Configuration.takeBusyLevel();
        if (checks.clocks) {
            deferredHclk += checks.clocks;
            result.cs5WaitClocks += checks.clocks;
            work += checks.clocks;
        }
        if (cs5Configuration.interruptState().interruptOutput() != lineBefore ||
            graphicsWireStatus != Imx1BootstrapAccess::ok) {
            // Not expected (a submit reaches only the engine): the full path, as jitChargedWrite's.
            uint64_t side = 0;
            if (!chargeDeferred()) return 1;
            flushSubmitBoard();
            updateInterrupts();
            quietCycles = boundaryStatus() == Imx1BootstrapAccess::ok ? quietCpuCycles(quietMode, true, true, &side) : 0;
            boardSlack = side > quietCycles ? side - quietCycles : 0;
        } else {
            quietCycles = remaining - cost;
            boardSlack = slackBefore;
            leanCs5 = checks;
            leanCs5.waitClocks = leanCs5.waitCost = 0;
        }
        cpuArm920JitCharge(cpu, 0, uint32_t(std::min<uint64_t>(quietCycles, UINT32_MAX)));
        if (cpuArm920JitCharged(cpu) + quietCycles + cpuArm920JitMaxUnchecked(cpu) < jitBudget) {
            jitHalt = true;
            cpuSetSlowPath(cpu, SLOW_PATH_REASON_RESCHEDULE);
        }
        return 1;
    }
    __attribute__((noinline)) bool jitChargedWrite(uint32_t pa, uint32_t size, uint32_t value, uint32_t ran, uint32_t pc) {
        if (failed || graphicsWireStatus != Imx1BootstrapAccess::ok || options.busObserver) return false;
        // A write inside the span can leave the board behind the chip (submitLag): the board stays quiet for the rest
        // of the span and its slack.
        const uint64_t used = uint64_t(ran) + leanCs5.waitCost;
        const uint64_t remaining = quietCycles > used ? quietCycles - used : 0;
        const uint64_t slackBefore = boardSlack;
        const bool keepBoard = submitLag && !options.idleStepped && remaining >= leanMinimumSpan &&
            uint64_t(pendingBoardCycles) + deferredCycles + ran <= UINT32_MAX &&
            uint64_t(pendingBoardHclk) + deferredHclk + leanCs5.waitClocks <= UINT32_MAX;
        deferredHclk += leanCs5.waitClocks;
        result.cs5WaitClocks += leanCs5.waitClocks;
        work += leanCs5.waitClocks;
        const LeanCs5 checks = leanCs5;
        leanCs5 = {};
        deferredCycles += ran;
        cpuArm920JitCharge(cpu, ran, 0);
        const auto lineBefore = cs5Configuration.interruptState().interruptOutput();
        if (deferSubmits && keepBoard && keepLeanChecks && checks.checked && checks.usable)
            if (const int deferred = deferSubmit(checks, remaining, slackBefore, pa, size, value, pc, lineBefore))
                return deferred > 0;
        if (!chargeDeferred(keepBoard)) return false;
        auto& t = result.trace;
        const auto saved = t;
        t.pc = pc;
        t.cpsr = cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR);
        graphicsSampledPin = 32;
        graphicsSampleOffset = 0;
        uint32_t datum = value;
        // Where the write's wait clocks pass with no change (as cs5Wait's quiet path would find), the write is taken
        // at once and its clocks are charged after it, as the lean path does for ring words: the batch starts that
        // many clocks earlier. The span after it has to hold their cost.
        uint32_t clocks = 0;
        // The run's lean checks, where they passed, cover these: since them only the chip has moved, within the
        // span, and with the board kept back the board hasn't.
        if (keepLeanChecks && keepBoard && checks.checked && checks.usable) {
            clocks = checks.clocks;
            leanSubmit = true;
        } else {
            leanSubmit = !peripherals.systemPllBusStallPending() && quietWriteClocks(clocks) &&
                         !graphicsFrame.scanDue(cs5Configuration, peripherals.eimState()) &&
                         acceptGraphics(cs5Configuration, false) == Imx1BootstrapAccess::ok;
        }
        const bool lean = leanSubmit;
        if (!lean && keepBoard) flushSubmitBoard();
        const bool ok = access(this, pa, uint_fast8_t(size), true, &datum);
        leanSubmit = false;
        t = saved;
        if (!ok) return false;
        ++jitChargedWrites;
        crossBlocked = false;
        // A batch start leaves its busy level for the next advance, which would end the quiet span at once; it
        // takes it now, so the run goes on to the batch's completion.
        cs5Configuration.takeBusyLevel();
        uint64_t cost = 0;
        if (lean && clocks) {
            deferredHclk += clocks;
            result.cs5WaitClocks += clocks;
            work += clocks;
            cost = leanClockCost(clocks);
        }
        uint64_t side = 0;
        if (keepBoard && lean && remaining > cost &&
            cs5Configuration.interruptState().interruptOutput() == lineBefore &&
            graphicsWireStatus == Imx1BootstrapAccess::ok) {
            // The board hasn't moved since the span's start, so the rest of boundaryStatus is its clock status as
            // checked then; the chip's period support is checked again at its next charge.
            // The board still lags. chargeDeferred cleared boardSlack; the board's quiet cycles from here are the rest
            // of the span and the slack it had.
            side = remaining + slackBefore;
            quietCycles = submitSpan(side);
            ++lagSubmits;
            // The run's lean checks still hold: the board hasn't moved, the line and the write clocks are the same,
            // and a submit changes only the engine's queue, which none of them read. The ring view, memoryReady and
            // the register checks read the ring and window registers, the EIM and the GPIO, which it doesn't write.
            // Here the write took its clocks from these checks.
            if (keepLeanChecks && checks.checked && checks.usable && !graphicsMemory->gpuResidency()) {
                leanCs5 = checks;
                leanCs5.waitClocks = leanCs5.waitCost = 0;
            }
        } else {
            flushSubmitBoard();
            updateInterrupts();
            quietCycles = boundaryStatus() == Imx1BootstrapAccess::ok ? quietCpuCycles(quietMode, true, true, &side) : 0;
        }
        boardSlack = side > quietCycles ? side - quietCycles : 0;
        if (cost) {
            if (quietCycles > cost) quietCycles -= cost;
            else {
                // A change comes within the clocks: they are charged now, after the write where cs5Wait charges them before it. The
                // old batch runs those clocks late, so this moves timing only, not the order the guest sees.
                if (chargeDeferred()) {
                    cs5Configuration.takeBusyLevel();
                    updateInterrupts();
                    quietCycles = boundaryStatus() == Imx1BootstrapAccess::ok ? quietCpuCycles(quietMode, true, true, &side) : 0;
                    boardSlack = side > quietCycles ? side - quietCycles : 0;
                }
            }
        }
        cpuArm920JitCharge(cpu, 0, uint32_t(std::min<uint64_t>(quietCycles, UINT32_MAX)));
        if (cpuArm920JitCharged(cpu) + quietCycles + cpuArm920JitMaxUnchecked(cpu) < jitBudget) {
            jitHalt = true;
            cpuSetSlowPath(cpu, SLOW_PATH_REASON_RESCHEDULE);
        }
        return true;
    }
    // A YMF768 byte access from generated code (the sound driver's index and data writes and its status reads): the
    // cycles before the run and the run's finished blocks (ran) are charged first, then access takes it as the step
    // does. The block it is in is charged at the run's end, so the access lands up to one block early. The interrupt
    // lines follow at once, and the run stops at its next block if the new quiet span ends before its budget.
    __attribute__((noinline)) bool jitYmfAccess(uint32_t pa, bool write, uint32_t& value, uint32_t ran, uint32_t pc) {
        if (failed || graphicsWireStatus != Imx1BootstrapAccess::ok || options.busObserver || options.cs4Observer ||
            !options.cs4Port0InputKnown || !peripherals.ymf768Enabled())
            return false;
        deferredHclk += leanCs5.waitClocks;
        result.cs5WaitClocks += leanCs5.waitClocks;
        work += leanCs5.waitClocks;
        leanCs5 = {};
        deferredCycles += ran;
        cpuArm920JitCharge(cpu, ran, 0);
        if (!chargeDeferred()) return false;
        uint8_t datum = uint8_t(value);
        auto& t = result.trace;
        const auto saved = t;
        t.pc = pc;
        t.cpsr = cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR);
        const bool ok = access(this, pa, 1, write, &datum);
        t = saved;
        if (!ok) return false;
        if (!write) value = datum;
        updateInterrupts();
        uint64_t side = 0;
        quietCycles = boundaryStatus() == Imx1BootstrapAccess::ok ? quietCpuCycles(quietMode, true, false, &side) : 0;
        boardSlack = side > quietCycles ? side - quietCycles : 0;
        spanCrosses = false;
        cpuArm920JitCharge(cpu, 0, uint32_t(std::min<uint64_t>(quietCycles, UINT32_MAX)));
        if (cpuArm920JitCharged(cpu) + quietCycles + cpuArm920JitMaxUnchecked(cpu) < jitBudget) {
            jitHalt = true;
            cpuSetSlowPath(cpu, SLOW_PATH_REASON_RESCHEDULE);
        }
        return true;
    }
    // The card driver's writes Fast blocks take in generated code (jitCardWrite): the MMCSD registers and DMA channels
    // 0 to 9, but a CCR write that starts a memory to memory transfer. None of these change which pages are plain (only
    // the PCM channel, 10, does), so the run's page table stays.
    static bool jitCardWriteAddress(uint32_t pa, uint32_t value) {
        if (pa >= 0x00214000 && pa <= 0x00214030) return !(pa & 3);
        if (pa < 0x00209080 || pa >= 0x00209300 || (pa & 3) || (pa & 0x3f) >= 0x1c) return false;
        return (pa & 0x3f) != 0x0c || !(value & 1) || (value & 8);
    }
    // A card driver write from generated code: the cycles before the run and the run's finished blocks (ran) are
    // charged first, then the board takes it as the step does, so it lands up to one block early. A command start
    // or a clock stop sets a new quiet span; a card DMA start moves the data already waiting. The interrupt lines
    // follow at once, and the run stops at its next block if the new span ends before its budget.
    // With the card lookahead a command register or idle DMA channel write is taken at its own time with nothing
    // charged (Imx1Bootstrap::cardRegisterLatch), as it changes no line and no span.
    __attribute__((noinline)) bool jitCardWrite(uint32_t pa, uint32_t value, uint32_t ran) {
        if (failed || graphicsWireStatus != Imx1BootstrapAccess::ok || options.busObserver) return false;
        if (quietCycles > uint64_t(ran) + leanCs5.waitCost && !pendingBoardCycles && !pendingBoardHclk &&
            peripherals.cardRegisterLatch(pa, value, deferredCycles + ran, quietMode, deferredHclk + leanCs5.waitClocks)) {
            ++result.busAccesses;
            ++jitCardWrites;
            ++jitCardLatches;
            return true;
        }
        deferredHclk += leanCs5.waitClocks;
        result.cs5WaitClocks += leanCs5.waitClocks;
        work += leanCs5.waitClocks;
        leanCs5 = {};
        deferredCycles += ran;
        cpuArm920JitCharge(cpu, ran, 0);
        if (!chargeDeferred()) return false;
        finishGraphicsBusStall();
        if (graphicsWireStatus != Imx1BootstrapAccess::ok) return false;
        uint32_t datum = value;
        if (peripherals.access(pa, 4, true, datum, cpuArm920ClockMode(cpu)) != Imx1BootstrapAccess::ok) return false;
        ++result.busAccesses;
        ++jitCardWrites;
        cardHaltOff = false;
        updateInterrupts();
        uint64_t side = 0;
        quietCycles = boundaryStatus() == Imx1BootstrapAccess::ok ? quietCpuCycles(quietMode, true, false, &side) : 0;
        boardSlack = side > quietCycles ? side - quietCycles : 0;
        spanCrosses = false;
        cpuArm920JitCharge(cpu, 0, uint32_t(std::min<uint64_t>(quietCycles, UINT32_MAX)));
        if (cpuArm920JitCharged(cpu) + quietCycles + cpuArm920JitMaxUnchecked(cpu) < jitBudget) {
            jitHalt = true;
            cpuSetSlowPath(cpu, SLOW_PATH_REASON_RESCHEDULE);
        }
        return true;
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
        uint64_t side = 0;
        quietCycles = boundaryStatus() == Imx1BootstrapAccess::ok ? quietCpuCycles(quietMode, true, false, &side) : 0;
        boardSlack = side > quietCycles ? side - quietCycles : 0;
        spanCrosses = false;
        if (quietCycles + cpuArm920JitMaxUnchecked(cpu) < jitBudget) {
            jitHalt = true;
            cpuSetSlowPath(cpu, SLOW_PATH_REASON_RESCHEDULE);
        }
        return true;
    }
    // The registers of a timer delay loop the CPU is in (ldr rT, [rB, #16]; sub rT, rT, rS; cmp rT, #n; bcc back, as
    // at 2009f678): the base and start registers and the wait.
    bool armDelayLoop(const uint32_t* r, unsigned& b, unsigned& sreg, uint32_t& wait) const {
        uint32_t w[4], pa;
        uint32_t start = r[15];
        if (!cpuArm920PeekWord(cpu, start, &w[0], &pa)) return false;
        // pc is on one of the loop's four words; find the load.
        for (unsigned back = 0; back < 4 && (w[0] & 0xfff00fffu) != 0xe5900010u; ++back) {
            start -= 4;
            if (back == 3 || !cpuArm920PeekWord(cpu, start, &w[0], &pa)) return false;
        }
        for (unsigned i = 1; i < 4; ++i)
            if (!cpuArm920PeekWord(cpu, start + 4 * i, &w[i], &pa)) return false;
        const unsigned t = (w[0] >> 12) & 15;
        b = (w[0] >> 16) & 15;
        sreg = w[1] & 15;
        if (t == 15 || b == 15 || sreg == 15 || sreg == t || t == b ||
            (w[1] & 0xfffffff0u) != (0xe0400000u | (t << 16) | (t << 12)) ||
            (w[2] & 0xffff0f00u) != (0xe3500000u | (t << 16)) || w[3] != 0x3afffffbu) return false;
        wait = w[2] & 0xff;
        return true;
    }
    // The same in Thumb with the wait in a register (ldr rT, [rB, #16]; subs rT, rT, rS; cmp rT, rW; bcc back, as at
    // 2008492c).
    bool thumbDelayLoop(const uint32_t* r, unsigned& b, unsigned& sreg, uint32_t& wait) const {
        auto half = [&](uint32_t va, uint16_t& h) {
            uint32_t word, pa;
            if (!cpuArm920PeekWord(cpu, va, &word, &pa)) return false;
            h = uint16_t((va & 2) ? word >> 16 : word);
            return true;
        };
        uint16_t h[4];
        uint32_t start = r[15] & ~1u;
        if (!half(start, h[0])) return false;
        for (unsigned back = 0; back < 4 && (h[0] & 0xffc0u) != 0x6900u; ++back) {
            start -= 2;
            if (back == 3 || !half(start, h[0])) return false;
        }
        for (unsigned i = 1; i < 4; ++i)
            if (!half(start + 2 * i, h[i])) return false;
        const unsigned t = h[0] & 7, wreg = (h[2] >> 3) & 7;
        b = (h[0] >> 3) & 7;
        sreg = (h[1] >> 6) & 7;
        if (t == b || sreg == t || wreg == t || (h[1] & 0xfe3fu) != (0x1a00u | (t << 3) | t) ||
            (h[2] & 0xffc7u) != (0x4280u | t) || h[3] != 0xd3fbu) return false;
        wait = r[wreg];
        return true;
    }
    // Fast blocks: a timer delay loop waits for timer count - rS to reach the wait. When the CPU is in one on the
    // timer count, the cycles up to the first count that ends it, within the quiet span, pass at once without running
    // the loop.
    // 0 where the CPU isn't in a delay loop or its wait is over, 1 where the wait couldn't be passed, 2 where it was.
    int skipDelay() {
        const uint32_t* r = cpuGetRegisters(cpu);
        if (quietCycles < leanMinimumSpan) return 1;
        unsigned b = 0, sreg = 0;
        uint32_t wait = 0;
        if (!(cpuIsThumb(cpu) ? thumbDelayLoop(r, b, sreg, wait) : armDelayLoop(r, b, sreg, wait))) return 0;
        flushSubmitBoard();
        uint32_t timer;
        if (!cpuArm920PeekWord(cpu, r[b] + 16, nullptr, &timer) || !Imx1Bootstrap::timerCountAddress(timer) ||
            failed || graphicsWireStatus != Imx1BootstrapAccess::ok) return 0;
        const uint32_t base = r[sreg];
        auto done = [&](uint64_t cycles, bool& known) {
            uint32_t count;
            // Wait clocks still deferred count after the cycles, as chargeDeferred charges them.
            known = timerCountAfter(peripherals, timer, 4, deferredCycles + cycles, quietMode, count, deferredHclk);
            return known && count - base >= wait;
        };
        bool known;
        if (done(0, known)) return 0;
        if (!known) return 1;
        // The last cycle of the span stays for the loop to see; a wait past the span passes all the rest of it.
        uint64_t low = 0, high = quietCycles - 1;
        // The skip counts as work, so it stops short of the slice's work bound, like the lean path.
        if (bounds) {
            const uint64_t used = work - sliceWorkStart;
            if (used + 1 >= bounds->work) return 1;
            high = std::min(high, bounds->work - used - 1);
        }
        if (!high) return 1;
        const bool reached = done(high, known);
        if (!known) return 1;
        // From one reading to the next with the cycles each lasts (sameUntil), the first that ends the wait is found
        // in a few steps where it's near; bisection does the rest.
        for (unsigned i = 0; reached && i < 16 && high - low > 1; ++i) {
            uint32_t count;
            uint64_t until;
            if (!peripherals.timerCountAfter(timer, 4, deferredCycles + low, quietMode, count, deferredHclk, &until) ||
                until < deferredCycles + low)
                break;
            if (until >= deferredCycles + high - 1) { low = high - 1; break; }
            const uint64_t next = until - deferredCycles + 1;
            if (done(next, known)) { high = next; low = next - 1; break; }
            if (!known) return 1;
            low = next;
        }
        while (reached && high - low > 1) {
            const uint64_t mid = low + (high - low) / 2;
            if (done(mid, known)) high = mid; else if (known) low = mid; else return 1;
        }
        deferredCycles += high;
        quietCycles -= high;
        work += high;
        ++delaySkips;
        delaySkipCycles += high;
        return 2;
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
        flushSubmitBoard();
        // What leanRegisterRead needs, none of which changes inside the span.
        if (!graphicsMemory || !cs5Configuration.virtualProfile() || options.cs5ConfigurationObserver || failed ||
            cs5Configuration.engineState().completed() || !leanCs5Ready())
            return 0;
        const bool user = (cpsr & 31) == ARM_SR_MODE_USR;
        if (peripherals.eimState().cs5Transfer(pa, 4, false, user) != Imx1EimAccess::ok || !leanRegisterReady()) return 0;
        // A round's cost model clocks: the load, the test and the taken branch.
        constexpr uint64_t roundClocks = cost::armCost(0xe590154cu, cost::Core::arm920t) +
                                         cost::armCost(0xe3110e40u, cost::Core::arm920t) +
                                         cost::armCost(0x0afffffcu, cost::Core::arm920t);
        const uint64_t clocks = leanCs5.clocks, round = roundClocks + uint64_t(leanCs5.cost);
        // The span keeps room for the JIT's last rounds; the slice for the work the rounds add.
        const uint64_t keep = leanMinimumSpan + 4 * round;
        if (quietCycles <= keep) return 0;
        const uint64_t most = std::min((quietCycles - keep) / round, hard / (2 * (3 + clocks) + 2));
        if (!most) return 0;
        const auto cpuPeriod = peripherals.cpuReferencePeriod(quietMode);
        const auto busPeriod = peripherals.busReferencePeriod();
        unsigned first = 0;
        // Round i reads after roundClocks i cycles and i + 1 reads' wait clocks.
        auto same = [&](uint64_t i, unsigned* region) {
            uint64_t ticks;
            uint32_t value;
            unsigned at;
            const uint64_t cycles = deferredCycles + roundClocks * i, hclk = deferredHclk + (i + 1) * clocks;
            if ((!cs5Configuration.ticksAfterQuick(cycles, cpuPeriod, hclk, busPeriod, leanCpuRate, leanBusRate, ticks) &&
                 !cs5Configuration.ticksAfter(cycles, cpuPeriod, hclk, busPeriod, ticks)) ||
                !cs5Configuration.quietScanStatus(peripherals.eimState(), pc, ticks, value, at, &leanScanRate))
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
        deferredCycles += roundClocks * n;
        quietCycles -= n * round;
        deferredHclk += n * clocks;
        result.cs5WaitClocks += n * clocks;
        work += n * (3 + clocks);
        retired += 3 * n;
        ++scanPollSkips;
        scanPollRounds += n;
        return 3 * n;
    }
    // Fast blocks: a UART status poll, ldr rT, [rN, #imm]; tst rT, #mask; beq or bne back to the load, like the ROM
    // serial driver's wait for TRDY after each character (204d0f24). A status register (USR1, USR2, UTS) changes only
    // at a UART event or a guest access, so inside the quiet span every round reads what the last one did. At the
    // load, after a round that read the value the loop goes on with, the rounds up to the span's end pass at once:
    // 3 instructions and their cost model clocks each, as the JIT takes them with the read on the lean path. The
    // last rounds of the span run as before and see the change. hard is the most work the slice has left. Returns
    // the instructions passed.
    uint64_t skipUartPoll(uint64_t hard) {
        if (!uartPollSkip || cpuIsThumb(cpu) || !cpuArm920JitCanRun(cpu) || failed ||
            graphicsWireStatus != Imx1BootstrapAccess::ok)
            return 0;
        const uint32_t* r = cpuGetRegisters(cpu);
        const uint32_t pc = r[15];
        uint32_t w[3], pa;
        for (unsigned i = 0; i < 3; ++i)
            if (!cpuArm920PeekWord(cpu, pc + 4 * i, &w[i], &pa)) return 0;
        // Always-executed ldr with an immediate offset and no writeback, tst of the loaded register, branch to the load.
        if ((w[0] & 0xfff00000u) != 0xe5900000u || (w[1] & 0xfff0f000u) != 0xe3100000u ||
            (w[2] != 0x0afffffcu && w[2] != 0x1afffffcu))
            return 0;
        const unsigned base = (w[0] >> 16) & 15, t = (w[0] >> 12) & 15;
        if (base == 15 || t == 15 || base == t || ((w[1] >> 16) & 15) != t) return 0;
        const uint32_t cpsr = cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR);
        const uint32_t va = r[base] + (w[0] & 0xfff);
        if ((va & 3) || (cpsr & 31) == ARM_SR_MODE_USR || !cpuArm920PeekWord(cpu, va, nullptr, &pa)) return 0;
        const uint32_t unit = pa & ~0xfffu, offset = pa & 0xfffu;
        if ((unit != 0x00206000 && unit != 0x00207000) || (offset != 0x94 && offset != 0x98 && offset != 0xb4)) return 0;
        flushSubmitBoard();
        uint32_t value;
        if (peripherals.access(pa, 4, false, value, cpuArm920ClockMode(cpu)) != Imx1BootstrapAccess::ok) return 0;
        const unsigned rotate = 2 * ((w[1] >> 8) & 15);
        const uint32_t mask = rotate ? ((w[1] & 0xff) >> rotate) | ((w[1] & 0xff) << (32 - rotate)) : w[1] & 0xff;
        const uint32_t result = value & mask;
        // The round before read the same value: rT holds it, and N, Z and C are what its tst left.
        uint32_t flags = (result & 0x80000000u) | (result ? 0 : 0x40000000u);
        flags |= rotate ? (mask & 0x80000000u) >> 2 : cpsr & 0x20000000u;
        const bool loops = w[2] == 0x0afffffcu ? !result : result != 0;
        if (!loops || r[t] != value || (cpsr & 0xe0000000u) != flags) return 0;
        const uint64_t round = cost::armCost(w[0], cost::Core::arm920t) + cost::armCost(w[1], cost::Core::arm920t) +
                               cost::armCost(w[2], cost::Core::arm920t);
        // The span keeps room for the last rounds; the slice for the work the rounds add.
        const uint64_t keep = leanMinimumSpan + 4 * round;
        if (quietCycles <= keep) return 0;
        const uint64_t n = std::min((quietCycles - keep) / round, hard / 8);
        if (!n) return 0;
        deferredCycles += round * n;
        quietCycles -= round * n;
        work += 3 * n;
        retired += 3 * n;
        ++uartPollSkips;
        uartPollRounds += n;
        return 3 * n;
    }
    // Whether pc is at a load skipUartPoll may pass: the JIT stops there, as the read isn't on the lean path.
    bool atUartPoll() const {
        if (!uartPollSkip || cpuIsThumb(cpu)) return false;
        uint32_t w, pa;
        return cpuArm920PeekWord(cpu, cpuGetRegisters(cpu)[15] + 8, &w, &pa) && (w == 0x0afffffcu || w == 0x1afffffcu) &&
               cpuArm920PeekWord(cpu, cpuGetRegisters(cpu)[15], &w, &pa) && (w & 0xfff00000u) == 0xe5900000u;
    }
    // Exact: the same UART status poll, passed after the step ran one whole round of it (stepped). Inside the quiet
    // span every round reads the value the last one did, takes the branch back and costs what the last one did, so
    // the rounds up to the span's end (less a few) add the round's instructions, work, bus accesses and cycles at
    // once; the cycles wait in deferredCycles as the step's own do. The last rounds of the span run as steps and see
    // the change. hard is the most instructions the slice has left. Returns the instructions passed.
    uint64_t skipUartPollStepped(uint64_t hard) {
        // Runs after every step: the checks on the probe's own state go first, the CPU calls after them.
        if (!uartPollSkip || failed || graphicsWireStatus != Imx1BootstrapAccess::ok || blPrefix || pcStop.armed ||
            !quietCycles || linesPending || deferredHclk || retired < 3)
            return 0;
        const uint32_t* r = cpuGetRegisters(cpu);
        const uint32_t pc = r[15];
        const SteppedInstruction& load = stepped[(retired - 3) % 3];
        const SteppedInstruction& test = stepped[(retired - 2) % 3];
        const SteppedInstruction& branch = stepped[(retired - 1) % 3];
        if (load.step != retired - 3 || test.step != retired - 2 || branch.step != retired - 1 || !load.plain ||
            !test.plain || !branch.plain || load.pc != pc || test.pc != pc + 4 || branch.pc != pc + 8 ||
            load.bus != 1 || test.bus || branch.bus)
            return 0;
        if (cpuIsThumb(cpu) || cpuArm920ClockMode(cpu) != quietMode || !cpuArm920StepSkipAllowed(cpu)) return 0;
        // As skipUartPoll: ldr with an immediate offset and no writeback, tst of the loaded register, branch back.
        const uint32_t w0 = load.instruction, w1 = test.instruction, w2 = branch.instruction;
        if ((w0 & 0xfff00000u) != 0xe5900000u || (w1 & 0xfff0f000u) != 0xe3100000u ||
            (w2 != 0x0afffffcu && w2 != 0x1afffffcu))
            return 0;
        const unsigned base = (w0 >> 16) & 15, t = (w0 >> 12) & 15;
        if (base == 15 || t == 15 || base == t || ((w1 >> 16) & 15) != t) return 0;
        // The load read a UART status register (USR1, USR2, UTS of either UART).
        const uint32_t unit = load.address & ~0xfffu, offset = load.address & 0xfffu;
        if ((unit != 0x00206000 && unit != 0x00207000) || (offset != 0x94 && offset != 0x98 && offset != 0xb4)) return 0;
        // A charge after the load (the span ended at the tst or the branch) can have changed the status: the board
        // as charged has to still read what rT holds. A status read changes nothing.
        uint32_t value;
        if (peripherals.access(load.address, 4, false, value, cpuArm920ClockMode(cpu)) != Imx1BootstrapAccess::ok ||
            value != r[t])
            return 0;
        const uint64_t round = uint64_t(load.cycles) + test.cycles + branch.cycles;
        if (!round) return 0;
        const uint64_t keep = leanMinimumSpan + 4 * round;
        if (quietCycles <= keep) return 0;
        const uint64_t n = std::min((quietCycles - keep) / round, hard / 3);
        if (!n) return 0;
        deferredCycles += round * n;
        quietCycles -= round * n;
        work += 3 * n;
        retired += 3 * n;
        result.busAccesses += n;
        ++uartPollSkips;
        uartPollRounds += n;
        return 3 * n;
    }
    // Whether pc is the ASP FIFO wait's load (aspPollPc) with the wait's code (aspPollCode).
    bool atAspPoll(uint32_t pc) {
        if (pc != aspPollPc) return false;
        uint32_t w = 0, pa, at = ~0u;
        for (unsigned i = 0; i < std::size(aspPollCode); ++i) {
            const uint32_t va = pc + 2 * i;
            if ((va & ~3u) != at) {
                at = va & ~3u;
                if (!cpuArm920PeekWord(cpu, at, &w, &pa)) return false;
            }
            if (((va & 2) ? w >> 16 : w & 0xffff) != aspPollCode[i]) return false;
        }
        return true;
    }
    // Fast blocks: the ROM pen routine's wait for the ASP's calibration samples (aspPollCode). FIFO_PTR changes only
    // at an ASP conversion's end, which ends the quiet span, or at a guest access, so inside the span every round reads
    // what the last one did. At the load, after a round that read a write pointer below 3 (r3 holds it, the flags are
    // what cmp r3, #3 left), the rounds up to the span's end pass at once: 5 instructions and their cost model clocks
    // each, as the JIT takes them with the read on the lean path. The last rounds of the span run in the JIT and see
    // the sample land. hard is the most work the slice has left. Returns the instructions passed.
    uint64_t skipAspPoll(uint64_t hard) {
        if (!aspPollSkip || !cpuIsThumb(cpu) || !cpuArm920JitCanRun(cpu) || failed ||
            graphicsWireStatus != Imx1BootstrapAccess::ok)
            return 0;
        const uint32_t* r = cpuGetRegisters(cpu);
        const uint32_t pc = r[15];
        if (!atAspPoll(pc)) return 0;
        const uint32_t cpsr = cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR);
        const uint32_t va = r[0] + 52;
        uint32_t pa;
        if ((va & 3) || (cpsr & 31) == ARM_SR_MODE_USR || !cpuArm920PeekWord(cpu, va, nullptr, &pa) || pa != 0x00215034)
            return 0;
        uint32_t value;
        if (peripherals.access(pa, 4, false, value, cpuArm920ClockMode(cpu)) != Imx1BootstrapAccess::ok) return 0;
        // The round before read the same pointer and stayed: cmp of a value below 3 with 3 leaves N set, Z, C and V clear.
        const uint32_t pointer = value & 15;
        if (pointer >= 3 || r[3] != pointer || (cpsr & 0xf0000000u) != 0x80000000u) return 0;
        uint64_t round = 0;
        for (const uint16_t h : aspPollCode) round += cost::thumbCost(h, cost::Core::arm920t);
        // The span keeps room for the last rounds; the slice for the work the rounds add.
        const uint64_t keep = leanMinimumSpan + 4 * round;
        if (quietCycles <= keep) return 0;
        const uint64_t n = std::min((quietCycles - keep) / round, hard / 8);
        if (!n) return 0;
        deferredCycles += round * n;
        quietCycles -= round * n;
        work += 5 * n;
        retired += 5 * n;
        ++aspPollSkips;
        aspPollRounds += n;
        aspPollCycles += round * n;
        return 5 * n;
    }
    // Whether the halfwords at va are code.
    bool thumbCodeAt(uint32_t va, const uint16_t* code, size_t count) {
        uint32_t w = 0, pa, at = ~0u;
        for (size_t i = 0; i < count; ++i) {
            const uint32_t a = va + 2 * uint32_t(i);
            if ((a & ~3u) != at) {
                at = a & ~3u;
                if (!cpuArm920PeekWord(cpu, at, &w, &pa)) return false;
            }
            if (((a & 2) ? w >> 16 : w & 0xffff) != code[i]) return false;
        }
        return true;
    }
    bool armCodeAt(uint32_t va, const uint32_t* code, size_t count) {
        uint32_t w, pa;
        for (size_t i = 0; i < count; ++i)
            if (!cpuArm920PeekWord(cpu, va + 4 * uint32_t(i), &w, &pa) || w != code[i]) return false;
        return true;
    }
    // The HAL call a card wait round makes through [r9 - 4] + slot: the Thumb veneer at veneer, its ARM branch, the
    // jump, the glue the table points at and the Thumb code it goes on to. Adds the instructions and their cost model
    // clocks to instructions and round.
    bool cardHalCall(uint32_t veneer, uint32_t branch, const uint32_t* jump, uint32_t slot, uint32_t glueAt,
                     const uint32_t* glue, uint32_t at, const uint16_t* code, unsigned run, uint64_t& instructions,
                     uint64_t& round) {
        const uint32_t* r = cpuGetRegisters(cpu);
        uint32_t w = 0, table = 0, target = 0, pa;
        const uint32_t jumpAt = veneer + 4 + 4 * int32_t(int32_t(branch << 8) >> 8) + 8;
        if (!thumbCodeAt(veneer, cardVeneerCode, std::size(cardVeneerCode)) ||
            !cpuArm920PeekWord(cpu, veneer + 4, &w, &pa) || w != branch || !armCodeAt(jumpAt, jump, 2) ||
            !cpuArm920PeekWord(cpu, r[9] - 4, &table, &pa) || !cpuArm920PeekWord(cpu, table + slot, &target, &pa) ||
            target != glueAt || !armCodeAt(glueAt, glue, 4) || !thumbCodeAt(at, code, run + 2))
            return false;
        // bx pc, b, the jump's two loads, the glue's three, then the Thumb code's run.
        round += cost::thumbCost(cardVeneerCode[0], cost::Core::arm920t) + cost::armCost(branch, cost::Core::arm920t);
        for (unsigned i = 0; i < 2; ++i) round += cost::armCost(jump[i], cost::Core::arm920t);
        for (unsigned i = 0; i < 3; ++i) round += cost::armCost(glue[i], cost::Core::arm920t);
        for (unsigned i = 0; i < run; ++i) round += cost::thumbCost(code[i], cost::Core::arm920t);
        instructions += 1 + 1 + 2 + 3 + run;
        return true;
    }
    // Fast blocks: slot 2's status waits (slot2Status2Code, slot2Status1Code). Inside the quiet span the SD block's
    // status stays as it is (no SD event is due), the identity read changes nothing, and a round stores only the
    // status it read to [sp], so every round runs the same instructions and leaves the same registers. At the
    // round's start, after a round that went on (the registers show what it left), the rounds up to the span's end
    // pass at once with their cost model clocks and the two reads' wait clocks, as the JIT takes them with the reads
    // on the lean path; the last rounds run in the JIT and see the change. hard is the most work the slice has left.
    // Returns the instructions passed.
    uint64_t skipSlot2Poll(uint64_t hard) {
        if (!slot2PollSkip || !slot2LeanOn || !cpuIsThumb(cpu) || !cpuArm920JitCanRun(cpu) || failed ||
            graphicsWireStatus != Imx1BootstrapAccess::ok || !graphicsMemory || options.cs5ConfigurationObserver ||
            options.busObserver || cs5Configuration.engineState().completed() || crossRefused())
            return 0;
        const uint32_t* r = cpuGetRegisters(cpu);
        const uint32_t pc = r[15];
        const uint32_t cpsr = cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR);
        const bool second = pc == slot2Status2Round || pc == slot2Status2LoadPc, head = pc == slot2Status2LoadPc;
        if ((!second && pc != slot2Status1LoadPc) || (cpsr & 31) == ARM_SR_MODE_USR) return 0;
        const bool user = false;
        // The status and identity addresses the round reads, and the halfword at [sp] it stored.
        uint32_t statusPa, identityPa, stored, pa;
        const uint32_t statusVa = second ? r[1] + 28 : r[2] + 24, identityVa = second ? r[4] : r[1];
        if (!cpuArm920PeekWord(cpu, statusVa, nullptr, &statusPa) || statusPa != (second ? 0x1620203cu : 0x16202038u) ||
            !cpuArm920PeekWord(cpu, identityVa, nullptr, &identityPa) || identityPa != 0x16010000 ||
            !cpuArm920PeekWord(cpu, r[13], &stored, &pa) || (r[13] & 3) || !Imx1Bootstrap::isRamAddress(pa))
            return 0;
        uint64_t round = 0, instructions = 0;
        uint32_t flags, status, identity;
        if (second) {
            if (!thumbCodeAt(slot2Status2LoadPc, slot2Status2Code, std::size(slot2Status2Code)))
                return 0;
            for (const uint8_t i : slot2Status2Run) round += cost::thumbCost(slot2Status2Code[i], cost::Core::arm920t);
            instructions = std::size(slot2Status2Run);
        } else {
            if (!thumbCodeAt(slot2Status1LoadPc, slot2Status1Code, std::size(slot2Status1Code))) return 0;
            for (const uint16_t half : slot2Status1Code) round += cost::thumbCost(half, cost::Core::arm920t);
            instructions = std::size(slot2Status1Code);
        }
        // Both reads as the lean path takes them, which changes nothing.
        leanCs5 = {};
        const bool ready = leanCs5Ready() &&
            peripherals.eimState().cs5Transfer(statusPa, 2, false, user) == Imx1EimAccess::ok &&
            peripherals.eimState().cs5Transfer(identityPa, 4, false, user) == Imx1EimAccess::ok && leanRegisterReady() &&
            cs5Configuration.quietSdRead(peripherals.eimState(), second ? slot2Status2LoadPc : slot2Status1LoadPc,
                                         statusPa, 2, status) &&
            cs5Configuration.quietSdRead(peripherals.eimState(), second ? slot2IdentityPc : slot2Identity1Pc,
                                         identityPa, 4, identity);
        const uint32_t clocks = leanCs5.clocks, cost = leanCs5.cost;
        leanCs5 = {};
        if (!ready) return 0;
        status &= 0xffff;
        if (second && head) {
            // At 23c2, after the bpl: r2 the status << 23 and its flags, N clear (bit 8), C bit 9, V as it was; r3 sp.
            const uint32_t shifted = status << 23;
            flags = (shifted ? 0 : 0x40000000u) | ((status & 0x200) << 20) | (cpsr & 0x10000000u);
            if ((status & 0x8140) || r[2] != shifted || r[3] != r[13] || (stored & 0xffff) != status) return 0;
        } else if (second) {
            // What the round leaves at 23d8: r2 status >> 15 (0), r3 sp, N set and Z, C and V clear from cmp r2, #1.
            flags = 0x80000000u;
            if ((status & 0x8140) || r[2] || r[3] != r[13] || (stored & 0xffff) != status) return 0;
        } else {
            // At 2408: r4 the status, r3 the status << 29 and its flags, N clear (bit 2), C bit 3, V as it was.
            const uint32_t shifted = status << 29;
            flags = (shifted ? 0 : 0x40000000u) | ((status & 8) << 26) | (cpsr & 0x10000000u);
            if ((status & 4) || r[4] != status || r[3] != shifted || (stored & 0xffff) != status) return 0;
        }
        if ((cpsr & 0xf0000000u) != flags) return 0;
        // The span keeps room for the last rounds; the slice for the work the rounds add.
        const uint64_t span = round + 2 * uint64_t(cost);
        const uint64_t keep = leanMinimumSpan + 4 * span;
        if (quietCycles <= keep) return 0;
        const uint64_t n = std::min((quietCycles - keep) / span, hard / (instructions + 1));
        if (!n) return 0;
        deferredCycles += round * n;
        quietCycles -= span * n;
        deferredHclk += 2 * uint64_t(clocks) * n;
        result.cs5WaitClocks += 2 * uint64_t(clocks) * n;
        work += 2 * uint64_t(clocks) * n + instructions * n;
        retired += instructions * n;
        ++slot2PollSkips;
        slot2PollRounds += n;
        return instructions * n;
    }
    // Fast blocks: the card driver's STATUS waits (cardWaitCode, cardDataCode). STATUS changes only at an MMCSD event
    // (a command's end, the data's end), which ends the quiet span, and the tick count only in the tick interrupt,
    // which can't come inside it, and a round stores nothing, so inside the span every round runs the same
    // instructions and leaves the same registers. At the loop's head, after a round that went on (the registers show
    // what it left), the rounds up to the span's end pass at once with their cost model clocks, as the JIT takes them
    // with the reads on the lean path; the last rounds run in the JIT and see the change. hard is the most work the
    // slice has left. Returns the instructions passed.
    // Whether the CPU is at the SWI vector from KALTaskDelay's SWI 30 in Xploids' draw retry (surfaceWaitLoop), with
    // the surface busy, the current task the kernel's chosen one and no interrupt raised. state gets the registers.
    bool atSurfaceWait(uint32_t (&state)[20]) {
        const uint32_t* r = cpuGetRegisters(cpu);
        const uint32_t cpsr = cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR);
        const uint32_t spsr = cpuGetRegExternal(cpu, ARM_REG_NUM_SPSR);
        if ((r[15] != 8 && r[15] != 0xffff0008) || r[14] != surfaceWaitSwiReturn || (cpsr & 0x3f) != ARM_SR_MODE_SVC ||
            !(spsr & ARM_SR_T) || (spsr & 0xc0) || ((spsr & 31) != ARM_SR_MODE_SYS && (spsr & 31) != ARM_SR_MODE_USR) ||
            cpuArm920InterruptRaised(cpu))
            return false;
        uint32_t sp, lr;
        cpuArm920UsrRegisters(cpu, &sp, &lr);
        const auto peek = [&](uint32_t va, uint32_t& w) {
            uint32_t pa;
            return !(va & 3) && cpuArm920PeekWord(cpu, va, &w, &pa);
        };
        // KALTaskDelay's frame, then SysTaskDelay's: their return addresses.
        uint32_t back, ret, w;
        if (!peek(sp + 12, back) || back != 0x20060cd3 || !peek(sp + 20, ret) || ret < 4 * surfaceWaitLoopReturn)
            return false;
        const uint32_t head = ret - 4 * surfaceWaitLoopReturn;
        for (unsigned i = 0; i < std::size(surfaceWaitLoop); ++i)
            if (!peek(head + 4 * i, w) || w != surfaceWaitLoop[i]) return false;
        for (unsigned i = 0; i < std::size(surfaceDalCode); ++i)
            if (!peek(surfaceDalAt + 4 * i, w) || w != surfaceDalCode[i]) return false;
        if (!thumbCodeAt(surfaceKalAt, surfaceKalCode, std::size(surfaceKalCode))) return false;
        uint32_t table, entry, tag, busy;
        if (!peek(0x2009bebc, w) || w != 0x7115 || !peek(r[10] + 0x772d8, table) || !peek(table + 1496, entry) ||
            entry != 0x2021444c || !peek(table + 644, entry) || entry != 0x20214294)
            return false;
        // The surface the DAL routine looks at: tagged and busy.
        uint32_t surface;
        if (!peek(r[6] + 12, surface) || !peek(surface, tag) || !peek(0x2009bec8, w) || tag != w ||
            !peek(surface + 44, busy) || !(busy & 1))
            return false;
        // The kernel's chosen ready task is the current one, and no lock is held.
        uint32_t k, kernel, chosen, current, locks;
        if (!peek(r[9], k) || !peek(k, k) || !peek(k + 0x4a3c, kernel) || !peek(kernel + 0x21c0, chosen) ||
            !peek(kernel + 0x21c4, current) || !peek(kernel + 0x21d0, locks) || !chosen || chosen != current ||
            (locks & 0xffff))
            return false;
        std::copy(r, r + 15, state);
        state[15] = cpsr;
        state[16] = spsr;
        state[17] = sp;
        state[18] = lr;
        state[19] = surface;
        return true;
    }
    // Fast blocks: Xploids' draw retry (surfaceWaitLoop). Back at the SWI vector in the state the round started from,
    // with only that round's JIT and lean runs since, the rounds up to the span's end pass at once, each the clocks and
    // instructions the round took. The last rounds of the span run. hard is the most work the slice has left.
    // Returns the instructions passed.
    uint64_t skipSurfaceWait(uint64_t hard) {
        uint32_t state[20];
        if (!surfaceWaitSkip) surfaceWait.armed = false;
        // Elsewhere in the round, the measuring goes on.
        if (!surfaceWaitSkip || !cpuArm920JitCanRun(cpu) || failed || graphicsWireStatus != Imx1BootstrapAccess::ok ||
            !atSurfaceWait(state))
            return 0;
        uint64_t passed = 0;
        if (surfaceWait.armed && surfaceWait.instructions && retired == surfaceWait.retiredAt + surfaceWait.instructions &&
            std::equal(state, state + 20, surfaceWait.state)) {
            const uint64_t round = surfaceWait.cycles, count = surfaceWait.instructions;
            const uint64_t keep = leanMinimumSpan + 2 * round;
            const uint64_t n = quietCycles > keep && round ? std::min((quietCycles - keep) / round, hard / count) : 0;
            if (n) {
                deferredCycles += round * n;
                quietCycles -= round * n;
                work += count * n;
                retired += count * n;
                ++surfaceWaitSkips;
                surfaceWaitRounds += n;
                passed = count * n;
            }
        }
        // The next round is measured from here.
        surfaceWait.armed = true;
        std::copy(state, state + 20, surfaceWait.state);
        surfaceWait.retiredAt = retired;
        surfaceWait.cycles = surfaceWait.instructions = 0;
        return passed;
    }
    // A JIT or lean run's clocks and instructions, for the round being measured. waits is whether the run had
    // graphics wait clocks or charged accesses, which end it.
    void surfaceWaitRan(uint64_t cycles, uint64_t instructions, bool waits) {
        if (!surfaceWait.armed) return;
        surfaceWait.armed = !waits;
        surfaceWait.cycles += cycles;
        surfaceWait.instructions += instructions;
    }
    uint64_t skipCardPoll(uint64_t hard) {
        if (!cardPollSkip || !cpuIsThumb(cpu) || !cpuArm920JitCanRun(cpu) || failed ||
            graphicsWireStatus != Imx1BootstrapAccess::ok)
            return 0;
        const uint32_t* r = cpuGetRegisters(cpu);
        const uint32_t pc = r[15];
        const uint32_t cpsr = cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR);
        const bool data = pc == cardDataPc;
        if ((!data && pc != cardWaitPc) || (cpsr & 31) == ARM_SR_MODE_USR) return 0;
        // STATUS, as the next round reads it.
        uint32_t pa, status;
        if (!cpuArm920PeekWord(cpu, r[4] + 4, nullptr, &pa) || pa != 0x00214004 ||
            peripherals.access(pa, 4, false, status, cpuArm920ClockMode(cpu)) != Imx1BootstrapAccess::ok)
            return 0;
        // With the card lookahead STATUS can change inside the span: the rounds stop short of that.
        const bool lookahead = peripherals.mmcsdLookaheadOn();
        if (lookahead && !peripherals.mmcsdStatusAfter(deferredCycles, quietMode, deferredHclk, status)) return 0;
        // The tick count the round reads: [[[r9]] + 0x4a28 + 16] + 28.
        uint32_t globals, block, ticks;
        if (!cpuArm920PeekWord(cpu, r[9], &globals, &pa) || !cpuArm920PeekWord(cpu, globals, &globals, &pa) ||
            !cpuArm920PeekWord(cpu, globals + 0x4a28 + 16, &block, &pa) ||
            !cpuArm920PeekWord(cpu, block + 28, &ticks, &pa) || !Imx1Bootstrap::isRamAddress(pa))
            return 0;
        // The flags cmp a, b leaves.
        const auto compare = [](uint32_t a, uint32_t b) {
            const uint32_t d = a - b;
            return (d & 0x80000000u) | (d ? 0 : 0x40000000u) | (a >= b ? 0x20000000u : 0) |
                   (((a ^ b) & (a ^ d)) >> 31 << 28);
        };
        uint64_t instructions = 0, round = 0;
        uint32_t flags;
        const auto add = [&](const uint16_t* code, const uint8_t* at, size_t count) {
            for (size_t i = 0; i < count; ++i) round += cost::thumbCost(code[at[i]], cost::Core::arm920t);
            instructions += count;
        };
        if (!data) {
            // Ticks passed since r5, as the round works them out (subs, cmp, bcs, adds), at most 100.
            const bool wrap = ticks < r[5];
            const uint32_t passed = wrap ? r[7] + ticks : ticks - r[5];
            static constexpr uint8_t run[] = {0, 1, 2, 3, 4, 5, 6}, wrapRun[] = {7}, end[] = {8, 9, 10, 11, 12};
            if ((status & r[6]) || r[6] != 0x2022 || r[7] != 0x80000000u - r[5] || passed > 100 ||
                !thumbCodeAt(pc, cardWaitCode, std::size(cardWaitCode)) ||
                !cardHalCall(0x204cfe98, cardGetterVeneer, cardGetterJump, 0, cardGetterGlueAt, cardGetterGlue,
                             cardGetterAt, cardGetterCode, 6, instructions, round))
                return 0;
            add(cardWaitCode, run, std::size(run));
            if (wrap) add(cardWaitCode, wrapRun, 1);
            add(cardWaitCode, end, std::size(end));
            // What the round leaves: r0 0 from ands, r1 the ticks passed, N clear and Z set from ands, C and V from
            // cmp r1, #100; r12 the getter (with its Thumb bit) from the glue, lr the BL's return.
            flags = 0x40000000u | (compare(passed, 100) & 0x30000000u);
            if (r[0] || r[1] != passed || r[12] != (cardGetterAt | 1) || r[14] != ((cardWaitPc + 4) | 1)) return 0;
        } else {
            uint32_t mask;
            const bool wrap = ticks < r[5];
            const uint32_t passed = wrap ? 0x80000000u - r[5] + ticks : ticks - r[5];
            const bool late = passed > 500;
            if ((status & 0x80d) || !(status & 0x100) || r[7] || !cpuArm920PeekWord(cpu, cardDataMaskAt, &mask, &pa) ||
                mask != 0x80d || !thumbCodeAt(pc, cardDataCode, std::size(cardDataCode)) ||
                !cardHalCall(0x204cfec0, cardTimerVeneer, cardTimerJump, 948, cardTimerGlueAt, cardTimerGlue,
                             cardTimerAt, cardTimerCode, 3, instructions, round) ||
                !cardHalCall(0x204cfe98, cardGetterVeneer, cardGetterJump, 0, cardGetterGlueAt, cardGetterGlue,
                             cardGetterAt, cardGetterCode, 6, instructions, round))
                return 0;
            add(cardDataCode, cardDataRound, std::size(cardDataRound));
            if (wrap) add(cardDataCode, cardDataWrap, std::size(cardDataWrap));
            add(cardDataCode, cardDataCompare, std::size(cardDataCompare));
            if (late) add(cardDataCode, cardDataLate, std::size(cardDataLate));
            add(cardDataCode, cardDataEnd, std::size(cardDataEnd));
            // What the round leaves: r0 0 and r1 0x80d from the last load and ands, N clear and Z set from ands, V
            // from cmp r1, #500, C from it too, or from lsls r0, #23 of STATUS when past 500; r12 the getter, lr the
            // second BL's return.
            const uint32_t compared = compare(passed, 500);
            flags = 0x40000000u | (compared & 0x10000000u) |
                    (late ? (status & 0x200) << 20 : compared & 0x20000000u);
            if (r[0] || r[1] != 0x80d || r[12] != (cardGetterAt | 1) || r[14] != (0x204c5ede | 1)) return 0;
        }
        if ((cpsr & 0xf0000000u) != flags) return 0;
        // The span keeps room for the last rounds; the slice for the work the rounds add.
        const uint64_t keep = leanMinimumSpan + 4 * round;
        if (quietCycles <= keep) return 0;
        uint64_t n = std::min((quietCycles - keep) / round, hard / (instructions + 1));
        if (lookahead && n) {
            // The last round passed reads STATUS before deferredCycles + round * n; STATUS changes once at most.
            const auto same = [&](uint64_t rounds) {
                uint32_t later;
                return peripherals.mmcsdStatusAfter(deferredCycles + round * rounds, quietMode, deferredHclk, later) &&
                       later == status;
            };
            if (!same(n)) {
                uint64_t low = 0, high = n;
                while (high - low > 1) {
                    const uint64_t mid = low + (high - low) / 2;
                    (same(mid) ? low : high) = mid;
                }
                n = low;
            }
        }
        if (!n) return 0;
        deferredCycles += round * n;
        quietCycles -= round * n;
        work += instructions * n;
        retired += instructions * n;
        ++cardPollSkips;
        cardPollRounds += n;
        return instructions * n;
    }
    // Fast blocks: the ROM's graphics FIFO wait (200b9010-200b9078) calls the Thumb delay at 20084912 for one timer
    // count, reads the engine's read pointer (1601021c) and loops while the ring has too little room, r5 counting
    // the reads that saw no change. The pointer moves only when a batch completes, which ends the quiet span, so
    // inside the span each round reads the same value and ends at the next timer count. At the delay's timer read
    // (2008492c) the rounds up to the last count that leaves room in the span pass at once: the time goes to the
    // start of that count, r3 takes it as the round's delay would and r5 counts the rounds passed. Only where a
    // count lasts well over a round's work, so that each round really starts in the count after the one before.
    // Returns whether rounds passed.
    bool skipFifoWait() {
        const uint32_t* r = cpuGetRegisters(cpu);
        if (spanCrosses || !fifoWaitSkip || r[15] != 0x2008492c || r[14] != 0x200b9018 || r[0] != 1 || r[6] != 1 || r[5] > r[11] ||
            !cpuIsThumb(cpu) || !cpuArm920JitCanRun(cpu))
            return false;
        static constexpr uint32_t caller[] = {0xe1a00006u, 0xeb000e28u, 0xe594101cu, 0xe5940014u, 0xe0010198u,
            0xe0841101u, 0xe2811ca0u, 0xe5911a0cu, 0xe591121cu, 0xe5841014u, 0xe1510000u, 0x1a000002u, 0xe0855006u,
            0xe155000bu, 0x9a000000u, 0xe3a05000u, 0xe5940008u, 0xe5942010u, 0xe0622120u, 0xe0821001u, 0xe2411001u,
            0xe08a0120u, 0xe0010000u, 0xe584100cu, 0xe584000cu, 0xe1500007u, 0xbaffffe4u};
        // 20084910-20084935, the delay from 20084912 (the last word's high half is the next function's).
        static constexpr uint32_t delay[] = {0x491d4770u, 0xd1002800u, 0x680a2001u, 0xd40405d2u, 0x604a221fu,
            0x320422ffu, 0x690b600au, 0x1ad2690au, 0xd3fb4282u, 0x00004770u};
        uint32_t w, pa;
        for (unsigned i = 0; i < std::size(caller); ++i)
            if (!cpuArm920PeekWord(cpu, 0x200b9010 + 4 * i, &w, &pa) || w != caller[i]) return false;
        for (unsigned i = 0; i < std::size(delay); ++i)
            if (!cpuArm920PeekWord(cpu, 0x20084910 + 4 * i, &w, &pa) ||
                (i + 1 < std::size(delay) ? w : w & 0xffff) != delay[i])
                return false;
        uint32_t timer;
        if (!cpuArm920PeekWord(cpu, r[1] + 16, nullptr, &timer) || !Imx1Bootstrap::timerCountAddress(timer) || failed ||
            graphicsWireStatus != Imx1BootstrapAccess::ok)
            return false;
        // The pointer read as the next round makes it, with the room it leaves (200b9018-200b9078).
        if (!graphicsMemory || !cs5Configuration.virtualProfile() || options.cs5ConfigurationObserver ||
            cs5Configuration.engineState().completed() || cs5Configuration.engineState().idleQuietTicks() == UINT64_MAX ||
            !leanCs5Ready())
            return false;
        uint32_t index, chip, last, bytes, producer;
        if (!cpuArm920PeekWord(cpu, r[4] + 28, &index, &pa) || !cpuArm920PeekWord(cpu, r[4] + 20, &last, &pa) ||
            !cpuArm920PeekWord(cpu, r[4] + 8, &bytes, &pa) || !cpuArm920PeekWord(cpu, r[4] + 16, &producer, &pa) ||
            !cpuArm920PeekWord(cpu, r[4] + (index * r[8] << 2) + 0xaa0c, &chip, &pa) ||
            !cpuArm920PeekWord(cpu, chip + 0x21c, nullptr, &pa) || pa != 0x1601021c)
            return false;
        const uint32_t cpsr = cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR);
        const bool user = (cpsr & 31) == ARM_SR_MODE_USR;
        uint32_t pointer;
        if (peripherals.eimState().cs5Transfer(pa, 4, false, user) != Imx1EimAccess::ok ||
            !cs5Configuration.quietRegisterRead(peripherals.eimState(), 0x200b9030, pa, 0, pointer) || pointer != last)
            return false;
        const uint32_t room = ((bytes >> 2) - producer + pointer - 1) & (r[10] + (bytes >> 2));
        if (int32_t(room) >= int32_t(r[7])) return false;
        // The round's delay started at count r3 and still waits for the next.
        const uint32_t start = r[3];
        uint32_t count;
        auto at = [&](uint64_t cycles) {
            return timerCountAfter(peripherals, timer, 4, deferredCycles + cycles, quietMode, count, deferredHclk);
        };
        // This round's delay read its count in the count's first half, so a round's work fits in one.
        uint64_t left;
        if (!peripherals.timerCountAfter(timer, 4, deferredCycles, quietMode, count, deferredHclk, &left) ||
            count != start || left < deferredCycles)
            return false;
        left -= deferredCycles - 1;
        // Room for the last round and the JIT in the span; the skip counts as work, like skipDelay.
        const uint64_t keep = leanMinimumSpan + 2 * fifoRoundMost;
        if (quietCycles <= keep) return false;
        uint64_t high = quietCycles - keep;
        if (bounds) {
            const uint64_t used = work - sliceWorkStart;
            if (used + keep >= bounds->work) return false;
            high = std::min(high, bounds->work - used - keep);
        }
        if (!at(high)) return false;
        const uint32_t rounds = count - start;
        if (rounds < 2 || rounds > (1u << 30)) return false;
        // The first cycle of count start + rounds.
        uint64_t low = 0;
        while (high - low > 1) {
            const uint64_t mid = low + (high - low) / 2;
            if (!at(mid)) return false;
            if (count - start >= rounds) high = mid; else low = mid;
        }
        uint32_t value;
        uint64_t until;
        if (!peripherals.timerCountAfter(timer, 4, deferredCycles + high, quietMode, value, deferredHclk, &until) ||
            value != start + rounds || until < deferredCycles + high + fifoRoundMost ||
            2 * left <= until - deferredCycles - high + 1)
            return false;
        deferredCycles += high;
        quietCycles -= high;
        work += high;
        cpuSetReg(cpu, 3, start + rounds);
        cpuSetReg(cpu, 5, uint32_t((uint64_t(r[5]) + rounds) % (uint64_t(r[11]) + 1)));
        ++fifoWaitSkips;
        fifoWaitRounds += rounds;
        return true;
    }
    // A board register read on the lean path (quietReadAddress): inside the quiet span it reads what the step would
    // after charging, so it needs no charge.
    // With the card lookahead the span can run past MMCSD events: STATUS is worked out at the read's own time, as a
    // timer count is, and an MMCSD or DMA register read past a data phase's end goes to the step, which charges first.
    __attribute__((noinline)) bool leanBoardRead(uint32_t pa, uint32_t ran, uint32_t& value) {
        if (quietCycles <= ran || failed || graphicsWireStatus != Imx1BootstrapAccess::ok) return false;
        // Every check comes before the access: a RES_FIFO read takes a response halfword, so a read refused after it
        // would take one twice.
        const bool card = peripherals.mmcsdLookaheadOn() &&
                          ((pa >= 0x00214000 && pa <= 0x00214034) || (pa >= 0x00209000 && pa < 0x00209300));
        uint32_t status = 0;
        if (card) {
            if (quietCycles <= uint64_t(ran) + leanCs5.waitCost) return false;
            if (chargeCardReads ||
                !peripherals.mmcsdStatusAfter(deferredCycles + ran, quietMode, deferredHclk + leanCs5.waitClocks, status))
                return inJit && jitChargedRead(pa, ran, value);
        }
        if (peripherals.access(pa, 4, false, value, cpuArm920ClockMode(cpu)) != Imx1BootstrapAccess::ok) return false;
        if (card && pa == 0x00214004) {
            value = status;
            ++leanStatusReads;
        }
        ++leanBoardReads;
        return true;
    }
    // MMCSD STATUS with the card lookahead, worked out at the read's time straight from the board's state (as
    // leanBoardRead would, without the access). False leaves it to leanBoardRead.
    bool leanStatusRead(uint32_t ran, uint32_t& value) {
        if (!peripherals.mmcsdLookaheadOn() || quietCycles <= uint64_t(ran) + leanCs5.waitCost || failed ||
            graphicsWireStatus != Imx1BootstrapAccess::ok ||
            !peripherals.mmcsdStatusAfter(deferredCycles + ran, quietMode, deferredHclk + leanCs5.waitClocks, value))
            return false;
        ++leanStatusReads;
        ++leanBoardReads;
        return true;
    }
    // A read from generated code past a card event the lookahead ran over: the run's cycles are charged first, as
    // jitCardWrite charges them, then the board answers it, and the span is worked out again.
    __attribute__((noinline)) bool jitChargedRead(uint32_t pa, uint32_t ran, uint32_t& value) {
        if (options.busObserver) return false;
        deferredHclk += leanCs5.waitClocks;
        result.cs5WaitClocks += leanCs5.waitClocks;
        work += leanCs5.waitClocks;
        leanCs5 = {};
        deferredCycles += ran;
        cpuArm920JitCharge(cpu, ran, 0);
        if (!chargeDeferred()) return false;
        finishGraphicsBusStall();
        if (graphicsWireStatus != Imx1BootstrapAccess::ok ||
            peripherals.access(pa, 4, false, value, cpuArm920ClockMode(cpu)) != Imx1BootstrapAccess::ok)
            return false;
        ++jitChargedReads;
        respan();
        return true;
    }
    // After a charge in generated code: the interrupt lines and a new quiet span, and the run stops at its next block
    // if the span ends before its budget.
    void respan() {
        updateInterrupts();
        uint64_t side = 0;
        quietCycles = boundaryStatus() == Imx1BootstrapAccess::ok ? quietCpuCycles(quietMode, true, false, &side) : 0;
        boardSlack = side > quietCycles ? side - quietCycles : 0;
        spanCrosses = false;
        cpuArm920JitCharge(cpu, 0, uint32_t(std::min<uint64_t>(quietCycles, UINT32_MAX)));
        if (cpuArm920JitCharged(cpu) + quietCycles + cpuArm920JitMaxUnchecked(cpu) < jitBudget) {
            jitHalt = true;
            cpuSetSlowPath(cpu, SLOW_PATH_REASON_RESCHEDULE);
        }
    }
    // A graphics memory read on the lean path, as leanWrite takes a write: inside the quiet span no batch completes,
    // so the memory holds what the step would read, and only the wait clocks are left, deferred as for writes.
    __attribute__((noinline)) bool leanGraphicsRead(uint32_t pa, uint32_t size, uint32_t ran, uint32_t left,
                                                    uint32_t& value, uint32_t& cost) {
        if (!cs5Configuration.virtualProfile() || options.cs5ConfigurationObserver || failed || quietCycles <= ran)
            return false;
        if (crossRefused()) return false;
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
        if (!leanSubmit && (pendingBoardCycles || pendingBoardHclk)) p.flushSubmitBoard();
        // A register write can change which pages are plain (SDRAM modes, the PCM DMA buffer).
        if (p.fastEngine && write && Imx1Bootstrap::plainPagesWrite(address)) cpuArm920LeanFlush(p.cpu);
        // ROM and RAM hold no time. Everything else sees the board at this instruction's own time, and so
        // does the PCM DMA when its buffer is written.
        // A timer count read inside the quiet cycles is worked out without charging them.
        uint32_t timerCount = 0;
        bool timerCountKnown = false;
        if (!deferSubmitAccess && (deferredCycles || deferredHclk || quietCycles) &&
            !(Imx1Bootstrap::isRamAddress(address) && !(write && p.peripherals.pcmDmaReads(address, size))) &&
            !(!write && p.resolve(address, size))) {
            timerCountKnown = !write && quietCycles && !deferredHclk && !p.failed && !memIsInstructionFetch(p.mem) &&
                timerCountAfter(p.peripherals, address, size, deferredCycles, quietMode, timerCount);
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
            // A preview works on a copy. A commit works on the chip in place and puts the copy back unless
            // the result is taken (accept), as it would have dropped a refused candidate. A write that only reaches
            // the engine (a ring submit) works on the chip in place in both, keeping just the engine and the
            // acquisition source, which a preview always puts back.
            const bool engineOnly = write && p.cs5Configuration.engineSubmitWrite(p.result.trace.pc, address);
            std::optional<Imx1Cs5Configuration> saved;
            Imx1Cs5Configuration::EngineUndo engineSaved{};
            if (engineOnly) engineSaved = p.cs5Configuration.engineUndo();
            else saved.emplace(p.cs5Configuration);
            auto& candidate = preview && !engineOnly ? *saved : p.cs5Configuration;
            bool kept = preview && !engineOnly;
            struct Restore {
                bool& kept; Imx1Cs5Configuration& chip; const std::optional<Imx1Cs5Configuration>& saved;
                const Imx1Cs5Configuration::EngineUndo& engineSaved;
                ~Restore() {
                    if (kept) return;
                    if (saved) chip = *saved;
                    else chip.undoEngine(engineSaved);
                }
            } restore{kept, p.cs5Configuration, saved, engineSaved};
            // acceptGraphics(candidate, !preview), with the commit already in place.
            auto accept = [&] {
                if (preview) return p.acceptGraphics(candidate, false);
                const auto drive = p.driveGraphicsLine(candidate, true);
                if (drive != Imx1BootstrapAccess::ok) return drive;
                kept = true;
                return p.graphicsCommitted();
            };
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
                 p.result.trace.pc == 0x200a9a9c || Imx1GraphicsEngine::stretchProducer(p.result.trace.pc) ||
                 Imx1GraphicsEngine::otherProducer(p.result.trace.pc) || player) &&
                p.graphicsMemory) {
                // The batch is decoded in place and undone where it's dropped, so the queued batches aren't copied.
                auto& commands = p.graphicsCommands;
                commands.keep(p.graphicsUndo);
                const auto prepared = commands.prepareCopy(candidate,p.peripherals.eimState(),*p.graphicsMemory);
                if (prepared != Imx1BootstrapAccess::ok) {
                    p.graphicsRefusal = commands.refusal();
                    commands.undo(p.graphicsUndo);
                    return prepared;
                }
                candidate.startEngine(commands.duration());
                // acceptGraphics runs graphicsCommitted on the commands from before the batch, which changes them
                // only with the ring off or a batch done. Where either holds, the new commands wait in a copy.
                if (!candidate.engineState().ringBytes() || candidate.engineState().completed()) {
                    auto decoded = commands;
                    commands.undo(p.graphicsUndo);
                    const auto accepted = accept();
                    if (accepted == Imx1BootstrapAccess::ok && !preview) p.graphicsCommands = std::move(decoded);
                    return accepted;
                }
                const auto accepted = accept();
                if (accepted != Imx1BootstrapAccess::ok || preview) commands.undo(p.graphicsUndo);
                return accepted;
            }
            return status == Imx1BootstrapAccess::ok ? accept() : status;
        };
        Imx1BootstrapAccess status;
        if (diagnosticCs5 && p.cs5Configuration.virtualProfile()) {
            uint32_t previewValue = value;
            const auto transfer = p.peripherals.eimState().cs5Transfer(address,size,write,
                (p.result.trace.cpsr & 31) == ARM_SR_MODE_USR);
            status = transfer == Imx1EimAccess::width ? Imx1BootstrapAccess::width :
                transfer != Imx1EimAccess::ok ? Imx1BootstrapAccess::sideEffect :
                leanSubmit ? deviceAccess(false,value) : deviceAccess(true,previewValue);
            if (status == Imx1BootstrapAccess::ok && !leanSubmit) {
                status = p.cs5Wait(waitClocks);
                // Recreate the candidate after elapsed events, retaining current clocks,
                // display, memory-controller progress and read-side-effect state.
                if (status == Imx1BootstrapAccess::ok) status = deviceAccess(false,value);
            }
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
        lastBusAddress = address;
        if (write && address == 0x00205034 && (p.result.trace.pc & 0x00ffffff) == 0x3b8 && status == Imx1BootstrapAccess::ok)
            imx1NoteStartup(&p);
        if (p.options.busObserver)
            p.options.busObserver(p.options.busContext, {p.result.trace.step, p.result.trace.pc,
                                   address, uint32_t(size), value, write, status, diagnosticCs4, diagnosticCs5Reset, diagnosticCs5Configuration, knownMask, waitClocks});
        if (status == Imx1BootstrapAccess::ok || status == Imx1BootstrapAccess::command) {
            // Probe RAM has no ArmRam write watches. Retain guest I-cache words.
            if (write && status == Imx1BootstrapAccess::ok &&
                Imx1Bootstrap::isRamAddress(address)) {
                // Fast drops code only when the store lands on one of its lines (cpuArm920LeanStoredBytes).
                if (!p.fastJit) cpuDropTranslatedCode(p.cpu);
                if (p.fastEngine) cpuArm920LeanStoredBytes(p.cpu, address, size);
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
// Undefined on the ARM920T (ARMv4T, ARM DDI0100I A3.16 and A6.2): the ARM undefined space and Thumb's undefined
// conditional branch. The core raises the undefined instruction exception for both.
bool undefinedInstruction(uint32_t i, bool thumb) {
    return thumb ? (i & 0xff00) == 0xde00 : i >> 28 != 15 && (i & 0x0e000010) == 0x06000010;
}
// PLD (ARMv5TE, NV condition) never executes on the ARM920T, as in the CPU model: TCPMP runs it when it plays
// through its ATI video drivers.
bool neverExecutedPld(uint32_t i) {
    return i >> 28 == 15 && (i & 0x0d70f000) == 0x0550f000;
}
// An abort or undefined instruction entry as the ARM920T takes it (ARM DDI0100I A2.6): the vector, abort or
// undefined mode, I set, F kept, ARM state, SPSR the old CPSR, and the link: the next instruction for undefined,
// the aborted instruction + 4 for a prefetch abort and + 8 for a data abort, in both states.
bool guestEntry(ArmCpu* cpu, const Imx1ProbeTrace& t, uint64_t before, bool undefined, bool fetchAbort) {
    if (cpuExceptionSequence(cpu) != before + 1) return false;
    const auto* r = cpuGetRegisters(cpu);
    const uint32_t vector = r[15] >= 0xffff0000 ? r[15] - 0xffff0000 : r[15];
    const uint32_t mode = undefined ? 0x1b : 0x17;
    const uint32_t want = undefined ? 0x04 : fetchAbort ? 0x0c : 0x10;
    const uint32_t link = t.pc + (undefined ? t.instructionSize : fetchAbort ? 4 : 8);
    return vector == want && r[14] == link && cpuGetRegExternal(cpu, ARM_REG_NUM_SPSR) == t.cpsr &&
           cpuGetRegExternal(cpu, ARM_REG_NUM_CPSR) == ((t.cpsr & ~0xbfu) | 0x80 | mode);
}
bool armMode(uint32_t mode) {
    return mode == 0x10 || mode == 0x11 || mode == 0x12 || mode == 0x13 ||
           mode == 0x17 || mode == 0x1b || mode == 0x1f;
}
// alignmentFaults: CP15 c1 A is set and the guest takes aborts. A misaligned word or halfword load or store
// then takes an alignment data abort on the ARM920T (DDI0151C 3.6), which the CPU model raises, so it runs.
// With A clear the ARMv4 result of a misaligned halfword is unpredictable and stays refused.
bool bootstrapInstructionChecked(uint32_t i, const uint32_t* registers, uint32_t cpsr, uint32_t spsr,
                                 bool alignmentFaults) {
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
            // An APCS-26 return (ldmdb fp, {..., pc}^) in User or System mode, which has no SPSR, loads as a plain
            // LDM, as the shared interpreter runs it (ASSUMPTION "ARM920T LDM^ with PC in User and System mode":
            // the ARM ARM calls it unpredictable; Big Money's ARMlets end with it and ran on OS 5 Palms).
            if ((mode == 0x10 || mode == 0x1f) && load && (i & 0x8000))
                return base != 15 && (!(i & 0x00200000) || !(i & (1u << base)));
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
        return !(i & 0x20) || !(address & 1) || alignmentFaults;
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
bool bootstrapInstruction(uint32_t i, const uint32_t* registers, uint32_t cpsr, uint32_t spsr) {
    return bootstrapInstructionChecked(i, registers, cpsr, spsr, false);
}
bool bootstrapThumb(uint16_t i, const uint32_t* r, bool blPrefix, bool alignmentFaults = false) {
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
        if (op == 0 || op == 4) return !(address & 3) || alignmentFaults;
        if (op == 1 || op == 5 || op == 7) return !(address & 1) || alignmentFaults;
        return true;
    }
    // Single loads and stores: see bootstrapInstructionChecked for alignmentFaults. LDM, STM, PUSH and POP
    // with a misaligned base stay refused.
    if ((i & 0xe000) == 0x6000) return (i & 0x1000) || !(r[(i >> 3) & 7] & 3) || alignmentFaults;
    if ((i & 0xf000) == 0x8000) return !(r[(i >> 3) & 7] & 1) || alignmentFaults;
    if ((i & 0xf000) == 0x9000) return !(r[13] & 3) || alignmentFaults;
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
    jitChargedWrites = 0;
    deferredHclk = 0;
    preCycles = preHclk = 0;
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
    p->dataGuard = cpuArm920DataGuardAt(p->cpu);
    // ARM920T cost table only: the board's own bus, CS5 and cache models time its memory.
    cpuSetCostModel(p->cpu, true, uint8_t(cost::Core::arm920t), false);
    if (p->options.bootstrapPeripherals) cpuArm920EnableWait(p->cpu);
    cpuArm920SetMemoryCheck(p->cpu, Probe::cacheMemoryCheck, p);
    cpuArm920SetIcacheIndexCompatibility(p->cpu, p->options.arm920IcacheIndexCompatibility);
    p->result.stop = Imx1ProbeStop::budget;
    p->observer = observer;
    p->observerContext = context;
    if (observer) {
        if (std::getenv("FROND_TWGLUE_LOG"))
            std::fprintf(stderr, "FROND_TWGLUE_LOG: this run has its own observer, so the TwGlue log is off.\n");
    } else if (auto* log = twGlueLogCreate(p->cpu, &p->peripherals.referenceTime(), Probe::peekPhysical, p)) {
            p->observer = twGlueLogObserve;
            p->observerContext = log;
        }
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
    if (p->pcStop.armed) {
        if (!p->pcStop.leave && p->pcStop.at(cpuGetRegisters(p->cpu)[15] & ~1u)) { p->pcStop.hit = true; return false; }
        p->pcStop.leave = false;
    }
    if (p->codeHooks.count) {
        if (!p->codeHooks.leave && p->codeHooks.at(cpuGetRegisters(p->cpu)[15] & ~1u)) { p->codeHooks.hit = true; return false; }
        p->codeHooks.leave = false;
    }
    const uint64_t step = p->retired;
    const uint64_t workBefore = p->work, busBefore = p->result.busAccesses, cs5Before = p->result.cs5WaitClocks,
                   idleBefore = p->result.idleHclk, entriesBefore = p->result.irqEntries + p->result.fiqEntries;
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
                uint64_t side = 0;
                quietCycles = p->quietCpuCycles(quietMode, true, false, &side);
                boardSlack = side > quietCycles ? side - quietCycles : 0;
                spanCrosses = false;
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
    // A fetch that aborts goes to the prefetch abort vector in the step below, outside a Thumb BL pair.
    bool fetchAbort = false;
    // The step's own fetch below takes this word again; a return before it forgets it.
    struct DropPrefetch { ArmCpu* cpu; ~DropPrefetch() { cpuDropPrefetch(cpu); } } dropPrefetch{p->cpu};
    if (!cpuPrefetchInstruction(p->cpu, t.pc, &t.instruction, &fsr)) {
        t.instruction = 0;
        t.fetched = false;
        if (p->failed) return false;
        if (!p->guestExceptions || p->blPrefix) {
            p->result.stop = Imx1ProbeStop::exception;
            p->result.address = t.pc;
            p->result.value = fsr;
            p->result.size = t.instructionSize;
            return false;
        }
        fetchAbort = true;
    }
    if (!fetchAbort && t.instructionSize == 4 && (t.instruction & 0x0f100f10) == 0x0e000f10) {
        const unsigned source = (t.instruction >> 12) & 15;
        t.operand = source == 15 ? t.pc + 8 : cpuGetRegisters(p->cpu)[source];
    }
    if (p->observer) p->observer(p->observerContext, t);
    const bool thumb = t.instructionSize == 2;
    const bool alignmentFaults = p->guestExceptions && cpuArm920AlignmentFaults(p->cpu);
    const bool refused = !fetchAbort && (p->prefixCapacityExceeded ||
        (p->blPrefix && (!thumb || t.pc != p->blNextPc || (t.instruction & 0xf800) != 0xf800)) ||
        (thumb ? !bootstrapThumb(t.instruction, cpuGetRegisters(p->cpu), p->blPrefix && t.pc == p->blNextPc,
                                 alignmentFaults) :
         (t.instruction >> 28 == 15 && !neverExecutedPld(t.instruction)) ||
         (condition(t.instruction, t.cpsr) &&
          !bootstrapInstructionChecked(t.instruction, cpuGetRegisters(p->cpu), t.cpsr,
                                       cpuGetRegExternal(p->cpu, ARM_REG_NUM_SPSR), alignmentFaults))));
    // A refused ARM instruction passed its condition, so an undefined one here takes the exception.
    const bool undefined = refused && p->guestExceptions && !p->prefixCapacityExceeded && !p->blPrefix &&
                           undefinedInstruction(t.instruction, thumb);
    if (refused && !undefined) {
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
    const auto cycles = cpuCycle<ARM_MEMORY_SYSTEM_ARM920T, false>(p->cpu, 1);
    if (p->failed) return false;
    if (p->dataGuard->active) {
        const auto dataGuard = *p->dataGuard;
        p->result.stop = Imx1ProbeStop::unsupportedDataCache;
        p->result.address = dataGuard.address;
        p->result.size = dataGuard.size;
        p->result.value = dataGuard.write;
        return false;
    }
    const bool softwareEntry = softwareInterrupt(t.instruction, thumb) &&
                               (thumb || condition(t.instruction, t.cpsr));
    const bool entered = cpuExceptionSequence(p->cpu) != exceptions;
    const bool guestTaken = entered && p->guestExceptions &&
                            !(!fetchAbort && !thumb && (t.instruction & 0x0f000f10) == 0x0e000f10) &&
                            guestEntry(p->cpu, t, exceptions, undefined, fetchAbort);
    if ((entered && !guestTaken &&
         !(softwareEntry && cpuExceptionSequence(p->cpu) == exceptions + 1 &&
           (cpuGetRegisters(p->cpu)[15] == 8 || cpuGetRegisters(p->cpu)[15] == 0xffff0008) &&
           cpuGetRegisters(p->cpu)[14] == t.pc + t.instructionSize &&
           cpuGetRegExternal(p->cpu, ARM_REG_NUM_SPSR) == t.cpsr &&
           cpuGetRegExternal(p->cpu, ARM_REG_NUM_CPSR) == ((t.cpsr & ~0xbfu) | 0x93))) ||
        ((undefined || fetchAbort) && !guestTaken)) {
        p->result.stop = undefined ? Imx1ProbeStop::unsupportedInstruction :
                         !fetchAbort && !thumb && (t.instruction & 0x0f000f10) == 0x0e000f10 ?
                         Imx1ProbeStop::unsupportedCp15 : Imx1ProbeStop::exception;
        if (fetchAbort) { p->result.address = t.pc; p->result.value = fsr; p->result.size = t.instructionSize; }
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
    stepped[step % 3] = {step, t.pc, t.instruction, cycles, uint32_t(p->result.busAccesses - busBefore), lastBusAddress,
                         !thumb && !entered && !fetchAbort && p->work == workBefore + 1 &&
                             p->result.cs5WaitClocks == cs5Before && p->result.idleHclk == idleBefore &&
                             p->result.irqEntries + p->result.fiqEntries == entriesBefore};
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
            uint64_t side = 0;
            const uint64_t span = p->quietCpuCycles(quietMode, true, false, &side);
            if (span > cycles && p->boundaryStatus() == Imx1BootstrapAccess::ok) {
                deferredCycles = cycles;
                quietCycles = span - cycles;
                boardSlack = side > span ? side - span : 0;
                spanCrosses = false;
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
    // A round seen in an earlier slice may be from before a state load.
    for (auto& e : stepped) e.step = UINT64_MAX;
    // Crossing starts afresh each slice, so a state load runs as the slice after a save would.
    spanCrosses = crossBlocked = false;
    // The card lookahead is for plain Fast runs alone, not stepped ones with an observer (a state load brings a board
    // with it off).
    p->peripherals.setMmcsdLookahead(cardLookahead && p->fastEngine && p->options.bootstrapPeripherals &&
                                     !p->observer && !p->options.diagnosticSnapshot && !p->options.busObserver);
    slot2LeanOn = slot2Lean && p->fastEngine && p->options.bootstrapPeripherals && !p->observer &&
                  !p->options.diagnosticSnapshot && !p->options.busObserver;
    if (!p->fastEngine || !p->options.bootstrapPeripherals || p->observer || p->options.diagnosticSnapshot ||
        p->options.busObserver) {
        // Exact: UART status polls pass at once (skipUartPollStepped). Not with an observer, which sees every step.
        const bool pollSkip = !p->fastEngine && p->options.bootstrapPeripherals && !p->observer &&
                              !p->options.diagnosticSnapshot && !p->options.busObserver;
        for (uint32_t slice = 0; slice < retiredBudget; ++slice) {
            if (!runStep(p, idleHclkLimit)) break;
            if (!pollSkip || slice + 2 >= retiredBudget) continue;
            // The slice always ends on a step.
            uint64_t hard = retiredBudget - slice - 2;
            if (p->bounds) {
                const uint64_t used = p->work - p->sliceWorkStart;
                hard = used + 1 >= p->bounds->work ? 0 : std::min(hard, p->bounds->work - used - 1);
            }
            if (!p->result.gpioEventApplied && p->options.gpioEventAfterInstructions > p->retired)
                hard = std::min(hard, p->options.gpioEventAfterInstructions - p->retired);
            slice += uint32_t(p->skipUartPollStepped(hard));
        }
    } else {
        // Fast: after each step, the rest of a quiet span runs in the JIT and on the lean path. Their instructions
        // are charged their cost model clocks through the deferred charge, as the step charges them, and they stop before anything the step has
        // to see (MMIO, CP15, faults, a guard refusal), which the next step runs. The JIT may pass the span by
        // up to its block length (the charge then comes a little late); the slice, the bounds and the GPIO
        // event it never passes. The last instruction before the slice's retired or work limit is left to the
        // step, so the slice ends on a step. When spanBoundary ends a slice without one, result.trace is the last stepped
        // instruction, not the last one the JIT ran.
        const uint32_t jitSlack = p->fastJit ? cpuArm920JitMaxUnchecked(p->cpu) : 0;
        for (uint32_t slice = 0; slice < retiredBudget;) {
            if (!runStep(p, idleHclkLimit)) break;
            ++slice;
            bool ended = false;
            delayHalted = delayHaltOff = delayPassed = false;
            scanHalted = scanHaltOff = false;
            aspHalted = aspHaltOff = false;
            cardHalted = cardHaltOff = false;
            slot2Halted = slot2HaltOff = false;
            surfaceWait.armed = false;
            for (;;) {
                // A span the JIT ran to its end, or one a step started with the interrupt lines still to look at,
                // goes on at once where it can (spanBoundary).
                if ((quietCycles < leanMinimumSpan || linesPending) && p->fastJit && !p->blPrefix &&
                    slice + 1 < retiredBudget && !p->pcStop.armed) {
                    surfaceWait.armed = false;
                    const int next = p->spanBoundary();
                    if (next < 0) { ended = true; break; }
                    if (!next) break;
                    cardHaltOff = false;
                    slot2HaltOff = false;
                }
                if (quietCycles < leanMinimumSpan || linesPending || p->blPrefix || slice + 1 >= retiredBudget ||
                    cpuArm920ClockMode(p->cpu) != quietMode || p->pcStop.armed) break;
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
                    if (delayHalted) {
                        delayHalted = false;
                        p->flushSubmitBoard();
                        const bool rounds = p->skipFifoWait();
                        const int skipped = p->skipDelay();
                        delayHaltOff = skipped == 1;
                        delayPassed = skipped == 2;
                        delayHaltSkips += skipped == 2;
                        // A skip counts as work: the slice's limits are worked out again before anything runs.
                        if (rounds || skipped == 2) continue;
                    }
                    const bool halted = std::exchange(scanHalted, false);
                    if (const uint64_t passed = p->skipScanPoll(hard - jitSlack)) {
                        slice += uint32_t(passed);
                        leanCs5 = {};
                        continue;
                    }
                    if (const uint64_t passed = p->skipUartPoll(hard - jitSlack)) {
                        slice += uint32_t(passed);
                        continue;
                    }
                    scanHaltOff |= halted;
                    if (std::exchange(aspHalted, false)) {
                        p->flushSubmitBoard();
                        if (const uint64_t passed = p->skipAspPoll(hard - jitSlack)) {
                            slice += uint32_t(passed);
                            continue;
                        }
                        aspHaltOff = true;
                    }
                    if (std::exchange(cardHalted, false)) {
                        p->flushSubmitBoard();
                        if (const uint64_t passed = p->skipCardPoll(hard - jitSlack)) {
                            slice += uint32_t(passed);
                            continue;
                        }
                        cardHaltOff = true;
                    }
                    if (std::exchange(slot2Halted, false)) {
                        p->flushSubmitBoard();
                        if (const uint64_t passed = p->skipSlot2Poll(hard - jitSlack)) {
                            slice += uint32_t(passed);
                            continue;
                        }
                        slot2HaltOff = true;
                    }
                    if (const uint64_t passed = p->skipSurfaceWait(hard - jitSlack)) {
                        slice += uint32_t(passed);
                        continue;
                    }
                    int stop;
                    jitBudget = uint32_t(std::min(quietCycles, hard - jitSlack + 1));
                    leanCs5 = {};
                    inJit = true;
                    // The slice limits instructions, the quiet span instructions and wait clocks together: the
                    // run's graphics accesses are checked against the span alone.
                    // ran is the run's cost model clocks, instructions what it retired (fewer or as many).
                    uint32_t instructions = 0;
                    const uint32_t ran = cpuArm920RunJit(p->cpu, jitBudget, &stop,
                                                         uint32_t(std::min<uint64_t>(quietCycles, UINT32_MAX)),
                                                         &instructions);
                    inJit = false;
                    // The board may stay behind into the next span (spanBoundary); everything else catches it up.
                    delayPassed = false;
                    if (jitHalt) {
                        jitHalt = false;
                        if (!cpuArm920Waiting(p->cpu)) cpuClearSlowPath(p->cpu, SLOW_PATH_REASON_RESCHEDULE);
                    }
                    // Graphics accesses the run answered, as after a lean run.
                    deferredHclk += leanCs5.waitClocks;
                    p->result.cs5WaitClocks += leanCs5.waitClocks;
                    p->work += leanCs5.waitClocks;
                    // A run past the span's end uses the board's slack after it.
                    const auto useSpan = [](uint64_t used) {
                        if (used > quietCycles) {
                            const uint64_t over = used - quietCycles;
                            boardSlack = over < boardSlack ? boardSlack - over : 0;
                        }
                        quietCycles = used < quietCycles ? quietCycles - used : 0;
                    };
                    const bool runWaits = leanCs5.waitCost || leanCs5.waitClocks || cpuArm920JitCharged(p->cpu);
                    useSpan(leanCs5.waitCost);
                    leanCs5 = {};
                    ++jitRuns;
                    if (ran) {
                        // Cycles a charged write took already (jitChargedWrite) aren't deferred again.
                        const uint32_t left = ran - std::min(ran, cpuArm920JitCharged(p->cpu));
                        deferredCycles += left;
                        useSpan(left);
                        p->retired += instructions;
                        jitRetired += instructions;
                        p->work += instructions;
                        slice += instructions;
                        p->blPrefix = false;  // a Thumb BL pair runs whole in the JIT
                    }
                    p->surfaceWaitRan(ran, instructions, runWaits);
                    if (stop == arm920JitBudget) {
                        delayPassed = p->skipDelay() == 2;
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
                    // A UART status poll's load ends the run; the next try starts with skipUartPoll.
                    if (ran && p->atUartPoll()) continue;
                    // A run that left an instruction with the span used up (a ring word or a register read refused
                    // at its end): the span ends through spanBoundary and the next run starts on it.
                    if (ran && quietCycles < leanMinimumSpan) continue;
                    // The instruction the JIT left goes to the lean path, then the JIT again. Code not compiled
                    // yet, or a JIT that keeps stopping straight away, runs on the lean path for a while.
                    leanCount = stop == arm920JitCold || instructions < 4 ? 32 : 1;
                }
                p->flushSubmitBoard();
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
                delayPassed = false;
                // budget is in cost model clocks, and bounds the instructions too (each costs 1 at least).
                uint32_t clocks = 0;
                const uint32_t ran = cpuArm920RunLeanFor(p->cpu, uint32_t(budget), leanCount, &blNext, &busAccesses,
                                                         &clocks);
                p->result.busAccesses += busAccesses;
                // Graphics memory writes add their wait clocks to the work and the deferred charge, and their
                // cost to the span used, as cs5Wait would.
                const bool leanWaits = leanCs5.waitCost || leanCs5.waitClocks;
                deferredHclk += leanCs5.waitClocks;
                p->result.cs5WaitClocks += leanCs5.waitClocks;
                p->work += leanCs5.waitClocks;
                quietCycles -= leanCs5.waitCost;
                leanCs5 = {};
                if (!ran) break;
                p->surfaceWaitRan(clocks, ran, leanWaits);
                deferredCycles += clocks;
                quietCycles -= clocks;
                p->retired += ran;
                leanRetired += ran;
                p->work += ran;
                slice += ran;
                p->blPrefix = blNext != ~0u;
                if (p->blPrefix) p->blNextPc = blNext;
                if (!p->fastJit) break;
            }
            p->flushSubmitBoard();
            if (ended) break;
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
    // read in the register read helper, the scan status reads and the engine's ring pointer polls. And the delay
    // loops' timer reads (delayLoopPc), the batch producers' ring pointer writes (jitChargedWrite) and the ASP FIFO
    // wait's pointer read (aspPollPc).
    static constexpr uint32_t accessPcs[] = {0x200871dc, 0x200adb08, 0x200adb14, 0x200ad7b0,
                                             0x200b8c64, 0x200b8c70, 0x200b8cb0, 0x200b8fcc, 0x200b9030,
                                             0x2009f678, 0x2008492c, 0x200aa504, 0x200a9ed4, 0x200a9788,
                                             aspPollPc, cardWaitLoadPc, cardDataLoadPc,
                                             slot2DataLoadPc, slot2Status2LoadPc, slot2Status1LoadPc,
                                             slot2IdentityPc, slot2Identity1Pc,
                                             // The rest of slot 2's driver's block and identity accesses (jitSlot2Access).
                                             0x204d2178, 0x204d217a, 0x204d2180, 0x204d2184, 0x204d218a, 0x204d218e,
                                             0x204d2300, 0x204d2310, 0x204d2314, 0x204d2316, 0x204d231c, 0x204d237c,
                                             0x204d2382, 0x204d23ae, 0x204d23b0, 0x204d23f4, 0x204d23f6, 0x204d2428,
                                             0x204d242a, 0x204d2502, 0x204d250a, 0x204d250c, 0x204d2512, 0x204d2518,
                                             0x204d251c, 0x204d251e, 0x204d2522, 0x204d252e, 0x204d2534, 0x204d254e,
                                             0x204d2574, 0x204d257e, 0x204d2bbe, 0x204d2bcc};
    cpuArm920SetJitAccessPcs(p->cpu, accessPcs, sizeof accessPcs / sizeof accessPcs[0]);
    cpuArm920SetJitSwiHalt(p->cpu, fast && surfaceWaitSkip ? surfaceWaitSwiReturn : 0);
    p->fastJit = fast && p->fastBlocks && p->options.bootstrapPeripherals && cpuArm920SetJit(p->cpu, true);
    if (!p->fastJit) cpuArm920SetJit(p->cpu, false);
    // Fast keeps no cache contents: dirty data goes to memory first.
    if (fast) cpuArm920LeanReset(p->cpu);
    p->attachGraphicsGpu();
    p->attachGraphicsWorker();
    return true;
}

bool imx1MachineFastEngine(const Imx1Machine* machine) { return machine->probe->fastEngine; }

bool imx1MachineSetCpuClockMultiplier(Imx1Machine* machine, uint32_t num, uint32_t den) {
    auto* p = machine->probe;
    // Between runs every cycle is charged (runSlice ends with chargeDeferred), so the next charge is the first
    // at the new rate.
    if (p->bounds || deferredCycles || deferredHclk) return false;
    p->peripherals.setCpuClockMultiplier(num, den);
    return true;
}

uint64_t imx1MachineLeanRetired(const Imx1Machine*) { return leanRetired; }

uint64_t imx1MachineJitRetired(const Imx1Machine*) { return jitRetired; }

uint64_t imx1MachineJitRuns(const Imx1Machine*) { return jitRuns; }
uint64_t imx1MachineJitReads(const Imx1Machine*) { return jitReads; }
uint64_t imx1MachineJitRegisterReads(const Imx1Machine*) { return jitRegisterReads; }
uint64_t imx1MachineJitBoardWrites(const Imx1Machine*) { return jitBoardWrites; }
uint64_t imx1MachineJitCardWrites(const Imx1Machine*) { return jitCardWrites; }
uint64_t imx1MachineCardPollSkips(const Imx1Machine*) { return cardPollSkips; }
uint64_t imx1MachineCardPollRounds(const Imx1Machine*) { return cardPollRounds; }
void imx1SetCardPollSkip(bool skip) { cardPollSkip = skip; }
void imx1SetCardLookahead(bool on) { cardLookahead = on; }
void imx1SetChargedCardReads(bool on) { chargeCardReads = on; }
void imx1SetSlot2Lean(bool on) { slot2Lean = on; }
uint64_t imx1MachineLeanSlot2Reads(const Imx1Machine*) { return leanSlot2Reads; }
uint64_t imx1MachineLeanSlot2Writes(const Imx1Machine*) { return leanSlot2Writes; }
uint64_t imx1MachineJitSlot2Accesses(const Imx1Machine*) { return jitSlot2Accesses; }
uint64_t imx1MachineSlot2PollRounds(const Imx1Machine*) { return slot2PollRounds; }
void imx1SetSlot2PollSkip(bool skip) { slot2PollSkip = skip; }
uint64_t imx1MachineLeanStatusReads(const Imx1Machine*) { return leanStatusReads; }
uint64_t imx1MachineJitCardLatches(const Imx1Machine*) { return jitCardLatches; }
uint64_t imx1MachineJitChargedReads(const Imx1Machine*) { return jitChargedReads; }
uint64_t imx1MachineJitChargedWrites(const Imx1Machine*) { return jitChargedWrites; }
uint64_t imx1MachineDelaySkips(const Imx1Machine*) { return delaySkips; }
uint64_t imx1MachineDelayHaltSkips(const Imx1Machine*) { return delayHaltSkips; }
uint64_t imx1MachineDelaySkipCycles(const Imx1Machine*) { return delaySkipCycles; }
uint64_t imx1MachineScanPollSkips(const Imx1Machine*) { return scanPollSkips; }
uint64_t imx1MachineScanPollRounds(const Imx1Machine*) { return scanPollRounds; }
uint64_t imx1MachineScanHalts(const Imx1Machine*) { return scanHalts; }
void imx1SetScanPollSkip(bool skip) { scanPollSkip = skip; }
uint64_t imx1MachineUartPollSkips(const Imx1Machine*) { return uartPollSkips; }
uint64_t imx1MachineUartPollRounds(const Imx1Machine*) { return uartPollRounds; }
void imx1SetUartPollSkip(bool skip) { uartPollSkip = skip; }
uint64_t imx1MachineAspPollSkips(const Imx1Machine*) { return aspPollSkips; }
uint64_t imx1MachineAspPollRounds(const Imx1Machine*) { return aspPollRounds; }
uint64_t imx1MachineAspPollCycles(const Imx1Machine*) { return aspPollCycles; }
void imx1SetAspPollSkip(bool skip) { aspPollSkip = skip; }
uint64_t imx1MachineSurfaceWaitSkips(const Imx1Machine*) { return surfaceWaitSkips; }
uint64_t imx1MachineSurfaceWaitRounds(const Imx1Machine*) { return surfaceWaitRounds; }
void imx1SetSurfaceWaitSkip(bool skip) { surfaceWaitSkip = skip; surfaceWait = {}; }
uint64_t imx1MachineCrossSpans(const Imx1Machine*) { return crossSpans; }
uint64_t imx1MachineCrossRefusals(const Imx1Machine*) { return crossRefusals; }
void imx1SetCrossCompletions(bool on) { crossCompletions = on; crossBlocked = spanCrosses = false; }
uint64_t imx1MachineLagSubmits(const Imx1Machine*) { return lagSubmits; }
uint64_t imx1MachineLagBoundaries(const Imx1Machine*) { return lagBoundaries; }
uint64_t imx1MachineDeferredSubmits(const Imx1Machine*) { return deferredSubmits; }
uint64_t imx1MachineCrossEnds(const Imx1Machine*) { return crossEnds; }
void imx1SetSubmitLag(bool on) { submitLag = on; }
bool imx1MachineSetGuestExceptions(Imx1Machine* machine, bool on) {
    auto* p = machine->probe;
    if (p->bounds || p->stopped) return false;
    p->guestExceptions = on;
    return true;
}
bool imx1MachineGuestExceptions(const Imx1Machine* machine) { return machine->probe->guestExceptions; }
uint64_t imx1MachineFifoWaitSkips(const Imx1Machine*) { return fifoWaitSkips; }
uint64_t imx1MachineFifoWaitRounds(const Imx1Machine*) { return fifoWaitRounds; }
void imx1SetFifoWaitSkip(bool skip) { fifoWaitSkip = skip; }
uint64_t imx1MachineYmfReadyReads(const Imx1Machine* machine) {
    return machine->probe->peripherals.ymf768().readyRegisterReads();
}

Imx1ReferenceTime imx1MachineYmfLastReadyRead(const Imx1Machine* machine) {
    return machine->probe->peripherals.ymf768().lastReadyRead();
}

void imx1MachineYmfSamples(const Imx1Machine* machine, uint64_t* samples, uint64_t* sounding) {
    const auto& ymf = machine->probe->peripherals.ymf768();
    *samples = ymf.engineSamples();
    *sounding = ymf.engineSoundingSamples();
}

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

std::string imx1MachineChipSummary(const Imx1Machine* machine) {
    const auto* p = machine->probe;
    std::string out;
    char text[64];
    const auto& ymf = p->peripherals.ymf768();
    std::snprintf(text, sizeof text, "ymf768 %s bank %u index %02x", p->peripherals.ymf768Enabled() ? "on" : "off",
                  ymf.selectedBank(), ymf.selectedIndex());
    out += text;
    for (unsigned bank = 0; bank < 2; ++bank) {
        std::snprintf(text, sizeof text, "\nymf768 bank %u", bank);
        out += text;
        for (unsigned i = 0; i < 256; ++i) {
            std::snprintf(text, sizeof text, "%s%02x", i % 32 ? "" : " ", ymf.readRegister(bank, i));
            out += text;
        }
    }
    const auto& clocks = p->cs5Configuration.clockState();
    out += "\ngraphics clocks";
    for (uint32_t offset = 0x80; offset <= 0xb0; offset += 4) {
        std::snprintf(text, sizeof text, " %08x", clocks.word(offset));
        out += text;
    }
    return out;
}

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
void imx1MachineSetFmOutputMuted(Imx1Machine* machine, bool muted) { machine->probe->peripherals.setFmOutputMuted(muted); }

extern uint64_t imx1PcmUnderrunWords;
uint64_t imx1MachinePcmUnderrunWords(const Imx1Machine*) { return imx1PcmUnderrunWords; }

Imx1MachineAudio imx1MachineAudio(const Imx1Machine* machine) {
    const auto& board = machine->probe->peripherals;
    Imx1MachineAudio a;
    board.pcmFrameRate(a.rateNumerator, a.rateDenominator);
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
    p->pcStop.hit = false;
    p->codeHooks.hit = false;
    runSlice(machine, bounds.retired, UINT64_MAX);
    p->bounds = nullptr; p->boundedYield = false;
    run.total = imx1MachineGetStatus(machine);
    run.retiredDelta = run.total.retired - before.retired;
    run.workDelta = run.total.work - before.work;
    run.elapsed = run.total.time.since(before.time);
    if (run.workDelta > bounds.work) run.workOvershoot = run.workDelta - bounds.work;
    if (run.total.time.compare(bounds.deadline) > 0) run.timeOvershoot = run.total.time.intervalSince(bounds.deadline);
    run.reason = p->terminal ? Imx1MachineYield::terminal : p->pcStop.hit ? Imx1MachineYield::pcStop :
        p->codeHooks.hit ? Imx1MachineYield::codeHook :
        run.total.status == Imx1MachineStatus::waiting ? Imx1MachineYield::waiting :
        run.total.time.compare(bounds.deadline) >= 0 ? Imx1MachineYield::deadline :
        run.workDelta >= bounds.work ? Imx1MachineYield::work : Imx1MachineYield::retired;
    return run;
}

bool imx1MachineSetPcStops(Imx1Machine* machine, const uint32_t* pcs, unsigned count) {
    auto* p = machine->probe;
    if (p->bounds || p->stopped || p->terminal || !count || count > imx1MachineMaximumPcStops) return false;
    p->pcStop = {};
    p->pcStop.armed = true;
    p->pcStop.count = count;
    for (unsigned i = 0; i < count; ++i) p->pcStop.pcs[i] = pcs[i] & ~1u;
    return true;
}
bool imx1MachineSetPcStop(Imx1Machine* machine, uint32_t pc) { return imx1MachineSetPcStops(machine, &pc, 1); }
void imx1MachineClearPcStop(Imx1Machine* machine) { machine->probe->pcStop = {}; }
void imx1MachineLeavePcStop(Imx1Machine* machine) {
    auto* p = machine->probe;
    p->pcStop.leave = p->pcStop.armed && p->pcStop.at(cpuGetRegisters(p->cpu)[15] & ~1u);
}
bool imx1MachineAtPcStop(const Imx1Machine* machine) {
    const auto* p = machine->probe;
    return p->pcStop.armed && p->pcStop.hit && p->pcStop.at(cpuGetRegisters(p->cpu)[15] & ~1u);
}
Imx1MachinePcStops imx1MachineSavePcStops(const Imx1Machine* machine) {
    const auto& stop = machine->probe->pcStop;
    Imx1MachinePcStops saved;
    saved.armed = stop.armed; saved.leave = stop.leave; saved.hit = stop.hit; saved.count = stop.count;
    std::copy(std::begin(stop.pcs), std::end(stop.pcs), saved.pcs);
    return saved;
}
void imx1MachineRestorePcStops(Imx1Machine* machine, const Imx1MachinePcStops& saved) {
    auto& stop = machine->probe->pcStop;
    stop.armed = saved.armed; stop.leave = saved.leave; stop.hit = saved.hit;
    stop.count = std::min(saved.count, imx1MachineMaximumPcStops);
    std::copy(std::begin(saved.pcs), std::end(saved.pcs), stop.pcs);
}
bool imx1MachineSetCodeHooks(Imx1Machine* machine, const uint32_t* pcs, unsigned count) {
    auto* p = machine->probe;
    if (p->bounds || count > imx1MachineMaximumCodeHooks || !cpuArm920SetCodeHooks(p->cpu, pcs, count)) return false;
    p->codeHooks = {};
    p->codeHooks.count = count;
    for (unsigned i = 0; i < count; ++i) p->codeHooks.pcs[i] = pcs[i] & ~1u;
    return true;
}
void imx1MachineLeaveCodeHook(Imx1Machine* machine) {
    auto* p = machine->probe;
    p->codeHooks.leave = p->codeHooks.at(cpuGetRegisters(p->cpu)[15] & ~1u);
}
bool imx1MachineAtCodeHook(const Imx1Machine* machine) {
    const auto* p = machine->probe;
    return p->codeHooks.hit && p->codeHooks.at(cpuGetRegisters(p->cpu)[15] & ~1u);
}
bool imx1MachineRegisters(const Imx1Machine* machine, uint32_t (&registers)[16], uint32_t* cpsr) {
    const auto* p = machine->probe;
    if (p->bounds || p->stopped || p->terminal) return false;
    std::memcpy(registers, cpuGetRegisters(p->cpu), sizeof(registers));
    if (cpuIsThumb(p->cpu)) registers[15] |= 1;
    if (cpsr) *cpsr = cpuGetRegExternal(p->cpu, ARM_REG_NUM_CPSR);
    return true;
}
bool imx1MachineSetRegister(Imx1Machine* machine, unsigned reg, uint32_t value) {
    auto* p = machine->probe;
    // Not between the halves of a Thumb BL here. Another task preempted between its halves
    // (interruptedPrefixes) is fine: its entry is matched on its own pc, CPSR, lr and sp when a context
    // return resumes it, which setting this task's registers doesn't touch.
    if (p->bounds || p->stopped || p->terminal || reg > ARM_REG_NUM_CPSR || p->blPrefix) return false;
    // The CPSR keeps its mode and state bits; only the flags and interrupt masks change.
    if (reg == ARM_REG_NUM_CPSR) {
        const uint32_t now = cpuGetRegExternal(p->cpu, ARM_REG_NUM_CPSR);
        value = (value & 0xf00000c0) | (now & ~0xf00000c0u);
    }
    cpuSetReg(p->cpu, uint_fast8_t(reg), value);
    return true;
}
bool imx1MachineAccessMemory(Imx1Machine* machine, uint32_t address, void* data, uint32_t size, bool write) {
    auto* p = machine->probe;
    if (p->bounds || p->stopped || p->terminal) return false;
    if (!size) return true;
    // Only mapped SDRAM and SRAM pages, and ROM for reads, so a call never reaches a register. The probe maps
    // the whole bus through one callback, so the CPU's own RAM range checks don't apply. Every page is checked
    // before anything is copied.
    if (uint64_t(address) + size > 0x100000000ull) return false;
    const uint64_t end = uint64_t(address) + size;
    for (int pass = 0; pass < 2; ++pass) {
        for (uint64_t va = address; va < end; va = (va | 0x3ff) + 1) {
            const auto t = mmuTranslateInspect(cpuGetMMU(p->cpu), uint32_t(va), write);
            if (!MMU_TRANSLATE_RESULT_OK(t)) return false;
            const uint32_t pa = MMU_TRANSLATE_RESULT_PA(t);
            const uint32_t length = uint32_t(std::min<uint64_t>(0x400 - (va & 0x3ff), end - va));
            const uint8_t* rom = write ? nullptr : p->resolve(pa, length);
            if (!rom && (!Imx1Bootstrap::isRamAddress(pa) || !Imx1Bootstrap::isRamAddress(pa + length - 1))) return false;
            if (!pass) continue;
            auto* bytes = static_cast<uint8_t*>(data) + (va - address);
            // ROM reads take no time and have no side effect (leanPage), so they come straight from the image.
            if (rom) {
                std::memcpy(bytes, rom, length);
                continue;
            }
            for (uint32_t i = 0; i < length;) {
                const uint32_t at = uint32_t(va) + i;
                const uint_fast8_t width = !(at & 3) && length - i >= 4 ? 4 : !(at & 1) && length - i >= 2 ? 2 : 1;
                if (!cpuMemOpExternal(p->cpu, bytes + i, at, width, write)) return false;
                i += width;
            }
        }
    }
    return true;
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
Imx1MachineAttention imx1MachineAttention(const Imx1Machine* machine) {
    if (!machine) return {};
    const auto& board = machine->probe->peripherals;
    Imx1MachineAttention a;
    a.power = zodiacPowerLed(board.gpioRegister(3, 0), board.gpioRegister(3, 8), board.gpioRegister(3, 7),
                             board.gpioRegister(3, 2), board.pwmDriving());
    if (!board.ymf768Enabled()) return a;
    a.led = board.ymf768().led();
    a.motor = board.ymf768().motor();
    return a;
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
Imx1BootstrapAccess imx1MachineSetAspUInput(Imx1Machine* machine, uint16_t word) {
    if (!machine) return Imx1BootstrapAccess::sequence;
    auto* p = machine->probe;
    if (p->stopped || p->terminal || !p->options.bootstrapPeripherals || !p->options.aspUSampleKnown)
        return Imx1BootstrapAccess::sequence;
    p->peripherals.setAspUInput(word);
    return Imx1BootstrapAccess::ok;
}
bool imx1MachineSystemStopped(const Imx1Machine* machine) {
    return machine && machine->probe->options.bootstrapPeripherals && machine->probe->peripherals.systemStopped();
}
uint32_t imx1MachineGpioInput(const Imx1Machine* machine, unsigned port) {
    return machine ? machine->probe->peripherals.gpioInputSample(port) : 0;
}
Imx1MachineUartLine imx1MachineUartLine(const Imx1Machine* machine, unsigned unit) {
    if (!machine || (unit != 1 && unit != 2) || !machine->probe->options.bootstrapPeripherals) return {};
    const auto& board = machine->probe->peripherals;
    const auto& uart = board.uartState(unit);
    Imx1MachineUartLine line;
    line.enabled = uart.enabled();
    line.receiverOn = uart.receiverOn();
    line.infrared = uart.infrared();
    line.ctsLow = uart.ctsLow();
    line.rtsLow = uart.rtsInputLow();
    line.format = uart.frameFormat();
    line.bitsPerSecond = line.enabled ? board.uartBitsPerSecond(unit) : 0;
    return line;
}
Imx1BootstrapAccess imx1MachineSetUartRts(Imx1Machine* machine, unsigned unit, bool low) {
    if (!machine || (unit != 1 && unit != 2)) return Imx1BootstrapAccess::sequence;
    auto* p = machine->probe;
    if (p->stopped || p->terminal || !p->options.bootstrapPeripherals) return Imx1BootstrapAccess::sequence;
    p->peripherals.setUartRts(unit, low);
    return Imx1BootstrapAccess::ok;
}
Imx1BootstrapAccess imx1MachineSetUartPeer(Imx1Machine* machine, unsigned unit, Imx1UartTransmit transmit,
                                          Imx1UartPending pending, Imx1UartTake take, void* context) {
    if (!machine || (unit != 1 && unit != 2)) return Imx1BootstrapAccess::sequence;
    auto* p = machine->probe;
    if (!p->options.bootstrapPeripherals) return Imx1BootstrapAccess::sequence;
    // Detaching always works, so a stopped machine lets go of its host.
    if ((transmit || take) && (p->stopped || p->terminal)) return Imx1BootstrapAccess::sequence;
    p->peripherals.setUartPeer(unit, transmit, pending, take, context);
    return Imx1BootstrapAccess::ok;
}
uint16_t imx1MachineAspUInput(const Imx1Machine* machine) {
    return machine && machine->probe->options.bootstrapPeripherals ? machine->probe->peripherals.aspUInput() : 0;
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
void imx1MachineSetMmcsdCardId(Imx1Machine* machine, const char* id) {
    if (machine) machine->probe->mmcsdCardId.assign(id ? id : "", id ? strnlen(id, imx1CardIdBytes) : 0);
}
VSD* imx1MachineMmcsdCard(const Imx1Machine* machine) {
    return machine ? machine->probe->peripherals.mmcsdCard() : nullptr;
}
Imx1BootstrapAccess imx1MachineSetGraphicsSdCard(Imx1Machine* machine, VSD* card, bool commit) {
    if (!machine) return Imx1BootstrapAccess::sequence;
    auto* p = machine->probe;
    if (card == p->cs5Configuration.sdState().attachedCard()) return Imx1BootstrapAccess::ok;
    if (p->stopped || p->terminal || !p->options.bootstrapPeripherals || !p->cs5Configuration.virtualProfile())
        return Imx1BootstrapAccess::sequence;
    auto candidate = p->cs5Configuration;
    candidate.attachSdCard(card);
    return p->acceptGraphics(candidate, commit);
}
void imx1MachineSetGraphicsSdWriteProtect(Imx1Machine* machine, bool on) {
    if (machine) machine->probe->cs5Configuration.sdSlot().setWriteProtect(on);
}
VSD* imx1MachineGraphicsSdCard(const Imx1Machine* machine) {
    return machine ? machine->probe->cs5Configuration.sdState().attachedCard() : nullptr;
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

Imx1BootstrapAccess imx1MachineSetGraphicsAux(Imx1Machine* machine, uint32_t sample, bool commit) {
    if (!machine) return Imx1BootstrapAccess::sequence;
    auto* p = machine->probe;
    if (p->stopped || p->terminal || !p->options.bootstrapPeripherals || !p->options.graphicsRawAuxKnown)
        return Imx1BootstrapAccess::sequence;
    if (sample > 1023) return Imx1BootstrapAccess::argument;
    if (commit) p->cs5Configuration.setAcquisitionAuxInput(sample);
    return Imx1BootstrapAccess::ok;
}
uint32_t imx1MachineGraphicsAux(const Imx1Machine* machine) {
    return machine ? machine->probe->cs5Configuration.acquisitionState().auxSample() : 0;
}

Imx1BootstrapAccess imx1MachineSetGraphicsMode1(Imx1Machine* machine, uint32_t sample) {
    if (!machine) return Imx1BootstrapAccess::sequence;
    auto* p = machine->probe;
    if (p->stopped || p->terminal || !p->options.bootstrapPeripherals || !p->options.graphicsRawMode1Known)
        return Imx1BootstrapAccess::sequence;
    if (sample > 1023) return Imx1BootstrapAccess::argument;
    p->cs5Configuration.setAcquisitionMode1Input(sample);
    return Imx1BootstrapAccess::ok;
}
uint32_t imx1MachineGraphicsMode1(const Imx1Machine* machine) {
    return machine ? machine->probe->cs5Configuration.acquisitionState().mode1Sample() : 0;
}

Imx1UsbFunctionState imx1MachineUsbState(const Imx1Machine* machine) {
    if (!machine) return {};
    const auto& usb = machine->probe->cs5Configuration.usbState();
    return {usb.listening(), usb.enableCount(), usb.address()};
}
// A host change on a copy of the chip, kept when the interrupt line it drives is accepted.
template <class R, class Change> static R usbHostChange(Imx1Machine* machine, R refused, Change change) {
    if (!machine) return refused;
    auto* p = machine->probe;
    if (p->stopped || p->terminal || !p->options.bootstrapPeripherals || !p->cs5Configuration.virtualProfile())
        return refused;
    auto candidate = p->cs5Configuration;
    const R result = candidate.usbHost(change);
    if (p->acceptGraphics(candidate, true) != Imx1BootstrapAccess::ok) return refused;
    return result;
}
bool imx1MachineUsbBusReset(Imx1Machine* machine) {
    return usbHostChange(machine, false, [](Imx1GraphicsUsb& usb) { return usb.busReset(); });
}
UsbPacketResult imx1MachineUsbSetup(Imx1Machine* machine, const uint8_t* data) {
    return usbHostChange(machine, UsbPacketResult::Timeout, [data](Imx1GraphicsUsb& usb) { return usb.setup(data); });
}
UsbPacketResult imx1MachineUsbOut(Imx1Machine* machine, uint8_t endpoint, const uint8_t* data, unsigned size) {
    return usbHostChange(machine, UsbPacketResult::Timeout, [&](Imx1GraphicsUsb& usb) {
        return endpoint ? usb.endpointOut(endpoint, data, size) : usb.ep0Out(data, size);
    });
}
UsbPacketResult imx1MachineUsbIn(Imx1Machine* machine, uint8_t endpoint, uint8_t* data, unsigned* size, bool take) {
    if (!take) {
        if (!machine) return UsbPacketResult::Timeout;
        auto copy = machine->probe->cs5Configuration.usbState();
        return endpoint ? copy.endpointIn(endpoint, nullptr, size, false) : copy.ep0In(nullptr, size, false);
    }
    return usbHostChange(machine, UsbPacketResult::Timeout, [&](Imx1GraphicsUsb& usb) {
        return endpoint ? usb.endpointIn(endpoint, data, size, true) : usb.ep0In(data, size, true);
    });
}
void imx1MachineSetUsbHostPending(Imx1Machine* machine, bool pending) {
    if (machine) machine->probe->usbHostPending = pending;
}
bool imx1MachineUsbOutReady(const Imx1Machine* machine, uint8_t endpoint) {
    return machine && machine->probe->cs5Configuration.usbState().endpointOutReady(endpoint);
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
constexpr uint32_t stateGraphicsMemoryVersion = 1, stateBusVersion = 1, stateCardIdVersion = 1, stateCardPulledVersion = 1;
// Slot 2's section: Imx1GraphicsSd::stateCard and the card's own bytes.
constexpr uint32_t stateGraphicsSdVersion = Imx1GraphicsSd::cardStateVersion;
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
static_assert(sizeof(Probe) - Imx1Bootstrap::unsavedBytes - Probe::unsavedBytes - Imx1Cs5Configuration::unsavedBytes - sizeof(Imx1GraphicsLookup) +
              Imx1GraphicsLookup::layoutBytes() == 26080, "update ProbeState, its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1ProbeResult) == 352, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Arm920SavedState) == 43328, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Bootstrap) - Imx1Bootstrap::unsavedBytes == 2344, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Aitc) == 80, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Cgm) - Imx1Cgm::unsavedBytes == 128, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Gpio) == 416, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Eim) == 56, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Timer) == 28, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Sysctrl) == 12, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Reset) == 4, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Bta) == 4, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Uart) - Imx1Uart::unsavedBytes == 264, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Mmcsd) - Imx1Mmcsd::unsavedBytes == 96, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1I2c) == 64, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Asp) == 100, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Pwm) == 64, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Watchdog) == 40, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Rtc) == 48, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Cs4Capture) == 1040, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Cs5Reset) == 8, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1Cs5Configuration) - Imx1GraphicsSd::unsavedBytes - Imx1Cs5Configuration::unsavedBytes - sizeof(Imx1GraphicsLookup) +
              Imx1GraphicsLookup::layoutBytes() == 5136, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsGpio) == 36, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsWindows) == 32, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(Imx1GraphicsLookup::layoutBytes() == 3080, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsController) == 32, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsEngine) == 96, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsDisplay) == 1504, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsInterrupts) == 12, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsAcquisition) == 80, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsSd) - Imx1GraphicsSd::unsavedBytes == 80, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsClocks) == 104, "update its stateVersion and the size in stateLayoutFingerprint");
static_assert(sizeof(Imx1GraphicsCommands) - Imx1GraphicsCommands::unsavedBytes == 10352, "update its stateVersion and the size in stateLayoutFingerprint");
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
    // Slot 2's card bytes from the SD2 section, for the card in the slot after the load.
    std::vector<uint8_t> graphicsSdCard;
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
        info.cardSection = true;
    } else if (info.status != S::ok) return info;
    if (open("BTAD", 1, false, a)) {
        a.DoBuffer(info.bluetoothAddress.data(), info.bluetoothAddress.size());
        if (!finish(a)) return info;
    } else if (info.status != S::ok) return info;
    // In states saved after a card was taken out of the slot in use.
    if (open("SDPL", stateCardPulledVersion, false, a)) {
        uint32_t pulled = 0;
        a.Do32(pulled).Require(pulled >= Imx1Mmcsd::pulledIdle && pulled <= Imx1Mmcsd::pulledWrite, "card pulled");
        if (!finish(a)) return info;
        staged.board->setMmcsdCardPulled(pulled);
    } else if (info.status != S::ok) return info;
    // The card's id, in states saved with a known card in.
    if (open("SDID", stateCardIdVersion, false, a)) {
        std::string id;
        if (!info.cardSection || !stateText(a, id, imx1CardIdBytes) || !finish(a))
            return fail(S::invalidValue, stateFourcc("SDID"), "card id");
        std::memcpy(info.cardId, id.data(), id.size());
        info.cardIdKnown = true;
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
    // The YMF768's power steps, only in states saved while one was under way.
    if (open("YMFR", Imx1Bootstrap::ymf768ReadyStateVersion, false, a)) {
        if (!staged.ymfSection) return fail(S::invalidValue, stateFourcc("YMFR"), "ready steps without the chip");
        staged.board->stateYmf768Ready(a);
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
    // Slot 2, only in states saved once it has had a card.
    if (open("SD2 ", stateGraphicsSdVersion, false, a)) {
        staged.graphics->sdSlot().stateCard(a);
        a.DoVector(staged.graphicsSdCard, 1u << 20);
        if (!finish(a)) return info;
        info.graphicsSdSection = true;
    } else if (info.status != S::ok) return info;
    // The USB function, only in states saved after its driver first touched it.
    if (open("USBF", Imx1GraphicsUsb::stateVersion, false, a)) {
        staged.graphics->usbWritable().state(a);
        if (!finish(a)) return info;
    } else if (info.status != S::ok) return info;
    staged.commands = std::make_unique<Imx1GraphicsCommands>();
    if (!open("GCMD", Imx1GraphicsCommands::stateVersion, true, a)) return info;
    staged.commands->state(a);
    if (!finish(a)) return info;
    // The last keyed copy's colour compare, in states saved after one. Without it none has been seen.
    if (open("GCMP", Imx1GraphicsCommands::compareStateVersion, false, a)) {
        staged.commands->stateCompare(a);
        if (!finish(a)) return info;
    } else if (info.status != S::ok) return info;
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

Imx1StateStatus imx1MachineSaveState(Imx1Machine* machine, std::vector<uint8_t>& out, const char* buildNote,
                                     std::array<uint8_t, 6> bluetoothAddress) {
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
    // The loaded machine starts without translated code, and on Fast with its caches cleaned and emptied, so
    // this one carries on the same way.
    cpuDropTranslatedCode(p->cpu);
    if (p->fastEngine) cpuArm920LeanReset(p->cpu);
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
        if (!p->mmcsdCardId.empty()) {
            auto id = file.begin(stateFourcc("SDID"), stateCardIdVersion);
            stateText(id, p->mmcsdCardId.c_str());
            file.end();
        }
    }
    if (p->peripherals.mmcsdCardPulled()) {
        auto a = file.begin(stateFourcc("SDPL"), stateCardPulledVersion);
        uint32_t pulled = p->peripherals.mmcsdCardPulled();
        a.Do32(pulled);
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
        if (p->peripherals.ymf768().readyBusy()) {
            auto r = file.begin(stateFourcc("YMFR"), Imx1Bootstrap::ymf768ReadyStateVersion);
            p->peripherals.stateYmf768Ready(r);
            file.end();
        }
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
    if (p->cs5Configuration.sdState().cardUsed()) {
        std::vector<uint8_t> card;
        p->cs5Configuration.sdState().saveCard(card);
        auto a = file.begin(stateFourcc("SD2 "), stateGraphicsSdVersion);
        p->cs5Configuration.sdSlot().stateCard(a);
        a.DoVector(card, 1u << 20);
        file.end();
    }
    if (p->cs5Configuration.usbState().used()) {
        auto a = file.begin(stateFourcc("USBF"), Imx1GraphicsUsb::stateVersion);
        p->cs5Configuration.usbWritable().state(a);
        file.end();
    }
    { auto a = file.begin(stateFourcc("GCMD"), Imx1GraphicsCommands::stateVersion); p->graphicsCommands.state(a); file.end(); }
    if (p->graphicsCommands.compareSeen()) {
        auto a = file.begin(stateFourcc("GCMP"), Imx1GraphicsCommands::compareStateVersion);
        p->graphicsCommands.stateCompare(a);
        file.end();
    }
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
    if (bluetoothAddress != std::array<uint8_t, 6>{}) {
        auto a = file.begin(stateFourcc("BTAD"), 1);
        a.DoBuffer(bluetoothAddress.data(), bluetoothAddress.size());
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
    // B15 stays as saved: the saving machine kept it on the chip's line, or at an older state's level the guest hasn't
    // touched the chip since, so driving it here would make a load differ from the run that saved.
    if (p->peripherals.ymf768Enabled()) staged.board->enableYmf768(false);
    // The Faster multiplier belongs to the run, not the state.
    staged.board->setCpuClockMultiplier(p->peripherals.cpuClockMultiplierNum(), p->peripherals.cpuClockMultiplierDen());
    p->peripherals = std::move(*staged.board);
    p->peripherals.setI2cBus(p->options.i2cBus);
    p->peripherals.setI2cObserver(Probe::i2cEvent, p);
    p->peripherals.setDmaStoreHook(Probe::dmaStored, p);
    p->peripherals.setDmaExternal(&p->graphicsDma);
    info.cardResumed = card && p->peripherals.attachMmcsdCard(card);
    p->cs4Capture = *staged.cs4;
    p->cs5Reset = *staged.cs5Reset;
    // Slot 2's card stays in, carrying on from the SD2 section when it takes it.
    VSD* const graphicsSdCard = p->cs5Configuration.sdState().attachedCard();
    p->cs5Configuration = std::move(*staged.graphics);
    info.graphicsSdResumed = p->cs5Configuration.reattachSdCard(
        graphicsSdCard, info.graphicsSdSection ? &staged.graphicsSdCard : nullptr);
    p->graphicsCommands = std::move(*staged.commands);
    if (p->graphicsWorker) p->graphicsWorker->attach(nullptr);
    if (p->graphicsMemory) p->graphicsMemory.swap(staged.graphicsMemory);
    if (p->graphicsGpu.gpu) p->graphicsGpu.gpu->attach(p->graphicsMemory.get());
    p->attachGraphicsWorker();
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
