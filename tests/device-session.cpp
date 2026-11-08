// Check refused-state rollback and continued input on a private saved PXA machine.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
#include "CPEndian.h"
#include "NandFile.h"
#include "device_profile5.h"
#include "session/session_file5.h"
#include "soc_PXA.h"

static void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
static std::vector<uint8_t> save(SocPXA &soc) {
    require(soc.Save(), "cannot save checkpoint");
    auto state = soc.GetSavestate();
    return {static_cast<uint8_t *>(state.data), static_cast<uint8_t *>(state.data) + state.size};
}
static std::vector<uint32_t> frame(SocPXA &soc, const DeviceProfile5 &profile) {
    uint32_t first{}, last{};
    auto *pixels = soc.GetPendingFrame(first, last);
    require(pixels != nullptr, "guest has no display frame");
    // Keep time and battery changes out of the body comparison.
    return {pixels + 40 * profile.frontend.width,
            pixels + profile.frontend.width * profile.frontend.height};
}
int main(int argc, char **argv) {
    try {
        require(argc == 2, "usage: test-device-session session");
        std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
        require(file && file.tellg() > 0 && file.tellg() < 512 * 1024 * 1024, "cannot read session");
        std::vector<uint8_t> data(size_t(file.tellg()));
        file.seekg(0);
        require(bool(file.read(reinterpret_cast<char *>(data.data()), data.size())), "short session read");
        SessionFile5 session;
        require(session.Deserialize(data.size(), data.data()), "invalid session");
        const auto *profile = deviceProfileNamed5(session.GetDeviceName().c_str());
        require(profile && deviceProfileRunnerEligible5(profile->deviceType), "unknown session device");
        if (profile->board == DeviceBoard5::paravirtual) return 77;
        require(!session.GetSdSize(), "embedded card is not supported by this check");
        std::vector<uint8_t> nor(session.GetNorSize()), nand(session.GetNandSize());
        memcpy(nor.data(), session.GetNor(), nor.size());
        if (!nand.empty()) memcpy(nand.data(), session.GetNand(), nand.size());
        // A v9 session's NAND is nand.frondnand beside it, read through a scratch copy so the
        // session folder isn't written.
        frondSession::Contents contents;
        require(frondSession::fromSession(session, contents), "invalid session media");
        const frondMachine::Media *nandMedia = nullptr;
        for (const auto &media : contents.media)
            if (media.kind == frondMachine::kindNand) nandMedia = &media;
        const auto folder = std::filesystem::path(argv[1]).parent_path();
        const auto scratch = std::filesystem::path("build") / "device-session-nand";
        if (nandMedia) {
            std::filesystem::remove_all(scratch);
            std::filesystem::create_directories(scratch);
            std::filesystem::copy_file(folder / frondMachine::nandSibling, scratch / frondMachine::nandSibling);
        }
        const bool pxa270 = profile->board == DeviceBoard5::pxa270Tx ||
                            profile->board == DeviceBoard5::pxa270Zire72;
        SocPXA soc(profile->deviceType, session.GetRamSize(), nor.data(), nor.size(), nand.data(),
                   nandMedia ? size_t(nandMedia->physicalSize) : nand.size(), -1, pxa270 ? 2 : 0);
        require(!nandMedia || frondNand::attachFile(soc.GetNand(), scratch, *nandMedia), "cannot open the NAND file");
        require(session.GetMemorySize() == soc.GetMemoryData().size && session.GetSavestateSize(),
                "complete RAM and peripheral state are required");
        memcpy(soc.GetMemoryData().data, session.GetMemory(), session.GetMemorySize());
        std::vector<uint8_t> initialState(session.GetSavestateSize());
        memcpy(initialState.data(), session.GetSavestate(), initialState.size());
        require(soc.Load(initialState.size(), initialState.data()), "session load failed");
        constexpr uint64_t cyclesPerSecond = 100000000;
        require(soc.Run(cyclesPerSecond / 10, cyclesPerSecond) > 0, "guest did not resume");
        const auto originalFrame = frame(soc, *profile);
        for (bool late : {false, true}) {
            const auto before = save(soc);
            auto bad = before;
            if (!late) bad.resize(bad.size() - 4);
            else {
                auto *words = reinterpret_cast<uint32_t *>(bad.data());
                const auto count = le32toh(words[0]);
                size_t offset = 1 + 2 * count;
                for (unsigned i = 0; i + 1 < count; ++i) offset += le32toh(words[2 + i * 2]);
                require(offset < bad.size() / 4, "invalid saved chunk table");
                words[offset] = htole32(0xffffffff);
            }
            auto memory = soc.GetMemoryData();
            std::vector<uint8_t> oldMemory(static_cast<uint8_t *>(memory.data),
                                           static_cast<uint8_t *>(memory.data) + memory.size);
            require(!soc.Load(bad.size(), bad.data()), "malformed state was accepted");
            require(save(soc) == before, "refused load changed CPU or peripheral state");
            require(memcmp(soc.GetMemoryData().data, oldMemory.data(), oldMemory.size()) == 0,
                    "refused load changed RAM");
            const auto oldTime = soc.GetTime();
            require(soc.Run(cyclesPerSecond / 10, cyclesPerSecond) > 0 && soc.GetTime() > oldTime,
                    "guest stopped after refused load");
        }
        soc.KeyDown(keyIdHard1);
        soc.Run(cyclesPerSecond / 4, cyclesPerSecond);
        soc.KeyUp(keyIdHard1);
        soc.Run(cyclesPerSecond, cyclesPerSecond);
        require(frame(soc, *profile) != originalFrame, "new input did not change the resumed guest");
        if (nandMedia) std::filesystem::remove_all(scratch);
        printf("PASS: %s fresh machine, truncated/late-corrupt state rollback and continued guest input\n",
               profile->name);
        return 0;
    } catch (const std::exception &error) {
        fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
