#ifndef FROND_IMX1_GRAPHICS_CONTROLLER_H
#define FROND_IMX1_GRAPHICS_CONTROLLER_H

#include <array>
#include "imx1_bootstrap.h"

// Virtual controller modes follow the qualified native setup sequence.
class Imx1GraphicsController {
   public:
    static bool owns(uint32_t address) { return address == 0x160101a4 || address == 0x16010180 || address == 0x1601018c ||
            address == 0x16010194 || address == 0x16010198 || address == 0x160101b0 || address == 0x160101b8; }
    void reset() { stage = externalStep = delay = 0; words = {}; initialised = false; }
    void advanceCpuCycles(uint32_t cycles) {
        if (!delay) return;
        if (cycles >= delay) { delay = 0; initialised = true; }
        else delay -= cycles;
    }
    bool externalInitialised() const { return initialised && delay == 0; }
    // The delay counts CPU cycles and ends by itself.
    bool delayRunning() const { return delay != 0; }
    bool configured() const { return stage >= 2; }
    bool ready() const { return stage == 3 && externalInitialised(); }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.DoBuffer32(words.data(), words.size()).Do32(stage).Do32(externalStep).Do32(delay).DoBool(initialised);
    }
    Imx1BootstrapAccess access(uint32_t pc, uint32_t address, uint32_t size, bool write, uint32_t& value) {
        if (size != 4) return Imx1BootstrapAccess::width;
        if (address == 0x16010194 && pc == 0x200c5f20 && write) {
            if (!ready()) return Imx1BootstrapAccess::sequence;
            if (value != 0x1545) return Imx1BootstrapAccess::sideEffect;
            word(0x194) = value;
            return Imx1BootstrapAccess::ok;
        }
        if (address == 0x160101a4 && pc == 0x200acb8c && write) {
            if (!ready()) return Imx1BootstrapAccess::sequence;
            if (value != 0x7800) return Imx1BootstrapAccess::sideEffect;
            return Imx1BootstrapAccess::ok;
        }
        if (address != 0x160101a4 && address != 0x16010180)
            return externalAccess(pc, address, write, value);
        if (!write || pc != (address == 0x160101a4 ? 0x200b31b0u : 0x200b31bcu))
            return Imx1BootstrapAccess::externalInput;
        const uint32_t expectedStage = address == 0x160101a4 ? 0 : 1;
        if (stage != expectedStage) return Imx1BootstrapAccess::sequence;
        if (value != (address == 0x160101a4 ? 0x7800u : 0x100007u)) return Imx1BootstrapAccess::sideEffect;
        ++stage;
        return Imx1BootstrapAccess::ok;
    }
    Imx1BootstrapAccess configureIo(uint32_t value) {
        if (stage != 2 || !externalInitialised()) return Imx1BootstrapAccess::sequence;
        if (value != 0x7bbb7444) return Imx1BootstrapAccess::sideEffect;
        stage = 3;
        return Imx1BootstrapAccess::ok;
    }
   private:
    // ASSUMPTION "chip memory status 1b8": bit 4 reads 0 at 200c4114 (the power down waits for it to clear,
    // no bound) and bit 0 reads 1 at 200c4380 once the mode command is done (the power up waits for it to
    // set, no bound). The ROM never writes 1b8 and nothing says what the bits are, so a read of 1b8 from
    // any other pc is refused. The mode command is taken as done after 32 charged guest CPU cycles.
    struct Operation { uint32_t pc, offset; bool write; uint32_t keep, set; };
    inline static constexpr std::array<Operation, 29> operations{{
        {0x200c4114, 0x1b8, false, 0, 0},
        {0x200c4130, 0x1b0, false, 0, 0}, {0x200c413c, 0x1b0, true, 0xfffffffd, 0xff0000},
        {0x200c4144, 0x194, false, 0, 0}, {0x200c4150, 0x194, true, 0xffefffff, 0xff000000},
        {0x200c4158, 0x18c, false, 0, 0}, {0x200c4164, 0x18c, true, 0xfffbffff, 0x80000},
        {0x200c4178, 0x18c, false, 0, 0}, {0x200c4188, 0x18c, true, 0x7ffdffff, 0x70000000},
        {0x200c4190, 0x18c, false, 0, 0}, {0x200c4198, 0x18c, true, 0xffffffff, 1},
        {0x200c41c4, 0x1b0, false, 0, 0}, {0x200c41d4, 0x1b0, true, 0xff00fffd, 0},
        {0x200c41e8, 0x194, false, 0, 0}, {0x200c4220, 0x194, true, 0xebc080, 0x1545},
        {0x200c4234, 0x18c, false, 0, 0}, {0x200c423c, 0x18c, true, 0xfff7ffff, 0},
        {0x200c4250, 0x198, false, 0, 0}, {0x200c4264, 0x18c, false, 0, 0},
        {0x200c42c0, 0x18c, true, 0xfffffff3, 0},
        {0x200c42d4, 0x18c, false, 0, 0}, {0x200c42e4, 0x18c, true, 0xfe7fffff, 0x9640000},
        {0x200c42f8, 0x18c, false, 0, 0}, {0x200c4304, 0x18c, true, 0xffdffff, 0},
        {0x200c4318, 0x18c, false, 0, 0}, {0x200c4320, 0x18c, true, 0xffffffff, 1},
        {0x200c4334, 0x198, true, 0x7ec0c000, 0x600021},
        {0x200c434c, 0x198, true, 0xffffffff, 0x80000000},
        {0x200c4364, 0x198, true, 0x7fffffff, 0}
    }};
    uint32_t& word(uint32_t offset) {
        return words[offset == 0x18c ? 0 : offset == 0x194 ? 1 : offset == 0x198 ? 2 : 3];
    }
    Imx1BootstrapAccess externalAccess(uint32_t pc, uint32_t address, bool write, uint32_t& value) {
        if (!configured()) return Imx1BootstrapAccess::sequence;
        if (!write && address == 0x160101b8 && pc == 0x200c4380) {
            if (externalStep != operations.size()) return Imx1BootstrapAccess::sequence;
            value = externalInitialised() ? 1 : 0;
            return Imx1BootstrapAccess::ok;
        }
        if (!write && address == 0x16010198 && pc == 0x200c4378) {
            if (externalStep != operations.size()) return Imx1BootstrapAccess::sequence;
            value = word(0x198);
            return Imx1BootstrapAccess::ok;
        }
        if (!write && address == 0x160101b0 && pc == 0x200c41c4 && ready() && externalStep == operations.size()) externalStep = 11;
        // The memory power down (200c4108) starts the list again on a chip that is up, and leaves it at the
        // power up half (200c41b8), which the ROM runs on the way back.
        if (!write && address == 0x160101b8 && pc == 0x200c4114 && ready() && externalStep == operations.size()) externalStep = 0;
        unsigned index = 0;
        for (; index < operations.size(); ++index)
            if (pc == operations[index].pc && address == 0x16010000 + operations[index].offset &&
                write == operations[index].write) break;
        if (index == operations.size()) return Imx1BootstrapAccess::externalInput;
        if (index != externalStep) return Imx1BootstrapAccess::sequence;
        const auto& op = operations[index];
        if (write) {
            uint32_t expected = (word(op.offset) & op.keep) | op.set;
            if (pc == 0x200c4220) expected |= value & 0xff000000;
            if (pc == 0x200c42c0) {
                const auto geometry = value & 0xc;
                if (geometry != 4 && geometry != 0xc) return Imx1BootstrapAccess::sideEffect;
                expected |= geometry;
            }
            if (pc == 0x200c4334 && (word(0x18c) & 0xc) == 0xc) expected |= 0x1000000;
            if (value != expected) return Imx1BootstrapAccess::sideEffect;
            word(op.offset) = value;
            if (pc == 0x200c4220) initialised = false;
            if (pc == 0x200c4364) { initialised = false; delay = 32; }
        } else value = op.offset == 0x1b8 ? 0 : word(op.offset);
        ++externalStep;
        return Imx1BootstrapAccess::ok;
    }
    std::array<uint32_t, 4> words{};
    uint32_t stage = 0, externalStep = 0, delay = 0;
    bool initialised = false;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1GraphicsController) == 32, "update state() and stateVersion");
#endif
#endif
