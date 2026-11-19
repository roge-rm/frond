// The Zodiac's Bluetooth on the real 1.1 ROM (ZODIAC_ROM) from a bring-up state (ZODIAC_STATE). Skips without them.
// ZODIAC_TEST_FAST=1 runs the Fast engine.
// trace DIR PC...: hard reset kept, Preferences, then the Bluetooth panel's radio on, logging every code hook hit
// (each one let run) and a frame per step into DIR.
#include <cassert>
#include <chrono>
#include <thread>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "GuestDatabaseOps.h"
#include "soc_IMX1.h"
#include "zodiac_panel.h"

namespace {
std::vector<uint8_t> readFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

constexpr uint64_t hz = 100000000;

// With a controller the guest keeps to host time, as btvirt's timers run on it.
bool paced = false;

void runFor(SocIMX1& soc, uint64_t ms) {
    const uint64_t until = soc.GetTime() + ms * 1000000;
    const auto hostStart = std::chrono::steady_clock::now();
    const uint64_t guestStart = soc.GetTime();
    while (soc.GetTime() < until && !soc.Halted()) {
        soc.Run(hz / 100, hz);
        if (paced) std::this_thread::sleep_until(hostStart + std::chrono::nanoseconds(soc.GetTime() - guestStart));
    }
}

// The frame as the view shows it (480x320).
bool writeFrame(SocIMX1& soc, const std::string& path) {
    uint32_t first, last;
    const uint32_t* frame = soc.GetPendingFrame(first, last);
    if (!frame) return false;
    std::vector<uint32_t> view(zodiacViewWidth * zodiacViewHeight);
    zodiacRotateFrame(frame, view.data());
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%u %u\n255\n", zodiacViewWidth, zodiacViewHeight);
    for (uint32_t p : view) {
        const uint8_t rgb[3] = {uint8_t(p), uint8_t(p >> 8), uint8_t(p >> 16)};
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

void press(SocIMX1& soc, KeyId key) {
    soc.KeyDown(key);
    runFor(soc, 300);
    soc.KeyUp(key);
    runFor(soc, 300);
}

void tap(SocIMX1& soc, int x, int y, uint64_t after = 1500) {
    soc.PenDown(x, y);
    runFor(soc, 100);
    soc.PenUp();
    runFor(soc, after);
}

// A hard reset answered Home keeps the data and opens Preferences (a 68k app).
void toPreferences(SocIMX1& soc) {
    runFor(soc, 200);
    soc.HardReset();
    runFor(soc, zodiacHardResetHoldMs + 2000);
    press(soc, keyIdHome);
    runFor(soc, 20000);
    assert(!soc.Halted());
}
}  // namespace

int main(int argc, char** argv) {
    const char* romPath = std::getenv("ZODIAC_ROM");
    const char* statePath = std::getenv("ZODIAC_STATE");
    if (!romPath || !*romPath || !statePath || !*statePath) {
        std::printf("SKIP: set ZODIAC_ROM and ZODIAC_STATE\n");
        return 0;
    }
    auto rom = readFile(romPath);
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "trace" && argc > 2) {
        SocIMX1* soc = start(rom);
        const std::string dir = argv[2];
        std::vector<uint32_t> pcs;
        for (int i = 3; i < argc; ++i) pcs.push_back(uint32_t(std::strtoul(argv[i], nullptr, 16)));
        if (const char* bt = std::getenv("BT")) {
            std::string error;
            if (!soc->SetBluetooth(bt, error)) { std::fprintf(stderr, "bluetooth: %s\n", error.c_str()); return 1; }
        }
        if (std::getenv("EARLY")) soc->SetTraceHooks(pcs);
        toPreferences(*soc);
        writeFrame(*soc, dir + "/0-prefs.ppm");
        soc->SetTraceHooks(pcs);
        int step = 1;
        auto shot = [&](const char* name) {
            char path[256];
            std::snprintf(path, sizeof path, "%s/%d-%s.ppm", dir.c_str(), step++, name);
            writeFrame(*soc, path);
        };
        const char* taps = std::getenv("TAPS");
        std::string list = taps ? taps : "";
        size_t at = 0;
        while (at < list.size()) {
            int x = 0, y = 0, wait = 1500;
            if (std::sscanf(list.c_str() + at, "%d,%d,%d", &x, &y, &wait) < 2) break;
            tap(*soc, x, y, uint64_t(wait));
            shot("tap");
            at = list.find(' ', at);
            if (at == std::string::npos) break;
            ++at;
        }
        std::printf("bluetooth: %s\n", soc->BluetoothState().c_str());
        std::printf("halted=%d\n", soc->Halted());
        return 0;
    }
    // steps DIR: hard reset to Preferences with Bluetooth on BT, then STEPS, ';' between them: tap X Y WAIT,
    // install PATH, launch NAME, wait MS, shot NAME.
    if (mode == "steps" && argc > 2) {
        SocIMX1* soc = start(rom);
        const std::string dir = argv[2];
        if (const char* bt = std::getenv("BT")) {
            std::string error;
            if (!soc->SetBluetooth(bt, error)) { std::fprintf(stderr, "bluetooth: %s\n", error.c_str()); return 1; }
        }
        toPreferences(*soc);
        paced = std::getenv("BT") != nullptr;
        std::string list = std::getenv("STEPS") ? std::getenv("STEPS") : "";
        size_t at = 0;
        while (at < list.size()) {
            size_t end = list.find(';', at);
            if (end == std::string::npos) end = list.size();
            std::string step = list.substr(at, end - at);
            at = end + 1;
            while (!step.empty() && step[0] == ' ') step.erase(0, 1);
            char what[32] = {}, arg[256] = {};
            int x = 0, y = 0, wait = 1500;
            if (std::sscanf(step.c_str(), "tap %d %d %d", &x, &y, &wait) >= 2) {
                tap(*soc, x, y, uint64_t(wait));
            } else if (std::sscanf(step.c_str(), "%31s %255[^;]", what, arg) == 2) {
                const std::string w = what;
                if (w == "install") {
                    auto prc = readFile(arg);
                    std::printf("install %s: %d\n", arg, GuestDatabaseOps::Install(*soc, prc.size(), prc.data()));
                    runFor(*soc, 1000);
                } else if (w == "launch") {
                    std::printf("launch %s: %d\n", arg, int(GuestDatabaseOps::Launch(*soc, arg)));
                    runFor(*soc, 2000);
                } else if (w == "wait") {
                    runFor(*soc, std::strtoull(arg, nullptr, 10));
                } else if (w == "shot") {
                    writeFrame(*soc, dir + "/" + arg + ".ppm");
                }
            }
            std::printf("%s: bluetooth %s, t %.3f\n", step.c_str(), soc->BluetoothState().c_str(),
                        double(soc->GetTime()) / 1e9);
            std::fflush(stdout);
        }
        std::printf("halted=%d\n", soc->Halted());
        return soc->Halted() ? 1 : 0;
    }
    std::printf("usage: trace DIR PC... | steps DIR\n");
    return 1;
}
