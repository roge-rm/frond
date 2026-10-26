#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "audio_queue.h"
#include "session/session_file5.h"
#include "soc_PXA.h"

// Fixed-guest-time cost of the known TX SunVox menu fixture; no SDL or governor.
int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("usage: benchmark-menu playing-tx.session mips");
        size_t end;
        const auto mips = std::stoul(argv[2], &end);
        if (argv[2][end] || !mips || mips > 1000)
            throw std::runtime_error("invalid MIPS budget");
        const uint64_t cps = mips * 1000000;
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
        const auto guestStart = soc.GetTime();
        const auto cpuStart = std::clock();
        uint32_t pcm[8192];
        unsigned trial = 0, white = 0;
        bool down = false, up = false, closed = false, responded = false;
        std::clock_t pressed = 0;
        constexpr uint64_t times[] = {8000000000ull, 14000000000ull, 20000000000ull};
        auto observe = [&]() {
            uint32_t first = 0, last = 0;
            auto frame = soc.GetPendingFrame(first, last);
            if (!frame) return;
            white = 0;
            uint64_t hash = 14695981039346656037ull;
            for (unsigned y = 180; y < 240; y++) {
                for (unsigned x = 196; x < 320; x++) {
                    const auto c = frame[y * 320 + x];
                    if ((c & 255) > 150 && ((c >> 8) & 255) > 150 && ((c >> 16) & 255) > 150)
                        white++;
                    for (unsigned byte = 0; byte < 4; byte++) {
                        hash ^= (c >> (byte * 8)) & 255;
                        hash *= 1099511628211ull;
                    }
                }
            }
            soc.ResetPendingFrame();
            if (down && !responded && white >= 900) {
                const auto cpu = double(std::clock() - pressed) / CLOCKS_PER_SEC;
                std::printf("MENU_BENCH {\"trial\":%u,\"budget_mips\":%lu,"
                            "\"guest_ms\":%.6f,\"cpu_seconds\":%.6f,\"roi_hash\":\"%016llx\"}\n",
                            trial, mips, double(soc.GetTime() - guestStart - times[trial]) / 1e6,
                            cpu, (unsigned long long)hash);
                responded = true;
            }
        };
        while (soc.GetTime() - guestStart < 24000000000ull) {
            const auto now = soc.GetTime() - guestStart;
            if (trial < 3) {
                if (!down && now >= times[trial]) {
                    if (white >= 900)
                        throw std::runtime_error("fixture menu must be closed before each tap");
                    pressed = std::clock();
                    soc.PenDown(274, 8);
                    down = true;
                }
                if (down && !up && now >= times[trial] + 100000000) {
                    soc.PenUp();
                    up = true;
                }
                if (down && !responded && now >= times[trial] + 2500000000ull)
                    throw std::runtime_error("menu did not respond within 2.5 guest seconds");
                if (down && !closed && now >= times[trial] + 3000000000ull) {
                    soc.PenDown(100, 50);
                    closed = true;
                }
                if (closed && now >= times[trial] + 3100000000ull) {
                    soc.PenUp();
                    trial++;
                    down = up = closed = responded = false;
                }
            }
            if (!soc.Run(cps / 1000, cps)) throw std::runtime_error("guest made no progress");
            audioQueuePopChunk(queue, pcm, 8192);
            observe();
        }
        std::printf("MENU_TOTAL cpu_seconds %.6f\n", double(std::clock() - cpuStart) / CLOCKS_PER_SEC);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}
