/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/frontend/A32/translate/a32_translate.h"

#include "dynarmic/frontend/A32/a32_location_descriptor.h"
#include "dynarmic/frontend/A32/decoder/arm.h"
#include "dynarmic/frontend/A32/decoder/asimd.h"
#include "dynarmic/frontend/A32/decoder/thumb16.h"
#include "dynarmic/frontend/A32/decoder/thumb32.h"
#include "dynarmic/frontend/A32/decoder/vfp.h"
#include "dynarmic/frontend/A32/translate/impl/a32_translate_impl.h"
#include "dynarmic/ir/basic_block.h"

namespace Dynarmic::A32 {

IR::Block TranslateArm(LocationDescriptor descriptor, TranslateCallbacks* tcb, const TranslationOptions& options);
IR::Block TranslateThumb(LocationDescriptor descriptor, TranslateCallbacks* tcb, const TranslationOptions& options);

IR::Block Translate(LocationDescriptor descriptor, TranslateCallbacks* tcb, const TranslationOptions& options) {
    return (descriptor.TFlag() ? TranslateThumb : TranslateArm)(descriptor, tcb, options);
}

bool TranslateSingleArmInstruction(IR::Block& block, LocationDescriptor descriptor, u32 instruction);
bool TranslateSingleThumbInstruction(IR::Block& block, LocationDescriptor descriptor, u32 instruction);

bool TranslateSingleInstruction(IR::Block& block, LocationDescriptor descriptor, u32 instruction) {
    return (descriptor.TFlag() ? TranslateSingleThumbInstruction : TranslateSingleArmInstruction)(block, descriptor, instruction);
}

void PrepareDecoders() {
    // Each decoder builds its table on its first call.
    (void)DecodeArm<TranslatorVisitor>(0);
    (void)DecodeASIMD<TranslatorVisitor>(0);
    (void)DecodeThumb16<TranslatorVisitor>(0);
    (void)DecodeThumb32<TranslatorVisitor>(0);
    (void)DecodeVFP<TranslatorVisitor>(0);
}

}  // namespace Dynarmic::A32
