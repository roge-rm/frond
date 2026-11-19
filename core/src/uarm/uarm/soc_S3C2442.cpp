#include "soc_S3C2442.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>

#include "CPCrc.h"
#include "device_configuration.h"
#include "device_profile5.h"
#include "nand.h"
#include "rx1950_board.h"
#include "rx1950_nand_image.h"
#include "rx1950_power.h"

namespace {
    // CEBL.nbf: a 0xa5a byte header, then eboot, which the loader copies to SDRAM and starts.
    constexpr uint32_t nbfHeader = 0xa5a, ebootBytes = 256 * 1024, ebootPhysical = 0x30080000;
    // Guest time one machine run covers at most, so input and frames are served often.
    constexpr uint64_t sliceNs = 1000000;
    // Host time one Run call may take, so input and frames are served while the guest is busy.
    constexpr auto runHostLimit = std::chrono::milliseconds(12);

    // Host keys to the rx1950's keys (rx1950_board.h): the four app keys, the five-way, power and record.
    const Rx1950Key* keyFor(KeyId key) {
        switch (key) {
            case keyIdHard1: return rx1950Key("calendar");
            case keyIdHard2: return rx1950Key("contacts");
            case keyIdHard3: return rx1950Key("inbox");
            case keyIdHard4: return rx1950Key("wireless");
            case keyIdUp: return rx1950Key("up");
            case keyIdDown: return rx1950Key("down");
            case keyIdLeft: return rx1950Key("left");
            case keyIdRight: return rx1950Key("right");
            case keyIdSelect: return rx1950Key("action");
            case keyIdPower: return rx1950Key("power");
            case keyIdVoice: return rx1950Key("record");
            default: return nullptr;
        }
    }
}

bool rx1950IsCebl(const uint8_t* data, size_t size) {
    return data && size == rx1950Cebl5.bytes && crc::CRC32(data, size) == rx1950Cebl5.crc32;
}

bool rx1950IsCeos(const uint8_t* data, size_t size) {
    return data && size == rx1950Ceos5.bytes && crc::CRC32(data, size) == rx1950Ceos5.crc32;
}

// The machine's NAND pages kept in the uARM NAND, whose file sessions keep crash safe (journal, then a
// rename over the old one). Writes mark the chip dirty and its dirty page bits (4224-byte units) as its own
// program and erase do, so IsNandDirty and GetNandDirtyPages see them.
struct SocS3C2442::NandPages : S3cNand::Store {
    NAND* chip;
    explicit NandPages(NAND* chip) : chip(chip) {}
    bool readPage(uint32_t page, uint8_t* out) override { return nandReadRawPage(chip, page, out); }
    bool writePage(uint32_t page, const uint8_t* in) override {
        if (!nandWriteRawPage(chip, page, in)) return false;
        markDirty(page, 1);
        return true;
    }
    bool eraseBlock(uint32_t firstPage, uint32_t count) override {
        uint8_t ones[S3cNand::pageSize];
        std::memset(ones, 0xff, sizeof ones);
        for (uint32_t p = 0; p < count; ++p)
            if (!nandWriteRawPage(chip, firstPage + p, ones)) return false;
        markDirty(firstPage, count);
        return true;
    }
    void markDirty(uint32_t firstPage, uint32_t count) {
        const Buffer bits = nandGetDirtyPages(chip);
        auto* words = static_cast<uint32_t*>(bits.data);
        const size_t first = size_t(firstPage) * S3cNand::pageSize / 4224;
        const size_t last = (size_t(firstPage + count) * S3cNand::pageSize - 1) / 4224;
        for (size_t unit = first; unit <= last && unit / 32 < bits.size / 4; ++unit) words[unit / 32] |= 1u << (unit % 32);
        nandSetDirty(chip, true);
    }
};

SocS3C2442* SocS3C2442::Create(const uint8_t* cebl, size_t size, uint8_t* flash, size_t flashSize, std::string& error) {
    if (!rx1950IsCebl(cebl, size)) {
        error = "not the rx1950's CEBL.nbf that Frond runs (HP's WM5 ROM 1.01.00 ENG)";
        return nullptr;
    }
    std::unique_ptr<SocS3C2442> soc(new SocS3C2442());
    soc->rom.assign(cebl, cebl + size);
    S3c2442Options options;
    options.rtcStart = rx1950RtcStart;
    // No serial cable.
    options.pinLevels[rx1950CablePort] &= ~(1u << rx1950CableBit);
    rx1950BatteryPins(soc->battery, options);
    soc->machine = S3c2442Machine::create(options, error);
    if (!soc->machine) return nullptr;

    if (!memoryBufferAllocate(&soc->nandPageBuffer, MEMORY_BUFFER_GRANULARITY)) {
        error = "the NAND page buffer could not be allocated";
        return nullptr;
    }
    static const NandSpecs specs = {
        .bytesPerPage = S3cNand::pageSize,
        .blocksPerDevice = S3cNand::blocks,
        .pagesPerBlockLg2 = 5,
        .flags = NAND_FLAG_SAMSUNG_ADDRESSED_VIA_AREAS,
        .devIdLen = 2,
        .devId = {0xec, 0x76},
    };
    const Reschedule noReschedule{.rescheduleCb = [](void*, uint32_t) {}, .ctx = nullptr};
    if (flashSize != size_t(S3cNand::pages) * S3cNand::pageSize ||
        !(soc->nand = nandInit(flash, &soc->nandPageBuffer, noReschedule, flashSize, &specs, nullptr, nullptr))) {
        error = "the NAND isn't the rx1950's 64 MB";
        return nullptr;
    }
    soc->pages = std::make_unique<NandPages>(soc->nand);
    soc->machine->nand.setStore(soc->pages.get());

    soc->machine->resetLoader = [s = soc.get()] { s->BootLoader(); };
    soc->BootLoader();
    rx1950ApplyBattery(*soc->machine, soc->battery);
    soc->displayConfiguration = deviceConfigurationDisplayConfigForDevice(deviceTypeRx1950, DisplayMode::mode_240x320);
    soc->ramSize = options.ramBytes;
    soc->StateChanged();
    // The owner keeps it for the life of the process, like the machine inside.
    return soc.release();
}

void SocS3C2442::BootLoader() {
    machine->loadRam(ebootPhysical, rom.data() + nbfHeader, ebootBytes);
    // ASSUMPTION "rx1950 IPL clocks": the block 0 loader sets the PLL before eboot, which never does.
    // 296.35 MHz from the 16.9344 MHz crystal (MDIV 97, PDIV 1, SDIV 2), FCLK:HCLK:PCLK 1:3:6.
    machine->clock.write(0x04, 0x61012);
    machine->clock.write(0x14, 0x7);
    rx1950LoaderPins(machine->gpio);
    machine->start(ebootPhysical);
}

void SocS3C2442::StateChanged() {
    const auto stop = machine->stopInfo().stop;
    halted = stop != S3c2442Stop::running && stop != S3c2442Stop::budget;
    framePending = false;
    redraw = true;
    jams.clear();
    keys.clear();
    // The host's keys and pen are up now.
    for (const auto& k : rx1950Keys) machine->setPin(k.port, k.bit, true);
    pen.clear(*machine);
}

void SocS3C2442::SetClockMultiplier(uint32_t num, uint32_t den) {
    SoC::SetClockMultiplier(num, den);
    machine->setClockMultiplier(ClockMultiplierNum(), ClockMultiplierDen());
}

uint64_t SocS3C2442::CoreHz() const {
    return machine->clock.cpuClock() * clockMultiplierNum / clockMultiplierDen;
}

// The reset button. ASSUMPTION "rx1950 reset button": it pulls the S3C2442's nRESET, which keeps the RTC,
// and the loader and eboot start the OS from flash as after a power on.
void SocS3C2442::Reset() {
    SoC::Reset();
    machine->reset();
    rx1950ApplyBattery(*machine, battery);
    StateChanged();
}

bool SocS3C2442::Save() {
    std::string why;
    if (halted || !machine->saveState(saved, why)) {
        std::cerr << "rx1950: not saved, " << (halted ? std::string("the machine has stopped") : why) << std::endl;
        return false;
    }
    return true;
}

bool SocS3C2442::Load(size_t savestateSize, void* savestateData) {
    // A machine started from the firmware has no state to load.
    if (!savestateSize || !savestateData) return true;
    std::string why;
    if (!machine->loadState(static_cast<const uint8_t*>(savestateData), savestateSize, why)) {
        std::cerr << "rx1950: saved state refused: " << why << std::endl;
        return false;
    }
    machine->nand.dropCache();
    rx1950ApplyBattery(*machine, battery);
    StateChanged();
    return true;
}

bool SocS3C2442::SetBattery(const BatteryPower& value) {
    battery = value;
    if (machine) rx1950ApplyBattery(*machine, battery);
    return true;
}

SoC::Attention SocS3C2442::GuestAttention() { return {rx1950NotificationLed(*machine), false}; }

bool SocS3C2442::LcdEnabled() { return !machine->asleep() && machine->lcd.enabled(); }

// The panel as the LCD controller scans it, 0xRRGGBB, to the frontend's ABGR8888. Black with the LCD off.
uint32_t* SocS3C2442::GetPendingFrame(uint32_t& firstDirtyLine, uint32_t& lastDirtyLine) {
    const unsigned width = displayConfiguration.width, height = displayConfiguration.height;
    if (view.size() != size_t(width) * height) view.assign(size_t(width) * height, 0xff000000);
    unsigned w = 0, h = 0;
    std::string why;
    bool changed = false;
    if (LcdEnabled() && machine->screen(scan, w, h, why) && w == width && h == height) {
        for (size_t i = 0; i < view.size(); ++i) {
            const uint32_t c = scan[i];
            const uint32_t px = 0xff000000u | (c & 0xff) << 16 | (c & 0xff00) | ((c >> 16) & 0xff);
            if (view[i] != px) { view[i] = px; changed = true; }
        }
    } else {
        for (auto& px : view)
            if (px != 0xff000000u) { px = 0xff000000u; changed = true; }
    }
    if (changed || redraw) framePending = true;
    if (redraw) {
        redraw = false;
        ClearFramebufferDirty();
    }
    if (!framePending) return nullptr;
    firstDirtyLine = 0;
    lastDirtyLine = height - 1;
    return view.data();
}

void SocS3C2442::JamKey(enum KeyId key, uint32_t durationMsec) {
    const auto* k = keyFor(key);
    if (!k || halted) return;
    machine->setPin(k->port, k->bit, false);
    jams.push_back({key, machine->nanoseconds() + uint64_t(durationMsec) * 1000000});
}

// Hands queued host input to the machine in the guest's own time: keys pull their pins low through the
// key pacer, the pen goes to the touch panel through the pen pacer, at most one down or up each call.
void SocS3C2442::PumpInput() {
    const uint64_t now = machine->nanoseconds();
    for (auto it = jams.begin(); it != jams.end();) {
        if (it->untilNs > now) { ++it; continue; }
        if (const auto* k = keyFor(it->key)) machine->setPin(k->port, k->bit, true);
        it = jams.erase(it);
    }
    while (keyEventQueue->GetSize()) {
        const KeyEvent event = keyEventQueue->Peek();
        keyEventQueue->Pop();
        if (const auto* k = keyFor(event.key)) keys.push(k->port, k->bit, event.keyDown);
    }
    while (penEventQueue->GetSize()) {
        const PenEvent event = penEventQueue->Peek();
        penEventQueue->Pop();
        const unsigned x = unsigned(std::clamp(event.x, 0, int(displayConfiguration.width) - 1));
        const unsigned y = unsigned(std::clamp(event.y, 0, int(displayConfiguration.height) - 1));
        pen.push(event.penDown, rx1950TouchRawX(x), rx1950TouchRawY(y));
    }
    keys.pump(*machine);
    pen.pump(*machine);
}

// Runs the guest for at least maxCycles / cyclesPerSecond of its own time, or for a short stretch of host
// time when it can't keep up, and returns the guest time that passed in the same cycle units. Each machine
// run covers up to sliceNs of guest time at the CPU clock it has when it starts.
uint64_t SocS3C2442::Run(uint64_t maxCycles, uint64_t cyclesPerSecond) {
    if (halted || !cyclesPerSecond) {
        keyEventQueue->Clear();
        penEventQueue->Clear();
        return maxCycles;
    }
    const uint64_t before = machine->nanoseconds(), resets = machine->watchdogResets();
    const uint64_t wanted = std::max<uint64_t>(1, uint64_t(__uint128_t(maxCycles) * 1000000000 / cyclesPerSecond));
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        // Input goes in every slice, so pen and key times are kept in guest time.
        PumpInput();
        const uint64_t hz = std::max<uint64_t>(CoreHz(), 1000000);
        const uint64_t left = before + wanted - std::min(before + wanted, machine->nanoseconds());
        const uint64_t ns = std::min(sliceNs, std::max<uint64_t>(left, 1));
        const auto stop = machine->run(std::max<uint64_t>(1, uint64_t(__uint128_t(ns) * hz / 1000000000)));
        if (stop != S3c2442Stop::budget) {
            halted = true;
            std::cerr << "rx1950: the machine stopped: " << machine->stopText() << ", after " << machine->retired()
                      << " instructions" << std::endl;
            break;
        }
        if (machine->nanoseconds() >= before + wanted) break;
        if (hostTimeLimit && std::chrono::steady_clock::now() - started >= runHostLimit) break;
    }
    runInstructions = machine->retired();
    if (halted) return maxCycles;
    // The guest reset itself through the watchdog: the board's inputs as after any reset.
    if (machine->watchdogResets() != resets) {
        rx1950ApplyBattery(*machine, battery);
        StateChanged();
    }
    const uint64_t elapsed = machine->nanoseconds() - before;
    const __uint128_t credit = __uint128_t(elapsed) * cyclesPerSecond + creditRemainder;
    creditRemainder = uint64_t(credit % 1000000000);
    return uint64_t(std::min<__uint128_t>(credit / 1000000000, UINT64_MAX));
}
