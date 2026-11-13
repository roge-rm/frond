#include "i2cdev_OV9640.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "cputil.h"
#include "savestate/savestateAll.h"

#define OV9640_ADDRESS 0x30
#define OV9640_STATE_VERSION 0

enum : uint8_t {
    regPid = 0x0a, regVer = 0x0b, regCom1 = 0x04, regClkrc = 0x11, regCom7 = 0x12,
    regMidh = 0x1c, regMidl = 0x1d, regTslb = 0x3a, regCom15 = 0x40,
};

struct Ov9640 {
    PxaGpio *gpio;
    Ov9640Pins pins;
    uint8_t regs[256];
    // SCCB transaction state: the sub-address comes first in a write phase, a read phase
    // returns from it; both step it on.
    bool inTransaction, addressed, ours, reading, subAddressSeen;
    uint8_t subAddress;
};

// Power-on values from the OV9640 datasheet for the identity, format and clock registers;
// the rest start at zero.
static void ov9640PrvDefaults(Ov9640 *ov) {
    memset(ov->regs, 0, sizeof(ov->regs));
    ov->regs[0x01] = 0x80;  // BLUE
    ov->regs[0x02] = 0x80;  // RED
    ov->regs[0x03] = 0x12;  // VREF
    ov->regs[0x09] = 0x01;  // COM2
    ov->regs[regPid] = 0x96;
    ov->regs[regVer] = 0x48;
    ov->regs[0x0e] = 0x01;  // COM5
    ov->regs[0x0f] = 0x43;  // COM6
    ov->regs[0x10] = 0x40;  // AECH
    ov->regs[0x13] = 0x8f;  // COM8
    ov->regs[0x14] = 0x4a;  // COM9
    ov->regs[0x17] = 0x1d;  // HSTART
    ov->regs[0x18] = 0xbd;  // HSTOP
    ov->regs[0x19] = 0x01;  // VSTART
    ov->regs[0x1a] = 0x81;  // VSTOP
    ov->regs[regMidh] = 0x7f;
    ov->regs[regMidl] = 0xa2;
    ov->regs[0x24] = 0x78;  // AEW
    ov->regs[0x25] = 0x68;  // AEB
    ov->regs[0x26] = 0xd4;  // VPT
    ov->regs[regTslb] = 0x0c;
    ov->regs[regCom15] = 0xc0;  // output range 00-FF
}

static bool ov9640PrvPin(const Ov9640 *ov, uint8_t pin) {
    return pxaGpioGetState(ov->gpio, pin) == PxaGpioStateHigh;
}

bool ov9640Running(const Ov9640 *ov) {
    return ov9640PrvPin(ov, ov->pins.power) == ov->pins.powerActiveHigh && !ov9640PrvPin(ov, ov->pins.reset) &&
           !ov9640PrvPin(ov, ov->pins.powerDown);
}

uint8_t ov9640Register(const Ov9640 *ov, uint8_t reg) { return ov->regs[reg]; }

static void ov9640PrvWrite(Ov9640 *ov, uint8_t reg, uint8_t value) {
    switch (reg) {
        case regPid: case regVer: case regMidh: case regMidl:
            return;  // read only
        case regCom7:
            // Bit 7 resets every register to its default and reads back clear.
            if (value & 0x80) { ov9640PrvDefaults(ov); return; }
            break;
    }
    ov->regs[reg] = value;
}

static uint_fast8_t ov9640PrvI2c(void *userData, enum ActionI2C stimulus, uint_fast8_t value) {
    auto *ov = static_cast<Ov9640 *>(userData);
    switch (stimulus) {
        case i2cStart:
            ov->inTransaction = true;
            ov->subAddressSeen = false;
            [[fallthrough]];
        case i2cRestart:
            ov->addressed = false;
            ov->ours = false;
            return 0;
        case i2cStop:
            ov->inTransaction = false;
            ov->ours = false;
            return 0;
        case i2cTx:
            if (!ov->inTransaction) return 0;
            if (!ov->addressed) {
                ov->addressed = true;
                ov->ours = (value >> 1) == OV9640_ADDRESS && ov9640Running(ov);
                ov->reading = value & 1;
                return ov->ours ? 1 : 0;
            }
            if (!ov->ours || ov->reading) return 0;
            if (!ov->subAddressSeen) {
                ov->subAddressSeen = true;
                ov->subAddress = value;
                return 1;
            }
            ov9640PrvWrite(ov, ov->subAddress++, value);
            return 1;
        case i2cRx:
            if (!ov->inTransaction || !ov->ours || !ov->reading) return 0;
            return ov->regs[ov->subAddress++];
    }
    return 0;
}

// Power going off or RESET going high clears the registers back to their defaults.
static void ov9640PrvPinChanged(void *userData, uint32_t pin, bool, bool high) {
    auto *ov = static_cast<Ov9640 *>(userData);
    if ((pin == ov->pins.power && high != ov->pins.powerActiveHigh) || (pin == ov->pins.reset && high)) {
        ov9640PrvDefaults(ov);
        ov->ours = false;
    }
}

Ov9640 *ov9640Init(PxaI2c *i2c, PxaGpio *gpio, Ov9640Pins pins) {
    auto *ov = static_cast<Ov9640 *>(calloc(1, sizeof(Ov9640)));
    if (!ov) ERR("cannot alloc OV9640");
    ov->gpio = gpio;
    ov->pins = pins;
    ov9640PrvDefaults(ov);
    if (!pxaI2cDeviceAdd(i2c, ov9640PrvI2c, ov)) ERR("cannot add OV9640 to I2C");
    pxaGpioSetNotif(gpio, pins.power, ov9640PrvPinChanged, ov);
    pxaGpioSetNotif(gpio, pins.reset, ov9640PrvPinChanged, ov);
    return ov;
}

void ov9640Destroy(Ov9640 *ov) { free(ov); }

Ov9640Output ov9640Output(const Ov9640 *ov, uint32_t mclkHz) {
    Ov9640Output out{};
    const uint8_t com7 = ov->regs[regCom7], com15 = ov->regs[regCom15];
    if (com7 & 0x01) out.format = Ov9640Format::rawBayer;
    else if (com7 & 0x04) out.format = (com15 & 0x30) == 0x30 ? Ov9640Format::rgb555 : Ov9640Format::rgb565;
    else out.format = Ov9640Format::yuv422;
    out.yuvOrder = (ov->regs[regTslb] >> 2) & 3;
    // COM7 picks the size, largest bit first; COM1 bit 5 halves QVGA and QCIF again.
    if (com7 & 0x40) { out.width = 640; out.height = 480; }
    else if (com7 & 0x20) { out.width = 352; out.height = 288; }
    else if (com7 & 0x10) { out.width = 320; out.height = 240; }
    else if (com7 & 0x08) { out.width = 176; out.height = 144; }
    else { out.width = 1280; out.height = 960; }
    if ((com7 & 0x18) && (ov->regs[regCom1] & 0x20)) { out.width /= 2; out.height /= 2; }
    // Two pixel clocks a pixel. Frame length is 1520x1050 clocks at SXGA and 800x500 at VGA
    // and CIF; QVGA and QCIF take half that and the COM1 quarter sizes half again, so a size
    // keeps its frame rate when CLKRC halves the clock with it (Linux ov9640 tables: VGA
    // divider 2, QVGA 4, QQVGA 8). CLKRC: bit 7 doubles the input clock, bit 6 uses it
    // directly, else it is divided by 2 * (bits 5:0 + 1). Kept between 1 and 60 frames a second.
    const uint8_t clkrc = ov->regs[regClkrc];
    uint64_t clocks = (com7 & 0x78) ? 2ull * 800 * 500 : 2ull * 1520 * 1050;
    if (com7 & 0x18) clocks /= 2;
    if ((com7 & 0x18) && (ov->regs[regCom1] & 0x20)) clocks /= 2;
    uint64_t pclk = uint64_t(mclkHz) * ((clkrc & 0x80) ? 2 : 1);
    if (!(clkrc & 0x40)) pclk /= 2u * ((clkrc & 0x3f) + 1);
    uint64_t ns = pclk ? clocks * 1000000000ull / pclk : 1000000000ull;
    out.frameNs = uint32_t(std::clamp<uint64_t>(ns, 1000000000ull / 60, 1000000000ull));
    return out;
}

template <typename H> static void ov9640PrvState(Ov9640 &ov, H &h) {
    h.Do(typename H::BoolPack() << ov.inTransaction << ov.addressed << ov.ours << ov.reading
                                << ov.subAddressSeen)
        .Do8(ov.subAddress)
        .DoBuffer(ov.regs, sizeof(ov.regs));
}

template <typename T> void ov9640Save(Ov9640 *ov, T &savestate) {
    auto *chunk = savestate.GetChunk(ChunkType::ov9640, OV9640_STATE_VERSION);
    if (!chunk) ERR("cannot allocate OV9640 state");
    SaveChunkHelper h(*chunk);
    ov9640PrvState(*ov, h);
}

template <typename T> bool ov9640Load(Ov9640 *ov, T &loader) {
    if (!loader.HasChunk(ChunkType::ov9640)) {
        ov9640PrvDefaults(ov);
        ov->inTransaction = ov->addressed = ov->ours = ov->reading = ov->subAddressSeen = false;
        ov->subAddress = 0;
        return true;
    }
    auto *chunk = loader.GetChunkOrFail(ChunkType::ov9640, OV9640_STATE_VERSION, "OV9640");
    if (!chunk) return false;
    Ov9640 next = *ov;
    LoadChunkHelper h(*chunk);
    ov9640PrvState(next, h);
    if (chunk->HasError() || next.regs[regPid] != 0x96 || next.regs[regVer] != 0x48) {
        loader.NotifyError();
        return false;
    }
    *ov = next;
    return true;
}

template void ov9640Save<Savestate<ChunkType>>(Ov9640 *, Savestate<ChunkType> &);
template void ov9640Save<SavestateProbe<ChunkType>>(Ov9640 *, SavestateProbe<ChunkType> &);
template bool ov9640Load<SavestateLoader<ChunkType>>(Ov9640 *, SavestateLoader<ChunkType> &);
