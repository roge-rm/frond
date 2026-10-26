#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include "pace_patch.h"

extern "C" void uarmAbort() { std::abort(); }

// The interpreter entry signature supported by the upstream bridge.
static const uint8_t signature[] = {
        0xF1, 0x4F, 0x2D, 0xE9, 0x00, 0x00, 0x9D, 0xE5, 0x44, 0xA0, 0x90, 0xE5, 0x48, 0x40, 0x90,
        0xE5, 0x04, 0x50, 0x80, 0xE2, 0x24, 0x60, 0x80, 0xE2, 0xB2, 0x70, 0xDA, 0xE0, 0x44, 0xB0,
        0x8F, 0xE2, 0x01, 0x00, 0x00, 0xEA, 0xC0, 0x0C, 0x14, 0xE3, 0x47, 0x17, 0x00, 0x1A, 0x07,
        0x84, 0xA0, 0xE1, 0x27, 0x84, 0x88, 0xE1, 0xF0, 0x0C, 0x08, 0xE2, 0x20, 0x05, 0x9B, 0xE7,
        0xB2, 0x70, 0xDA, 0xE0, 0x00, 0xF0, 0x8B, 0xE0, 0x00, 0x00, 0x9D, 0xE5, 0x02, 0xA0, 0x4A,
        0xE2, 0x44, 0xA0, 0x80, 0xE5, 0x48, 0x40, 0x80, 0xE5, 0x00, 0x80, 0x80, 0xE5, 0x18, 0x90,
        0x9D, 0xE5, 0x00, 0x00, 0xA0, 0xE3, 0xF2, 0x4F, 0xBD, 0xE8, 0x1E, 0xFF, 0x2F, 0xE1};

static std::vector<uint32_t> image(uint32_t location) {
    std::vector<uint32_t> rom((location + 0x7000) / 4, 0);
    std::memcpy(reinterpret_cast<uint8_t*>(rom.data()) + location, signature, sizeof(signature));
    const uint32_t sites[] = {0x25894, 0x22ab0, 0x22a5c, 0x25b10, 0x25a18,
                              0x25a54, 0x25808, 0x25844, 0x25b30};
    // ARM BL to the following instruction (PC + 8).
    for (auto site : sites) rom[(location + site - 0x1fd28) / 4] = 0xeb000000;
    return rom;
}

int main() {
    PacePatch patch{};
    pacePatchInit(&patch, 0x20000000, nullptr, 0);
    uint32_t tiny = 0;
    pacePatchInit(&patch, 0x20000000, &tiny, sizeof(tiny));
    assert(!patch.enterPace);

    auto rom = image(0);
    pacePatchInit(&patch, 0x20000000, rom.data(), rom.size() * 4);
    assert(patch.enterPace == 0x20000000);
    assert(rom[0] == INSTR_PACE_ENTER);
    assert(rom[1] == INSTR_PACE_RESUME);
    assert(rom[2] == INSTR_PACE_RETURN_FROM_CALLOUT);
    assert(patch.calloutSyscall == 0x20000000 + 0x25894 - 0x1fd28 + 8);

    // A RAM session already contains the three bridge instructions.
    const auto patchedImage = rom;
    PacePatch restored{};
    pacePatchInit(&restored, 0x20000000, rom.data(), rom.size() * 4);
    assert(std::memcmp(&patch, &restored, sizeof(patch)) == 0 && rom == patchedImage);

    // The patched prefix does not excuse a damaged interpreter signature.
    rom[3] ^= 1;
    const auto damaged = rom;
    restored = {};
    pacePatchInit(&restored, 0x20000000, rom.data(), rom.size() * 4);
    assert(!restored.enterPace && rom == damaged);

    patch = {};
    rom = image(0x100);
    const auto original = rom;
    // Entry signature fits, but the relative callout table does not.
    pacePatchInit(&patch, 0x20000000, rom.data(), 0x100 + sizeof(signature));
    assert(!patch.enterPace && rom == original);

    // A decodable branch outside the available image must also be rejected.
    rom[(0x100 + 0x25894 - 0x1fd28) / 4] = 0xeb100000;
    const auto outside = rom;
    pacePatchInit(&patch, 0x20000000, rom.data(), rom.size() * 4);
    assert(!patch.enterPace && rom == outside);

    // More than one interpreter signature is ambiguous: leave memory untouched.
    rom = image(0x100);
    std::memcpy(reinterpret_cast<uint8_t*>(rom.data()) + 0x200, signature, sizeof(signature));
    const auto duplicate = rom;
    pacePatchInit(&patch, 0x20000000, rom.data(), rom.size() * 4);
    assert(!patch.enterPace && rom == duplicate);
}
