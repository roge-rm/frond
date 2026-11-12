#include "imx1_sysctrl.h"

const char* Imx1Sysctrl::registerName(uint32_t address) {
    if ((address & ~3u) == 0x0021b80c) return "SCM.GPCR";
    if ((address & ~3u) == 0x0021b808) return "SCM.FMCR";
    return (address & ~3u) == 0x0021b810 ? "SCM.GCCR" : nullptr;
}

Imx1SysctrlAccess Imx1Sysctrl::access(uint32_t address, uint32_t size, bool write, uint32_t& value) {
    if (!registerName(address)) return Imx1SysctrlAccess::unmapped;
    // Diagnostic subset of the 32-bit registers in MC9328MX1RM 8.1.
    if (size != 4 || (address & 3)) return Imx1SysctrlAccess::width;
    if (address == 0x0021b80c) {
        if (write) {
            // MC9328MX1RM table 8-4: bit 3 stays set for normal operation.
            if ((value & ~0x9ffbu) || !(value & 8)) return Imx1SysctrlAccess::reservedBits;
            // MEMC test routing and MMA/CSI access control are unqualified. BTAEN only gates the clock
            // inputs of the BTA module (table 8-4).
            if ((value ^ gpcr) & 0x8003) return Imx1SysctrlAccess::sideEffect;
            gpcr = value;
        } else value = gpcr;
        return Imx1SysctrlAccess::ok;
    }
    if (address == 0x0021b808) {
        if (write) {
            if (value & ~0x1ffu) return Imx1SysctrlAccess::reservedBits;
            // Changing SDRAM chip selects would invalidate the current memory map.
            if ((value ^ fmcr) & 3) return Imx1SysctrlAccess::sideEffect;
            if ((value ^ fmcr) & 4) return Imx1SysctrlAccess::externalInput;
            fmcr = value;
        } else value = fmcr;
        return Imx1SysctrlAccess::ok;
    }
    if (write) {
        if (value & ~0x7fu) return Imx1SysctrlAccess::reservedBits;
        // Table 8-5: the BROM HCLK override depends on the unknown BOOT[3:0] pins.
        if ((value ^ gccr) & 0x10) return Imx1SysctrlAccess::externalInput;
        gccr = value;
    } else value = gccr;
    return Imx1SysctrlAccess::ok;
}
