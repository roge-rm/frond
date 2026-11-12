/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <functional>
#include <memory>
#include <mutex>

#include <boost/icl/interval_set.hpp>
#include <fmt/format.h>
#include <mcl/assert.hpp>
#include <mcl/bit_cast.hpp>
#include <mcl/scope_exit.hpp>
#include <mcl/stdint.hpp>

#include "dynarmic/backend/x64/a32_emit_x64.h"
#include "dynarmic/backend/x64/a32_jitstate.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/callback.h"
#include "dynarmic/backend/x64/devirtualize.h"
#include "dynarmic/backend/x64/jitstate_info.h"
#include "dynarmic/common/atomic.h"
#include "dynarmic/common/frond_compile_thread.h"
#include "dynarmic/common/x64_disassemble.h"
#include "dynarmic/frontend/A32/translate/a32_translate.h"
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/location_descriptor.h"
#include "dynarmic/ir/opt/passes.h"

namespace Dynarmic::A32 {

using namespace Backend::X64;

static RunCodeCallbacks GenRunCodeCallbacks(A32::UserCallbacks* cb, CodePtr (*LookupBlock)(void* lookup_block_arg), void* arg, const A32::UserConfig& conf) {
    return RunCodeCallbacks{
        std::make_unique<ArgCallback>(LookupBlock, reinterpret_cast<u64>(arg)),
        std::make_unique<ArgCallback>(Devirtualize<&A32::UserCallbacks::AddTicks>(cb)),
        std::make_unique<ArgCallback>(Devirtualize<&A32::UserCallbacks::GetTicksRemaining>(cb)),
        conf.enable_cycle_counting,
    };
}

static std::function<void(BlockOfCode&)> GenRCP(const A32::UserConfig& conf) {
    return [conf](BlockOfCode& code) {
        if (conf.page_table) {
            code.mov(code.r14, mcl::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer) {
            code.mov(code.r13, *conf.fastmem_pointer);
        }
    };
}

static Optimization::PolyfillOptions GenPolyfillOptions(const BlockOfCode& code) {
    return Optimization::PolyfillOptions{
        .sha256 = !code.HasHostFeature(HostFeature::SHA),
        .vector_multiply_widen = true,
    };
}

struct Jit::Impl {
    Impl(Jit* jit, A32::UserConfig conf)
            : block_of_code(GenRunCodeCallbacks(conf.callbacks, &GetCurrentBlockThunk, this, conf), JitStateInfo{jit_state}, conf.code_cache_size, GenRCP(conf))
            , emitter(block_of_code, conf, jit)
            , polyfill_options(GenPolyfillOptions(block_of_code))
            , conf(std::move(conf))
            , jit_interface(jit) {}

    ~Impl() = default;

    HaltReason Run() {
        ASSERT(!jit_interface->is_executing);
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        if (compile_thread && InstallCompiled(false)) {
            PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));
        }

        jit_interface->is_executing = true;
        SCOPE_EXIT {
            jit_interface->is_executing = false;
        };

        const CodePtr current_codeptr = [this] {
            // RSB optimization
            const u32 new_rsb_ptr = (jit_state.rsb_ptr - 1) & A32JitState::RSBPtrMask;
            if (jit_state.GetUniqueHash() == jit_state.rsb_location_descriptors[new_rsb_ptr]) {
                jit_state.rsb_ptr = new_rsb_ptr;
                return reinterpret_cast<CodePtr>(jit_state.rsb_codeptrs[new_rsb_ptr]);
            }

            return GetCurrentBlock();
        }();

        const HaltReason hr = block_of_code.RunCode(&jit_state, current_codeptr);

        PerformRequestedCacheInvalidation(hr);

        return hr;
    }

    HaltReason Step() {
        ASSERT(!jit_interface->is_executing);
        PerformRequestedCacheInvalidation(static_cast<HaltReason>(Atomic::Load(&jit_state.halt_reason)));

        jit_interface->is_executing = true;
        SCOPE_EXIT {
            jit_interface->is_executing = false;
        };

        const HaltReason hr = block_of_code.StepCode(&jit_state, GetCurrentSingleStep());

        PerformRequestedCacheInvalidation(hr);

        return hr;
    }

    void ClearCache() {
        std::unique_lock lock{invalidation_mutex};
        invalidate_entire_cache = true;
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void InvalidateCacheRange(std::uint32_t start_address, std::size_t length) {
        std::unique_lock lock{invalidation_mutex};
        invalid_cache_ranges.add(boost::icl::discrete_interval<u32>::closed(start_address, static_cast<u32>(start_address + length - 1)));
        HaltExecution(HaltReason::CacheInvalidation);
    }

    void Reset() {
        ASSERT(!jit_interface->is_executing);
        jit_state = {};
    }

    void HaltExecution(HaltReason hr) {
        Atomic::Or(&jit_state.halt_reason, static_cast<u32>(hr));
    }

    void ClearHalt(HaltReason hr) {
        Atomic::And(&jit_state.halt_reason, ~static_cast<u32>(hr));
    }

    void ClearExclusiveState() {
        jit_state.exclusive_state = 0;
    }

    std::array<u32, 16>& Regs() {
        return jit_state.Reg;
    }

    const std::array<u32, 16>& Regs() const {
        return jit_state.Reg;
    }

    std::array<u32, 64>& ExtRegs() {
        return jit_state.ExtReg;
    }

    const std::array<u32, 64>& ExtRegs() const {
        return jit_state.ExtReg;
    }

    u32 Cpsr() const {
        return jit_state.Cpsr();
    }

    void SetCpsr(u32 value) {
        return jit_state.SetCpsr(value);
    }

    u32 Fpscr() const {
        return jit_state.Fpscr();
    }

    void SetFpscr(u32 value) {
        return jit_state.SetFpscr(value);
    }

    void DumpDisassembly() const {
        const size_t size = reinterpret_cast<const char*>(block_of_code.getCurr()) - reinterpret_cast<const char*>(block_of_code.GetCodeBegin());
        Common::DumpDisassembledX64(block_of_code.GetCodeBegin(), size);
    }

    std::vector<std::string> Disassemble() const {
        const size_t size = reinterpret_cast<const char*>(block_of_code.getCurr()) - reinterpret_cast<const char*>(block_of_code.GetCodeBegin());
        return Common::DisassembleX64(block_of_code.GetCodeBegin(), size);
    }

private:
    static CodePtr GetCurrentBlockThunk(void* this_voidptr) {
        Jit::Impl& this_ = *static_cast<Jit::Impl*>(this_voidptr);
        return this_.GetCurrentBlock();
    }

    IR::LocationDescriptor GetCurrentLocation() const {
        return IR::LocationDescriptor{jit_state.GetUniqueHash()};
    }

    CodePtr GetCurrentBlock() {
        const IR::LocationDescriptor location = GetCurrentLocation();
        if (const auto block = emitter.GetBasicBlock(location)) {
            return block->entrypoint;
        }
        // Frond: the callbacks can leave new code to the caller for now (they halt the run).
        const A32::LocationDescriptor a32_location{location};
        if (!conf.callbacks->ShouldCompile(a32_location.PC(), a32_location.TFlag())) {
            if (compile_thread && compile_thread->AheadOn()) {
                compile_thread->QueueCold(location, compiled_fn(), snapshot_fn());
            }
            return block_of_code.GetReturnFromRunCodeAddress();
        }
        if (compile_thread) {
            if (const CodePtr entry = CompileOffThread(location)) {
                return entry;
            }
        }
        return GetBasicBlock(location).entrypoint;
    }

    using CompileThread = Common::FrondCompileThread<A32EmitX64::EmittedBlock>;
    static constexpr size_t MINIMUM_REMAINING_CODESIZE = 1 * 1024 * 1024;
    // The worker leaves blocks to the guest thread below this, which clears the cache.
    static constexpr size_t WORKER_MINIMUM_CODESIZE = 256 * 1024;

public:
    void SetCompileThread(Jit::CompileThread mode) {
        ASSERT(!jit_interface->is_executing);
        compile_thread.reset();
        compile_sync = mode == Jit::CompileThread::Sync;
        if (mode != Jit::CompileThread::Off) {
            compile_thread = std::make_unique<CompileThread>([this](CompileThread::Job& job) { CompileJob(job); });
        }
    }

    void SetCompileAhead(unsigned depth) {
        if (compile_thread) {
            compile_thread->SetAhead(depth);
        }
    }

    // Frond: on the guest thread. Queues the block at location and returns the dispatcher's
    // exit (the callbacks halt the run), or in sync mode compiles it (or waits for it) and returns it.
    // Null leaves it to be compiled in place.
    CodePtr CompileOffThread(IR::LocationDescriptor location) {
        const A32::LocationDescriptor a32_location{location};
        const u32 pc = a32_location.PC();
        const bool thumb = a32_location.TFlag();
        if (auto job = compile_thread->TakeReady(location)) {
            std::lock_guard lock{code_mutex};
            if (conf.callbacks->CodeUnchanged(*job->snapshot)) {
                const auto block = emitter.Install(job->result);
                compile_thread->NoteInstalled();
                compile_thread->NoteAheadUsed();
                QueueAhead(*job, 1);
                return block.entrypoint;
            }
            compile_thread->NoteStale();
        }
        if (compile_thread->IsPending(location)) {
            compile_thread->Want(location);
            if (!compile_sync) {
                conf.callbacks->CompileQueued(pc, thumb);
                return block_of_code.GetReturnFromRunCodeAddress();
            }
            // Sync, with an ahead job for it in flight.
            compile_thread->WaitIdle();
            InstallCompiled(true);
            if (const auto block = emitter.GetBasicBlock(location)) {
                return block->entrypoint;
            }
        }
        if (space_remaining.load(std::memory_order_acquire) < MINIMUM_REMAINING_CODESIZE) {
            invalidate_entire_cache = true;
            PerformRequestedCacheInvalidation(HaltReason::CacheInvalidation);
        }
        for (bool whole = compile_thread->Overflowed(location);; whole = true) {
            auto snapshot = conf.callbacks->SnapshotCode(pc, thumb, whole);
            if (!snapshot) {
                return nullptr;
            }
            if (compile_sync && !compile_thread->AheadOn()) {
                compile_thread->CompileHere(location, std::move(snapshot));
            } else {
                compile_thread->Push(location, std::move(snapshot));
                if (!compile_sync) {
                    conf.callbacks->CompileQueued(pc, thumb);
                    return block_of_code.GetReturnFromRunCodeAddress();
                }
                compile_thread->WaitIdle();
            }
            InstallCompiled(true);
            if (const auto block = emitter.GetBasicBlock(location)) {
                return block->entrypoint;
            }
            if (whole || !compile_thread->Overflowed(location)) {
                return nullptr;
            }
        }
    }

    // Frond: on the worker, or on the guest thread in sync mode (CompileHere).
    void CompileJob(CompileThread::Job& job) {
        if (job.ahead && space_remaining.load(std::memory_order_acquire) < conf.code_cache_size / 4) {
            job.status = CompileThread::Status::Skipped;
            compile_thread->NoteHighWater();
            return;
        }
        IR::Block ir_block = Translate(job.location, job.snapshot.get());
        if (job.snapshot->overflow) {
            job.status = CompileThread::Status::Overflow;
            return;
        }
        Common::FrondBlockTargets(ir_block, job.targets);
        std::lock_guard lock{code_mutex};
        if (job.epoch != compile_thread->Epoch()) {
            job.status = CompileThread::Status::Cancelled;
            return;
        }
        if (block_of_code.SpaceRemaining() < WORKER_MINIMUM_CODESIZE) {
            job.status = CompileThread::Status::NoSpace;
            return;
        }
        block_of_code.EnsureMemoryCommitted(MINIMUM_REMAINING_CODESIZE);
        job.result = emitter.EmitUnregistered(ir_block);
        job.first_pc = job.result.first_pc;
        job.last_pc = job.result.last_pc;
        job.status = CompileThread::Status::Done;
        space_remaining.store(block_of_code.SpaceRemaining(), std::memory_order_release);
    }

    // Frond: on the guest thread, between runs (or in sync mode where the block was asked
    // for). Without wait it gives up if the worker is emitting. True if the cache has to be
    // cleared first (it ran out of space).
    bool InstallCompiled(bool wait) {
        if (!compile_thread->HasDone()) {
            return false;
        }
        std::unique_lock lock{code_mutex, std::defer_lock};
        if (wait) {
            lock.lock();
        } else if (!lock.try_lock()) {
            return false;
        }
        bool clear = false;
        for (auto& job : compile_thread->TakeDone()) {
            if (job->status == CompileThread::Status::NoSpace) {
                clear = true;
                continue;
            }
            if (job->status != CompileThread::Status::Done) {
                continue;
            }
            if (job->ahead && !job->wanted) {
                if (!emitter.GetBasicBlock(job->location)) {
                    if (compile_thread->StillValid(*job)) {
                        QueueAhead(*job, job->depth + 1);
                    }
                    compile_thread->KeepReady(std::move(job));
                }
                continue;
            }
            if (!compile_thread->StillValid(*job) || emitter.GetBasicBlock(job->location) || !conf.callbacks->CodeUnchanged(*job->snapshot)) {
                compile_thread->NoteStale();
                continue;
            }
            emitter.Install(job->result);
            compile_thread->NoteInstalled();
            if (job->ahead) {
                compile_thread->NoteAheadUsed();
            }
            QueueAhead(*job, 1);
        }
        lock.unlock();
        if (clear) {
            std::unique_lock inval_lock{invalidation_mutex};
            invalidate_entire_cache = true;
            HaltExecution(HaltReason::CacheInvalidation);
        }
        return clear;
    }

    s64 TicksRemainingNow() const { return block_of_code.TicksRemainingNow(); }

    Jit::CompileStats GetCompileStats() const {
        Jit::CompileStats stats = compile_thread ? compile_thread->GetStats() : Jit::CompileStats{};
        stats.cache_clears = cache_clears;
        return stats;
    }

private:
    // Frond: on the guest thread. Queues the blocks job's block goes to, depth from one asked for.
    void QueueAhead(const CompileThread::Job& job, unsigned depth) {
        if (!compile_thread->AheadOn()) {
            return;
        }
        compile_thread->QueueAhead(job.targets, depth, compiled_fn(), snapshot_fn());
    }

    std::function<bool(IR::LocationDescriptor)> compiled_fn() {
        return [this](IR::LocationDescriptor location) { return emitter.GetBasicBlock(location).has_value(); };
    }

    std::function<std::unique_ptr<A32::CodeSnapshot>(IR::LocationDescriptor)> snapshot_fn() {
        return [this](IR::LocationDescriptor location) {
            const A32::LocationDescriptor a32_location{location};
            return conf.callbacks->SnapshotCodeAhead(a32_location.PC(), a32_location.TFlag(), compile_thread->Overflowed(location));
        };
    }

    IR::Block Translate(IR::LocationDescriptor descriptor, A32::TranslateCallbacks* tcb) {
        IR::Block ir_block = A32::Translate(A32::LocationDescriptor{descriptor}, tcb, {conf.arch_version, conf.define_unpredictable_behaviour, conf.hook_hint_instructions, conf.psr_coprocessor, conf.max_unchecked_instructions, conf.coprocessor_unscaled_offsets});
        Optimization::PolyfillPass(ir_block, polyfill_options);
        Optimization::NamingPass(ir_block);
        if (conf.HasOptimization(OptimizationFlag::GetSetElimination)) {
            Optimization::A32GetSetElimination(ir_block, {.convert_nz_to_nzc = true, .memory_barriers = conf.check_halt_on_memory_access});
            Optimization::DeadCodeElimination(ir_block);
        }
        if (conf.HasOptimization(OptimizationFlag::ConstProp)) {
            Optimization::A32ConstantMemoryReads(ir_block, conf.callbacks);
            Optimization::ConstantPropagation(ir_block);
            Optimization::DeadCodeElimination(ir_block);
        }
        Optimization::IdentityRemovalPass(ir_block);
#ifndef NDEBUG
        Optimization::VerificationPass(ir_block);
#endif
        return ir_block;
    }

    CodePtr GetCurrentSingleStep() {
        return GetBasicBlock(A32::LocationDescriptor{GetCurrentLocation()}.SetSingleStepping(true)).entrypoint;
    }

    A32EmitX64::BlockDescriptor GetBasicBlock(IR::LocationDescriptor descriptor) {
        auto block = emitter.GetBasicBlock(descriptor);
        if (block)
            return *block;

        if (block_of_code.SpaceRemaining() < MINIMUM_REMAINING_CODESIZE) {
            invalidate_entire_cache = true;
            PerformRequestedCacheInvalidation(HaltReason::CacheInvalidation);
        }

        IR::Block ir_block = Translate(descriptor, conf.callbacks);
        std::lock_guard lock{code_mutex};
        block_of_code.EnsureMemoryCommitted(MINIMUM_REMAINING_CODESIZE);
        const auto block_desc = emitter.Emit(ir_block);
        space_remaining.store(block_of_code.SpaceRemaining(), std::memory_order_release);
        return block_desc;
    }

    void PerformRequestedCacheInvalidation(HaltReason hr) {
        if (Has(hr, HaltReason::CacheInvalidation)) {
            std::unique_lock lock{invalidation_mutex};

            ClearHalt(HaltReason::CacheInvalidation);

            if (!invalidate_entire_cache && invalid_cache_ranges.empty()) {
                return;
            }

            jit_state.ResetRSB();
            // Frond: waits for a block the compile thread is emitting.
            std::lock_guard code_lock{code_mutex};
            if (invalidate_entire_cache) {
                cache_clears++;
                block_of_code.ClearCache();
                emitter.ClearCache();
                if (compile_thread) {
                    compile_thread->Cleared();
                }
                space_remaining.store(block_of_code.SpaceRemaining(), std::memory_order_release);
            } else {
                emitter.InvalidateCacheRanges(invalid_cache_ranges);
                if (compile_thread) {
                    compile_thread->Invalidated(invalid_cache_ranges);
                }
            }
            invalid_cache_ranges.clear();
            invalidate_entire_cache = false;
        }
    }

    A32JitState jit_state;
    BlockOfCode block_of_code;
    A32EmitX64 emitter;
    Optimization::PolyfillOptions polyfill_options;

    const A32::UserConfig conf;

    Jit* jit_interface;

    // Requests made during execution to invalidate the cache are queued up here.
    bool invalidate_entire_cache = false;
    boost::icl::interval_set<u32> invalid_cache_ranges;
    std::mutex invalidation_mutex;

    // Frond: the code cache, emitter and code space are shared with the compile thread
    // under code_mutex. space_remaining follows SpaceRemaining for the guest thread.
    std::mutex code_mutex;
    std::atomic<size_t> space_remaining{block_of_code.SpaceRemaining()};
    // Frond: whole cache clears, for GetCompileStats. Guest thread only.
    u64 cache_clears = 0;
    bool compile_sync = false;
    std::unique_ptr<CompileThread> compile_thread;  // last, so its worker stops first
};

Jit::Jit(UserConfig conf)
        : impl(std::make_unique<Impl>(this, std::move(conf))) {
    // Frond: the decoder tables are built here, so the first compile doesn't stall.
    PrepareDecoders();
}

Jit::~Jit() = default;

HaltReason Jit::Run() {
    return impl->Run();
}

HaltReason Jit::Step() {
    return impl->Step();
}

void Jit::ClearCache() {
    impl->ClearCache();
}

void Jit::InvalidateCacheRange(std::uint32_t start_address, std::size_t length) {
    impl->InvalidateCacheRange(start_address, length);
}

void Jit::Reset() {
    impl->Reset();
}

void Jit::HaltExecution(HaltReason hr) {
    impl->HaltExecution(hr);
}

void Jit::ClearHalt(HaltReason hr) {
    impl->ClearHalt(hr);
}

std::array<std::uint32_t, 16>& Jit::Regs() {
    return impl->Regs();
}

const std::array<std::uint32_t, 16>& Jit::Regs() const {
    return impl->Regs();
}

std::array<std::uint32_t, 64>& Jit::ExtRegs() {
    return impl->ExtRegs();
}

const std::array<std::uint32_t, 64>& Jit::ExtRegs() const {
    return impl->ExtRegs();
}

std::uint32_t Jit::Cpsr() const {
    return impl->Cpsr();
}

void Jit::SetCpsr(std::uint32_t value) {
    impl->SetCpsr(value);
}

std::uint32_t Jit::Fpscr() const {
    return impl->Fpscr();
}

void Jit::SetFpscr(std::uint32_t value) {
    impl->SetFpscr(value);
}

void Jit::ClearExclusiveState() {
    impl->ClearExclusiveState();
}

void Jit::SetCompileThread(CompileThread mode) {
    impl->SetCompileThread(mode);
}

void Jit::SetCompileAhead(unsigned depth) {
    impl->SetCompileAhead(depth);
}

Jit::CompileStats Jit::GetCompileStats() const {
    return impl->GetCompileStats();
}

std::int64_t Jit::TicksRemainingNow() const {
    return impl->TicksRemainingNow();
}

void Jit::DumpDisassembly() const {
    impl->DumpDisassembly();
}

}  // namespace Dynarmic::A32
