#include "imx1_eim.h"

const char* Imx1Eim::registerName(uint32_t address) {
    static const char* names[] = {"EIM.CS0U", "EIM.CS0L", "EIM.CS1U", "EIM.CS1L",
                                  "EIM.CS2U", "EIM.CS2L", "EIM.CS3U", "EIM.CS3L",
                                  "EIM.CS4U", "EIM.CS4L", "EIM.CS5U", "EIM.CS5L", "EIM.WEIM"};
    if (address < 0x00220000 || address >= 0x00220034) return nullptr;
    return names[(address - 0x00220000) >> 2];
}

Imx1EimAccess Imx1Eim::access(uint32_t address, uint32_t size, bool write, uint32_t& value) {
    if (!registerName(address)) return Imx1EimAccess::unmapped;
    // MC9328MX1RM 11.5.7: control registers require word accesses.
    if (size != 4 || (address & 3)) return Imx1EimAccess::width;
    if (address == 0x00220030) {
        if (write) {
            if (value & ~4u) return Imx1EimAccess::reservedBits;
            if (value & 4) return Imx1EimAccess::sideEffect;
            configuration = value;
        } else value = configuration;
        return Imx1EimAccess::ok;
    }
    const unsigned chip = (address - 0x00220000) >> 3;
    const bool low = address & 4;
    uint32_t& reg = low ? lower[chip] : upper[chip];
    if (!write) {
        if (!chip && low && !bootWidthKnown) return Imx1EimAccess::externalInput;
        value = reg;
        return Imx1EimAccess::ok;
    }
    // Sections 11.6.1/2, table 11-5. CS0 has no PA output bit.
    const uint32_t mask = low ? (chip ? 0xffffff53 : 0xffffff51) : (chip == 5 ? 0xbfffff7f : 0x3fffff7f);
    if (value & ~mask) return Imx1EimAccess::reservedBits;
    if (!low) {
        // Table 11-5 and AN2422: external acknowledge is available on asynchronous CS5 only.
        if ((value & 0x00300000) || (chip != 5 && (value & 0x3f00) == 0x3f00) ||
            ((value & 0x80000000) && (value & 0x3f00) != 0x3f00))
            return Imx1EimAccess::sideEffect;
    } else if (!chip) {
        // Keep the probe's read-only 16-bit boot-ROM path enabled and unprotected.
        if ((value & 0x00000751) != 0x00000501) return Imx1EimAccess::sideEffect;
        bootWidthKnown = true;
    }
    reg = value;
    return Imx1EimAccess::ok;
}
