#include "spitz_nand.h"

#include <algorithm>
#include <array>
#include <cstring>

#include "mem.h"
#include "nand.h"
#include "savestate/savestateAll.h"

namespace {
    constexpr uint32_t base = 0x0c000000;
    constexpr uint32_t regionSize = 0x1000;
    constexpr uint32_t pageData = 512;
    constexpr uint32_t pageSpare = 16;
    constexpr uint32_t pageBytes = pageData + pageSpare;
    constexpr uint32_t pagesPerBlock = 32;
    constexpr uint32_t blockData = pagesPerBlock * pageData;
    constexpr uint32_t blockBytes = pagesPerBlock * pageBytes;
    constexpr uint32_t blocks = 1024;
    constexpr uint32_t headerBytes = 16;

    constexpr uint8_t chipDisable = 0x01;
    constexpr uint8_t commandLatch = 0x02;
    constexpr uint8_t addressLatch = 0x04;
    constexpr uint8_t writeEnable = 0x08;
    constexpr uint8_t unusedChipDisable = 0x10;
    constexpr uint8_t ready = 0x20;
    constexpr uint8_t statusCommand = 0x70;
    constexpr uint8_t resetCommand = 0xff;

    uint16_t read16(const uint8_t* bytes) {
        return uint16_t(bytes[0]) | uint16_t(bytes[1]) << 8;
    }

    uint32_t read32(const uint8_t* bytes) {
        return uint32_t(read16(bytes)) | uint32_t(read16(bytes + 2)) << 16;
    }
}

bool spitzImportDbk(const uint8_t* data, size_t size, std::vector<uint8_t>& raw) {
    constexpr size_t recordBytes = headerBytes + blockBytes;
    if (!data || size != headerBytes + blocks * recordBytes ||
        std::memcmp(data, "[I]JFFS2TOP:", 12) != 0 || read32(data + 12) != 0x6a0000)
        return false;

    std::vector<uint8_t> image(spitzNandRawSize);
    for (uint32_t block = 0; block < blocks; ++block) {
        const auto* record = data + headerBytes + block * recordBytes;
        const auto* tag = record + 4;
        if (read32(record) != block * blockData ||
            (std::memcmp(tag, "[GL]", 4) && std::memcmp(tag, "[GP]", 4) &&
             std::memcmp(tag, "[E] ", 4)))
            return false;

        // Error records keep their spare bytes. The tag alone is no proof of a bad-block marker.
        const auto* main = record + headerBytes;
        const auto* spare = main + blockData;
        auto* dest = image.data() + block * blockBytes;
        for (uint32_t page = 0; page < pagesPerBlock; ++page) {
            std::memcpy(dest + page * pageBytes, main + page * pageData, pageData);
            std::memcpy(dest + page * pageBytes + pageData, spare + page * pageSpare, pageSpare);
        }
    }

    raw.swap(image);
    return true;
}

bool spitzExtractKernel(const uint8_t* raw, size_t size, std::vector<uint8_t>& kernel) {
    if (!raw || size != spitzNandRawSize) return false;

    constexpr uint32_t bootBlocks = 0x700000 / blockData;
    constexpr uint32_t logicalBlocks = 424;
    constexpr uint32_t kernelBlock = 0xe0000 / blockData;
    constexpr uint32_t noBlock = blocks;
    constexpr uint32_t duplicateBlock = blocks + 1;
    std::array<uint32_t, logicalBlocks> physical;
    physical.fill(noBlock);

    for (uint32_t block = 0; block < bootBlocks; ++block) {
        const auto* spare = raw + block * blockBytes + pageData;
        if (read16(spare + 4) != 0xffff) continue;
        uint16_t tag = read16(spare + 8);
        const uint16_t second = read16(spare + 10);
        const uint16_t third = read16(spare + 12);
        if (tag != second && tag != third) {
            if (second != third) continue;
            tag = second;
        }
        const uint32_t logical = (tag >> 1) & 0x3ff;
        if (!logical || logical >= physical.size() || __builtin_parity(tag)) continue;
        physical[logical] = physical[logical] == noBlock ? block : duplicateBlock;
    }

    if (physical[kernelBlock] >= blocks) return false;
    const auto* header = raw + physical[kernelBlock] * blockBytes;
    const uint32_t start = read32(header + 0x28);
    const uint32_t end = read32(header + 0x2c);
    if (read32(header + 0x24) != 0x016f2818 || end <= start) return false;
    size_t remaining = end - start;
    if (remaining < 48 || remaining > 0x13c000) return false;

    std::vector<uint8_t> image;
    image.reserve(remaining);
    for (uint32_t logical = kernelBlock; remaining; ++logical) {
        if (logical >= physical.size() || physical[logical] >= blocks) return false;
        const auto* page = raw + physical[logical] * blockBytes;
        for (uint32_t p = 0; p < pagesPerBlock && remaining; ++p, page += pageBytes) {
            const size_t count = std::min<size_t>(pageData, remaining);
            image.insert(image.end(), page, page + count);
            remaining -= count;
        }
    }
    kernel.swap(image);
    return true;
}

struct SpitzNand {
    ArmMem* mem{};
    NAND* flash{};
    uint8_t control{chipDisable | unusedChipDisable};
    uint8_t command{};
    uint8_t eccCount{};
    uint8_t eccColumns{};
    uint16_t eccLines{};
    bool protectedOp{};

    template <typename T>
    void DoSaveLoad(T& helper) {
        helper.Do(typename T::Pack8() << control << command << eccCount << eccColumns)
            .Do16(eccLines)
            .DoBool(protectedOp);
    }

    void AddEccByte(uint8_t value) {
        eccColumns ^= value;
        if (__builtin_parity(value))
            for (unsigned bit = 0; bit < 8; ++bit)
                eccLines ^= 1u << (2 * bit + ((eccCount >> bit) & 1));
        ++eccCount;  // A 256-byte ECC step wraps the counter to zero.
    }

    uint8_t ColumnParity() const {
        constexpr uint8_t masks[] = {0x55, 0xaa, 0x33, 0xcc, 0x0f, 0xf0};
        uint8_t parity = 0;
        for (unsigned bit = 0; bit < 6; ++bit)
            parity |= __builtin_parity(eccColumns & masks[bit]) << bit;
        return parity;
    }

    void WriteFlash(uint8_t value) {
        if (control & chipDisable) return;
        const bool cle = control & commandLatch;
        const bool ale = control & addressLatch;
        if (cle && !ale) {
            if (!nandIsReady(flash) && value != statusCommand && value != resetCommand) return;
            switch (value) {
                case 0x10:
                case 0xd0:
                    if (!(control & writeEnable)) {
                        protectedOp = true;
                        nandWrite(flash, true, false, resetCommand);
                        return;
                    }
                    break;
                case 0x60:
                case 0x80:
                case resetCommand:
                    protectedOp = false;
                    break;
                case 0x00:
                case 0x01:
                case 0x50:
                case statusCommand:
                case 0x90:
                    break;
                default:
                    return;
            }
            command = value;
        } else if (!nandIsReady(flash)) {
            return;
        } else if (!cle && !ale) {
            AddEccByte(value);
        }
        nandWrite(flash, cle, ale, value);
    }

    uint8_t ReadFlash() {
        if (control & chipDisable) return 0xff;
        uint8_t value;
        if (!nandRead(flash, control & commandLatch, control & addressLatch, &value)) return 0xff;
        if (command == statusCommand) {
            if (control & writeEnable) value |= 0x80;
            if (protectedOp) value |= 0x01;
        } else {
            AddEccByte(value);
        }
        return value;
    }
};

static bool spitzNandAccess(void* context, uint32_t pa, uint_fast8_t size, bool write, void* buffer) {
    if (size != 1 && size != 2 && size != 4) return false;
    auto& nand = *static_cast<SpitzNand*>(context);
    auto* bytes = static_cast<uint8_t*>(buffer);
    const uint32_t reg = pa - base;
    if (write) {
        switch (reg) {
            case 0x10:
                nand.eccLines = nand.eccColumns = nand.eccCount = 0;
                break;
            case 0x14:
                nand.WriteFlash(bytes[0]);
                break;
            case 0x18:
                nand.control = bytes[0] & 0x1f;
                break;
        }
        return true;
    }

    uint8_t value = 0;
    switch (reg) {
        case 0x00: value = nand.eccLines; break;
        case 0x04: value = nand.eccLines >> 8; break;
        case 0x08: value = nand.ColumnParity(); break;
        case 0x0c: value = nand.eccCount; break;
        case 0x14: value = nand.ReadFlash(); break;
        case 0x18: value = nand.control | (nandIsReady(nand.flash) ? ready : 0); break;
    }
    std::memset(bytes, 0, size);
    bytes[0] = value;
    return true;
}

SpitzNand* spitzNandInit(ArmMem* mem, Reschedule reschedule, uint8_t* raw, size_t size,
                         const MemoryBuffer* pageBuffer) {
    if (size != spitzNandRawSize) return nullptr;

    auto* controller = new SpitzNand;
    if (!memRegionAdd(mem, base, regionSize, spitzNandAccess, controller)) {
        delete controller;
        return nullptr;
    }
    // The stock kernel expects device 0x73 for Spitz. Use the Samsung small-page ID.
    constexpr NandSpecs specs{pageBytes, blocks, 5, NAND_FLAG_SAMSUNG_ADDRESSED_VIA_AREAS,
                             2, {0xec, 0x73}};
    controller->mem = mem;
    controller->flash = nandInit(raw, pageBuffer, reschedule, size, &specs, nullptr, nullptr);
    return controller;
}

void spitzNandDestroy(SpitzNand* controller) {
    if (!controller) return;
    memRegionRemove(controller->mem, base, regionSize, controller);
    nandDestroy(controller->flash);
    delete controller;
}

NAND* spitzNandGetNand(SpitzNand* controller) { return controller->flash; }

template <typename T>
void spitzNandSave(SpitzNand* controller, T& savestate) {
    auto chunk = savestate.GetChunk(ChunkType::spitzNand, 0);
    if (!chunk) std::abort();
    SaveChunkHelper helper(*chunk);
    controller->DoSaveLoad(helper);
    nandSave(controller->flash, savestate);
}

template <typename T>
void spitzNandLoad(SpitzNand* controller, T& loader) {
    auto chunk = loader.GetChunkOrFail(ChunkType::spitzNand, 0, "Spitz NAND");
    if (!chunk) return;
    LoadChunkHelper helper(*chunk);
    controller->DoSaveLoad(helper);
    nandLoad(controller->flash, loader);
}

template void spitzNandSave(SpitzNand*, Savestate<ChunkType>&);
template void spitzNandSave(SpitzNand*, SavestateProbe<ChunkType>&);
template void spitzNandLoad(SpitzNand*, SavestateLoader<ChunkType>&);
