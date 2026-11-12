/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/arm64/a32_address_space.h"

#include "dynarmic/backend/arm64/a32_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/devirtualize.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/stack_layout.h"
#include "dynarmic/common/cast_util.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A32/translate/a32_translate.h"
#include "dynarmic/interface/A32/config.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include "dynarmic/ir/opt/passes.h"

namespace Dynarmic::Backend::Arm64 {

template<auto mfp, typename T>
static void* EmitCallTrampoline(oaknut::CodeGenerator& code, T* this_) {
    using namespace oaknut::util;

    const auto info = Devirtualize<mfp>(this_);

    oaknut::Label l_addr, l_this;

    void* target = code.xptr<void*>();
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BR(Xscratch0);

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);

    return target;
}

// Frond: a memory read or write callback, which first stores the cycles left (Xticks) in *ticks
// for Jit::TicksRemainingNow.
template<auto mfp, typename T>
static void* EmitReadCallTrampoline(oaknut::CodeGenerator& code, T* this_, s64* ticks) {
    using namespace oaknut::util;

    const auto info = Devirtualize<mfp>(this_);

    oaknut::Label l_addr, l_this, l_ticks;

    void* target = code.xptr<void*>();
    code.LDR(Xscratch0, l_ticks);
    code.STR(Xticks, Xscratch0);
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BR(Xscratch0);

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);
    code.l(l_ticks);
    code.dx(mcl::bit_cast<u64>(ticks));

    return target;
}

template<auto mfp, typename T>
static void* EmitWrappedReadCallTrampoline(oaknut::CodeGenerator& code, T* this_, s64* ticks) {
    using namespace oaknut::util;

    const auto info = Devirtualize<mfp>(this_);

    oaknut::Label l_addr, l_this, l_ticks;

    constexpr u64 save_regs = ABI_CALLER_SAVE & ~ToRegList(Xscratch0);

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, save_regs, 0);
    // Frond: X2 is saved above and not an argument here.
    code.LDR(X2, l_ticks);
    code.STR(Xticks, X2);
    code.LDR(X0, l_this);
    code.MOV(X1, Xscratch0);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    code.MOV(Xscratch0, X0);
    ABI_PopRegisters(code, save_regs, 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);
    code.l(l_ticks);
    code.dx(mcl::bit_cast<u64>(ticks));

    return target;
}

template<auto callback, typename T>
static void* EmitExclusiveReadCallTrampoline(oaknut::CodeGenerator& code, const A32::UserConfig& conf) {
    using namespace oaknut::util;

    oaknut::Label l_addr, l_this;

    auto fn = [](const A32::UserConfig& conf, A32::VAddr vaddr) -> T {
        return conf.global_monitor->ReadAndMark<T>(conf.processor_id, vaddr, [&]() -> T {
            return (conf.callbacks->*callback)(vaddr);
        });
    };

    void* target = code.xptr<void*>();
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BR(Xscratch0);

    code.align(8);
    code.l(l_this);
    code.dx(mcl::bit_cast<u64>(&conf));
    code.l(l_addr);
    code.dx(mcl::bit_cast<u64>(Common::FptrCast(fn)));

    return target;
}

template<auto mfp, typename T>
static void* EmitWrappedWriteCallTrampoline(oaknut::CodeGenerator& code, T* this_, s64* ticks) {
    using namespace oaknut::util;

    const auto info = Devirtualize<mfp>(this_);

    oaknut::Label l_addr, l_this, l_ticks;

    constexpr u64 save_regs = ABI_CALLER_SAVE;

    void* target = code.xptr<void*>();
    ABI_PushRegisters(code, save_regs, 0);
    // Frond: Xticks for Jit::TicksRemainingNow, through X0 before it takes this.
    code.LDR(X0, l_ticks);
    code.STR(Xticks, X0);
    code.LDR(X0, l_this);
    code.MOV(X1, Xscratch0);
    code.MOV(X2, Xscratch1);
    code.LDR(Xscratch0, l_addr);
    code.BLR(Xscratch0);
    ABI_PopRegisters(code, save_regs, 0);
    code.RET();

    code.align(8);
    code.l(l_this);
    code.dx(info.this_ptr);
    code.l(l_addr);
    code.dx(info.fn_ptr);
    code.l(l_ticks);
    code.dx(mcl::bit_cast<u64>(ticks));

    return target;
}

template<auto callback, typename T>
static void* EmitExclusiveWriteCallTrampoline(oaknut::CodeGenerator& code, const A32::UserConfig& conf) {
    using namespace oaknut::util;

    oaknut::Label l_addr, l_this;

    auto fn = [](const A32::UserConfig& conf, A32::VAddr vaddr, T value) -> u32 {
        return conf.global_monitor->DoExclusiveOperation<T>(conf.processor_id, vaddr,
                                                            [&](T expected) -> bool {
                                                                return (conf.callbacks->*callback)(vaddr, value, expected);
                                                            })
                 ? 0
                 : 1;
    };

    void* target = code.xptr<void*>();
    code.LDR(X0, l_this);
    code.LDR(Xscratch0, l_addr);
    code.BR(Xscratch0);

    code.align(8);
    code.l(l_this);
    code.dx(mcl::bit_cast<u64>(&conf));
    code.l(l_addr);
    code.dx(mcl::bit_cast<u64>(Common::FptrCast(fn)));

    return target;
}

A32AddressSpace::A32AddressSpace(const A32::UserConfig& conf)
        : AddressSpace(conf.code_cache_size)
        , conf(conf) {
    EmitPrelude();
}

A32AddressSpace::~A32AddressSpace() {
    compile_thread.reset();  // stops the worker before the code space goes
}

IR::Block A32AddressSpace::GenerateIR(IR::LocationDescriptor descriptor) const {
    return GenerateIR(descriptor, conf.callbacks);
}

IR::Block A32AddressSpace::GenerateIR(IR::LocationDescriptor descriptor, A32::TranslateCallbacks* tcb) const {
    IR::Block ir_block = A32::Translate(A32::LocationDescriptor{descriptor}, tcb, {conf.arch_version, conf.define_unpredictable_behaviour, conf.hook_hint_instructions, conf.psr_coprocessor, conf.max_unchecked_instructions, conf.coprocessor_unscaled_offsets});

    Optimization::PolyfillPass(ir_block, {});
    Optimization::NamingPass(ir_block);
    if (conf.HasOptimization(OptimizationFlag::GetSetElimination)) {
        Optimization::A32GetSetElimination(ir_block, {.convert_nzc_to_nz = true, .memory_barriers = conf.check_halt_on_memory_access});
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

void A32AddressSpace::InvalidateCacheRanges(const boost::icl::interval_set<u32>& ranges) {
    std::lock_guard lock{code_mutex};  // waits for a block the compile thread is emitting
    InvalidateBasicBlocks(block_ranges.InvalidateRanges(ranges));
    if (compile_thread) {
        compile_thread->Invalidated(ranges);
    }
}

void A32AddressSpace::ClearCacheLocked() {
    std::lock_guard lock{code_mutex};
    ClearCacheHeld();
}

void A32AddressSpace::ClearCacheHeld() {
    ClearCache();
    if (compile_thread) {
        compile_thread->Cleared();
    }
}

// The run entry's lookup: the fast dispatch table, then GetOrEmitIfWanted.
CodePtr A32AddressSpace::GetForRun(IR::LocationDescriptor descriptor) {
    if (fast_dispatch_table) {
        const FastDispatchEntry& slot = FastDispatchSlot(descriptor);
        if (slot.location_descriptor == descriptor.Value()) {
            return slot.code_ptr;
        }
    }
    const CodePtr entry = GetOrEmitIfWanted(descriptor);
    FillFastDispatch(descriptor, entry);
    return entry;
}

CodePtr A32AddressSpace::GetOrEmitIfWanted(IR::LocationDescriptor descriptor) {
    if (CodePtr block_entry = Get(descriptor)) {
        return block_entry;
    }
    const A32::LocationDescriptor a32_location{descriptor};
    if (!conf.callbacks->ShouldCompile(a32_location.PC(), a32_location.TFlag())) {
        if (compile_thread && compile_thread->AheadOn()) {
            compile_thread->QueueCold(descriptor, CompiledFn(), SnapshotFn());
        }
        return reinterpret_cast<CodePtr>(prelude_info.return_to_dispatcher);
    }
    if (compile_thread) {
        if (CodePtr entry = CompileOffThread(descriptor)) {
            return entry;
        }
    }
    std::lock_guard lock{code_mutex};
    if (compile_thread && GetRemainingSize() < 1024 * 1024) {
        ClearCacheHeld();
    }
    return GetOrEmit(descriptor);
}

void A32AddressSpace::SetCompileThread(A32::Jit::CompileThread mode) {
    compile_thread.reset();
    compile_sync = mode == A32::Jit::CompileThread::Sync;
    if (mode != A32::Jit::CompileThread::Off) {
        compile_thread = std::make_unique<CompileThread>([this](CompileThread::Job& job) { CompileJob(job); });
    }
}

void A32AddressSpace::SetCompileAhead(unsigned depth) {
    if (compile_thread) {
        compile_thread->SetAhead(depth);
    }
}

// On the guest thread. Queues the blocks job's block goes to, depth from one asked for.
void A32AddressSpace::QueueAhead(const CompileThread::Job& job, unsigned depth) {
    if (!compile_thread->AheadOn()) {
        return;
    }
    compile_thread->QueueAhead(job.targets, depth, CompiledFn(), SnapshotFn());
}

std::function<bool(IR::LocationDescriptor)> A32AddressSpace::CompiledFn() {
    return [this](IR::LocationDescriptor location) { return Get(location) != nullptr; };
}

std::function<std::unique_ptr<A32::CodeSnapshot>(IR::LocationDescriptor)> A32AddressSpace::SnapshotFn() {
    return [this](IR::LocationDescriptor location) {
        const A32::LocationDescriptor a32_location{location};
        return conf.callbacks->SnapshotCodeAhead(a32_location.PC(), a32_location.TFlag(), compile_thread->Overflowed(location));
    };
}

// On the guest thread, with code_mutex held: installs a block that was emitted already.
void A32AddressSpace::InstallJob(const CompileThread::Job& job) {
    Install(job.location, job.result.info);
    block_ranges.AddRange(boost::icl::discrete_interval<u32>::closed(job.first_pc, job.last_pc), job.location);
    compile_thread->NoteInstalled();
    if (job.ahead) {
        compile_thread->NoteAheadUsed();
    }
    QueueAhead(job, 1);
}

A32::Jit::CompileStats A32AddressSpace::GetCompileStats() const {
    A32::Jit::CompileStats stats = compile_thread ? compile_thread->GetStats() : A32::Jit::CompileStats{};
    stats.cache_clears = cache_clears;
    return stats;
}

// On the guest thread. Queues the block and returns the dispatcher (the callbacks halt the
// run), or in sync mode compiles it (or waits for it) and returns it. Null leaves it to be
// compiled in place.
CodePtr A32AddressSpace::CompileOffThread(IR::LocationDescriptor descriptor) {
    const A32::LocationDescriptor a32_location{descriptor};
    const u32 pc = a32_location.PC();
    const bool thumb = a32_location.TFlag();
    if (auto job = compile_thread->TakeReady(descriptor)) {
        std::lock_guard lock{code_mutex};
        if (conf.callbacks->CodeUnchanged(*job->snapshot)) {
            InstallJob(*job);
            return job->result.info.entry_point;
        }
        compile_thread->NoteStale();
    }
    if (compile_thread->IsPending(descriptor)) {
        compile_thread->Want(descriptor);
        if (!compile_sync) {
            conf.callbacks->CompileQueued(pc, thumb);
            return reinterpret_cast<CodePtr>(prelude_info.return_to_dispatcher);
        }
        // Sync, with an ahead job for it in flight.
        compile_thread->WaitIdle();
        InstallCompiled(true);
        if (CodePtr entry = Get(descriptor)) {
            return entry;
        }
    }
    {
        // As Emit does in place. The worker only moves the offset on, so a stale read
        // just clears a little early.
        std::unique_lock lock{code_mutex};
        if (GetRemainingSize() < 1024 * 1024) {
            ClearCacheHeld();
        }
    }
    for (bool whole = compile_thread->Overflowed(descriptor);; whole = true) {
        auto snapshot = conf.callbacks->SnapshotCode(pc, thumb, whole);
        if (!snapshot) {
            return nullptr;
        }
        if (compile_sync && !compile_thread->AheadOn()) {
            compile_thread->CompileHere(descriptor, std::move(snapshot));
        } else {
            compile_thread->Push(descriptor, std::move(snapshot));
            if (!compile_sync) {
                conf.callbacks->CompileQueued(pc, thumb);
                return reinterpret_cast<CodePtr>(prelude_info.return_to_dispatcher);
            }
            compile_thread->WaitIdle();
        }
        InstallCompiled(true);
        if (CodePtr entry = Get(descriptor)) {
            return entry;
        }
        if (whole || !compile_thread->Overflowed(descriptor)) {
            return nullptr;
        }
    }
}

// On the worker, or on the guest thread in sync mode (CompileHere).
void A32AddressSpace::CompileJob(CompileThread::Job& job) {
    IR::Block ir_block = GenerateIR(job.location, job.snapshot.get());
    if (job.snapshot->overflow) {
        job.status = CompileThread::Status::Overflow;
        return;
    }
    const A32::LocationDescriptor descriptor{ir_block.Location()};
    const A32::LocationDescriptor end_location{ir_block.EndLocation()};
    Common::FrondBlockTargets(ir_block, job.targets);
    std::lock_guard lock{code_mutex};
    if (job.epoch != compile_thread->Epoch()) {
        job.status = CompileThread::Status::Cancelled;
        return;
    }
    if (job.ahead && GetRemainingSize() < conf.code_cache_size / 4) {
        job.status = CompileThread::Status::Skipped;
        compile_thread->NoteHighWater();
        return;
    }
    if (GetRemainingSize() < 256 * 1024) {
        job.status = CompileThread::Status::NoSpace;
        return;
    }
    job.result.info = EmitUnregistered(std::move(ir_block));
    job.first_pc = descriptor.PC();
    job.last_pc = end_location.PC() - 1;
    job.status = CompileThread::Status::Done;
}

void A32AddressSpace::InstallCompiled(bool wait) {
    if (!compile_thread || !compile_thread->HasDone()) {
        return;
    }
    std::unique_lock lock{code_mutex, std::defer_lock};
    if (wait) {
        lock.lock();
    } else if (!lock.try_lock()) {
        return;
    }
    for (auto& job : compile_thread->TakeDone()) {
        if (job->status == CompileThread::Status::NoSpace) {
            ClearCacheHeld();
            continue;
        }
        if (job->status != CompileThread::Status::Done) {
            continue;
        }
        if (job->ahead && !job->wanted) {
            if (!Get(job->location)) {
                if (compile_thread->StillValid(*job)) {
                    QueueAhead(*job, job->depth + 1);
                }
                compile_thread->KeepReady(std::move(job));
            }
            continue;
        }
        if (!compile_thread->StillValid(*job) || Get(job->location) || !conf.callbacks->CodeUnchanged(*job->snapshot)) {
            compile_thread->NoteStale();
            continue;
        }
        InstallJob(*job);
    }
}

void A32AddressSpace::EmitPrelude() {
    using namespace oaknut::util;

    UnprotectCodeMemory();

    prelude_info.read_memory_8 = EmitReadCallTrampoline<&A32::UserCallbacks::MemoryRead8>(code, conf.callbacks, &ticks_now);
    prelude_info.read_memory_16 = EmitReadCallTrampoline<&A32::UserCallbacks::MemoryRead16>(code, conf.callbacks, &ticks_now);
    prelude_info.read_memory_32 = EmitReadCallTrampoline<&A32::UserCallbacks::MemoryRead32>(code, conf.callbacks, &ticks_now);
    prelude_info.read_memory_64 = EmitReadCallTrampoline<&A32::UserCallbacks::MemoryRead64>(code, conf.callbacks, &ticks_now);
    prelude_info.wrapped_read_memory_8 = EmitWrappedReadCallTrampoline<&A32::UserCallbacks::MemoryRead8>(code, conf.callbacks, &ticks_now);
    prelude_info.wrapped_read_memory_16 = EmitWrappedReadCallTrampoline<&A32::UserCallbacks::MemoryRead16>(code, conf.callbacks, &ticks_now);
    prelude_info.wrapped_read_memory_32 = EmitWrappedReadCallTrampoline<&A32::UserCallbacks::MemoryRead32>(code, conf.callbacks, &ticks_now);
    prelude_info.wrapped_read_memory_64 = EmitWrappedReadCallTrampoline<&A32::UserCallbacks::MemoryRead64>(code, conf.callbacks, &ticks_now);
    prelude_info.exclusive_read_memory_8 = EmitExclusiveReadCallTrampoline<&A32::UserCallbacks::MemoryRead8, u8>(code, conf);
    prelude_info.exclusive_read_memory_16 = EmitExclusiveReadCallTrampoline<&A32::UserCallbacks::MemoryRead16, u16>(code, conf);
    prelude_info.exclusive_read_memory_32 = EmitExclusiveReadCallTrampoline<&A32::UserCallbacks::MemoryRead32, u32>(code, conf);
    prelude_info.exclusive_read_memory_64 = EmitExclusiveReadCallTrampoline<&A32::UserCallbacks::MemoryRead64, u64>(code, conf);
    prelude_info.write_memory_8 = EmitReadCallTrampoline<&A32::UserCallbacks::MemoryWrite8>(code, conf.callbacks, &ticks_now);
    prelude_info.write_memory_16 = EmitReadCallTrampoline<&A32::UserCallbacks::MemoryWrite16>(code, conf.callbacks, &ticks_now);
    prelude_info.write_memory_32 = EmitReadCallTrampoline<&A32::UserCallbacks::MemoryWrite32>(code, conf.callbacks, &ticks_now);
    prelude_info.write_memory_64 = EmitReadCallTrampoline<&A32::UserCallbacks::MemoryWrite64>(code, conf.callbacks, &ticks_now);
    prelude_info.wrapped_write_memory_8 = EmitWrappedWriteCallTrampoline<&A32::UserCallbacks::MemoryWrite8>(code, conf.callbacks, &ticks_now);
    prelude_info.wrapped_write_memory_16 = EmitWrappedWriteCallTrampoline<&A32::UserCallbacks::MemoryWrite16>(code, conf.callbacks, &ticks_now);
    prelude_info.wrapped_write_memory_32 = EmitWrappedWriteCallTrampoline<&A32::UserCallbacks::MemoryWrite32>(code, conf.callbacks, &ticks_now);
    prelude_info.wrapped_write_memory_64 = EmitWrappedWriteCallTrampoline<&A32::UserCallbacks::MemoryWrite64>(code, conf.callbacks, &ticks_now);
    prelude_info.exclusive_write_memory_8 = EmitExclusiveWriteCallTrampoline<&A32::UserCallbacks::MemoryWriteExclusive8, u8>(code, conf);
    prelude_info.exclusive_write_memory_16 = EmitExclusiveWriteCallTrampoline<&A32::UserCallbacks::MemoryWriteExclusive16, u16>(code, conf);
    prelude_info.exclusive_write_memory_32 = EmitExclusiveWriteCallTrampoline<&A32::UserCallbacks::MemoryWriteExclusive32, u32>(code, conf);
    prelude_info.exclusive_write_memory_64 = EmitExclusiveWriteCallTrampoline<&A32::UserCallbacks::MemoryWriteExclusive64, u64>(code, conf);
    prelude_info.call_svc = EmitCallTrampoline<&A32::UserCallbacks::CallSVC>(code, conf.callbacks);
    prelude_info.interpreter_fallback = EmitCallTrampoline<&A32::UserCallbacks::InterpreterFallback>(code, conf.callbacks);
    prelude_info.exception_raised = EmitCallTrampoline<&A32::UserCallbacks::ExceptionRaised>(code, conf.callbacks);
    prelude_info.isb_raised = EmitCallTrampoline<&A32::UserCallbacks::InstructionSynchronizationBarrierRaised>(code, conf.callbacks);
    prelude_info.add_ticks = EmitCallTrampoline<&A32::UserCallbacks::AddTicks>(code, conf.callbacks);
    prelude_info.get_ticks_remaining = EmitCallTrampoline<&A32::UserCallbacks::GetTicksRemaining>(code, conf.callbacks);

    oaknut::Label return_from_run_code, l_return_to_dispatcher, l_fast_dispatch_table;

    prelude_info.run_code = code.xptr<PreludeInfo::RunCodeFuncType>();
    {
        ABI_PushRegisters(code, ABI_CALLEE_SAVE | (1 << 30), sizeof(StackLayout));

        code.MOV(X19, X0);
        code.MOV(Xstate, X1);
        code.MOV(Xhalt, X2);
        if (conf.page_table) {
            code.MOV(Xpagetable, mcl::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer) {
            code.MOV(Xfastmem, *conf.fastmem_pointer);
        }

        if (conf.HasOptimization(OptimizationFlag::ReturnStackBuffer)) {
            code.LDR(Xscratch0, l_return_to_dispatcher);
            for (size_t i = 0; i < RSBCount; i++) {
                code.STR(Xscratch0, SP, offsetof(StackLayout, rsb) + offsetof(RSBEntry, code_ptr) + i * sizeof(RSBEntry));
            }
        }
        if (conf.HasOptimization(OptimizationFlag::FastDispatch)) {
            code.LDR(Xscratch0, l_fast_dispatch_table);
            code.STR(Xscratch0, SP, offsetof(StackLayout, fast_dispatch_table));
        }

        if (conf.enable_cycle_counting) {
            code.BL(prelude_info.get_ticks_remaining);
            code.MOV(Xticks, X0);
            code.STR(Xticks, SP, offsetof(StackLayout, cycles_to_run));
        }

        code.LDR(Wscratch0, Xstate, offsetof(A32JitState, upper_location_descriptor));
        code.AND(Wscratch0, Wscratch0, 0xffff0000);
        code.MRS(Xscratch1, oaknut::SystemReg::FPCR);
        code.STR(Wscratch1, SP, offsetof(StackLayout, save_host_fpcr));
        {
            // Frond: writing FPCR stalls the pipeline; skip it when it already holds the value.
            oaknut::Label fpcr_set;
            code.CMP(Xscratch1, Xscratch0);
            code.B(EQ, fpcr_set);
            code.MSR(oaknut::SystemReg::FPCR, Xscratch0);
            code.l(fpcr_set);
        }

        code.LDAR(Wscratch0, Xhalt);
        code.CBNZ(Wscratch0, return_from_run_code);

        code.BR(X19);
    }

    prelude_info.step_code = code.xptr<PreludeInfo::RunCodeFuncType>();
    {
        ABI_PushRegisters(code, ABI_CALLEE_SAVE | (1 << 30), sizeof(StackLayout));

        code.MOV(X19, X0);
        code.MOV(Xstate, X1);
        code.MOV(Xhalt, X2);
        if (conf.page_table) {
            code.MOV(Xpagetable, mcl::bit_cast<u64>(conf.page_table));
        }
        if (conf.fastmem_pointer) {
            code.MOV(Xfastmem, *conf.fastmem_pointer);
        }

        if (conf.HasOptimization(OptimizationFlag::ReturnStackBuffer)) {
            code.LDR(Xscratch0, l_return_to_dispatcher);
            for (size_t i = 0; i < RSBCount; i++) {
                code.STR(Xscratch0, SP, offsetof(StackLayout, rsb) + offsetof(RSBEntry, code_ptr) + i * sizeof(RSBEntry));
            }
        }
        if (conf.HasOptimization(OptimizationFlag::FastDispatch)) {
            code.LDR(Xscratch0, l_fast_dispatch_table);
            code.STR(Xscratch0, SP, offsetof(StackLayout, fast_dispatch_table));
        }

        if (conf.enable_cycle_counting) {
            code.MOV(Xticks, 1);
            code.STR(Xticks, SP, offsetof(StackLayout, cycles_to_run));
        }

        code.LDR(Wscratch0, Xstate, offsetof(A32JitState, upper_location_descriptor));
        code.AND(Wscratch0, Wscratch0, 0xffff0000);
        code.MRS(Xscratch1, oaknut::SystemReg::FPCR);
        code.STR(Wscratch1, SP, offsetof(StackLayout, save_host_fpcr));
        {
            // Frond: writing FPCR stalls the pipeline; skip it when it already holds the value.
            oaknut::Label fpcr_set;
            code.CMP(Xscratch1, Xscratch0);
            code.B(EQ, fpcr_set);
            code.MSR(oaknut::SystemReg::FPCR, Xscratch0);
            code.l(fpcr_set);
        }

        oaknut::Label step_hr_loop;
        code.l(step_hr_loop);
        code.LDAXR(Wscratch0, Xhalt);
        code.CBNZ(Wscratch0, return_from_run_code);
        code.ORR(Wscratch0, Wscratch0, static_cast<u32>(HaltReason::Step));
        code.STLXR(Wscratch1, Wscratch0, Xhalt);
        code.CBNZ(Wscratch1, step_hr_loop);

        code.BR(X19);
    }

    prelude_info.return_to_dispatcher = code.xptr<void*>();
    {
        oaknut::Label l_this, l_addr;

        code.LDAR(Wscratch0, Xhalt);
        code.CBNZ(Wscratch0, return_from_run_code);

        if (conf.enable_cycle_counting) {
            code.CMP(Xticks, 0);
            code.B(LE, return_from_run_code);
        }

        // Frond: the fast dispatch table first; its empty slots point at the call below.
        if (conf.HasOptimization(OptimizationFlag::FastDispatch)) {
            oaknut::Label miss;
            code.LDR(Xscratch1, l_fast_dispatch_table);
            EmitA32FastDispatch(code, miss);
            code.l(miss);
            fast_dispatch_miss = code.xptr<void*>();
        }

        code.LDR(X0, l_this);
        code.MOV(X1, Xstate);
        code.LDR(Xscratch0, l_addr);
        code.BLR(Xscratch0);
        code.BR(X0);

        const auto fn = [](A32AddressSpace& self, A32JitState& context) -> CodePtr {
            const IR::LocationDescriptor descriptor = context.GetLocationDescriptor();
            const CodePtr entry = self.GetOrEmitIfWanted(descriptor);
            self.FillFastDispatch(descriptor, entry);
            return entry;
        };

        code.align(8);
        code.l(l_this);
        code.dx(mcl::bit_cast<u64>(this));
        code.l(l_addr);
        code.dx(mcl::bit_cast<u64>(Common::FptrCast(fn)));
    }

    prelude_info.return_from_run_code = code.xptr<void*>();
    {
        code.l(return_from_run_code);

        if (conf.enable_cycle_counting) {
            code.LDR(X1, SP, offsetof(StackLayout, cycles_to_run));
            code.SUB(X1, X1, Xticks);
            code.BL(prelude_info.add_ticks);
        }

        code.LDR(Wscratch0, SP, offsetof(StackLayout, save_host_fpcr));
        {
            oaknut::Label fpcr_restored;
            code.MRS(Xscratch1, oaknut::SystemReg::FPCR);
            code.CMP(Xscratch1, Xscratch0);
            code.B(EQ, fpcr_restored);
            code.MSR(oaknut::SystemReg::FPCR, Xscratch0);
            code.l(fpcr_restored);
        }

        oaknut::Label exit_hr_loop;
        code.l(exit_hr_loop);
        code.LDAXR(W0, Xhalt);
        code.STLXR(Wscratch0, WZR, Xhalt);
        code.CBNZ(Wscratch0, exit_hr_loop);

        ABI_PopRegisters(code, ABI_CALLEE_SAVE | (1 << 30), sizeof(StackLayout));
        code.RET();
    }

    code.align(8);
    code.l(l_return_to_dispatcher);
    code.dx(mcl::bit_cast<u64>(prelude_info.return_to_dispatcher));
    if (conf.HasOptimization(OptimizationFlag::FastDispatch)) {
        fast_dispatch_table = std::make_unique<std::array<FastDispatchEntry, 1u << fast_dispatch_bits>>();
        ClearFastDispatchTable();
    }
    code.l(l_fast_dispatch_table);
    code.dx(mcl::bit_cast<u64>(fast_dispatch_table ? fast_dispatch_table->data() : nullptr));

    prelude_info.end_of_prelude = code.offset();

    // Frond: only the prelude is written yet, and each block is invalidated when it's
    // emitted. Invalidating the whole code cache here took about 7 ms per JIT on a phone.
    mem.invalidate(mem.ptr(), static_cast<std::size_t>(prelude_info.end_of_prelude));
    ProtectCodeMemory();
}

EmitConfig A32AddressSpace::GetEmitConfig() {
    return EmitConfig{
        .optimizations = conf.unsafe_optimizations ? conf.optimizations : conf.optimizations & all_safe_optimizations,

        .hook_isb = conf.hook_isb,

        .cntfreq_el0{},
        .ctr_el0{},
        .dczid_el0{},
        .tpidrro_el0{},
        .tpidr_el0{},

        .check_halt_on_memory_access = conf.check_halt_on_memory_access,
        .check_halt_on_link = conf.check_halt_on_link,
        .max_unchecked_instructions = conf.enable_cycle_counting ? conf.max_unchecked_instructions : 0,

        .page_table_pointer = mcl::bit_cast<u64>(conf.page_table),
        .page_table_address_space_bits = 32,
        .page_table_pointer_mask_bits = conf.page_table_pointer_mask_bits,
        .page_table_write_protect_bit = conf.page_table_write_protect_bit,
        .silently_mirror_page_table = true,
        .absolute_offset_page_table = conf.absolute_offset_page_table,
        .detect_misaligned_access_via_page_table = conf.detect_misaligned_access_via_page_table,
        .only_detect_misalignment_via_page_table_on_page_boundary = conf.only_detect_misalignment_via_page_table_on_page_boundary,

        .fastmem_pointer = conf.fastmem_pointer,
        .recompile_on_fastmem_failure = conf.recompile_on_fastmem_failure,
        .fastmem_address_space_bits = 32,
        .silently_mirror_fastmem = true,

        .wall_clock_cntpct = conf.wall_clock_cntpct,
        .enable_cycle_counting = conf.enable_cycle_counting,

        .always_little_endian = conf.always_little_endian,

        .descriptor_to_fpcr = [](const IR::LocationDescriptor& location) { return FP::FPCR{A32::LocationDescriptor{location}.FPSCR().Value()}; },
        .emit_cond = EmitA32Cond,
        .emit_condition_failed_terminal = EmitA32ConditionFailedTerminal,
        .emit_terminal = EmitA32Terminal,
        .emit_check_memory_abort = EmitA32CheckMemoryAbort,

        .state_nzcv_offset = offsetof(A32JitState, cpsr_nzcv),
        .state_fpsr_offset = offsetof(A32JitState, fpsr),
        .state_exclusive_state_offset = offsetof(A32JitState, exclusive_state),

        .coprocessors = conf.coprocessors,

        .very_verbose_debugging_output = conf.very_verbose_debugging_output,
    };
}

void A32AddressSpace::RegisterNewBasicBlock(const IR::Block& block, const EmittedBlockInfo&) {
    const A32::LocationDescriptor descriptor{block.Location()};
    const A32::LocationDescriptor end_location{block.EndLocation()};
    const auto range = boost::icl::discrete_interval<u32>::closed(descriptor.PC(), end_location.PC() - 1);
    block_ranges.AddRange(range, descriptor);
}

}  // namespace Dynarmic::Backend::Arm64
