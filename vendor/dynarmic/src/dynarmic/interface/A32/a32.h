/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/halt_reason.h"

namespace Dynarmic {
namespace A32 {

class Jit final {
public:
    explicit Jit(UserConfig conf);
    ~Jit();

    /**
     * Runs the emulated CPU.
     * Cannot be recursively called.
     */
    HaltReason Run();

    /**
     * Steps the emulated CPU.
     * Cannot be recursively called.
     */
    HaltReason Step();

    /**
     * Clears the code cache of all compiled code.
     * Can be called at any time. Halts execution if called within a callback.
     */
    void ClearCache();

    /**
     * Invalidate the code cache at a range of addresses.
     * @param start_address The starting address of the range to invalidate.
     * @param length The length (in bytes) of the range to invalidate.
     */
    void InvalidateCacheRange(std::uint32_t start_address, std::size_t length);

    /**
     * Reset CPU state to state at startup. Does not clear code cache.
     * Cannot be called from a callback.
     */
    void Reset();

    /**
     * Stops execution in Jit::Run.
     */
    void HaltExecution(HaltReason hr = HaltReason::UserDefined1);

    /**
     * Clears a halt reason from flags.
     * Warning: Only use this if you're sure this won't introduce races.
     */
    void ClearHalt(HaltReason hr = HaltReason::UserDefined1);

    /// View and modify registers.
    std::array<std::uint32_t, 16>& Regs();
    const std::array<std::uint32_t, 16>& Regs() const;
    std::array<std::uint32_t, 64>& ExtRegs();
    const std::array<std::uint32_t, 64>& ExtRegs() const;

    /// View and modify CPSR.
    std::uint32_t Cpsr() const;
    void SetCpsr(std::uint32_t value);

    /// View and modify FPSCR.
    std::uint32_t Fpscr() const;
    void SetFpscr(std::uint32_t value);

    /// Clears exclusive state for this core.
    void ClearExclusiveState();

    /// Frond: compiling on a thread of this Jit's own. Off compiles in place. On queues each
    /// block ShouldCompile wants (UserCallbacks::CompileQueued) and installs it at the start
    /// of a later Run. Sync waits for each one where it was asked for, so runs stop where
    /// they would with Off. Set before the first Run.
    enum class CompileThread { Off, On, Sync };
    void SetCompileThread(CompileThread mode);

    /// Frond: compile ahead on the compile thread when it is idle: blocks reached but not
    /// wanted yet (ShouldCompile false), and blocks up to depth links on from one that was
    /// asked for. 0, the default, is off.
    void SetCompileAhead(unsigned depth);

    /// Frond: counts for the compile thread, since the Jit was made. Ahead: jobs queued,
    /// compiled, later asked for and installed, and left undone (cache past its high-water
    /// mark, or cleared first). Cache clears: whole code cache clears, asked for or for lack of
    /// space, counted with the compile thread off too.
    struct CompileStats {
        std::uint64_t queued = 0, installed = 0, stale = 0, cancelled = 0, overflow = 0, no_space = 0;
        std::uint64_t ahead_queued = 0, ahead_compiled = 0, ahead_used = 0, ahead_skipped = 0;
        std::uint64_t cache_clears = 0;
    };
    CompileStats GetCompileStats() const;

    /// Frond: the cycles left in the current Run, as GetTicksRemaining gave them less the
    /// blocks finished since (a block's cycles are counted at its end). Only valid inside a
    /// memory read or write callback during Run. Negative past the budget.
    std::int64_t TicksRemainingNow() const;

    /**
     * Returns true if Jit::Run was called but hasn't returned yet.
     * i.e.: We're in a callback.
     */
    bool IsExecuting() const {
        return is_executing;
    }

    /// Debugging: Dump a disassembly all compiled code to the console.
    void DumpDisassembly() const;

    /**
     * Disassemble the instructions following the current pc and return
     * the resulting instructions as a vector of their string representations.
     */
    std::vector<std::string> Disassemble() const;

private:
    bool is_executing = false;

    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace A32
}  // namespace Dynarmic
