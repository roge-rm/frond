#ifndef FROND_IMX1_GRAPHICS_MEMORY_H
#define FROND_IMX1_GRAPHICS_MEMORY_H

#include <array>
#include <cstring>
#include <vector>
#include "imx1_cs5_configuration.h"

class Imx1GraphicsMemory;

// One engine fill for a host GPU, in bank offsets (bytes, even). sourceLength is how far from sourceBase the fill
// may read. pixels: width by height inline pixels, or none.
struct Imx1GpuFill {
    unsigned bank = 0, sourceBank = 0;
    uint32_t base = 0, pitch = 0, width = 0, height = 0, colour = 0;
    uint32_t sourceBase = 0, sourcePitch = 0, sourceLength = 0, sourceX = 0, sourceY = 0, paint = 0;
    bool copy = false, keyed = false, doubled = false, invert = false, a4 = false, painted = false, keyEqual = false;
    const uint16_t* pixels = nullptr;
    uint32_t end() const { return base + (height - 1) * pitch + width * 2; }
};

// Where a host GPU holds a copy of graphics memory (Fast only): each 4 KB page of a bank is the same in both,
// newer in the bytes (the GPU copy needs an upload) or newer on the GPU (the bytes need a download). The memory
// asks it to bring pages back before the CPU reads them and marks them before the CPU writes them.
class Imx1GraphicsResidency {
   public:
    static constexpr uint32_t pageShift = 12;
    enum Page : uint8_t { same, cpuNewer, gpuNewer };
    virtual ~Imx1GraphicsResidency() = default;
    // Downloads the GPU-newer pages between bank offsets first and end.
    virtual void cpuReads(unsigned bank, uint32_t first, uint32_t end) = 0;
    // Before the CPU writes between bank offsets first and end: marks the pages newer in the bytes, or for a page
    // newer on the GPU records which bytes the CPU wrote, so the write needn't wait for the GPU.
    virtual void cpuWrites(unsigned bank, uint32_t first, uint32_t end) = 0;
    // Differential mode: a CPU write of size bytes the shadow copy takes too.
    virtual void mirrorWrite(unsigned bank, uint32_t offset, uint32_t size, uint32_t value) = 0;
    // Draws a fill on the GPU copy and records its writes; false leaves it to the CPU.
    virtual bool fill(const Imx1GpuFill& fill) = 0;
    // Differential mode: the CPU-only copy every fill also runs on, and the check of a GPU fill's pages against it.
    virtual Imx1GraphicsMemory* shadowMemory() = 0;
    virtual void compare(const Imx1GpuFill& fill) = 0;
    std::array<std::vector<uint8_t>, 2> pages;
    uint32_t gpuNewerPages = 0;
    bool mirror = false;
    // Fills left to the CPU while the GPU was attached.
    uint64_t cpuFills = 0;
    // What the next cpuReads or cpuWrites is for, for the GPU's download counts: a CPU read or write, a span (a
    // display row or a CPU fill), a copied range, a whole bank (hash, state) or a CPU fill's writes.
    enum Cause : uint8_t { access, accessWrite, span, copy, whole, writes, causes };
    Cause cause = access;
};

// Storage belongs to the probe, separately from transactional control state.
class Imx1GraphicsMemory {
   public:
    static constexpr uint32_t internalBytes = 0x60000, externalBytes = 0x800000;
    struct Dirty {
        uint32_t first = 0, end = 0;
        bool pending = false;
        uint64_t generation = 0;
    };
    enum class Consumer : unsigned { legacy, scanout, upload, persistence, count };
    Imx1GraphicsMemory() : bytes{std::vector<uint8_t>(internalBytes), std::vector<uint8_t>(externalBytes)} {}
    Imx1GraphicsMemory(const Imx1GraphicsMemory&) = delete;
    Imx1GraphicsMemory& operator=(const Imx1GraphicsMemory&) = delete;
    static bool owns(uint32_t address) {
        return (address >= 0x16080000 && address < 0x160c0000) ||
               (address >= 0x16100000 && address < 0x16160000) ||
               (address >= 0x16800000 && address < 0x17000000);
    }
    // Borrowed backing for private diagnostics, without acknowledging dirty consumers.
    const uint8_t* diagnosticData(unsigned bank, uint32_t& size) const {
        if (residency && bank < 2) {
            residency->cause = Imx1GraphicsResidency::whole;
            residency->cpuReads(bank, 0, uint32_t(bytes[bank].size()));
        }
        size = bank < 2 ? bytes[bank].size() : 0;
        return bank < 2 ? bytes[bank].data() : nullptr;
    }
    Dirty dirty(unsigned bank, Consumer consumer = Consumer::legacy) const {
        return bank < 2 && unsigned(consumer) < changes.size() ? changes[unsigned(consumer)][bank] : Dirty{};
    }
    void acknowledgeDirty(unsigned bank, Consumer consumer, uint64_t generation) {
        if (bank < 2 && unsigned(consumer) < changes.size()) {
            auto& change = changes[unsigned(consumer)][bank];
            if (change.generation == generation) change = {};
        }
    }
    // Dirty ranges and the write count.
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        for (auto& consumer : changes)
            for (auto& d : consumer) a.Do32(d.first).Do32(d.end).DoBool(d.pending).Do64(d.generation);
        a.Do64(generation);
    }
    template <class A> void stateBytes(A& a, unsigned bank) {
        const size_t size = bytes[bank].size();
        if (residency) {
            residency->cause = Imx1GraphicsResidency::whole;
            residency->cpuReads(bank, 0, uint32_t(size));
        }
        a.DoVector(bytes[bank], size).Require(bytes[bank].size() == size, "graphics memory size");
    }
    void clearDirty(unsigned bank) { acknowledgeDirty(bank, Consumer::legacy, dirty(bank).generation); }
    bool contains(const Imx1Cs5Configuration& configuration, const Imx1Eim& eim,
                  uint32_t address, uint64_t length, unsigned* bank = nullptr, uint32_t* offset = nullptr) const {
        uint32_t cpuAddress = 0;
        if (!length || !gpuToCpu(address, cpuAddress) || !configuration.memoryReady(eim)) return false;
        const unsigned selected = address < 0x800000 ? 0 : 1;
        const auto& windows = configuration.memoryWindows();
        const auto range = selected == 0 ? windows.internal() : windows.external();
        if (!range.known || address < range.first || address > range.last || length > uint64_t(range.last) + 1 - address ||
            uint64_t(address - range.first) + length > bytes[selected].size()) return false;
        if (bank) *bank = selected;
        if (offset) *offset = address - range.first;
        return true;
    }
    // Bank storage from the start of a range contains() accepts, for loops that have checked their whole range.
    // Writes made through it are recorded with noteWrites.
    uint8_t* gpuSpan(const Imx1Cs5Configuration& configuration, const Imx1Eim& eim, uint32_t address, uint64_t length,
                     unsigned& bank, uint32_t& offset) {
        if (!contains(configuration, eim, address, length, &bank, &offset)) return nullptr;
        if (residency) {
            residency->cause = Imx1GraphicsResidency::span;
            residency->cpuReads(bank, offset, uint32_t(offset + length));
        }
        return bytes[bank].data() + offset;
    }
    // count writes into bank, between offsets first and end, recorded as accessGpu records them one at a time.
    void noteWrites(unsigned bank, uint32_t first, uint32_t end, uint64_t count) {
        if (!count) return;
        if (residency) {
            residency->cause = Imx1GraphicsResidency::writes;
            residency->cpuWrites(bank, first, end);
        }
        noteDirty(bank, first, end, count);
    }
    // Writes made on the GPU copy, recorded for the dirty consumers as noteWrites does. The caller marks the pages.
    void noteDirty(unsigned bank, uint32_t first, uint32_t end, uint64_t count) {
        if (!count) return;
        generation += count;
        for (auto& consumer : changes) {
            auto& change = consumer[bank];
            if (!change.pending) change = {first, end, true, generation};
            else {
                if (first < change.first) change.first = first;
                if (end > change.end) change.end = end;
                change.generation = generation;
            }
        }
    }
    bool copyGpuRange(const Imx1Cs5Configuration& configuration, const Imx1Eim& eim,
                      uint32_t address, uint32_t length, uint8_t* destination) const {
        unsigned bank = 0;
        uint32_t offset = 0;
        if (!destination || !contains(configuration, eim, address, length, &bank, &offset)) return false;
        if (residency) {
            residency->cause = Imx1GraphicsResidency::copy;
            residency->cpuReads(bank, offset, offset + length);
        }
        std::memcpy(destination, bytes[bank].data() + offset, length);
        return true;
    }
    static bool cpuToGpu(uint32_t address, uint32_t& gpuAddress) {
        if (!owns(address) || address < 0x16100000) return false;
        gpuAddress = address - 0x16000000;
        return true;
    }
    static bool gpuToCpu(uint32_t address, uint32_t& cpuAddress) {
        if (address < 0x100000 || address > 0xffffff || !owns(0x16000000 + address)) return false;
        cpuAddress = 0x16000000 + address;
        return true;
    }
    Imx1BootstrapAccess access(const Imx1Cs5Configuration& configuration, const Imx1Eim& eim,
                              uint32_t address, uint32_t size, bool write, uint32_t& value) {
        uint32_t gpuAddress = 0;
        const auto status = resolve(configuration, address, size, gpuAddress);
        if (status != Imx1BootstrapAccess::ok) return status;
        return accessGpu(configuration, eim, gpuAddress, size, write, value);
    }
    Imx1BootstrapAccess validate(const Imx1Cs5Configuration& configuration, const Imx1Eim& eim,
                                uint32_t cpuAddress, uint32_t size) const {
        uint32_t address = 0;
        const auto status = resolve(configuration, cpuAddress, size, address);
        if (status != Imx1BootstrapAccess::ok) return status;
        if ((size != 1 && size != 2 && size != 4) || (address & (size-1))) return Imx1BootstrapAccess::width;
        return contains(configuration,eim,address,size) ? Imx1BootstrapAccess::ok : Imx1BootstrapAccess::sequence;
    }
    Imx1BootstrapAccess accessGpu(const Imx1Cs5Configuration& configuration, const Imx1Eim& eim,
                                 uint32_t gpuAddress, uint32_t size, bool write, uint32_t& value) {
        uint32_t cpuAddress = 0;
        if (!gpuToCpu(gpuAddress, cpuAddress)) return Imx1BootstrapAccess::unmapped;
        if ((size != 1 && size != 2 && size != 4) || (gpuAddress & (size - 1)))
            return Imx1BootstrapAccess::width;
        if (!configuration.memoryReady(eim)) return Imx1BootstrapAccess::sequence;
        const unsigned bank = gpuAddress < 0x800000 ? 0 : 1;
        const auto& windows = configuration.memoryWindows();
        const auto range = bank == 0 ? windows.internal() : windows.external();
        if (!range.known || gpuAddress < range.first || uint64_t(gpuAddress) + size > uint64_t(range.last) + 1)
            return Imx1BootstrapAccess::sequence;
        const uint32_t offset = gpuAddress - range.first;
        if (uint64_t(offset) + size > bytes[bank].size()) return Imx1BootstrapAccess::sequence;
        transfer(bank, offset, size, write, value);
        return Imx1BootstrapAccess::ok;
    }
    // validate and then access in one, for a caller that has seen configuration.memoryReady(eim) hold with the
    // configuration and EIM as they are now. True where both would give ok.
    bool accessReady(const Imx1Cs5Configuration& configuration, uint32_t cpuAddress, uint32_t size, bool write,
                     uint32_t& value) {
        uint32_t gpuAddress = 0, cpu = 0;
        if (resolve(configuration, cpuAddress, size, gpuAddress) != Imx1BootstrapAccess::ok ||
            (size != 1 && size != 2 && size != 4) || (gpuAddress & (size - 1)) || !gpuToCpu(gpuAddress, cpu))
            return false;
        const unsigned bank = gpuAddress < 0x800000 ? 0 : 1;
        const auto& windows = configuration.memoryWindows();
        const auto range = bank == 0 ? windows.internal() : windows.external();
        if (!range.known || gpuAddress < range.first || uint64_t(gpuAddress) + size > uint64_t(range.last) + 1)
            return false;
        const uint32_t offset = gpuAddress - range.first;
        if (uint64_t(offset) + size > bytes[bank].size()) return false;
        transfer(bank, offset, size, write, value);
        return true;
    }
    // The residency pointer, left out of the saved layout's size.
    static constexpr size_t unsavedBytes = sizeof(Imx1GraphicsResidency*);
    // The host GPU's copy, or none. Not saved; a load gives a new memory and the GPU marks every page again.
    void setResidency(Imx1GraphicsResidency* gpu) { residency = gpu; }
    Imx1GraphicsResidency* gpuResidency() const { return residency; }
    // Bank storage with no residency check, for the GPU's uploads, downloads and shadow copy.
    uint8_t* rawBank(unsigned bank, uint32_t& size) {
        size = bank < 2 ? uint32_t(bytes[bank].size()) : 0;
        return bank < 2 ? bytes[bank].data() : nullptr;
    }
   private:
    void transfer(unsigned bank, uint32_t offset, uint32_t size, bool write, uint32_t& value) {
        if (residency) {
            // An access never crosses a page, as it is aligned to its size.
            const uint8_t page = residency->pages[bank][offset >> Imx1GraphicsResidency::pageShift];
            residency->cause = write ? Imx1GraphicsResidency::accessWrite : Imx1GraphicsResidency::access;
            if (write) {
                if (page != Imx1GraphicsResidency::cpuNewer) residency->cpuWrites(bank, offset, offset + size);
                if (residency->mirror) residency->mirrorWrite(bank, offset, size, value);
            } else if (page == Imx1GraphicsResidency::gpuNewer) residency->cpuReads(bank, offset, offset + size);
        }
        if (write) {
            for (uint32_t i = 0; i < size; ++i) bytes[bank][offset + i] = uint8_t(value >> (i * 8));
            ++generation;
            for (auto& consumer : changes) {
                auto& change = consumer[bank];
                if (!change.pending) change = {offset, offset + size, true, generation};
                else {
                    if (offset < change.first) change.first = offset;
                    if (offset + size > change.end) change.end = offset + size;
                    change.generation = generation;
                }
            }
        } else {
            value = 0;
            for (uint32_t i = 0; i < size; ++i) value |= uint32_t(bytes[bank][offset + i]) << (i * 8);
        }
    }
    static Imx1BootstrapAccess resolve(const Imx1Cs5Configuration& configuration,
                                       uint32_t address, uint32_t size, uint32_t& gpuAddress) {
        using A = Imx1BootstrapAccess;
        if (address >= 0x16080000 && address < 0x160c0000) {
            if (size != 4 || (address & 3)) return A::width;
            const auto wrap = configuration.memoryWindows().wrap();
            const auto& engine = configuration.engineState();
            const uint32_t offset = address - 0x16000000;
            if (!wrap.known || !engine.ringBytes() || offset < wrap.first ||
                uint64_t(offset) + size > uint64_t(wrap.last) + 1) return A::sequence;
            // The configured CPU wrap view aliases the enabled ring's storage.
            gpuAddress = engine.ringAddress() + (offset - wrap.first) % engine.ringBytes();
            return A::ok;
        }
        return cpuToGpu(address, gpuAddress) ? A::ok : A::unmapped;
    }
    std::array<std::vector<uint8_t>, 2> bytes;
    Imx1GraphicsResidency* residency = nullptr;
    std::array<std::array<Dirty, 2>, unsigned(Consumer::count)> changes{};
    uint64_t generation = 0;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1GraphicsMemory) - Imx1GraphicsMemory::unsavedBytes == 248, "update state() and stateVersion");
#endif
#endif
