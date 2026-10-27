// Optional private-firmware acceptance. Run under an external host deadline.
// Fixed 50M virtual cycles/second; PCM here excludes SDL and physical output.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "CPU.h"
#include "audio_queue.h"
#include "md5.h"
#include "sdcard.h"
#include "session/session_file5.h"
#include "soc_PXA.h"

namespace fs = std::filesystem;
static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static std::vector<uint8_t> copy(const void* data, size_t size) {
    if (!size) return {};
    require(data, "missing bytes");
    auto p = static_cast<const uint8_t*>(data);
    return {p, p + size};
}
static std::vector<uint8_t> read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    require(in && in.tellg() > 0 && in.tellg() <= 512 * 1024 * 1024, "invalid input");
    std::vector<uint8_t> bytes(size_t(in.tellg()));
    in.seekg(0);
    require(bool(in.read(reinterpret_cast<char*>(bytes.data()), bytes.size())), "short read");
    return bytes;
}
static void write(const fs::path& path, const std::vector<uint8_t>& bytes) {
    require(!fs::exists(path), "output already exists");
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    out.flush();
    require(bool(out), "output write failed");
}
struct Pcm {
    uint64_t frames{}, signal{}, zeros{}, run{}, longest{};
    uint64_t signalLcdOff{};
    bool sleepSeen{}, lcdOffSeen{};
    unsigned peak{};
    void add(uint32_t value) {
        ++frames;
        if (value) { ++signal; run = 0; }
        else { ++zeros; longest = std::max(longest, ++run); }
        peak = std::max({peak, unsigned(std::abs(int(int16_t(value)))),
                         unsigned(std::abs(int(int16_t(value >> 16))))});
    }
};
struct Machine {
    std::vector<uint8_t> nor, nand, metadata;
    std::unique_ptr<SocPXA> soc;
    AudioQueue* queue{audioQueueCreate(8192)};
    fs::path output;
    std::string label;
    void restore(const std::vector<uint8_t>& bytes) {
        SessionFile5 session;
        require(session.Deserialize(bytes.size(), bytes.data()), "invalid session");
        require(session.GetVersion() == 4 || session.GetVersion() == 5,
                "only complete outer session v4/v5 supported");
        require(session.GetDeviceType() == deviceTypeTX, "TX session required");
        const auto size = session.GetSdSize();
        require(!size || (session.GetVersion() == 5 &&
                    (size == (32u << 20) || size == (64u << 20) || size == (128u << 20))),
                "unsupported embedded SD size");
        soc.reset();
        sdCardReset();
        // Initialize immutable snapshot bytes before loading controller state.
        // SdRemount below reconnects the new MMC pointer; SdInsert would add an edge.
        if (size) {
            auto* owned = static_cast<uint8_t*>(std::malloc(size));
            require(owned, "SD allocation failed");
            std::memcpy(owned, session.GetSd(), size);
            sdCardInitializeWithData(size / SD_SECTOR_SIZE, owned, md5(owned, size).c_str());
        }
        nor = copy(session.GetNor(), session.GetNorSize());
        nand = copy(session.GetNand(), session.GetNandSize());
        metadata = copy(session.GetMetadata(), session.GetMetadataSize());
        require(session.GetRamSize() == 32u * 1024 * 1024, "32 MiB TX RAM required");
        soc = std::make_unique<SocPXA>(deviceTypeTX, session.GetRamSize(), nor.data(),
                                     nor.size(), nand.data(), nand.size(), -1, 2);
        require(soc->GetMemoryData().size == session.GetMemorySize(), "incomplete RAM");
        std::memcpy(soc->GetMemoryData().data, session.GetMemory(), session.GetMemorySize());
        auto state = copy(session.GetSavestate(), session.GetSavestateSize());
        require(soc->Load(state.size(), state.data()), "state restore failed");
        require(!soc->SdInserted() || size, "inserted state without embedded SD");
        if (soc->SdInserted()) require(soc->SdRemount(), "restored SD backing ID mismatch");
        soc->SetAudioQueue(queue);
        soc->SetPcmOutputEnabled(true);
        soc->SetPcmSuspended(false);
        audioQueueClear(queue);
    }
    std::vector<uint8_t> snapshot() {
        auto sd = sdCardData();
        if (sd.size) sdCardRekey(md5(static_cast<uint8_t*>(sd.data), sd.size).c_str());
        require(soc->Save(), "state save failed");
        auto rom = soc->GetRomData(), flash = soc->GetNandData();
        auto memory = soc->GetMemoryData(), state = soc->GetSavestate();
        SessionFile5 session;
        session.SetDeviceType(deviceTypeTX).SetDisplayMode(int(soc->GetDisplayMode()))
            .SetRamSize(soc->GetRamSize()).SetNor(rom.size, rom.data)
            .SetNand(flash.size, flash.data).SetMemory(memory.size, memory.data)
            .SetSavestate(state.size, state.data).SetSd(sd.size, sd.data)
            .SetMetadata(metadata.size(), metadata.data());
        require(session.Serialize(), "session serialization failed");
        return copy(session.GetSerializedSession(), session.GetSerializedSessionSize());
    }
    void roundtrip(const std::string& phase) {
        const auto sd = sdCardData();
        const auto backing = copy(sd.data, sd.size);
        const bool inserted = soc->SdInserted(), sleeping = soc->PowerSleeping();
        auto saved = snapshot();
        write(output / (label + "-" + phase + ".frond"), saved);
        restore(saved);
        const auto restored = sdCardData();
        require(restored.size == backing.size() &&
                    (!restored.size || !std::memcmp(restored.data, backing.data(), restored.size)),
                "SD snapshot bytes changed on restore");
        require(soc->SdInserted() == inserted, "insertion state changed on restore");
        require(soc->PowerSleeping() == sleeping, "power state changed on restore");
        std::printf("ROUNDTRIP fixture=%s phase=%s sd_bytes=%zu inserted=%u sleeping=%u exact_backing=1\n",
                    label.c_str(), phase.c_str(), backing.size(), inserted, sleeping);
        std::fflush(stdout);
    }
    Pcm run(unsigned ms, const std::string& phase = "") {
        Pcm pcm;
        const auto start = soc->GetTime();
        const auto wall = std::chrono::steady_clock::now();
        std::ofstream raw;
        if (!phase.empty()) {
            auto path = output / (label + "-" + phase + ".raw");
            require(!fs::exists(path), "PCM output already exists");
            raw.open(path, std::ios::binary);
            require(bool(raw), "PCM open failed");
        }
        while (soc->GetTime() - start < uint64_t(ms) * 1000000) {
            require(soc->Run(250000, 50000000) > 0, "guest made no progress");
            uint32_t samples[8192];
            const auto count = audioQueuePopChunk(queue, samples, 8192);
            pcm.sleepSeen |= soc->PowerSleeping();
            pcm.lcdOffSeen |= !soc->LcdEnabled();
            if (!phase.empty()) raw.write(reinterpret_cast<char*>(samples), count * sizeof(*samples));
            for (size_t i = 0; i < count; ++i) {
                pcm.add(samples[i]);
                if (samples[i] && !soc->LcdEnabled()) ++pcm.signalLcdOff;
            }
        }
        if (!phase.empty()) {
            raw.flush(); require(bool(raw), "PCM write failed");
            const double seconds = (soc->GetTime() - start) / 1e9;
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - wall).count();
            std::printf("PHASE fixture=%s name=%s guest_seconds=%.6f host_seconds=%.6f frames=%llu nonzero=%llu peak=%u longest_zero=%llu lcd=%u sleeping=%u sleep_seen=%u lcd_off_seen=%u nonzero_lcd_off=%llu sd_bytes=%zu inserted=%u\n",
                label.c_str(), phase.c_str(), seconds, elapsed,
                (unsigned long long)pcm.frames, (unsigned long long)pcm.signal, pcm.peak,
                (unsigned long long)pcm.longest, soc->LcdEnabled(), soc->PowerSleeping(),
                pcm.sleepSeen, pcm.lcdOffSeen, (unsigned long long)pcm.signalLcdOff,
                sdCardData().size, soc->SdInserted());
            std::fflush(stdout);
        }
        return pcm;
    }
    void pulse() { soc->KeyDown(keyIdPower); run(100); soc->KeyUp(keyIdPower); }
    void test(const fs::path& path, bool audio) {
        label = audio ? "audio" : "plain";
        restore(read(path));
        auto before = run(5000, "before");
        require(soc->LcdEnabled(), "initial LCD not enabled");
        if (audio) require(before.signal > 0 && before.frames >= 4 * 44100, "audio fixture has no sustained PCM");
        roundtrip("awake");
        auto resumed = run(5000, "awake-restored");
        if (audio) require(resumed.signal > 0 && resumed.frames >= 4 * 44100, "awake restore lost PCM");
        const auto awake = snapshot();
        pulse();
        // Bounded characterization: an application may keep power sleep disabled.
        run(20000, "power-entry");
        const bool slept = soc->PowerSleeping();
        std::printf("POWER_POLICY fixture=%s true_sleep=%u lcd_off=%u\n", label.c_str(), slept, !soc->LcdEnabled());
        std::fflush(stdout);
        if (!audio) require(slept && !soc->LcdEnabled(), "plain fixture did not enter true sleep");
        if (slept) {
            roundtrip("sleep");
            const auto card = sdCardData();
            const auto sleepBacking = copy(card.data, card.size);
            const auto pc = cpuGetRegExternal(soc->GetCpu(), 15);
            auto sleeping = run(10000, "sleep-restored");
            require(soc->PowerSleeping() && !soc->LcdEnabled(), "unsolicited wake");
            require(pc == cpuGetRegExternal(soc->GetCpu(), 15), "CPU executed in sleep");
            require(!sleeping.signal, "nonzero stale PCM during true sleep");
            const auto afterSleep = sdCardData();
            require(afterSleep.size == sleepBacking.size() &&
                        (!afterSleep.size || !std::memcmp(afterSleep.data, sleepBacking.data(), afterSleep.size)),
                    "card bytes changed during true sleep");
            pulse();
            auto wake = run(10000, "wake");
            require(!soc->PowerSleeping() && soc->LcdEnabled(), "wake failed");
            if (audio) require(wake.signal > 0, "audio sleep wake has no PCM; characterize app policy");
        } else if (!soc->LcdEnabled()) {
            pulse();
            auto wake = run(5000, "display-wake");
            require(soc->LcdEnabled(), "display wake failed");
            if (audio) require(wake.signal > 0, "display wake has no PCM");
        }
        // Reset from the known active checkpoint, independent of power-key policy.
        restore(awake);
        const bool resetInserted = soc->SdInserted();
        const auto resetCardSize = sdCardData().size;
        soc->Reset();
        require(!audioQueuePendingSamples(queue), "reset retained queued host PCM");
        run(60000, "reset-recovery");
        require(soc->LcdEnabled() && !soc->PowerSleeping(), "reset did not recover enabled LCD");
        require(soc->SdInserted() == resetInserted && sdCardData().size == resetCardSize,
                "reset lost card attachment/backing");
        roundtrip("reset");
        run(1000, "reset-restored");
        require(soc->LcdEnabled(), "reset session did not restore LCD");
        // An enabled LCD proves only a controller state, not UI/app recovery.
        std::printf("PASS fixture=%s awake_pcm=%s true_sleep=%u reset_lcd_only=1\n",
                    label.c_str(), audio ? "nonzero" : "not-required", slept);
        std::fflush(stdout);
    }
};
int main(int argc, char** argv) {
    try {
        fs::path plain, audio, output;
        for (int i = 1; i < argc; i += 2) {
            require(i + 1 < argc, "missing option value");
            const std::string option(argv[i]);
            if (option == "--plain-session" && plain.empty()) plain = argv[i + 1];
            else if (option == "--audio-session" && audio.empty()) audio = argv[i + 1];
            else if (option == "--output" && output.empty()) output = argv[i + 1];
            else throw std::runtime_error("unknown or duplicate option");
        }
        require(!output.empty() && (!plain.empty() || !audio.empty()),
                "usage: tx-lifecycle-rom --output NEW_PRIVATE_DIR [--plain-session SESSION] [--audio-session SESSION]");
        require(fs::create_directory(output), "output directory already exists");
        Machine machine;
        machine.output = output;
        try {
            if (!plain.empty()) machine.test(plain, false);
            if (!audio.empty()) machine.test(audio, true);
        } catch (...) {
            // Keep an inspectable failed guest when serialization still works.
            try {
                if (machine.soc) write(output / (machine.label + "-failed.frond"), machine.snapshot());
            } catch (const std::exception& e) {
                std::fprintf(stderr, "Failure snapshot unavailable: %s\n", e.what());
            }
            throw;
        }
        sdCardReset();
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
