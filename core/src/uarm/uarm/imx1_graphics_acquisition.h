#ifndef FROND_IMX1_GRAPHICS_ACQUISITION_H
#define FROND_IMX1_GRAPHICS_ACQUISITION_H

#include "imx1_bootstrap.h"
#include <array>

// Explicit virtual board routing, independent of unqualified reset pin levels.
enum class Imx1GraphicsMode2Route { unknown, defaultAux, alternateMux };
inline bool imx1GraphicsMode2RouteValid(Imx1GraphicsMode2Route route) {
    return route == Imx1GraphicsMode2Route::unknown || route == Imx1GraphicsMode2Route::defaultAux ||
           route == Imx1GraphicsMode2Route::alternateMux;
}

// Virtual acquisition policy for the native tagged-result consumer. No silicon timing or IRQ claim.
class Imx1GraphicsAcquisition {
   public:
    static constexpr uint32_t conversionTicks = 20000; // 100 us at the virtual 200 MHz reference.
    static bool owns(uint32_t a) { return a >= 0x16010d08 && a <= 0x16010d1c; }
    bool setInput(uint32_t first, uint32_t second) {
        if (first > 1023 || second > 1023) return false;
        input = {first, second}; inputKnown = true; return true;
    }
    bool setMode1Input(uint32_t sample) {
        if (sample > 1023) return false;
        mode1Input = sample; mode1InputKnown = true; return true;
    }
    bool setAuxInput(uint32_t sample) {
        if (sample > 1023) return false;
        auxInput = sample; auxInputKnown = true; return true;
    }
    bool setMode2Route(Imx1GraphicsMode2Route route) {
        if (!imx1GraphicsMode2RouteValid(route)) return false;
        mode2Route = route; return true;
    }
    // Sources 0/1 are the qualified alternate stick patterns, 2 is B22 high.
    void selectSource(uint32_t source) { selectedSource = source; }
    uint32_t source() const { return selectedSource; }
    // The registers access reads with no side effect, all but the FIFO (d14).
    bool peek(uint32_t a, uint32_t& v) const {
        switch (a) {
            case 0x16010d08: v = control; return true;
            case 0x16010d0c: v = trigger; return true;
            case 0x16010d10: v = configuration | (count << 20); return true;
            case 0x16010d1c: v = retained; return true;
            default: return false;
        }
    }
    void reset() {
        const auto saved = input; const bool known = inputKnown;
        const auto savedMode1 = mode1Input; const bool knownMode1 = mode1InputKnown;
        const auto savedAux = auxInput; const bool knownAux = auxInputKnown;
        const auto route = mode2Route;
        *this = {}; input = saved; inputKnown = known;
        mode1Input = savedMode1; mode1InputKnown = knownMode1;
        auxInput = savedAux; auxInputKnown = knownAux; mode2Route = route;
    }
    void advance(uint64_t ticks) {
        if (!remaining) return;
        if (ticks < remaining) { remaining -= ticks; return; }
        remaining = 0; count = capturedCount; head = 0;
        fifo = capturedRecords;
    }
    uint32_t queued() const { return count; }
    // The route and the raw inputs start from the options.
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.Do32(control).Do32(trigger).Do32(configuration).Do32(retained).DoEnum(mode2Route, 3);
        a.DoBool(inputKnown).DoBool(mode1InputKnown).DoBool(auxInputKnown).Do32(auxInput).Do32(mode1Input);
        a.DoBuffer32(input.data(), 2).DoBuffer32(capturedRecords.data(), 2).DoBuffer32(fifo.data(), 2);
        a.Do32(count).Do32(head).Do32(capturedCount).Do32(selectedSource).Do64(remaining);
        a.Require(count <= 2 && head <= 2 - count && capturedCount <= 2 && selectedSource <= 3, "acquisition fifo");
    }
    uint64_t deadlineRemaining() const { return remaining; }
    Imx1BootstrapAccess access(uint32_t, uint32_t a, uint32_t size, bool write, uint32_t& v) {
        using A = Imx1BootstrapAccess;
        if (!owns(a)) return A::unmapped;
        if (size != 4 || (a & 3)) return A::width;
        if (!write) {
            switch (a) {
                case 0x16010d08: v = control; break;
                case 0x16010d0c: v = trigger; break;
                case 0x16010d10: v = configuration | (count << 20); break;
                case 0x16010d14:
                    if (!count) return A::externalInput;
                    v = fifo[head++]; --count; break;
                case 0x16010d1c: v = retained; break;
                default: return A::externalInput;
            }
            return A::ok;
        }
        switch (a) {
            case 0x16010d08:
                // This is the admitted virtual vocabulary, not a silicon writable mask.
                if (v & ~0x2c03u) return A::sideEffect;
                if ((v & 7) > 2) return A::sideEffect;
                if (remaining && v != control && !(v & 0x800)) return A::sequence;
                if ((v & 0x800) && !(control & 0x800)) clearAcquisition();
                control = v; break;
            case 0x16010d10: {
                const uint32_t next = v & ~0x01f00000u; // Read-only count on native RMW writes.
                if (next & ~0x000f700au) return A::sideEffect;
                if (((next >> 16) & 15) != 0 && ((next >> 16) & 15) != 1 && ((next >> 16) & 15) != 2)
                    return A::sideEffect;
                if (remaining && next != configuration) return A::sequence;
                configuration = next; break;
            }
            case 0x16010d1c:
                if (v & ~8u) return A::sideEffect;
                retained = v; break;
            case 0x16010d0c:
                if (v & ~0x400u) return A::sideEffect;
                if ((v & 0x400) && !(trigger & 0x400)) {
                    if ((control & 0x2c00) != 0x2400 || !(retained & 8)) return A::sequence;
                    const uint32_t mode = control & 7;
                    const bool paired = mode == 0;
                    if (((configuration >> 16) & 15) != (paired ? 2u : 1u) ||
                        (configuration & 0x700a) != 0x700a) return A::sideEffect;
                    const bool aux = mode == 2 && (mode2Route == Imx1GraphicsMode2Route::defaultAux ||
                        (mode2Route == Imx1GraphicsMode2Route::alternateMux && selectedSource == 2));
                    const bool axes = mode == 0 || (mode == 2 &&
                        mode2Route == Imx1GraphicsMode2Route::alternateMux && selectedSource < 2);
                    if (mode == 1 ? !mode1InputKnown : aux ? !auxInputKnown : !axes || !inputKnown)
                        return A::externalInput;
                    if (remaining || count) return A::sequence;
                    capturedCount = paired ? 2 : 1;
                    capturedRecords = paired ? std::array<uint32_t,2>{0x01000000 | input[0], 0x02000000 | input[1]} :
                                               std::array<uint32_t,2>{mode == 1 ? 0x09000000 | mode1Input :
                                                                               0x0a000000 | (aux ? auxInput : input[selectedSource]), 0};
                    remaining = conversionTicks;
                } else if (!(v & 0x400)) {
                    // Stop cancels unfinished work. Completed records remain available to the guest.
                    remaining = 0;
                }
                trigger = v; break;
            default: return A::sideEffect;
        }
        return A::ok;
    }
   private:
    void clearAcquisition() { remaining = 0; count = head = trigger = 0; fifo = {}; }
    uint32_t control = 0, trigger = 0, configuration = 0, retained = 0;
    Imx1GraphicsMode2Route mode2Route = Imx1GraphicsMode2Route::unknown;
    bool inputKnown = false, mode1InputKnown = false, auxInputKnown = false;
    uint32_t auxInput = 0;
    uint32_t mode1Input = 0;
    std::array<uint32_t, 2> input{}, capturedRecords{}, fifo{};
    uint32_t count = 0, head = 0, capturedCount = 0, selectedSource = 3;
    uint64_t remaining = 0;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1GraphicsAcquisition) == 80, "update state() and stateVersion");
#endif
#endif
