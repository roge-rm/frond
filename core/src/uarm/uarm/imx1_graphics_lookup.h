#ifndef FROND_IMX1_GRAPHICS_LOOKUP_H
#define FROND_IMX1_GRAPHICS_LOOKUP_H

#include <array>
#include <memory>
#include "imx1_bootstrap.h"

// Native initialization loads 256 pairs through two ordered data ports.
class Imx1GraphicsLookup {
   public:
    struct Entry {
        uint32_t first = 0, second = 0;
        bool firstKnown = false, secondKnown = false;
    };
    static bool owns(uint32_t address) {
        return address == 0x16010240 || address == 0x16010244 ||
               address == 0x1601024c || address == 0x16010250;
    }
    void reset() { entries = std::make_shared<Entries>(); selected = indexed = secondPending = false; index = 0; }
    Entry entry(unsigned slot) const { return slot < entries->size() ? (*entries)[slot] : Entry{}; }
    unsigned nextIndex() const { return index; }
    Imx1BootstrapAccess access(uint32_t pc, uint32_t address, uint32_t size,
                              bool write, uint32_t& value) {
        if (size != 4) return Imx1BootstrapAccess::width;
        const uint32_t expectedPc = address == 0x16010240 ? 0x200baf54 :
            address == 0x16010244 ? 0x200baf60 : address == 0x1601024c ? 0x200baf7c :
            address == 0x16010250 ? 0x200baf88 : 0;
        if (!expectedPc) return Imx1BootstrapAccess::unmapped;
        if (!write || pc != expectedPc) return Imx1BootstrapAccess::externalInput;
        if (address == 0x16010240) {
            if (value != 0x401f0000) return Imx1BootstrapAccess::sideEffect;
            selected = true;
            indexed = secondPending = false;
        } else if (address == 0x16010244) {
            if (!selected) return Imx1BootstrapAccess::sequence;
            if (value != 0) return Imx1BootstrapAccess::sideEffect;
            index = 0;
            indexed = true;
            secondPending = false;
        } else {
            if (!indexed || index >= entries->size() ||
                secondPending != (address == 0x16010250)) return Imx1BootstrapAccess::sequence;
            auto& item = writable()[index];
            if (!secondPending) {
                item.first = value;
                item.firstKnown = true;
                secondPending = true;
            } else {
                item.second = value;
                item.secondKnown = true;
                secondPending = false;
                ++index;
            }
        }
        return Imx1BootstrapAccess::ok;
    }
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        for (auto& e : writable()) a.Do32(e.first).Do32(e.second).DoBool(e.firstKnown).DoBool(e.secondKnown);
        a.Do32(index).DoBool(selected).DoBool(indexed).DoBool(secondPending);
        a.Require(index <= entries->size(), "lookup index");
    }
   private:
    using Entries = std::array<Entry, 256>;
    // The table is shared between copies and copied on the first write, as the probe copies the whole
    // graphics chip on every CS5 access and the table only changes during the boot's load.
    std::shared_ptr<Entries> entries = std::make_shared<Entries>();
    unsigned index = 0;
    bool selected = false, indexed = false, secondPending = false;
    Entries& writable() {
        if (entries.use_count() != 1) entries = std::make_shared<Entries>(*entries);
        return *entries;
    }

   public:
    // The size the layout had with the table held in place, for the state layout checks.
    static constexpr size_t layoutBytes();
};
constexpr size_t Imx1GraphicsLookup::layoutBytes() {
    return sizeof(Imx1GraphicsLookup) - sizeof(std::shared_ptr<Entries>) + sizeof(Entries);
}
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(Imx1GraphicsLookup::layoutBytes() == 3080, "update state() and stateVersion");
#endif
#endif
