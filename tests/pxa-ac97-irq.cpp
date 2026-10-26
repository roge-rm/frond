#include <cstdio>
#include <cstdlib>

#include "pxa_AC97.h"

static ArmMemAccessF accessRegister;
static void* controller;
static bool irq;

static void check(bool success) {
    if (!success) { std::fputs("FAIL: AC97 completion IRQ regression\n", stderr); std::abort(); }
}

bool memRegionAdd(ArmMem*, uint32_t, uint32_t, ArmMemAccessF access, void* context) {
    accessRegister = access;
    controller = context;
    return true;
}
void pxaIcInt(PxaIc*, uint_fast8_t, bool raised) { irq = raised; }
void pxaDmaExternalReq(PxaDma*, uint_fast8_t, bool) {}
extern "C" void uarmAbort() { std::abort(); }

static bool codecWrite(void*, uint32_t, uint16_t) { return true; }
static bool codecRead(void*, uint32_t, uint16_t* value) { *value = 0x1234; return true; }
static void write(uint32_t address, uint32_t value) {
    check(accessRegister(controller, address, 4, true, &value));
}
static uint32_t read(uint32_t address) {
    uint32_t value = 0;
    check(accessRegister(controller, address, 4, false, &value));
    return value;
}

int main() {
    auto* ac97 = pxaAC97Init(nullptr, nullptr, nullptr);
    pxaAC97clientAdd(ac97, Ac97PrimaryAudio, codecRead, codecWrite, nullptr);
    constexpr uint32_t gcr = 0x4050000c, gsr = 0x4050001c;
    constexpr uint32_t codec = 0x40500200, cdone = 1u << 19, sdone = 1u << 18;
    write(gcr, 0);
    write(codec + 0x76 * 2, 0x3f18);
    check(!irq && (read(gsr) & cdone));  // polling mode must not interrupt
    write(gcr, cdone);
    check(irq);  // unmasking an already completed command asserts the line
    write(gsr, cdone);
    check(!irq && !(read(gsr) & cdone));
    read(codec + 0x7c * 2);
    check(!irq && (read(gsr) & sdone));
    write(gcr, sdone);
    check(irq);
    write(gcr, 0);
    check(!irq && (read(gsr) & sdone));  // masking preserves the completion flag
    write(gsr, sdone);
    check(!irq && !(read(gsr) & sdone));
    constexpr uint32_t micr = 0x40500108, modemFifo = 0x40500140;
    write(micr, 2);  // modem receive FIFO service interrupt
    for (unsigned i = 0; i < 7; ++i)
        pxaAC97clientClientHaveData(ac97, Ac97PrimaryModem, i);
    check(!irq);
    pxaAC97clientClientHaveData(ac97, Ac97PrimaryModem, 7);
    check(irq && (read(gsr) & (1u << 1)));  // includes GSR modem input summary
    check(read(modemFifo) == 0);
    check(!irq && !(read(gsr) & (1u << 1)));  // summary clears on drain too
    pxaAC97clientClientHaveData(ac97, Ac97PrimaryModem, 8);
    check(irq);
    write(micr, 0);
    check(!irq);
    constexpr uint32_t mccr = 0x40500008, micFifo = 0x40500060;
    write(mccr, 2);
    for (unsigned i = 0; i < 8; ++i)
        pxaAC97clientClientHaveData(ac97, Ac97SecondaryAudio, i);
    check(irq && (read(gsr) & (1u << 7)));
    check(read(micFifo) == 0);
    check(!irq);
    write(mccr, 0);
    constexpr uint32_t mocr = 0x40500100;
    write(mocr, 2);
    uint32_t data = 0;
    pxaAC97clientClientWantData(ac97, Ac97PrimaryModem, &data);
    check(irq && (read(gsr) & (1u << 2)));  // empty transmit FIFO requests service too
    write(mocr, 0);
    check(!irq);
    // A FIFO underrun is an AC97 status condition, not a physical bus fault.
    auto* empty = pxaAC97Init(nullptr, nullptr, nullptr);
    (void)empty;
    constexpr uint32_t misr = 0x40500118;
    write(micr, 8);  // enable underrun/error interrupt
    check(read(modemFifo) == 0);
    check(irq && (read(misr) & 0x10) && (read(gsr) & 2));
    write(misr, 0x10);
    check(!irq);
    uint16_t halfword[2] = {0, 0xbeef};
    check(accessRegister(controller, modemFifo, 2, false, halfword));
    check(halfword[0] == 0 && halfword[1] == 0xbeef);
    write(micr, 0);
    // Empty receive reads expose the stale entry without advancing the pointer.
    for (unsigned i = 0; i < 16; ++i)
        pxaAC97clientClientHaveData(empty, Ac97PrimaryModem, 0x9000 + i);
    for (unsigned i = 0; i < 16; ++i) check(read(modemFifo) == 0x9000 + i);
    check(read(modemFifo) == 0x9000);
    check(read(modemFifo) == 0x9000);
    check(read(misr) & 0x10);
    // Overflowing a mapped transmit FIFO also completes the bus write.
    for (unsigned i = 0; i < 17; ++i) write(0x40500040, i);
    check(read(0x40500010) & 0x10);
    for (unsigned i = 0; i < 16; ++i) {
        pxaAC97clientClientWantData(empty, Ac97PrimaryAudio, &data);
        check(data == i);
    }
    pxaAC97clientClientWantData(empty, Ac97PrimaryAudio, &data);
    check(data == 15);  // documented transmit underrun repeats the last sample
    std::puts("PASS: AC97 IRQ masks, FIFO service, underrun/overflow, and halfword access");
}
