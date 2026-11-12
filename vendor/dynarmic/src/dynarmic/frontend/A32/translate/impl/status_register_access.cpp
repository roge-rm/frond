/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <mcl/bit/bit_field.hpp>
#include <mcl/bit/rotate.hpp>

#include "dynarmic/frontend/A32/translate/impl/a32_translate_impl.h"

namespace Dynarmic::A32 {

// CPS<effect> <iflags>{, #<mode>}
// CPS #<mode>
bool TranslatorVisitor::arm_CPS() {
    return InterpretThisInstruction();
}

// MRS<c> <Rd>, <spec_reg>
bool TranslatorVisitor::arm_MRS(Cond cond, Reg d) {
    if (d == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    ir.SetRegister(d, ir.GetCpsr());
    return true;
}

// MSR<c> <spec_reg>, #<const>
bool TranslatorVisitor::arm_MSR_imm(Cond cond, unsigned mask, int rotate, Imm<8> imm8) {
    ASSERT_MSG(mask != 0, "Decode error");

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const bool write_nzcvq = mcl::bit::get_bit<3>(mask);
    const bool write_g = mcl::bit::get_bit<2>(mask);
    const bool write_e = mcl::bit::get_bit<1>(mask);
    const u32 imm32 = ArmExpandImm(rotate, imm8);

    if (write_nzcvq) {
        ir.SetCpsrNZCVQ(ir.Imm32(imm32 & 0xF8000000));
    }

    if (write_g) {
        ir.SetGEFlagsCompressed(ir.Imm32(imm32 & 0x000F0000));
    }

    if (write_e) {
        const bool E = (imm32 & 0x00000200) != 0;
        if (E != ir.current_location.EFlag()) {
            ir.SetTerm(IR::Term::LinkBlock{ir.current_location.AdvancePC(4).SetEFlag(E)});
            return false;
        }
    }

    return true;
}

// MSR<c> <spec_reg>, <Rn>
bool TranslatorVisitor::arm_MSR_reg(Cond cond, unsigned mask, Reg n) {
    if (mask == 0) {
        return UnpredictableInstruction();
    }

    if (n == Reg::PC) {
        return UnpredictableInstruction();
    }

    if (!ArmConditionPassed(cond)) {
        return true;
    }

    const bool write_nzcvq = mcl::bit::get_bit<3>(mask);
    const bool write_g = mcl::bit::get_bit<2>(mask);
    const bool write_e = mcl::bit::get_bit<1>(mask);
    const auto value = ir.GetRegister(n);

    if (!write_e) {
        if (write_nzcvq) {
            ir.SetCpsrNZCVQ(ir.And(value, ir.Imm32(0xF8000000)));
        }

        if (write_g) {
            ir.SetGEFlagsCompressed(ir.And(value, ir.Imm32(0x000F0000)));
        }
    } else {
        const u32 cpsr_mask = (write_nzcvq ? 0xF8000000 : 0) | (write_g ? 0x000F0000 : 0) | 0x00000200;
        const auto old_cpsr = ir.And(ir.GetCpsr(), ir.Imm32(~cpsr_mask));
        const auto new_cpsr = ir.And(value, ir.Imm32(cpsr_mask));
        ir.SetCpsr(ir.Or(old_cpsr, new_cpsr));
        ir.PushRSB(ir.current_location.AdvancePC(4));
        ir.BranchWritePC(ir.Imm32(ir.current_location.PC() + 4));
        ir.SetTerm(IR::Term::CheckHalt{IR::Term::PopRSBHint{}});
        return false;
    }

    return true;
}

// Frond: MRS Rd, SPSR, MSR to SPSR or to CPSR's control, extension and status fields, and
// the MOVS PC and SUBS PC exception returns, through options.psr_coprocessor. Returns false
// for anything else, which then decodes as usual (MSR CPSR_f stays a plain flags write).
bool TranslatorVisitor::frond_PSR(u32 instruction, bool& should_continue) {
    const auto cond = static_cast<Cond>(instruction >> 28);
    if (cond == Cond::NV) {
        return false;
    }
    const size_t coproc = *options.psr_coprocessor;

    if ((instruction & 0x0FFF0FFF) == 0x014F0000) {  // MRS Rd, SPSR
        const auto d = static_cast<Reg>((instruction >> 12) & 0xF);
        if (d == Reg::PC) {
            return false;
        }
        should_continue = true;
        if (ArmConditionPassed(cond)) {
            ir.SetRegister(d, ir.CoprocGetOneWord(coproc, false, 0, CoprocReg::C0, CoprocReg::C0, 0));
        }
        return true;
    }

    // MOVS PC, Rm and SUBS PC, Rn, #imm: exception return, CPSR = SPSR.
    const bool movs = (instruction & 0x0FFFFFF0) == 0x01B0F000;
    const bool subs = (instruction & 0x0FF0F000) == 0x0250F000;
    if (movs || subs) {
        const auto n = static_cast<Reg>(movs ? instruction & 0xF : (instruction >> 16) & 0xF);
        if (n == Reg::PC) {
            return false;
        }
        should_continue = true;
        if (!ArmConditionPassed(cond)) {
            return true;
        }
        IR::U32 target = ir.GetRegister(n);
        if (subs) {
            target = IR::U32{ir.Sub(target, ir.Imm32(mcl::bit::rotate_right<u32>(instruction & 0xFF, static_cast<int>((instruction >> 8) & 0xF) * 2)))};
        }
        ir.CoprocSendTwoWords(coproc, false, 2, CoprocReg::C0, target, ir.Imm32(0));
        ir.SetTerm(IR::Term::FastDispatchHint{});
        should_continue = false;
        return true;
    }

    const bool imm = (instruction & 0x0FB0F000) == 0x0320F000;
    if (!imm && (instruction & 0x0FB0FFF0) != 0x0120F000) {
        return false;
    }
    const bool spsr = mcl::bit::get_bit<22>(instruction);
    const u32 mask = (instruction >> 16) & 0xF;
    if (mask == 0 || (!spsr && mask == 0b1000)) {
        return false;
    }
    if (!imm && static_cast<Reg>(instruction & 0xF) == Reg::PC) {
        return false;
    }

    should_continue = true;
    if (!ArmConditionPassed(cond)) {
        return true;
    }
    const IR::U32 value = imm ? ir.Imm32(mcl::bit::rotate_right<u32>(instruction & 0xFF, static_cast<int>((instruction >> 8) & 0xF) * 2))
                              : ir.GetRegister(static_cast<Reg>(instruction & 0xF));
    ir.CoprocSendTwoWords(coproc, false, spsr ? 1 : 0, CoprocReg::C0, value, ir.Imm32(mask));
    if (!spsr) {
        ir.SetTerm(IR::Term::LinkBlock{ir.current_location.AdvancePC(4)});
        should_continue = false;
    }
    return true;
}

// RFE{<amode>} <Rn>{!}
bool TranslatorVisitor::arm_RFE() {
    return InterpretThisInstruction();
}

// SETEND <endian_specifier>
bool TranslatorVisitor::arm_SETEND(bool E) {
    ir.SetTerm(IR::Term::LinkBlock{ir.current_location.AdvancePC(4).SetEFlag(E)});
    return false;
}

// SRS{<amode>} SP{!}, #<mode>
bool TranslatorVisitor::arm_SRS() {
    return InterpretThisInstruction();
}

}  // namespace Dynarmic::A32
