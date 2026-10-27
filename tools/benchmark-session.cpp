#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "audio_queue.h"
#include "session/session_file5.h"
#include "soc_PXA.h"

// Run a saved TX workload at a fixed virtual instruction budget, without SDL.
int main(int argc, char** argv) {
    try {
        if (argc < 4 || argc > 5)
            throw std::runtime_error("usage: benchmark-session session seconds mips [output.raw]");
        auto number = [](const char* value, unsigned maximum) {
            size_t end;
            const auto result = std::stoul(value, &end);
            if (value[end] || !result || result > maximum)
                throw std::runtime_error("invalid benchmark duration or MIPS");
            return result;
        };
        const auto seconds = number(argv[2], 600);
        const uint64_t cps = number(argv[3], 1000) * 1000000;
        std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
        if (!input || input.tellg() <= 0 || input.tellg() > 512 * 1024 * 1024)
            throw std::runtime_error("cannot read session");
        std::vector<uint8_t> data(size_t(input.tellg()));
        input.seekg(0);
        if (!input.read(reinterpret_cast<char*>(data.data()), data.size()))
            throw std::runtime_error("incomplete session read");
        SessionFile5 session;
        if (!session.Deserialize(data.size(), data.data()) ||
            session.GetDeviceType() != int(DeviceType5::deviceTypeTX))
            throw std::runtime_error("a saved TX session is required");
        if (session.GetSdSize())
            throw std::runtime_error("embedded SD is unsupported by this benchmark; use test-tx-lifecycle-rom for lifecycle checks");
        std::vector<uint8_t> nor(session.GetNorSize()), nand(session.GetNandSize());
        std::memcpy(nor.data(), session.GetNor(), nor.size());
        std::memcpy(nand.data(), session.GetNand(), nand.size());
        const auto ramSize = session.GetRamSize();
        SocPXA soc(DeviceType5::deviceTypeTX, ramSize ? ramSize : 32 * 1024 * 1024,
                   nor.data(), nor.size(), nand.data(), nand.size(), -1, 2);
        if (session.GetMemorySize() != soc.GetMemoryData().size || !session.GetSavestateSize())
            throw std::runtime_error("complete TX RAM and peripheral state are required");
        std::memcpy(soc.GetMemoryData().data, session.GetMemory(), session.GetMemorySize());
        std::vector<uint8_t> state(session.GetSavestateSize());
        std::memcpy(state.data(), session.GetSavestate(), state.size());
        if (!soc.Load(state.size(), state.data()))
            throw std::runtime_error("session restore failed");
        auto queue = audioQueueCreate(8192);
        soc.SetAudioQueue(queue);
        soc.SetPcmOutputEnabled(true);
        soc.SetPcmSuspended(false);
        std::ofstream capture;
        if (argc == 5) {
            if (std::filesystem::exists(argv[4]))
                throw std::runtime_error("PCM output already exists; choose a new path");
            capture.open(argv[4], std::ios::binary);
            if (!capture) throw std::runtime_error("cannot write PCM capture");
        }
        const auto guestStart = soc.GetTime();
        const auto started = std::chrono::steady_clock::now();
        const auto cpuStarted = std::clock();
        uint64_t cycles = 0, frames = 0, zeros = 0, run = 0, longest = 0;
        unsigned peak = 0;
        uint32_t pcm[8192];
        while (soc.GetTime() - guestStart < seconds * 1000000000ull) {
            const auto done = soc.Run(cps / 200, cps);
            if (!done) throw std::runtime_error("guest made no progress");
            cycles += done;
            const auto count = audioQueuePopChunk(queue, pcm, 8192);
            if (argc == 5) capture.write(reinterpret_cast<char*>(pcm), count * sizeof(*pcm));
            for (size_t i = 0; i < count; ++i) {
                ++frames;
                peak = std::max(peak, unsigned(std::abs(int(int16_t(pcm[i])))));
                peak = std::max(peak, unsigned(std::abs(int(int16_t(pcm[i] >> 16)))));
                if (frames <= 3 * 44100) continue;
                if (pcm[i] == 0) { ++zeros; longest = std::max(longest, ++run); }
                else run = 0;
            }
        }
        const double wall = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        const double cpu = double(std::clock() - cpuStarted) / CLOCKS_PER_SEC;
        if (argc == 5) {
            capture.flush();
            if (!capture) throw std::runtime_error("PCM write failed");
        }
        std::printf("BENCH {\"budget_mips\":%llu,\"cycles\":%llu,\"guest_seconds\":%.6f,"
                    "\"wall_seconds\":%.6f,\"cpu_seconds\":%.6f,\"frames\":%llu,"
                    "\"peak\":%u,\"silent_frames_after_warmup\":%llu,"
                    "\"longest_silence_after_warmup_frames\":%llu}\n",
                    (unsigned long long)(cps / 1000000), (unsigned long long)cycles,
                    (soc.GetTime() - guestStart) / 1e9, wall, cpu,
                    (unsigned long long)frames, peak, (unsigned long long)zeros,
                    (unsigned long long)longest);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
