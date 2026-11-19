// The Zodiac's hard reset on the real 1.1 ROM (ZODIAC_ROM) from a bring-up state
// (ZODIAC_STATE). Skips without them. ZODIAC_TEST_FAST=1 runs the Fast engine.
// hard-reset-yes and hard-reset-no: a hard reset answered Function or Home.
// probe MS HOLD_MS DIR: runs MS of guest time, hard resets holding power HOLD_MS and writes a frame a
// second for 40 s into DIR (PPM), for looking at, pressing key id KEY (keys.h) at 5 s if given.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "soc_IMX1.h"
#include "zodiac_panel.h"

namespace {
std::vector<uint8_t> readFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

constexpr uint64_t hz = 100000000;

// Runs ms of guest time.
void runFor(SocIMX1& soc, uint64_t ms) {
    const uint64_t until = soc.GetTime() + ms * 1000000;
    while (soc.GetTime() < until && !soc.Halted()) soc.Run(hz / 100, hz);
}

bool writeFrame(SocIMX1& soc, const std::string& path) {
    uint32_t first, last;
    const uint32_t* frame = soc.GetPendingFrame(first, last);
    if (!frame) return false;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%u %u\n255\n", zodiacPanelWidth, zodiacPanelHeight);
    for (uint32_t i = 0; i < zodiacPanelWidth * zodiacPanelHeight; ++i) {
        const uint8_t rgb[3] = {uint8_t(frame[i]), uint8_t(frame[i] >> 8), uint8_t(frame[i] >> 16)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
    return true;
}

SocIMX1* start(std::vector<uint8_t>& rom) {
    std::string why;
    SocIMX1* soc = SocIMX1::Create(rom.data(), rom.size(), why);
    if (!soc) { std::fprintf(stderr, "create: %s\n", why.c_str()); std::exit(1); }
    soc->SetHostTimeLimit(false);
    const char* fast = std::getenv("ZODIAC_TEST_FAST");
    if (fast && std::string(fast) == "1") assert(soc->SetFastEngine(true));
    const auto state = readFile(std::getenv("ZODIAC_STATE"));
    if (!soc->LoadBringUpState(state.data(), state.size())) { std::fprintf(stderr, "state refused\n"); std::exit(1); }
    return soc;
}

// Presses a key for 300 ms of guest time.
void press(SocIMX1& soc, KeyId key) {
    soc.KeyDown(key);
    runFor(soc, 300);
    soc.KeyUp(key);
    runFor(soc, 300);
}

void probe(SocIMX1& soc, uint64_t ms, uint32_t holdMs, const std::string& dir, int key) {
    runFor(soc, ms);
    writeFrame(soc, dir + "/before.ppm");
    soc.HardResetHolding(holdMs);
    for (int s = 1; s <= 40; ++s) {
        if (s == 6 && key >= 0) press(soc, KeyId(key));
        runFor(soc, 1000);
        char name[64];
        std::snprintf(name, sizeof(name), "/t%02d.ppm", s);
        writeFrame(soc, dir + name);
        std::printf("t=%d halted=%d pending=%d\n", s, soc.Halted(), soc.HardResetHoldPending());
    }
}
// Pixel counts on the frame: grey (each channel 40 to 110) in the bottom rows, where the erase prompt draws the
// Zodiac's buttons, and blue (blue over red by 60), which fills the first-run Zodiac Setup screen.
struct Look { unsigned promptGrey, blue; };
Look look(SocIMX1& soc) {
    uint32_t first, last;
    const uint32_t* frame = soc.GetPendingFrame(first, last);
    assert(frame);
    Look l{};
    for (uint32_t y = 0; y < zodiacPanelHeight; ++y)
        for (uint32_t x = 0; x < zodiacPanelWidth; ++x) {
            const uint32_t p = frame[y * zodiacPanelWidth + x];
            const int r = p & 0xff, g = (p >> 8) & 0xff, b = (p >> 16) & 0xff;
            l.promptGrey += y >= 330 && r >= 40 && r <= 110 && g >= 40 && g <= 110 && b >= 40 && b <= 110;
            l.blue += b > r + 60;
        }
    return l;
}

// SoC::HardReset holds power from the reset, the ROM asks "Erase all data?" once it's let go, and Function
// (yes) erases and starts the first-run setup while Home (no) keeps the data.
void hardReset(SocIMX1& soc, bool erase) {
    runFor(soc, 200);
    soc.HardReset();
    assert(soc.HardResetHoldPending());
    runFor(soc, zodiacHardResetHoldMs + 2000);
    assert(!soc.HardResetHoldPending() && !soc.Halted());
    const Look prompt = look(soc);
    std::printf("prompt grey %u blue %u\n", prompt.promptGrey, prompt.blue);
    assert(prompt.promptGrey > 10000 && !prompt.blue);
    press(soc, erase ? keyIdFunction : keyIdHome);
    runFor(soc, 20000);
    const Look after = look(soc);
    std::printf("after %s grey %u blue %u\n", erase ? "yes" : "no", after.promptGrey, after.blue);
    assert(!soc.Halted() && !after.promptGrey && (erase ? after.blue > 30000 : after.blue < 10000));
}

}  // namespace

int main(int argc, char** argv) {
    const char* romPath = std::getenv("ZODIAC_ROM");
    const char* statePath = std::getenv("ZODIAC_STATE");
    if (!romPath || !statePath || !*romPath || !*statePath) {
        std::printf("zodiac guest calls: skipped, needs ZODIAC_ROM and ZODIAC_STATE\n");
        return 0;
    }
    auto rom = readFile(romPath);
    SocIMX1* soc = start(rom);
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "probe" && argc >= 5) {
        probe(*soc, std::strtoull(argv[2], nullptr, 0), uint32_t(std::strtoul(argv[3], nullptr, 0)), argv[4],
              argc > 5 ? std::atoi(argv[5]) : -1);
        return 0;
    }
    if (mode == "hard-reset-yes" || mode == "hard-reset-no") {
        hardReset(*soc, mode == "hard-reset-yes");
        std::printf("zodiac %s %s: PASS\n", mode.c_str(), soc->FastEngine() ? "Fast" : "Exact");
        return 0;
    }
    std::fprintf(stderr, "unknown mode\n");
    return 1;
}
