#ifndef FROND_IMX1_GRAPHICS_WINDOWS_H
#define FROND_IMX1_GRAPHICS_WINDOWS_H

#include <array>
#include "imx1_bootstrap.h"

// Native 90003 ranges use 256-byte units; direct wrap bounds use halfwords.
class Imx1GraphicsWindows {
   public:
    struct Range {
        uint32_t first = 0, last = 0;
        bool known = false;
    };
    static bool owns(uint32_t address) {
        for (auto offset : offsets) if (address == 0x16010000 + offset) return true;
        return false;
    }
    void reset() { words = {}; known = {}; }
    Range internal() const { return range(0); }
    Range external() const { return range(1); }
    Range aperture1() const { return range(2); }
    Range aperture2() const { return range(3); }
    Range wrap() const {
        return {words[4] << 1, (words[5] + 1) * 2 - 1, known[4] && known[5]};
    }
    Imx1BootstrapAccess access(uint32_t pc, uint32_t address, uint32_t size,
                              bool write, uint32_t& value) {
        if (size != 4) return Imx1BootstrapAccess::width;
        for (unsigned i = 0; i < offsets.size(); ++i) {
            if (address != 0x16010000 + offsets[i]) continue;
            if (pc != (write ? writePcs[i] : readPcs[i]) &&
                !(!write && i == 0 && pc == 0x200a62ce)) return Imx1BootstrapAccess::externalInput;
            if (write) {
                if (value != settings[i]) return Imx1BootstrapAccess::sideEffect;
                words[i] = value;
                known[i] = true;
            } else {
                if (!known[i]) return Imx1BootstrapAccess::sequence;
                value = words[i];
            }
            return Imx1BootstrapAccess::ok;
        }
        return Imx1BootstrapAccess::unmapped;
    }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.DoBuffer32(words.data(), words.size());
        for (auto& k : known) a.DoBool(k);
    }
   private:
    Range range(unsigned index) const {
        return {(words[index] & 0xffff) << 8, ((words[index] >> 16) + 1) * 256 - 1, known[index]};
    }
    inline static constexpr std::array<uint32_t, 6> offsets{0x188, 0x190, 0x1bc, 0x1c0, 0x14, 0x10};
    inline static constexpr std::array<uint32_t, 6> settings{0x15ff1000, 0xffff8000, 0x7fff4000, 0x203f2000, 0x40000, 0x5ffff};
    inline static constexpr std::array<uint32_t, 6> writePcs{0x200c50f4, 0x200c5130, 0x200c5168, 0x200c51a0, 0x200c51d8, 0x200c51fc};
    inline static constexpr std::array<uint32_t, 6> readPcs{0x200c50fc, 0x200c5138, 0x200c5170, 0x200c51a8, 0x200c51e4, 0x200c5204};
    std::array<uint32_t, 6> words{};
    std::array<bool, 6> known{};
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1GraphicsWindows) == 32, "update state() and stateVersion");
#endif
#endif
