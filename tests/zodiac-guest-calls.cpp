// The Zodiac's hard reset and guest calls on the real 1.1 ROM (ZODIAC_ROM) from a bring-up state
// (ZODIAC_STATE). Skips without them. ZODIAC_TEST_FAST=1 runs the Fast engine. No argument: guest calls in
// Preferences and the launcher;
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

#include "GuestDatabaseOps.h"
#include "soc_IMX1.h"
#include "zip.h"
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

constexpr uint16_t trapEvtGetEvent = 0xa11d, trapTimGetTicks = 0xa0f7, trapMemNumCards = 0xa03a;

// The guest at the syscall site: its registers and the 68k state r0 points at.
struct Site {
    uint32_t registers[16], cpsr, state;
    uint8_t m68k[0x4c];
};

Site atSite(SocIMX1& soc) {
    Site site{};
    assert(soc.GuestRegisters(site.registers, &site.cpsr));
    assert(site.registers[15] == soc.PaceSites().syscallSite);
    site.state = site.registers[0];
    assert(soc.ReadGuest(site.state, site.m68k, sizeof(site.m68k)));
    return site;
}

void restore(SocIMX1& soc, const Site& site) {
    assert(soc.WriteGuest(site.state, site.m68k, sizeof(site.m68k)));
    for (unsigned r = 0; r < 15; ++r) assert(soc.SetGuestRegister(r, site.registers[r]));
    assert(soc.SetGuestRegister(16, site.cpsr));
    assert(soc.SetGuestRegister(15, site.registers[15]));
}

// A 68k trap with no arguments through the ROM's syscall dispatcher, from the site; D0 after it.
uint32_t call(SocIMX1& soc, const Site& site, uint16_t trap) {
    assert(soc.SetGuestRegister(1, trap));
    assert(soc.SetGuestRegister(14, zodiacInjectedReturn));
    assert(soc.SetGuestRegister(15, soc.PaceSites().calloutSyscall));
    assert(soc.ExecuteInjected(4 * hz, hz));
    uint32_t d0;
    assert(soc.ReadGuest(site.state + 4, &d0, 4));
    restore(soc, site);
    return d0;
}

// A 68k app waiting for events with no timeout never gets back to EvtGetEvent by itself; the preparation wakes
// it through EvtWakeup in the idle loop (SocIMX1::WakeIdle), with no key or pen.
bool runToEvtGetEvent(SocIMX1& soc) { return syscallDispatchPrepare(soc.GetSyscallDispatch(), SYSCALL_PREPARE_WAKE); }


// The one database in a backup ZIP, or empty.
std::vector<uint8_t> unzipOne(const std::vector<uint8_t>& archive) {
    std::vector<uint8_t> out;
    zip_t* zip = zip_stream_open(reinterpret_cast<const char*>(archive.data()), archive.size(), 0, 'r');
    if (!zip) return out;
    if (zip_entries_total(zip) == 1 && zip_entry_openbyindex(zip, 0) == 0) {
        void* data = nullptr;
        size_t size = 0;
        if (zip_entry_read(zip, &data, &size) >= 0 && data) out.assign((uint8_t*)data, (uint8_t*)data + size);
        std::free(data);
        zip_entry_close(zip);
    }
    zip_stream_close(zip);
    return out;
}

bool listed(SocIMX1& soc, const char* name) {
    DbMetadataList list;
    assert(GuestDatabaseOps::List(soc, list, true));
    for (const auto& db : list)
        if (!std::strcmp(db.name, name)) return true;
    return false;
}

// Install, list and export through the desktop socket's and CLI's own path (GuestDatabaseOps), from a 68k app
// that waits with no timeout and then from the native launcher: FrondBench goes in, shows in the list and comes
// back out byte for byte.
void databases(SocIMX1& soc, const char* prcPath) {
    auto prc = readFile(prcPath);
    assert(prc.size() > 78);
    assert(!listed(soc, "FrondBench"));
    const int installed = GuestDatabaseOps::Install(soc, prc.size(), prc.data());
    std::printf("install %d\n", installed);
    assert(installed == DB_INSTALL_RESULT_OK);
    runFor(soc, 1000);
    assert(!soc.Halted());
    assert(listed(soc, "FrondBench"));
    std::vector<uint8_t> archive;
    assert(GuestDatabaseOps::Export(soc, "FrondBench", archive));
    auto exported = unzipOne(archive);
    // The ROM's ExgDBWrite leaves stale bytes after the name's NUL in the 32-byte name field.
    if (exported.size() >= 32)
        for (size_t i = std::strlen(reinterpret_cast<const char*>(exported.data())) + 1; i < 32; ++i)
            exported[i] = prc[i];
    std::printf("export %zu bytes of %zu\n", exported.size(), prc.size());
    if (exported != prc) {
        size_t at = 0;
        while (at < exported.size() && at < prc.size() && exported[at] == prc[at]) ++at;
        std::printf("first difference at %zu\n", at);
        FILE* f = std::fopen("build/z12/FrondBench-export.prc", "wb");
        if (f) { std::fwrite(exported.data(), 1, exported.size(), f); std::fclose(f); }
    }
    assert(exported == prc);
    runFor(soc, 1000);
    assert(!soc.Halted());
    // GuestDatabaseOps takes the native boundary in a 68k app too; the ROM PACE path still lists it.
    assert(GuestDatabaseOps::NativePrepare(soc));
    {
        GuestExport guest(soc, SYSCALL_PREPARE_WAKE);
        assert(guest.Ready());
        DbMetadataList all;
        assert(dbListGet(soc.GetSyscallDispatch(), all));
        guest.Finish();
        bool found = false;
        for (const auto& db : all) found |= !std::strcmp(db.name, "FrondBench");
        assert(found);
    }
    runFor(soc, 1000);
    assert(!soc.Halted());
    // The Tapwave launcher is native ARM: its calls go through the native boundary (ZodiacNative) at its
    // EvtGetSysEvent wait. List, export, delete and install again from there.
    press(soc, keyIdHome);
    runFor(soc, 3000);
    assert(listed(soc, "FrondBench"));
    archive.clear();
    assert(GuestDatabaseOps::Export(soc, "FrondBench", archive));
    auto again = unzipOne(archive);
    for (size_t i = std::strlen(reinterpret_cast<const char*>(again.data())) + 1; again.size() >= 32 && i < 32; ++i)
        again[i] = prc[i];
    assert(again == prc);
    const auto deleted = GuestDatabaseOps::Delete(soc, "FrondBench");
    assert(deleted && *deleted == DeleteDbResult::deleted);
    runFor(soc, 1000);
    assert(!listed(soc, "FrondBench"));
    const int reinstalled = GuestDatabaseOps::Install(soc, prc.size(), prc.data());
    std::printf("launcher: export %zu bytes, delete, install %d\n", again.size(), reinstalled);
    assert(reinstalled == DB_INSTALL_RESULT_OK);
    runFor(soc, 1000);
    assert(listed(soc, "FrondBench"));
    runFor(soc, 1000);
    assert(!soc.Halted());
}

void calls(SocIMX1& soc) {
    // REA facts of the 1.1 ROM: the syscall frame's bl and the dispatcher veneer it calls.
    assert(soc.PaceSites().entry == 0x20167b4c);
    assert(soc.PaceSites().syscallSite == 0x2016d6b8 && soc.PaceSites().calloutSyscall == 0x20176a78);
    assert(!soc.GuestCallPending());
    assert(runToEvtGetEvent(soc));
    assert(soc.GuestCallPending());
    const Site site = atSite(soc);
    assert((site.registers[1] & 0xffff) == trapEvtGetEvent);
    const uint32_t first = call(soc, site, trapTimGetTicks);
    const uint32_t second = call(soc, site, trapTimGetTicks);
    const uint32_t cards = call(soc, site, trapMemNumCards) & 0xffff;
    std::printf("ticks %u then %u, cards %u\n", first, second, cards);
    assert(first > 0 && second >= first && cards == 1);
    // Put back exactly, and the guest goes on from the site.
    const Site after = atSite(soc);
    assert(!std::memcmp(after.registers, site.registers, sizeof(site.registers)) && after.cpsr == site.cpsr);
    assert(!std::memcmp(after.m68k, site.m68k, sizeof(site.m68k)));
    runFor(soc, 2000);
    assert(!soc.Halted() && !soc.GuestCallPending());
    assert(runToEvtGetEvent(soc));
    runFor(soc, 500);
    assert(!soc.Halted());
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
    if (mode.empty()) {
        // The bring-up state's HotSync app is native ARM and makes no 68k calls. A hard reset kept with Home
        // opens Preferences, which is a 68k app.
        hardReset(*soc, false);
        calls(*soc);
        databases(*soc, std::getenv("FROND_BENCH_PRC") ? std::getenv("FROND_BENCH_PRC") : "tools/frondbench/FrondBench.prc");
        std::printf("zodiac guest calls %s: PASS\n", soc->FastEngine() ? "Fast" : "Exact");
        return 0;
    }
    std::fprintf(stderr, "unknown mode\n");
    return 1;
}
