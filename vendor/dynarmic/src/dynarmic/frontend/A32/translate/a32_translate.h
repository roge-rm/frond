/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include <mcl/stdint.hpp>

#include "dynarmic/interface/A32/arch_version.h"

namespace Dynarmic::IR {
class Block;
}  // namespace Dynarmic::IR

namespace Dynarmic::A32 {

class LocationDescriptor;
struct TranslateCallbacks;

struct TranslationOptions {
    ArchVersion arch_version;

    /// This changes what IR we emit when we translate an unpredictable instruction.
    /// If this is false, the ExceptionRaised IR instruction is emitted.
    /// If this is true, we define some behaviour for some instructions.
    bool define_unpredictable_behaviour = false;

    /// This changes what IR we emit when we translate a hint instruction.
    /// If this is false, we treat the instruction as a NOP.
    /// If this is true, we emit an ExceptionRaised instruction.
    bool hook_hint_instructions = true;

    /// Frond: see UserConfig::psr_coprocessor.
    std::optional<std::size_t> psr_coprocessor;

    /// Frond: see UserConfig::max_unchecked_instructions. 0 means no limit.
    std::size_t max_instructions = 0;

    /// Frond: see UserConfig::coprocessor_unscaled_offsets.
    std::uint16_t coprocessor_unscaled_offsets = 0;
};

/**
 * This function translates instructions in memory into our intermediate representation.
 * @param descriptor The starting location of the basic block. Includes information like PC, Thumb state, &c.
 * @param tcb The callbacks we should use to read emulated memory.
 * @param options Configures how certain instructions are translated.
 * @return A translated basic block in the intermediate representation.
 */
IR::Block Translate(LocationDescriptor descriptor, TranslateCallbacks* tcb, const TranslationOptions& options);

/**
 * This function translates a single provided instruction into our intermediate representation.
 * @param block The block to append the IR for the instruction to.
 * @param descriptor The location of the instruction. Includes information like PC, Thumb state, &c.
 * @param instruction The instruction to translate.
 * @return The translated instruction translated to the intermediate representation.
 */
bool TranslateSingleInstruction(IR::Block& block, LocationDescriptor descriptor, u32 instruction);

/// Frond: builds the decoder tables, which are otherwise built by the first translation
/// (the ARM one takes about 17 ms on x64). Safe to call more than once and from any thread.
void PrepareDecoders();

}  // namespace Dynarmic::A32
