#include "pace_patch.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cputil.h"

#ifndef __EMSCRIPTEN__
static void* diagnosticOriginalShadow = nullptr;
#endif

static const uint8_t paceStartPattern[] = {
        0xF1, 0x4F, 0x2D, 0xE9, 0x00, 0x00, 0x9D, 0xE5, 0x44, 0xA0, 0x90, 0xE5, 0x48, 0x40, 0x90,
        0xE5, 0x04, 0x50, 0x80, 0xE2, 0x24, 0x60, 0x80, 0xE2, 0xB2, 0x70, 0xDA, 0xE0, 0x44, 0xB0,
        0x8F, 0xE2, 0x01, 0x00, 0x00, 0xEA, 0xC0, 0x0C, 0x14, 0xE3, 0x47, 0x17, 0x00, 0x1A, 0x07,
        0x84, 0xA0, 0xE1, 0x27, 0x84, 0x88, 0xE1, 0xF0, 0x0C, 0x08, 0xE2, 0x20, 0x05, 0x9B, 0xE7,
        0xB2, 0x70, 0xDA, 0xE0, 0x00, 0xF0, 0x8B, 0xE0, 0x00, 0x00, 0x9D, 0xE5, 0x02, 0xA0, 0x4A,
        0xE2, 0x44, 0xA0, 0x80, 0xE5, 0x48, 0x40, 0x80, 0xE5, 0x00, 0x80, 0x80, 0xE5, 0x18, 0x90,
        0x9D, 0xE5, 0x00, 0x00, 0xA0, 0xE3, 0xF2, 0x4F, 0xBD, 0xE8, 0x1E, 0xFF, 0x2F, 0xE1};

#define findCallout(var, name, location)                                                      \
    const size_t site_##var = size_t(paceLocation) + location - 0x1fd28;                       \
    if (site_##var > romSize || romSize - site_##var < 4) return;                             \
    uint32_t var = decodeBBlBlx(rom, site_##var);                                             \
    if (!var || (var & ~1u) >= romSize) {                                                    \
        fprintf(stderr, "unable to locate callout for " name ", PACE will not be patched\n"); \
        return;                                                                            \
    }                                                                                      \
    var += romBase;                                                                        \
    printf("found PACE callout for " name " at %#010x\n", var);

static uint32_t decodeBBlBlx(void* rom, uint32_t addr) {
    if (addr & 0x03) ERR("alignment fault in decode decodeBBlBlx: 0x%08x", addr);

    const uint32_t instruction = reinterpret_cast<uint32_t*>(rom)[addr >> 2];
    const uint8_t prefix = instruction >> 24;

    int32_t offset = instruction << 8;
    offset >>= 6;
    offset += 8;

    switch (prefix) {
        case 0xea:
        case 0xeb:
            return addr + offset;

        case 0xfa:
            return addr + offset + 1;

        case 0xfb:
            return addr + offset + 3;

        default:
            return 0;
    }
}

namespace {
// The ROM PACE dispatches each 68k instruction with "add pc, fp, r0" (fp = enterPace + 0x68,
// the opcode table). The Zire 72, E2 and TG50 builds have 52 of them within 0x7c00 bytes of
// the entry, the first one in the loop at entry + 0x40.
constexpr uint32_t paceDispatchInstr = 0xe08bf000;
constexpr size_t paceDispatchSpan = 0x8000, paceDispatchSites = 52, paceLoopDispatch = 0x40;

uint32_t paceWord(const void* code, size_t offset) {
    uint32_t word;
    memcpy(&word, static_cast<const uint8_t*>(code) + offset, sizeof(word));
    return word;
}

// Counts the dispatch sites in the span after entry, hooked or not.
size_t paceCountDispatch(const void* code, size_t entry, size_t size) {
    size_t count = 0;
    const size_t end = size - entry < paceDispatchSpan ? size : entry + paceDispatchSpan;
    for (size_t offset = entry; offset + 4 <= end; offset += 4) {
        const uint32_t word = paceWord(code, offset);
        if (word == paceDispatchInstr || word == INSTR_PACE_DISPATCH) ++count;
    }
    return count;
}

void paceHookDispatch(void* code, size_t entry, size_t size, uint32_t from, uint32_t to) {
    const size_t end = size - entry < paceDispatchSpan ? size : entry + paceDispatchSpan;
    for (size_t offset = entry; offset + 4 <= end; offset += 4)
        if (paceWord(code, offset) == from)
            memcpy(static_cast<uint8_t*>(code) + offset, &to, sizeof(to));
}

// The start pattern, allowing the entry hooks and the hooked loop dispatch.
bool paceStartMatches(const uint8_t* code, size_t prefix) {
    return !memcmp(code + prefix, paceStartPattern + prefix, paceLoopDispatch - prefix) &&
           (paceWord(code, paceLoopDispatch) == paceDispatchInstr ||
            paceWord(code, paceLoopDispatch) == INSTR_PACE_DISPATCH) &&
           !memcmp(code + paceLoopDispatch + 4, paceStartPattern + paceLoopDispatch + 4,
                   sizeof(paceStartPattern) - paceLoopDispatch - 4);
}
}

struct PacePatch* createPacePatch() {
    struct PacePatch* patch = reinterpret_cast<PacePatch*>(malloc(sizeof(*patch)));
    memset(patch, 0, sizeof(*patch));

    return patch;
}

void pacePatchInit(struct PacePatch* patch, uint32_t romBase, void* rom, size_t romSize) {


    if (!rom || romSize < sizeof(paceStartPattern) || romSize > UINT32_MAX) return;

    uint32_t paceLocation = UINT32_MAX;
    for (uint32_t i = 0; i <= romSize - sizeof(paceStartPattern); i += 4) {
        const auto *entry = reinterpret_cast<const uint32_t *>(
            reinterpret_cast<const uint8_t *>(rom) + i);
        const bool patched = entry[0] == INSTR_PACE_ENTER && entry[1] == INSTR_PACE_RESUME &&
                             entry[2] == INSTR_PACE_RETURN_FROM_CALLOUT;
        const size_t prefix = patched ? 12 : 0;
        if (!paceStartMatches(reinterpret_cast<const uint8_t *>(rom) + i, prefix)) continue;

        if (paceLocation != UINT32_MAX) {
            fprintf(stderr,
                    "PACE signature found in both %#010x and %#010x, PACE will not be patched\n",
                    paceLocation, i);
            return;
        }

        paceLocation = i;
    }

    if (paceLocation == UINT32_MAX) {
        fprintf(stderr, "unable to locate PACE, will not be patched\n");
        return;
    }

    const uint32_t enterPace = romBase + paceLocation;
    printf("found PACE entry at %#010x\n", enterPace);

    findCallout(calloutSyscall, "syscall", 0x25894);
    findCallout(calloutDivu, "divu", 0x22ab0);
    findCallout(calloutDivs, "divs", 0x22a5c);
    findCallout(calloutIllegalInstr, "illegal instruction", 0x25b10);
    findCallout(calloutLine1010, "line 1010", 0x25a18);
    findCallout(calloutLine1111, "line 1111", 0x25a54);
    findCallout(calloutTrap0, "trap 0", 0x25808);
    findCallout(calloutTrap8, "trap 8", 0x25844);
    findCallout(calloutUnimplementedInstr, "unimplemented instruction", 0x25b30);

    ((uint32_t*)rom)[(paceLocation >> 2)] = INSTR_PACE_ENTER;
    ((uint32_t*)rom)[(paceLocation >> 2) + 1] = INSTR_PACE_RESUME;
    ((uint32_t*)rom)[(paceLocation >> 2) + 2] = INSTR_PACE_RETURN_FROM_CALLOUT;

    patch->enterPace = enterPace;
    patch->resumePace = patch->enterPace + 4;
    patch->returnFromCallout = patch->enterPace + 8;
    patch->calloutSyscall = calloutSyscall;
    patch->calloutIllegalInstr = calloutIllegalInstr;
    patch->calloutDivu = calloutDivu;
    patch->calloutDivs = calloutDivs;
    patch->calloutLine1010 = calloutLine1010;
    patch->calloutLine1111 = calloutLine1111;
    patch->calloutTrap0 = calloutTrap0;
    patch->calloutTrap8 = calloutTrap8;
    patch->calloutUnimplementedInstr = calloutUnimplementedInstr;

    if (paceCountDispatch(rom, paceLocation, romSize) == paceDispatchSites) {
        paceHookDispatch(rom, paceLocation, romSize, paceDispatchInstr, INSTR_PACE_DISPATCH);
        patch->romDispatchHooked = true;
    } else {
        fprintf(stderr, "PACE dispatch sites not as expected, 68k faults won't use the ROM PACE\n");
    }

    printf("patching PACE\n");

    return;
}

namespace {
constexpr size_t tg50Database = 0x0d2f68;
constexpr size_t tg50CodeStart = 0x0d2fe8, tg50CodeEnd = 0x10b0d8;
constexpr uint32_t tg50VirtualBase = 0x20000000;

uint32_t readLe32(const uint8_t* data, size_t offset) {
    return uint32_t(data[offset]) | uint32_t(data[offset + 1]) << 8 |
           uint32_t(data[offset + 2]) << 16 | uint32_t(data[offset + 3]) << 24;
}

struct Tg50Callout {
    size_t site, target;
    uint32_t branch, prologue[4];
    uint32_t PacePatch::*field;
};
// Relative to the matched entry. Pin both destinations and the original call ABI.
constexpr Tg50Callout tg50Callouts[] = {
    {0x5b6c, 0x5e0c, 0xeb0000a6, {0xe92d4030,0xe1a04000,0xe3c10cf0,0xe3805ca0}, &PacePatch::calloutSyscall},
    {0x2d88, 0x6158, 0xeb000cf2, {0xe92d4070,0xe1b05002,0xe1a04000,0x01a00004}, &PacePatch::calloutDivu},
    {0x2d34, 0x60dc, 0xeb000ce8, {0xe92d4070,0xe1b05002,0xe1a04000,0x01a00004}, &PacePatch::calloutDivs},
    {0x5de8, 0x61d0, 0xea0000f8, {0xe92d4010,0xe1a04000,0xe5900044,0xe24dd068}, &PacePatch::calloutIllegalInstr},
    {0x5cf0, 0x62c4, 0xeb000173, {0xe92d4010,0xe1a04000,0xe5900044,0xe24dd068}, &PacePatch::calloutLine1010},
    {0x5d2c, 0x633c, 0xeb000182, {0xe92d4010,0xe1a04000,0xe5900044,0xe24dd068}, &PacePatch::calloutLine1111},
    {0x5ae0, 0x63b8, 0xeb000234, {0xe3a01080,0xeaff5a7f,0xe3a010bc,0xeaff5a7d}, &PacePatch::calloutTrap0},
    {0x5b1c, 0x63c0, 0xeb000227, {0xe3a010bc,0xeaff5a7d,0xe92d4070,0xe24dd080}, &PacePatch::calloutTrap8},
    {0x5e08, 0x6248, 0xea00010e, {0xe92d4010,0xe1a04000,0xe5900044,0xe24dd068}, &PacePatch::calloutUnimplementedInstr}
};
constexpr uint32_t tg50SyscallFrame[] = {
    0xe59d0000,0xe24aa002,0xe580a044,0xe5804048,0xe5808000,0xe59d9018,
    0xe59d0000,0xe1a0100c,0xeb0000a6,0xe59d0000,0xe590a044,0xe5904048,
    0xe2805004,0xe2806024,0xe0da70b2,0xeaffe925
};
bool tg50Words(const uint8_t* data, size_t offset, const uint32_t* words, size_t count) {
    if (offset < tg50CodeStart || offset > tg50CodeEnd || count > (tg50CodeEnd - offset) / 4)
        return false;
    for (size_t i = 0; i < count; ++i)
        if (readLe32(data, offset + 4 * i) != words[i]) return false;
    return true;
}

// The automatic Sleep/Wake walk (200ec8dc) calls each 68k library's Sleep or Wake through
// 200ec798 with no stack arguments, so the library reads its refnum from whatever word sits
// at the caller's A7. The hook replaces "mov r3, #0" with INSTR_TG50_LIBRARY_REFNUM, which
// writes the table index (r1) below the 68k A7 and passes argBytes 2 in r3.
constexpr size_t tg50RefnumSite = 0x0ec948;
constexpr uint32_t tg50RefnumCode[] = {
    0xe1a01804,  // 200ec940 mov r1, r4, lsl #16
    0xe1a01821,  //          mov r1, r1, lsr #16
    0xe3a03000,  // 200ec948 mov r3, #0 (hooked)
    0xe1a02008,  //          mov r2, r8
    0xe1a00007,  //          mov r0, r7 (state)
    0xebffff8f   // 200ec954 bl 200ec798
};
constexpr size_t tg50RefnumStart = tg50RefnumSite - 8;
constexpr size_t tg50RefnumEnd = tg50RefnumStart + sizeof(tg50RefnumCode);

bool tg50RefnumHookable(const uint8_t* data) {
    return tg50Words(data, tg50RefnumStart, tg50RefnumCode,
                     sizeof(tg50RefnumCode) / sizeof(tg50RefnumCode[0]));
}
}

bool pacePatchDiscoverTg50(PacePatch* patch, uint32_t romBase, const void* original, size_t size) {
    if (!patch) return false;
    *patch = {};
    if (!original || size != 0xc70000 || uint64_t(romBase) + size > uint64_t(UINT32_MAX) + 1)
        return false;
    const auto* data = static_cast<const uint8_t*>(original);
    // Native little-endian database and resource directory, before reading executable bytes.
    const uint8_t name[32] = {'P','A','C','E'};
    if (memcmp(data + tg50Database, name, sizeof(name)) ||
        readLe32(data, tg50Database + 0x20) != 0x00010003 ||
        readLe32(data, tg50Database + 0x3c) != 0x6c696272 ||
        readLe32(data, tg50Database + 0x40) != 0x6136386b ||
        readLe32(data, tg50Database + 0x48) != 0 ||
        (readLe32(data, tg50Database + 0x4c) & 0xffff) != 3) return false;
    const uint32_t types[] = {0x616d6463,0x616d6464,0x616d6469};
    const uint32_t pointers[] = {tg50VirtualBase + tg50CodeStart, tg50VirtualBase + tg50CodeEnd,
                                 tg50VirtualBase + 0x10b12c};
    for (size_t i = 0; i < 3; ++i) {
        const size_t entry = tg50Database + 0x50 + i * 12;
        if (readLe32(data, entry) != types[i] || readLe32(data, entry + 4) != 0 ||
            readLe32(data, entry + 8) != pointers[i]) return false;
    }
    size_t entry = SIZE_MAX;
    for (size_t offset = tg50CodeStart; offset <= tg50CodeEnd - sizeof(paceStartPattern); offset += 4) {
        if (memcmp(data + offset, paceStartPattern, sizeof(paceStartPattern))) continue;
        if (entry != SIZE_MAX) return false;
        entry = offset;
    }
    if (entry == SIZE_MAX || entry > tg50CodeEnd - 0x63d0) return false;
    PacePatch candidate{};
    candidate.enterPace = romBase + entry;
    candidate.resumePace = candidate.enterPace + 4;
    candidate.returnFromCallout = candidate.enterPace + 8;
    for (const auto& callout : tg50Callouts) {
        if (!tg50Words(data, entry + callout.site, &callout.branch, 1) ||
            !tg50Words(data, entry + callout.target, callout.prologue, 4)) return false;
        const uint32_t instruction = readLe32(data, entry + callout.site);
        int32_t displacement = int32_t((instruction & 0xffffff) << 8) >> 6;
        const int64_t target = int64_t(entry + callout.site) + 8 + displacement;
        if (target != int64_t(entry + callout.target)) return false;
        candidate.*(callout.field) = romBase + uint32_t(target);
    }
    if (!tg50Words(data, entry + 0x5b4c, tg50SyscallFrame,
                   sizeof(tg50SyscallFrame) / sizeof(tg50SyscallFrame[0]))) return false;
    *patch = candidate;
    return true;
}

namespace {
// The shadow from offset to the end of the PACE code equals NOR, apart from dispatch sites
// that may already be hooked.
bool tg50PaceShadowMatches(const uint8_t* code, const uint8_t* data, size_t offset) {
    for (; offset < tg50CodeEnd; offset += 4) {
        const uint32_t word = readLe32(code, offset), original = readLe32(data, offset);
        if (word != original && !(original == paceDispatchInstr && word == INSTR_PACE_DISPATCH))
            return false;
    }
    return true;
}
}

bool pacePatchInitTg50(PacePatch* patch, uint32_t romBase, const void* original,
                      void* shadow, size_t size) {
    if (!patch) return false;
#ifndef __EMSCRIPTEN__
    if (shadow == diagnosticOriginalShadow && diagnosticOriginalShadow) return false;
#endif
    *patch = {};
    PacePatch candidate{};
    if (!shadow || shadow == original ||
        !pacePatchDiscoverTg50(&candidate, romBase, original, size)) return false;
    const size_t entry = candidate.enterPace - romBase;
    const auto* data = static_cast<const uint8_t*>(original);
    auto* code = static_cast<uint8_t*>(shadow);
    const uint32_t replacement[] = {INSTR_PACE_ENTER, INSTR_PACE_RESUME, INSTR_PACE_RETURN_FROM_CALLOUT};
    const bool patched = readLe32(code, entry) == replacement[0] &&
                         readLe32(code, entry + 4) == replacement[1] &&
                         readLe32(code, entry + 8) == replacement[2];
    const size_t prefix = patched ? 12 : 0;
    // The refnum hook word may already be in the shadow; everything else must be original.
    const bool hookable = tg50RefnumHookable(data) && tg50RefnumEnd <= entry;
    const bool hooked = hookable && readLe32(code, tg50RefnumSite) == INSTR_TG50_LIBRARY_REFNUM;
    const size_t skip = hooked ? 4 : 0;
    if (memcmp(code + tg50Database, data + tg50Database, tg50CodeStart - tg50Database) ||
        (hookable
             ? memcmp(code + tg50CodeStart, data + tg50CodeStart, tg50RefnumSite - tg50CodeStart) ||
                   memcmp(code + tg50RefnumSite + skip, data + tg50RefnumSite + skip,
                          entry - tg50RefnumSite - skip)
             : memcmp(code + tg50CodeStart, data + tg50CodeStart, entry - tg50CodeStart)) ||
        !tg50PaceShadowMatches(code, data, entry + prefix))
        return false;
    memcpy(code + entry, replacement, sizeof(replacement));
    if (paceCountDispatch(data, entry, tg50CodeEnd) == paceDispatchSites) {
        paceHookDispatch(code, entry, tg50CodeEnd, paceDispatchInstr, INSTR_PACE_DISPATCH);
        candidate.romDispatchHooked = true;
    }
    if (hookable) {
        const uint32_t hook = INSTR_TG50_LIBRARY_REFNUM;
        memcpy(code + tg50RefnumSite, &hook, sizeof(hook));
    }
    *patch = candidate;
    return true;
}

#ifndef __EMSCRIPTEN__
bool pacePatchOriginalTg50EntryForDiagnostic(PacePatch* patch, uint32_t romBase,
                                            const void* original, void* shadow, size_t size) {
    if (!std::getenv("FROND_TG50_ORIGINAL_PACE_DIAGNOSTIC") || !patch ||
        !shadow || shadow == original || diagnosticOriginalShadow) return false;
    PacePatch candidate{};
    if (!pacePatchDiscoverTg50(&candidate, romBase, original, size) ||
        patch->enterPace != candidate.enterPace) return false;
    const size_t offset = candidate.enterPace - romBase;
    const uint32_t replacement[] = {INSTR_PACE_ENTER, INSTR_PACE_RESUME,
                                    INSTR_PACE_RETURN_FROM_CALLOUT};
    auto* code = static_cast<uint8_t*>(shadow);
    if (memcmp(code + offset, replacement, sizeof(replacement))) return false;
    memcpy(code + offset, static_cast<const uint8_t*>(original) + offset, sizeof(replacement));
    paceHookDispatch(code, offset, tg50CodeEnd, INSTR_PACE_DISPATCH, paceDispatchInstr);
    patch->romDispatchHooked = false;
    diagnosticOriginalShadow = shadow;
    return true;
}
#endif
