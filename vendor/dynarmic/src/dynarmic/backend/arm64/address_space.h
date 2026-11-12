/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <map>
#include <memory>
#include <optional>

#include <mcl/stdint.hpp>
#include <oaknut/code_block.hpp>
#include <oaknut/oaknut.hpp>
#include <tsl/robin_map.h>
#include <tsl/robin_set.h>

#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/fastmem.h"
#include "dynarmic/interface/halt_reason.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend::Arm64 {

class AddressSpace {
public:
    explicit AddressSpace(size_t code_cache_size);
    virtual ~AddressSpace();

    virtual IR::Block GenerateIR(IR::LocationDescriptor) const = 0;

    CodePtr Get(IR::LocationDescriptor descriptor);

    // Returns "most likely" LocationDescriptor assocated with the emitted code at that location
    std::optional<IR::LocationDescriptor> ReverseGetLocation(CodePtr host_pc);

    // Returns "most likely" entry_point associated with the emitted code at that location
    CodePtr ReverseGetEntryPoint(CodePtr host_pc);

    CodePtr GetOrEmit(IR::LocationDescriptor descriptor);

    void InvalidateBasicBlocks(const tsl::robin_set<IR::LocationDescriptor>& descriptors);

    void ClearCache();

    void DumpDisassembly() const;

    // Frond: fast dispatch. A direct-mapped table from location descriptor to entry point,
    // read by the emitted dispatcher and by the indirect branches in blocks, so a jump to a
    // block that isn't linked doesn't leave JIT code. Only the guest thread fills or clears
    // it; a block leaves it whenever it leaves block_entries.
    struct FastDispatchEntry {
        u64 location_descriptor;
        CodePtr code_ptr;
    };
    static_assert(sizeof(FastDispatchEntry) == 16);
    static constexpr unsigned fast_dispatch_bits = 12;
    // Matches EmitFastDispatchIndex: PC bits 1-12 xor bits 13-24 and the T flag. Index bit 0 is
    // PC bit 1 xor bit 13, so ARM blocks use every slot over 16 KB of code, Thumb over 8 KB.
    static u32 FastDispatchIndex(u64 location_descriptor) {
        u32 x = static_cast<u32>(location_descriptor ^ (location_descriptor >> 31));
        x ^= x >> 12;
        return (x >> 1) & ((1u << fast_dispatch_bits) - 1);
    }
    FastDispatchEntry& FastDispatchSlot(IR::LocationDescriptor descriptor) {
        return (*fast_dispatch_table)[FastDispatchIndex(descriptor.Value())];
    }
    void FillFastDispatch(IR::LocationDescriptor descriptor, CodePtr entry);
    void ClearFastDispatchTable();

protected:
    virtual EmitConfig GetEmitConfig() = 0;
    virtual void RegisterNewBasicBlock(const IR::Block& block, const EmittedBlockInfo& block_info) = 0;

    void ProtectCodeMemory() {
#if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT) || defined(__APPLE__) || defined(__OpenBSD__)
        mem.protect();
#endif
    }

    void UnprotectCodeMemory() {
#if defined(DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT) || defined(__APPLE__) || defined(__OpenBSD__)
        mem.unprotect();
#endif
    }

    size_t GetRemainingSize();
    EmittedBlockInfo Emit(IR::Block ir_block);
    // Frond: Emit in two steps, for the compile thread. EmitUnregistered writes and links
    // the code but leaves it unreachable; Install registers it and links the blocks that
    // jump to it.
    EmittedBlockInfo EmitUnregistered(IR::Block ir_block);
    void Install(IR::LocationDescriptor location, const EmittedBlockInfo& block_info);
    void Link(EmittedBlockInfo& block);
    void LinkBlockLinks(const CodePtr entry_point, const CodePtr target_ptr, const std::vector<BlockRelocation>& block_relocations_list);
    void RelinkForDescriptor(IR::LocationDescriptor target_descriptor, CodePtr target_ptr);

    FakeCall FastmemCallback(u64 host_pc);

    const size_t code_cache_size;
    // Frond: ClearCache calls, for A32::Jit::CompileStats. Guest thread only.
    u64 cache_clears = 0;
    oaknut::CodeBlock mem;
    oaknut::CodeGenerator code;

    // A IR::LocationDescriptor will have one current CodePtr.
    // However, there can be multiple other CodePtrs which are older, previously invalidated blocks.
    tsl::robin_map<IR::LocationDescriptor, CodePtr> block_entries;
    std::map<CodePtr, IR::LocationDescriptor> reverse_block_entries;
    tsl::robin_map<CodePtr, EmittedBlockInfo> block_infos;
    tsl::robin_map<IR::LocationDescriptor, tsl::robin_set<CodePtr>> block_references;

    ExceptionHandler exception_handler;
    FastmemManager fastmem_manager;

    // Empty slots point at the dispatcher's slow path, so even a false match is safe. Allocated
    // on its own: making AddressSpace 64 KB bigger changed the arm64 Fast results (cause not
    // found).
    std::unique_ptr<std::array<FastDispatchEntry, 1u << fast_dispatch_bits>> fast_dispatch_table;
    void* fast_dispatch_miss = nullptr;

    struct PreludeInfo {
        std::ptrdiff_t end_of_prelude;

        using RunCodeFuncType = HaltReason (*)(CodePtr entry_point, void* jit_state, volatile u32* halt_reason);
        RunCodeFuncType run_code;
        RunCodeFuncType step_code;
        void* return_to_dispatcher;
        void* return_from_run_code;

        void* read_memory_8;
        void* read_memory_16;
        void* read_memory_32;
        void* read_memory_64;
        void* read_memory_128;
        void* wrapped_read_memory_8;
        void* wrapped_read_memory_16;
        void* wrapped_read_memory_32;
        void* wrapped_read_memory_64;
        void* wrapped_read_memory_128;
        void* exclusive_read_memory_8;
        void* exclusive_read_memory_16;
        void* exclusive_read_memory_32;
        void* exclusive_read_memory_64;
        void* exclusive_read_memory_128;
        void* write_memory_8;
        void* write_memory_16;
        void* write_memory_32;
        void* write_memory_64;
        void* write_memory_128;
        void* wrapped_write_memory_8;
        void* wrapped_write_memory_16;
        void* wrapped_write_memory_32;
        void* wrapped_write_memory_64;
        void* wrapped_write_memory_128;
        void* exclusive_write_memory_8;
        void* exclusive_write_memory_16;
        void* exclusive_write_memory_32;
        void* exclusive_write_memory_64;
        void* exclusive_write_memory_128;

        void* call_svc;
        void* interpreter_fallback;
        void* exception_raised;
        void* dc_raised;
        void* ic_raised;
        void* isb_raised;

        void* get_cntpct;
        void* add_ticks;
        void* get_ticks_remaining;
    } prelude_info;
};

}  // namespace Dynarmic::Backend::Arm64
