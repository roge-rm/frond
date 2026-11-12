#ifndef FROND_IMX1_GRAPHICS_ENGINE_H
#define FROND_IMX1_GRAPHICS_ENGINE_H

#include <algorithm>
#include <vector>
#include "imx1_bootstrap.h"
#include "imx1_graphics_windows.h"

// Virtual cold state has no work, a disabled ring and no configured clip bounds.
class Imx1GraphicsEngine {
   public:
    static bool owns(uint32_t address) {
        return address == 0x160113e8 || address == 0x16010144 || address == 0x160110a8 ||
               address == 0x160104dc || address == 0x16010214 || address == 0x16010220 || address == 0x1601021c ||
               address == 0x16010008 || address == 0x16010230 || address == 0x16010210 || address == 0x16011280 || address == 0x16010140;
    }
    void reset() { *this = {}; }
    bool setupComplete() const { return stage == 5; }
    bool clipContains(uint32_t x, uint32_t y) const {
        return stage >= 4 && x <= clipRight && y <= clipBottom;
    }
    uint32_t ringAddress() const { return ringBase; }
    uint32_t ringBaseWord() const { return ringBase | ringBaseTag; }
    uint32_t ringBytes() const { return ringEnabled ? 8u << ringExponent : 0; }
    bool emptyRing() const { return ringEnabled && readPointer == writePointer; }
    uint32_t consumer() const { return readPointer; }
    uint32_t producer() const { return writePointer; }
    bool hostDataSubmission() const { return hostData; }
    bool keyedCopySubmission() const { return keyedCopy; }
    bool a4Submission() const { return a4; }
    bool copyHelperSubmission() const { return copyHelper; }
    bool fillSubmission() const { return fill; }
    bool stretchCopySubmission() const { return stretchCopy; }
    // The driver's other submit sites the decoder knows: the chip's stretch blit (200b0644), the two
    // brush uploads of 200aedd0, the line routine (200afd00), the constant alpha blend (200a9414) and the
    // rotate and mirror blit (200aa564).
    enum Other : uint8_t { none, stretchBlit, monoBrush, colourBrush, line, blend, transform };
    Other otherSubmission() const { return other; }
    static Other otherProducer(uint32_t pc) {
        return pc == 0x200b0b14 ? stretchBlit : pc == 0x200af1fc ? monoBrush : pc == 0x200af448 ? colourBrush :
               pc == 0x200affe4 ? line : pc == 0x200a9788 ? blend : pc == 0x200aa8dc ? transform : none;
    }
    uint32_t submissionBegin() const { return previousProducer; }
    void start(uint64_t ticks) {
        if (previousProducer != writePointer) pending.push_back({writePointer, ticks});
    }
    void advance(uint64_t ticks) {
        for (auto& batch : pending) {
            const auto elapsed = std::min(batch.remaining, ticks);
            batch.remaining -= elapsed;
            ticks -= elapsed;
            if (!ticks) break;
        }
    }
    // Ticks until the first queued batch finishes. 0 when one waits to be completed.
    uint64_t idleQuietTicks() const { return pending.empty() ? UINT64_MAX : pending.front().remaining; }
    bool completed() const { return ringEnabled && !pending.empty() && !pending.front().remaining; }
    void complete(uint32_t option) {
        readPointer = pending.front().end;
        pending.erase(pending.begin());
        nativeOption = option;
        optionRead = false;
    }
    uint32_t nativeOptionWord() const { return nativeOption; }
    // Queued batches included. No more of them than ring words.
    static constexpr uint32_t stateVersion = 1;
    template <class A> void state(A& a) {
        a.Do32(stage).Do32(control).Do32(arbitration).Do32(crcControl).Do32(clipRight).Do32(clipBottom);
        a.Do32(ringStage).Do32(ringBase).Do32(ringBaseTag).Do32(ringExponent);
        a.Do32(readPointer).Do32(writePointer).Do32(scratch);
        a.DoBool(ringEnabled).DoBool(hostData).DoBool(keyedCopy).DoBool(a4).DoBool(copyHelper).Do32(previousProducer);
        a.Require(ringExponent <= 13, "engine ring size");
        uint64_t count = pending.size();
        a.Do64(count).Require(count <= 16384, "engine batches");
        if (!a.ok()) return;
        pending.resize(size_t(count));
        for (auto& batch : pending) a.Do32(batch.end).Do64(batch.remaining);
        a.Do32(nativeOption).DoBool(optionRead);
    }
    // The ring reads of the draw wait (200b8c64, 200b8c70, 200b8cb0) and the other read pointer polls, as access
    // answers them, changing nothing. The pointers move only at a submit or a batch's completion.
    bool quietRead(uint32_t pc, uint32_t address, uint32_t& value) const {
        if (!ringEnabled) return false;
        if (address == 0x1601021c && (pc == 0x200b8fcc || pc == 0x200b9030 || pc == 0x200b8c64)) value = readPointer;
        else if (address == 0x16010220 && pc == 0x200b8c70) value = writePointer;
        else if (address == 0x16010140 && pc == 0x200b8cb0) value = emptyRing() ? 0 : 0x80000000;
        else return false;
        return true;
    }
    Imx1BootstrapAccess access(const Imx1GraphicsWindows& windows, uint32_t pc, uint32_t address,
                               uint32_t size, bool write, uint32_t& value) {
        using A = Imx1BootstrapAccess;
        if (size != 4) return A::width;
        if (address == 0x1601021c) {
            if (write || (pc != 0x200b8fcc && pc != 0x200b9030 && pc != 0x200b8c64)) return A::externalInput;
            if (!ringEnabled) return A::sequence;
            value = readPointer;
            return A::ok;
        }
        if (address == 0x16010140) {
            if (write || pc != 0x200b8cb0) return A::externalInput;
            if (!ringEnabled) return A::sequence;
            value = emptyRing() ? 0 : 0x80000000;
            return A::ok;
        }
        if (address == 0x16010220 && pc != 0x200b8e88) {
            // 200b1568 and 200b1790 are the software stretch (200b0644), between its packets and after the last.
            const bool stretch = pc == 0x200b1568 || pc == 0x200b1790;
            const Other otherPc = otherProducer(pc);
            if (write ? (pc != 0x200a9ed4 && pc != 0x200b0200 && pc != 0x200ac300 && pc != 0x200aa504 && pc != 0x200a9a9c && !stretch && !otherPc) : pc != 0x200b8c70) return A::externalInput;
            if (!ringEnabled) return A::sequence;
            if (write) {
                const uint32_t mask = ringBytes() / 4 - 1;
                if (value > mask) return A::sideEffect;
                const uint32_t added = (value - writePointer) & mask;
                const uint32_t occupied = (writePointer - readPointer) & mask;
                if (!added && occupied) return A::sequence;
                if (added > mask - occupied || (!added && (pc == 0x200ac300 || pc == 0x200aa504 || pc == 0x200a9a9c || stretch || otherPc)))
                    return A::sideEffect;
                previousProducer = writePointer;
                hostData = pc == 0x200ac300;
                keyedCopy = pc == 0x200aa504;
                a4 = pc == 0x200a9a9c;
                copyHelper = pc == 0x200a9ed4;
                fill = pc == 0x200b0200;
                stretchCopy = stretch;
                other = otherPc;
                writePointer = value;
            } else value = writePointer;
            return A::ok;
        }
        if (address == 0x16011280) {
            if (!emptyRing()) return A::sequence;
            if (pc != (write ? 0x200b02fcu : 0x200b02acu)) return A::externalInput;
            if (write) {
                if (!optionRead) return A::sequence;
                if (value != (nativeOption | 0x10000)) return A::sideEffect;
                nativeOption = value;
                optionRead = false;
            } else {
                value = nativeOption;
                optionRead = true;
            }
            return A::ok;
        }
        if (address == 0x160113e8 && pc == 0x200acb98 && !write) {
            if (stage != 0) return A::sequence;
            value = control;
            stage = 1;
            return A::ok;
        }
        struct Operation { uint32_t pc, address, value; };
        constexpr Operation setup[] = {{0x200acba8, 0x160113e8, 0x400},
            {0x200acbb4, 0x16010144, 0x2440}, {0x200acbc4, 0x160110a8, 0x1fff1fff},
            {0x200acbd0, 0x160104dc, 0xc0000}};
        for (unsigned i = 0; i < 4; ++i) {
            if (pc != setup[i].pc || address != setup[i].address || !write) continue;
            if (stage != i + 1) return A::sequence;
            if (value != setup[i].value) return A::sideEffect;
            if (i == 0) control = value;
            if (i == 1) arbitration = value;
            if (i == 2) { clipRight = value & 0x1fff; clipBottom = value >> 16; }
            if (i == 3) crcControl = value;
            ++stage;
            return A::ok;
        }
        if (!write) return A::externalInput;
        if (stage != 5) return A::sequence;
        if (address == 0x16010214 && pc == 0x200b8e80) {
            if (ringStage != 0) return A::sequence;
            // Native address construction inserts bits 2..23, preserving bit 28.
            // The virtual profile retains that opaque tag without another address view.
            const uint32_t addressField = value & 0xfffffc;
            if ((value & ~0x10fffffcu) || !inBank(windows, addressField, 8)) return A::sideEffect;
            ringBase = addressField;
            ringBaseTag = value & 0x10000000;
        } else if (address == 0x16010220 && pc == 0x200b8e88) {
            if (ringStage != 1) return A::sequence;
            if (value != 0) return A::sideEffect;
            readPointer = value;
        } else if (address == 0x16010008 && pc == 0x200b8f04) {
            if (ringStage != 2) return A::sequence;
            const auto exponent = (value >> 24) + 9;
            if ((value & 0xffffff) != ringBase || exponent > 13 || (ringBase & ((8u << exponent) - 1)) ||
                !inBank(windows, ringBase, 8u << exponent)) return A::sideEffect;
            ringExponent = exponent;
        } else if (address == 0x16010230 && pc == 0x200b8f10) {
            if (ringStage != 3) return A::sequence;
            if (value != 0x80000000) return A::sideEffect;
            scratch = value;
        } else if (address == 0x16010210 && pc == 0x200b8f20) {
            if (ringStage != 4) return A::sequence;
            if (value != (0x08000000 | ringExponent)) return A::sideEffect;
            // Enabling the ring starts with no queued work.
            ringEnabled = true;
            writePointer = 0;
        } else return A::externalInput;
        ++ringStage;
        return A::ok;
    }
   private:
    static bool inBank(const Imx1GraphicsWindows& windows, uint32_t address, uint32_t bytes) {
        for (auto range : {windows.internal(), windows.external()})
            if (range.known && address >= range.first && uint64_t(address) + bytes <= uint64_t(range.last) + 1)
                return true;
        return false;
    }
    uint32_t stage = 0, control = 0, arbitration = 0, crcControl = 0;
    uint32_t clipRight = 0, clipBottom = 0;
    uint32_t ringStage = 0, ringBase = 0, ringBaseTag = 0, ringExponent = 0;
    uint32_t readPointer = 0, writePointer = 0, scratch = 0;
    bool ringEnabled = false, hostData = false, keyedCopy = false, a4 = false, copyHelper = false;
    // Set by the 220 write and read by the command decoder inside the same bus access, so not saved.
    bool fill = false, stretchCopy = false;
    Other other = none;
    uint32_t previousProducer = 0;
    struct Batch { uint32_t end; uint64_t remaining; };
    std::vector<Batch> pending;
    uint32_t nativeOption = 0;
    bool optionRead = false;
};
#if defined(__GLIBCXX__) && defined(__x86_64__)
static_assert(sizeof(Imx1GraphicsEngine) == 96, "update state() and stateVersion");
#endif
#endif
