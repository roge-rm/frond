#include "imx1_aitc.h"

const char* Imx1Aitc::registerName(uint32_t address) {
    static const char* const names[] = {
        "AITC.INTCNTL", "AITC.NIMASK", "AITC.INTENNUM", "AITC.INTDISNUM",
        "AITC.INTENABLEH", "AITC.INTENABLEL", "AITC.INTTYPEH", "AITC.INTTYPEL",
        "AITC.NIPRIORITY7", "AITC.NIPRIORITY6", "AITC.NIPRIORITY5", "AITC.NIPRIORITY4",
        "AITC.NIPRIORITY3", "AITC.NIPRIORITY2", "AITC.NIPRIORITY1", "AITC.NIPRIORITY0",
        "AITC.NIVECSR", "AITC.FIVECSR", "AITC.INTSRCH", "AITC.INTSRCL",
        "AITC.INTFRCH", "AITC.INTFRCL", "AITC.NIPNDH", "AITC.NIPNDL",
        "AITC.FIPNDH", "AITC.FIPNDL"
    };
    return address >= 0x00223000 && address < 0x00223068 ? names[(address - 0x00223000) / 4] : nullptr;
}

uint64_t Imx1Aitc::normalEligible() const {
    // NIMASK 31 masks nothing.
    if (mask == 31) return ~uint64_t(0);
    uint64_t result = 0;
    for (unsigned source = 0; source < 64; ++source)
        if (mask == 31 || level(source) > mask) result |= uint64_t(1) << source;
    return result;
}

uint64_t Imx1Aitc::pending(bool fast) const {
    return (inputs | force) & enabled & (fast ? type : ~type);
}

uint64_t Imx1Aitc::unknown(bool fast) const {
    return ~knownInputs & ~force & enabled & (fast ? type : ~type);
}

Imx1AitcLine Imx1Aitc::irq() const {
    // Only the pending and unknown sources can move the line, so the mask is worked out for those alone.
    uint64_t eligible = 0;
    for (uint64_t left = pending(false) | unknown(false); left; left &= left - 1) {
        const unsigned source = __builtin_ctzll(left);
        if (mask == 31 || level(source) > mask) eligible |= uint64_t(1) << source;
    }
    return pending(false) & eligible ? Imx1AitcLine::asserted :
           unknown(false) & eligible ? Imx1AitcLine::unknown : Imx1AitcLine::clear;
}

Imx1AitcLine Imx1Aitc::fiq() const {
    return pending(true) ? Imx1AitcLine::asserted :
           unknown(true) ? Imx1AitcLine::unknown : Imx1AitcLine::clear;
}

uint32_t Imx1Aitc::vector(bool fast) const {
    const uint64_t requests = pending(fast) & (fast ? ~uint64_t(0) : normalEligible());
    int selected = -1;
    unsigned best = 0;
    for (unsigned source = 0; source < 64; ++source) {
        if (!(requests & (uint64_t(1) << source))) continue;
        const auto candidate = level(source);
        if (fast || selected < 0 || candidate >= best) { selected = source; best = candidate; }
    }
    if (selected < 0) return 0xffffffff;
    return fast ? uint32_t(selected) : (uint32_t(selected) << 16) | best;
}

Imx1AitcAccess Imx1Aitc::access(uint32_t address, uint32_t size, bool write, uint32_t& value) {
    if (!registerName(address)) return Imx1AitcAccess::unmapped;
    if (size != 4 || (address & 3)) return Imx1AitcAccess::width;
    const unsigned reg = (address - 0x00223000) / 4;
    uint32_t* word = nullptr;
    uint64_t* pair = nullptr;
    uint32_t valid = 0xffffffff;
    if (reg == 0) { word = &control; valid = 0x00180000; }
    else if (reg == 1) { word = &mask; valid = 31; }
    else if (reg == 2 || reg == 3) {
        if (!write) { value = 0; return Imx1AitcAccess::ok; }
        if (value & ~63u) return Imx1AitcAccess::reservedBits;
        if (reg == 2) enabled |= uint64_t(1) << value;
        else enabled &= ~(uint64_t(1) << value);
        return Imx1AitcAccess::ok;
    } else if (reg == 4 || reg == 5) pair = &enabled;
    else if (reg == 6 || reg == 7) pair = &type;
    else if (reg >= 8 && reg <= 15) word = &priority[15 - reg];
    else if (reg == 20 || reg == 21) pair = &force;
    else {
        // Read-only writes and vector reads do not acknowledge or clear requests.
        if (write) return Imx1AitcAccess::ok;
        if (reg == 16 || reg == 17) {
            const bool fast = reg == 17;
            if (unknown(fast) & (fast ? ~uint64_t(0) : normalEligible())) return Imx1AitcAccess::externalInput;
            value = vector(fast);
        } else {
            const unsigned shift = (reg & 1) ? 0 : 32;
            const uint64_t requests = reg < 20 ? inputs | force : pending(reg >= 24);
            const uint64_t unavailable = reg < 20 ? ~knownInputs & ~force : unknown(reg >= 24);
            if (uint32_t(unavailable >> shift)) return Imx1AitcAccess::externalInput;
            value = requests >> shift;
        }
        return Imx1AitcAccess::ok;
    }
    if (word) {
        if (write) {
            // Section 10.4.2 explicitly permits writing -1 to disable no levels.
            if ((value & ~valid) && !(reg == 1 && value == 0xffffffff)) return Imx1AitcAccess::reservedBits;
            *word = value & valid;
        } else value = *word;
    } else {
        const unsigned shift = (reg & 1) ? 0 : 32;
        if (write) *pair = (*pair & ~(uint64_t(0xffffffff) << shift)) | (uint64_t(value) << shift);
        else value = *pair >> shift;
    }
    return Imx1AitcAccess::ok;
}
