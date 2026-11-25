/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>

#include <unistd.h>

#include <mcl/bit_cast.hpp>

#include "dynarmic/backend/arm64/a64_address_space.h"
#include "dynarmic/backend/arm64/a64_jitstate.h"
#include "dynarmic/backend/arm64/abi.h"
#include "dynarmic/backend/arm64/devirtualize.h"
#include "dynarmic/backend/arm64/emit_arm64.h"
#include "dynarmic/backend/arm64/stack_layout.h"
#include "dynarmic/common/cast_util.h"
#include "dynarmic/common/fp/fpcr.h"
#include "dynarmic/common/llvm_disassemble.h"
#include "dynarmic/interface/exclusive_monitor.h"

namespace Dynarmic::Backend::Arm64 {

namespace {
// Frond: with FROND_JIT_PERF_MAP set to a folder, each block's host range and location go to perf-PID.map
// there (perf's map format) and its code to perf-PID.code (host start and size as u64, then the bytes), for
// matching profile samples to guest code and instructions. Off otherwise.
std::mutex perf_map_mutex;
std::FILE* perf_map_file = nullptr;
std::FILE* perf_code_file = nullptr;
bool perf_map_failed = false;

void PerfMapRegister(IR::LocationDescriptor location, const EmittedBlockInfo& info) {
    static const char* const dir = std::getenv("FROND_JIT_PERF_MAP");
    if (!dir || !*dir) return;
    std::lock_guard lock{perf_map_mutex};
    if (perf_map_failed) return;
    if (!perf_map_file) {
        const std::string base = std::string(dir) + "/perf-" + std::to_string(getpid());
        perf_map_file = std::fopen((base + ".map").c_str(), "w");
        perf_code_file = std::fopen((base + ".code").c_str(), "wb");
        if (!perf_map_file || !perf_code_file) {
            // Either failed to open: close the other and don't try again.
            if (perf_map_file) std::fclose(perf_map_file);
            if (perf_code_file) std::fclose(perf_code_file);
            perf_map_file = perf_code_file = nullptr;
            perf_map_failed = true;
            return;
        }
    }
    const u64 start = mcl::bit_cast<u64>(info.entry_point), size = info.size, value = location.Value();
    std::fprintf(perf_map_file, "%016llx %llx a32_%08x_%08x\n", (unsigned long long)start, (unsigned long long)size,
                 u32(value >> 32), u32(value));
    std::fflush(perf_map_file);
    std::fwrite(&start, sizeof start, 1, perf_code_file);
    std::fwrite(&size, sizeof size, 1, perf_code_file);
    std::fwrite(reinterpret_cast<const void*>(info.entry_point), 1, size, perf_code_file);
    std::fflush(perf_code_file);
}
}  // namespace

AddressSpace::AddressSpace(size_t code_cache_size, [[maybe_unused]] bool fastmem)
        : code_cache_size(code_cache_size)
        , mem(code_cache_size)
        , code(mem.ptr(), mem.ptr())
        , fastmem_manager(exception_handler) {
    ASSERT_MSG(code_cache_size <= 128 * 1024 * 1024, "code_cache_size > 128 MiB not currently supported");

#ifdef __ANDROID__
    if (!fastmem) return;
#endif
    exception_handler.Register(mem, code_cache_size);
    exception_handler.SetFastmemCallback([this](u64 host_pc) {
        return FastmemCallback(host_pc);
    });
}

AddressSpace::~AddressSpace() = default;

CodePtr AddressSpace::Get(IR::LocationDescriptor descriptor) {
    if (const auto iter = block_entries.find(descriptor); iter != block_entries.end()) {
        return iter->second;
    }
    return nullptr;
}

std::optional<IR::LocationDescriptor> AddressSpace::ReverseGetLocation(CodePtr host_pc) {
    if (auto iter = reverse_block_entries.upper_bound(host_pc); iter != reverse_block_entries.begin()) {
        // upper_bound locates the first value greater than host_pc, so we need to decrement
        --iter;
        return iter->second;
    }
    return std::nullopt;
}

CodePtr AddressSpace::ReverseGetEntryPoint(CodePtr host_pc) {
    if (auto iter = reverse_block_entries.upper_bound(host_pc); iter != reverse_block_entries.begin()) {
        // upper_bound locates the first value greater than host_pc, so we need to decrement
        --iter;
        return iter->first;
    }
    return nullptr;
}

CodePtr AddressSpace::GetOrEmit(IR::LocationDescriptor descriptor) {
    if (CodePtr block_entry = Get(descriptor)) {
        return block_entry;
    }

    IR::Block ir_block = GenerateIR(descriptor);
    const EmittedBlockInfo block_info = Emit(std::move(ir_block));
    return block_info.entry_point;
}

void AddressSpace::InvalidateBasicBlocks(const tsl::robin_set<IR::LocationDescriptor>& descriptors) {
    InvalidateBasicBlocks(descriptors, nullptr);
}

// Frond: a dropped block also leaves block_infos, reverse_block_entries and every target's
// block_references set, so later installs and drops only relink live blocks. Its code stays
// in the cache until ClearCache. keep_info is a block still running (a fastmem fault inside
// it): it keeps its block_infos and reverse entries so a later fault in the same run is found.
void AddressSpace::InvalidateBasicBlocks(const tsl::robin_set<IR::LocationDescriptor>& descriptors, CodePtr keep_info) {
    UnprotectCodeMemory();

    for (const auto& descriptor : descriptors) {
        if (fast_dispatch_table) {
            if (FastDispatchEntry& slot = FastDispatchSlot(descriptor); slot.location_descriptor == descriptor.Value()) {
                slot = {~u64(0), static_cast<CodePtr>(fast_dispatch_miss)};
            }
        }

        const auto iter = block_entries.find(descriptor);
        if (iter == block_entries.end()) {
            continue;
        }
        const CodePtr entry_point = iter->second;

        // Unlink before removal because InvalidateBasicBlocks can be called within a fastmem callback,
        // and the currently executing block may have references to itself which need to be unlinked.
        RelinkForDescriptor(descriptor, nullptr);

        block_entries.erase(iter);

        // The callers of this descriptor stay in block_references[descriptor]: they are live
        // and link to the next block emitted for it.
        if (const auto info_iter = block_infos.find(entry_point); info_iter != block_infos.end()) {
            for (const auto& [target_descriptor, list] : info_iter->second.block_relocations) {
                if (auto ref_iter = block_references.find(target_descriptor); ref_iter != block_references.end()) {
                    ref_iter.value().erase(entry_point);
                    if (ref_iter->second.empty()) {
                        block_references.erase(ref_iter);
                    }
                }
            }
            if (entry_point != keep_info) {
                block_infos.erase(info_iter);
                reverse_block_entries.erase(entry_point);
            }
        }
    }

    ProtectCodeMemory();
}

void AddressSpace::ClearCache() {
    cache_clears++;
    ClearFastDispatchTable();
    block_entries.clear();
    reverse_block_entries.clear();
    block_infos.clear();
    block_references.clear();
    code.set_offset(prelude_info.end_of_prelude);
}

// Only entries in block_entries go in: never the dispatcher, which a cold block's lookup returns.
void AddressSpace::FillFastDispatch(IR::LocationDescriptor descriptor, CodePtr entry) {
    if (fast_dispatch_table && entry != prelude_info.return_to_dispatcher) {
        FastDispatchSlot(descriptor) = {descriptor.Value(), entry};
    }
}

void AddressSpace::ClearFastDispatchTable() {
    if (!fast_dispatch_table) {
        return;
    }
    fast_dispatch_table->fill({~u64(0), static_cast<CodePtr>(fast_dispatch_miss)});
}

void AddressSpace::DumpDisassembly() const {
    for (u32* ptr = mem.ptr(); ptr < code.xptr<u32*>(); ptr++) {
        std::printf("%s", Common::DisassembleAArch64(*ptr, mcl::bit_cast<u64>(ptr)).c_str());
    }
}

size_t AddressSpace::GetRemainingSize() {
    return code_cache_size - static_cast<size_t>(code.offset());
}

EmittedBlockInfo AddressSpace::Emit(IR::Block block) {
    if (GetRemainingSize() < 1024 * 1024) {
        ClearCache();
    }

    UnprotectCodeMemory();

    EmittedBlockInfo block_info = EmitArm64(code, std::move(block), GetEmitConfig(), fastmem_manager);

    ASSERT(block_entries.insert({block.Location(), block_info.entry_point}).second);
    ASSERT(reverse_block_entries.insert({block_info.entry_point, block.Location()}).second);
    ASSERT(block_infos.insert({block_info.entry_point, block_info}).second);

    Link(block_info);
    for (const auto& [target_descriptor, list] : block_info.block_relocations) {
        block_references[target_descriptor].insert(block_info.entry_point);
    }
    RelinkForDescriptor(block.Location(), block_info.entry_point);

    mem.invalidate(reinterpret_cast<u32*>(block_info.entry_point), block_info.size);
    ProtectCodeMemory();

    RegisterNewBasicBlock(block, block_info);
    PerfMapRegister(block.Location(), block_info);

    return block_info;
}

EmittedBlockInfo AddressSpace::EmitUnregistered(IR::Block block) {
    UnprotectCodeMemory();

    EmittedBlockInfo block_info = EmitArm64(code, std::move(block), GetEmitConfig(), fastmem_manager);
    Link(block_info);

    mem.invalidate(reinterpret_cast<u32*>(block_info.entry_point), block_info.size);
    ProtectCodeMemory();

    return block_info;
}

void AddressSpace::Install(IR::LocationDescriptor location, const EmittedBlockInfo& block_info) {
    UnprotectCodeMemory();

    ASSERT(block_entries.insert({location, block_info.entry_point}).second);
    ASSERT(reverse_block_entries.insert({block_info.entry_point, location}).second);
    ASSERT(block_infos.insert({block_info.entry_point, block_info}).second);

    // Outgoing links were resolved when the block was emitted; targets may have been dropped
    // or installed since, so link them again against what is there now.
    for (const auto& [target_descriptor, list] : block_info.block_relocations) {
        block_references[target_descriptor].insert(block_info.entry_point);
        LinkBlockLinks(block_info.entry_point, Get(target_descriptor), list);
    }
    mem.invalidate(reinterpret_cast<u32*>(block_info.entry_point), block_info.size);

    RelinkForDescriptor(location, block_info.entry_point);

    ProtectCodeMemory();

    // The rest of the code was written and its cache lines cleaned on the compile thread.
    __asm__ volatile("isb" ::: "memory");
    PerfMapRegister(location, block_info);
}

void AddressSpace::Link(EmittedBlockInfo& block_info) {
    using namespace oaknut;
    using namespace oaknut::util;

    for (auto [ptr_offset, target] : block_info.relocations) {
        CodeGenerator c{mem.ptr(), mem.ptr()};
        c.set_xptr(reinterpret_cast<u32*>(block_info.entry_point + ptr_offset));

        switch (target) {
        case LinkTarget::ReturnToDispatcher:
            c.B(prelude_info.return_to_dispatcher);
            break;
        case LinkTarget::ReturnFromRunCode:
            c.B(prelude_info.return_from_run_code);
            break;
        case LinkTarget::ReadMemory8:
            c.BL(prelude_info.read_memory_8);
            break;
        case LinkTarget::ReadMemory16:
            c.BL(prelude_info.read_memory_16);
            break;
        case LinkTarget::ReadMemory32:
            c.BL(prelude_info.read_memory_32);
            break;
        case LinkTarget::ReadMemory64:
            c.BL(prelude_info.read_memory_64);
            break;
        case LinkTarget::ReadMemory128:
            c.BL(prelude_info.read_memory_128);
            break;
        case LinkTarget::WrappedReadMemory8:
            c.BL(prelude_info.wrapped_read_memory_8);
            break;
        case LinkTarget::WrappedReadMemory16:
            c.BL(prelude_info.wrapped_read_memory_16);
            break;
        case LinkTarget::WrappedReadMemory32:
            c.BL(prelude_info.wrapped_read_memory_32);
            break;
        case LinkTarget::WrappedReadMemory64:
            c.BL(prelude_info.wrapped_read_memory_64);
            break;
        case LinkTarget::WrappedReadMemory128:
            c.BL(prelude_info.wrapped_read_memory_128);
            break;
        case LinkTarget::ExclusiveReadMemory8:
            c.BL(prelude_info.exclusive_read_memory_8);
            break;
        case LinkTarget::ExclusiveReadMemory16:
            c.BL(prelude_info.exclusive_read_memory_16);
            break;
        case LinkTarget::ExclusiveReadMemory32:
            c.BL(prelude_info.exclusive_read_memory_32);
            break;
        case LinkTarget::ExclusiveReadMemory64:
            c.BL(prelude_info.exclusive_read_memory_64);
            break;
        case LinkTarget::ExclusiveReadMemory128:
            c.BL(prelude_info.exclusive_read_memory_128);
            break;
        case LinkTarget::WriteMemory8:
            c.BL(prelude_info.write_memory_8);
            break;
        case LinkTarget::WriteMemory16:
            c.BL(prelude_info.write_memory_16);
            break;
        case LinkTarget::WriteMemory32:
            c.BL(prelude_info.write_memory_32);
            break;
        case LinkTarget::WriteMemory64:
            c.BL(prelude_info.write_memory_64);
            break;
        case LinkTarget::WriteMemory128:
            c.BL(prelude_info.write_memory_128);
            break;
        case LinkTarget::WrappedWriteMemory8:
            c.BL(prelude_info.wrapped_write_memory_8);
            break;
        case LinkTarget::WrappedWriteMemory16:
            c.BL(prelude_info.wrapped_write_memory_16);
            break;
        case LinkTarget::WrappedWriteMemory32:
            c.BL(prelude_info.wrapped_write_memory_32);
            break;
        case LinkTarget::WrappedWriteMemory64:
            c.BL(prelude_info.wrapped_write_memory_64);
            break;
        case LinkTarget::WrappedWriteMemory128:
            c.BL(prelude_info.wrapped_write_memory_128);
            break;
        case LinkTarget::ExclusiveWriteMemory8:
            c.BL(prelude_info.exclusive_write_memory_8);
            break;
        case LinkTarget::ExclusiveWriteMemory16:
            c.BL(prelude_info.exclusive_write_memory_16);
            break;
        case LinkTarget::ExclusiveWriteMemory32:
            c.BL(prelude_info.exclusive_write_memory_32);
            break;
        case LinkTarget::ExclusiveWriteMemory64:
            c.BL(prelude_info.exclusive_write_memory_64);
            break;
        case LinkTarget::ExclusiveWriteMemory128:
            c.BL(prelude_info.exclusive_write_memory_128);
            break;
        case LinkTarget::CallSVC:
            c.BL(prelude_info.call_svc);
            break;
        case LinkTarget::InterpreterFallback:
            c.BL(prelude_info.interpreter_fallback);
            break;
        case LinkTarget::ExceptionRaised:
            c.BL(prelude_info.exception_raised);
            break;
        case LinkTarget::InstructionSynchronizationBarrierRaised:
            c.BL(prelude_info.isb_raised);
            break;
        case LinkTarget::InstructionCacheOperationRaised:
            c.BL(prelude_info.ic_raised);
            break;
        case LinkTarget::DataCacheOperationRaised:
            c.BL(prelude_info.dc_raised);
            break;
        case LinkTarget::GetCNTPCT:
            c.BL(prelude_info.get_cntpct);
            break;
        case LinkTarget::AddTicks:
            c.BL(prelude_info.add_ticks);
            break;
        case LinkTarget::GetTicksRemaining:
            c.BL(prelude_info.get_ticks_remaining);
            break;
        default:
            ASSERT_FALSE("Invalid relocation target");
        }
    }

    // Install registers a compile-thread block in block_references; Emit does it for its own.
    for (auto [target_descriptor, list] : block_info.block_relocations) {
        LinkBlockLinks(block_info.entry_point, Get(target_descriptor), list);
    }
}

void AddressSpace::LinkBlockLinks(const CodePtr entry_point, const CodePtr target_ptr, const std::vector<BlockRelocation>& block_relocations_list) {
    using namespace oaknut;
    using namespace oaknut::util;

    for (auto [ptr_offset, type] : block_relocations_list) {
        CodeGenerator c{mem.ptr(), mem.ptr()};
        c.set_xptr(reinterpret_cast<u32*>(entry_point + ptr_offset));

        switch (type) {
        case BlockRelocationType::Branch:
            if (target_ptr) {
                c.B((void*)target_ptr);
            } else {
                c.NOP();
            }
            break;
        case BlockRelocationType::MoveToScratch1:
            if (target_ptr) {
                c.ADRL(Xscratch1, (void*)target_ptr);
            } else {
                c.ADRL(Xscratch1, prelude_info.return_to_dispatcher);
            }
            break;
        default:
            ASSERT_FALSE("Invalid BlockRelocationType");
        }
    }
}

// Only the patched instructions are written, so only their span is cleaned and invalidated.
void AddressSpace::RelinkForDescriptor(IR::LocationDescriptor target_descriptor, CodePtr target_ptr) {
    const auto ref_iter = block_references.find(target_descriptor);
    if (ref_iter == block_references.end()) {
        return;
    }
    for (auto code_ptr : ref_iter->second) {
        const auto block_iter = block_infos.find(code_ptr);
        if (block_iter == block_infos.end()) {
            continue;
        }
        const EmittedBlockInfo& block_info = block_iter->second;
        const auto relocation_iter = block_info.block_relocations.find(target_descriptor);
        if (relocation_iter == block_info.block_relocations.end() || relocation_iter->second.empty()) {
            continue;
        }
        LinkBlockLinks(block_info.entry_point, target_ptr, relocation_iter->second);

        // A Branch is one instruction, a MoveToScratch1 (ADRL) two.
        std::ptrdiff_t first = PTRDIFF_MAX;
        std::ptrdiff_t last = 0;
        for (const auto& relocation : relocation_iter->second) {
            first = std::min(first, relocation.code_offset);
            last = std::max(last, relocation.code_offset + 8);
        }
        mem.invalidate(reinterpret_cast<u32*>(block_info.entry_point + first), static_cast<size_t>(last - first));
    }
}

std::optional<DoNotFastmemMarker> AddressSpace::TakeFrondFastmemFault() {
    return std::exchange(frond_fastmem_fault, std::nullopt);
}

FakeCall AddressSpace::FastmemCallback(u64 host_pc) {
    {
        const auto host_ptr = mcl::bit_cast<CodePtr>(host_pc);

        const auto entry_point = ReverseGetEntryPoint(host_ptr);
        if (!entry_point) {
            goto fail;
        }

        const auto block_info = block_infos.find(entry_point);
        if (block_info == block_infos.end()) {
            goto fail;
        }

        const auto patch_entry = block_info->second.fastmem_patch_info.find(host_ptr - entry_point);
        if (patch_entry == block_info->second.fastmem_patch_info.end()) {
            goto fail;
        }

        const auto fc = patch_entry->second.fc;

        if (patch_entry->second.defer) {
            frond_fastmem_fault = patch_entry->second.marker;
        } else if (patch_entry->second.recompile) {
            const auto marker = patch_entry->second.marker;
            fastmem_manager.MarkDoNotFastmem(marker);
            InvalidateBasicBlocks({std::get<0>(marker)}, entry_point);
        }

        return fc;
    }

fail:
#ifdef __ANDROID__
    // Leave unexpected JIT faults for Android's crash reporter as well.
    return {0};
#else
    fmt::print("dynarmic: Segfault happened within JITted code at host_pc = {:016x}\n", host_pc);
    fmt::print("Segfault wasn't at a fastmem patch location!\n");
    ASSERT_FALSE("segfault");
#endif
}

}  // namespace Dynarmic::Backend::Arm64
