#ifndef FROND_IMX1_GPIO_H
#define FROND_IMX1_GPIO_H

#include <cstdint>

enum class Imx1GpioAccess { ok, unmapped, width, reservedBits, sideEffect, externalInput };
enum class Imx1GpioLine { clear, asserted, unknown };

// MC9328MX1RM chapter 32. Pin mux state is separate from external board devices.
class Imx1Gpio {
   public:
    Imx1Gpio();
    Imx1GpioAccess access(uint32_t address, uint32_t size, bool write, uint32_t& value);
    // Supply an immutable whole-port level before the port is accessed.
    bool setInput(unsigned port, uint32_t sample) { return setInputMasked(port, sample, 0xffffffff); }
    bool setInputMasked(unsigned port, uint32_t sample, uint32_t mask);
    // Reserve external input lanes before MMIO; diagnostic samples cannot own them.
    bool reserveInput(unsigned port, uint32_t mask);
    Imx1GpioAccess driveInput(unsigned port, uint32_t sample, uint32_t known, bool baseline = false);
    // Runtime board samples are separate from immutable diagnostic inputs.
    Imx1GpioAccess updateInput(unsigned port, uint32_t sample, uint32_t known);
    // Change only lanes owned by an explicit diagnostic baseline.
    Imx1GpioAccess transitionInput(unsigned port, uint32_t sample, uint32_t mask);
    Imx1GpioLine interruptOutput(unsigned port) const;
    // The port's input sample and the lanes fixed by setInputMasked.
    uint32_t inputSample(unsigned port) const { return port < 4 ? ports[port].sample : 0; }
    uint32_t fixedInputs(unsigned port) const { return port < 4 && ports[port].sampleKnown ? ports[port].fixedMask : 0; }
    uint32_t registerValue(unsigned port, unsigned index) const {
        return port < 4 && index < 17 ? ports[port].registers[index] : 0;
    }
    bool i2cPinsSelected() const {
        constexpr uint32_t pins = (1u << 15) | (1u << 16);
        return !((ports[0].registers[8] | ports[0].registers[14] | ports[0].registers[0]) & pins);
    }
    bool cs5PinsSelected(bool acknowledge) const {
        const auto& r = ports[0].registers;
        const uint32_t pins = (1u << 23) | (acknowledge ? 1u << 17 : 0);
        // Table 11-3: primary mux for CS5/DTACK, with PA17 configured as input.
        return !((r[8] | r[14]) & pins) && (!acknowledge || !(r[0] & (1u << 17)));
    }
    bool timer2OutputIsolated() const {
        const auto& r = ports[3].registers;
        // Tables 26-1 and 32-4/6/12: D31 must use GPIO input or DR output.
        return (r[8] & 0x80000000) && (!(r[0] & 0x80000000) || (r[2] >> 30) == 3);
    }
    bool outputPattern(unsigned port, uint32_t inputs, uint32_t high, uint32_t low) const {
        if (port >= 4 || (inputs & (high | low)) || (high & low)) return false;
        const auto& r = ports[port].registers;
        const uint32_t outputs = high | low, pins = inputs | outputs;
        if ((r[8] & pins) != pins || (r[0] & pins) != outputs || (r[7] & outputs) != high) return false;
        for (unsigned pin = 0; pin < 32; ++pin)
            if ((outputs & (1u << pin)) && ((r[pin < 16 ? 1 : 2] >> ((pin % 16) * 2)) & 3) != 3) return false;
        return true;
    }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        for (auto& p : ports) {
            a.DoBuffer32(p.registers, 17).Do32(p.sample).DoBool(p.sampleKnown).Do32(p.fixedMask).Do32(p.drivenMask);
            a.DoBool(p.accessed).DoBool(p.boardInput).Do32(p.inputKnown).Do32(p.statusKnown).Do32(p.pad).Do32(p.padKnown);
        }
    }
    static const char* registerName(uint32_t address);

   private:
    struct Port {
        uint32_t registers[17]{};
        uint32_t sample = 0;
        bool sampleKnown = false;
        uint32_t fixedMask = 0, drivenMask = 0;
        bool accessed = false;
        bool boardInput = false;
        uint32_t inputKnown = 0;
        uint32_t statusKnown = 0xffffffff;
        uint32_t pad = 0;
        uint32_t padKnown = 0;
    } ports[4];
    static void resolvePad(Port& port);
    static Imx1GpioAccess updateInterrupts(Port& next, const Port& old, unsigned index, bool sampleEvent);
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1Gpio) == 416, "update state() and stateVersion");
#endif

#endif
