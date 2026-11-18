#include "s3c2442_blocks.h"

namespace {
    enum { sdicon = 0, sdipre, sdicarg, sdiccon, sdicsta, sdirsp0, sdirsp3 = 8, sdidtimer, sdibsize, sdidcon,
           sdidcnt, sdidsta, sdifsta, sdiimsk, sdidat };
    constexpr uint32_t cmdStart = 1u << 8, cmdTimeout = 1u << 10;
}

void S3cSdi::reset() {
    for (auto& r : regs) r = 0;
    regs[sdidtimer] = 0x10000;
}

S3cAccess S3cSdi::read(uint32_t offset, uint32_t& value) {
    if (offset & 3 || offset > 0x40) return S3cAccess::unknown;
    // Nothing comes from an empty slot: no FIFO data, no transfer.
    value = offset / 4 == sdidat || offset / 4 == sdifsta ? 0 : regs[offset / 4];
    return S3cAccess::ok;
}

S3cAccess S3cSdi::write(uint32_t offset, uint32_t value) {
    if (offset & 3 || offset > 0x40) return S3cAccess::unknown;
    const unsigned r = offset / 4;
    if (r == sdicsta || r == sdidsta) {
        regs[r] &= ~value;  // status bits clear by writing one
        return S3cAccess::ok;
    }
    if (r >= sdirsp0 && r <= sdirsp3) return S3cAccess::ok;  // read only
    regs[r] = value;
    if (r == sdiccon && (value & cmdStart)) {
        // No card answers: CmdOn falls and CmdTout rises, the response stays zero.
        ++commands;
        regs[sdicsta] = (regs[sdicsta] & ~0x1ffu) | cmdTimeout;
        regs[sdiccon] &= ~cmdStart;
    }
    return S3cAccess::ok;
}
