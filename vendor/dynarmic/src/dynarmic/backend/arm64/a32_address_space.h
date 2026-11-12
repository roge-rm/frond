/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

#include "dynarmic/backend/arm64/address_space.h"
#include "dynarmic/backend/block_range_information.h"
#include "dynarmic/common/frond_compile_thread.h"
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/config.h"

namespace Dynarmic::Backend::Arm64 {

struct EmittedBlockInfo;

class A32AddressSpace final : public AddressSpace {
public:
    explicit A32AddressSpace(const A32::UserConfig& conf);

    IR::Block GenerateIR(IR::LocationDescriptor) const override;

    void InvalidateCacheRanges(const boost::icl::interval_set<u32>& ranges);

    // Frond: GetOrEmit, unless the callbacks leave new code to the caller for now (they halt
    // the run); then the dispatcher, which ends the run.
    CodePtr GetOrEmitIfWanted(IR::LocationDescriptor descriptor);
    CodePtr GetForRun(IR::LocationDescriptor descriptor);

    // Frond: the compile thread (A32::Jit::SetCompileThread).
    void SetCompileThread(A32::Jit::CompileThread mode);
    void SetCompileAhead(unsigned depth);
    // On the guest thread, between runs. Without wait it gives up if the worker is emitting.
    void InstallCompiled(bool wait);
    A32::Jit::CompileStats GetCompileStats() const;
    // Frond: Xticks as the last memory read or write callback found it (Jit::TicksRemainingNow).
    s64 TicksRemainingNow() const { return ticks_now; }
    // ClearCache and InvalidateCacheRanges for the Jit, with the compile thread told.
    void ClearCacheLocked();

    ~A32AddressSpace() override;

protected:
    friend class A32Core;

    s64 ticks_now = 0;

    void EmitPrelude();
    EmitConfig GetEmitConfig() override;
    void RegisterNewBasicBlock(const IR::Block& block, const EmittedBlockInfo& block_info) override;

    const A32::UserConfig conf;
    BlockRangeInformation<u32> block_ranges;

    struct Compiled {
        EmittedBlockInfo info;
    };
    using CompileThread = Common::FrondCompileThread<Compiled>;
    IR::Block GenerateIR(IR::LocationDescriptor descriptor, A32::TranslateCallbacks* tcb) const;
    CodePtr CompileOffThread(IR::LocationDescriptor descriptor);
    void CompileJob(CompileThread::Job& job);
    void QueueAhead(const CompileThread::Job& job, unsigned depth);
    void InstallJob(const CompileThread::Job& job);
    std::function<bool(IR::LocationDescriptor)> CompiledFn();
    std::function<std::unique_ptr<A32::CodeSnapshot>(IR::LocationDescriptor)> SnapshotFn();
    void ClearCacheHeld();

    // The code space and block maps are shared with the compile thread under code_mutex.
    std::mutex code_mutex;
    bool compile_sync = false;
    std::unique_ptr<CompileThread> compile_thread;
};

}  // namespace Dynarmic::Backend::Arm64
