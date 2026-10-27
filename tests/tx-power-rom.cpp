// Optional, user-supplied TX firmware test. No firmware or sessions are bundled.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

#include "CPU.h"
#include "session/session_file5.h"
#include "soc_PXA.h"

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static std::vector<uint8_t> read(const char* path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(bool(input) && input.tellg() > 0 && input.tellg() <= 512 * 1024 * 1024,
            "invalid input file");
    std::vector<uint8_t> bytes(size_t(input.tellg()));
    input.seekg(0);
    require(bool(input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())), "short input read");
    return bytes;
}
struct Machine {
    std::vector<uint8_t> nor, nand;
    std::unique_ptr<SocPXA> soc;
    void create(uint32_t ramSize) {
        soc = std::make_unique<SocPXA>(deviceTypeTX, ramSize, nor.data(), nor.size(),
                                      nand.data(), nand.size(), -1, 2);
    }
    void restore(const std::vector<uint8_t>& bytes) {
        SessionFile5 session;
        require(session.Deserialize(bytes.size(), bytes.data()), "invalid session");
        require(session.GetDeviceType() == deviceTypeTX, "TX session required");
        require(!session.GetSdSize(), "embedded SD requires test-tx-lifecycle-rom");
        auto copy = [](const void* data, size_t size) {
            return std::vector<uint8_t>(static_cast<const uint8_t*>(data),
                                       static_cast<const uint8_t*>(data) + size);
        };
        soc.reset();
        nor = copy(session.GetNor(), session.GetNorSize());
        nand = copy(session.GetNand(), session.GetNandSize());
        create(session.GetRamSize());
        require(session.GetMemorySize() == soc->GetMemoryData().size, "incomplete RAM");
        std::memcpy(soc->GetMemoryData().data, session.GetMemory(), session.GetMemorySize());
        auto state = copy(session.GetSavestate(), session.GetSavestateSize());
        require(soc->Load(state.size(), state.data()), "state restore failed");
    }
    std::vector<uint8_t> snapshot() {
        require(soc->Save(), "state save failed");
        auto state = soc->GetSavestate(), memory = soc->GetMemoryData();
        auto flash = soc->GetNandData(), rom = soc->GetRomData();
        SessionFile5 session;
        session.SetDeviceType(deviceTypeTX).SetDisplayMode(6).SetRamSize(32 * 1024 * 1024)
            .SetNor(rom.size, rom.data).SetNand(flash.size, flash.data)
            .SetMemory(memory.size, memory.data).SetSavestate(state.size, state.data);
        require(session.Serialize(), "session serialization failed");
        const auto* data = static_cast<const uint8_t*>(session.GetSerializedSession());
        return {data, data + session.GetSerializedSessionSize()};
    }
    void run(unsigned ms) {
        for (unsigned i = 0; i < ms; i += 5)
            require(soc->Run(250000, 50000000) > 0, "CPU stopped making progress");
    }
    void pulse() { soc->KeyDown(keyIdPower); run(100); soc->KeyUp(keyIdPower); }
    bool nonblankFrame() {
        run(100);
        uint32_t first, last;
        const auto* frame = soc->GetPendingFrame(first, last);
        if (!frame) return false;
        unsigned changes = 0;
        for (unsigned i = 1; i < 320 * 480; ++i) changes += frame[i] != frame[i - 1];
        return changes > 100;
    }
};
int main(int argc, char** argv) {
    try {
        require(argc == 2 || argc == 3, "usage: test-tx-power-rom SESSION | NOR NAND");
        Machine machine;
        if (argc == 2) { machine.restore(read(argv[1])); machine.run(1000); }
        else {
            machine.nor = read(argv[1]); machine.nand = read(argv[2]);
            require(machine.nor.size() == 16 * 1024 * 1024 && machine.nand.size() == 132 * 1024 * 1024,
                    "TX NOR/NAND sizes required");
            machine.create(32 * 1024 * 1024); machine.run(60000);
        }
        require(machine.soc->LcdEnabled() && machine.nonblankFrame(), "initial LCD is not ready");
        bool prematureResume = false;
        for (unsigned cycle = 0; cycle < 2; ++cycle) {
            machine.pulse();
            for (unsigned i = 0; i < 4000 && (machine.soc->LcdEnabled() || !machine.soc->PowerSleeping()); ++i) machine.run(5);
            require(!machine.soc->LcdEnabled(), "power key did not switch the display off");
            require(machine.soc->PowerSleeping(), "ROM did not enter power sleep");
            // Round-trip into a fresh SoC after actual PWRMODE=3 entry, not just LCD disable.
            auto saved = machine.snapshot(); machine.restore(saved);
            require(!machine.soc->LcdEnabled() && machine.soc->PowerSleeping(), "sleep state was lost during restore");
            const uint32_t sleepingPc = cpuGetRegExternal(machine.soc->GetCpu(), 15);
            machine.run(10000);
            const bool resumedWithoutKey = machine.soc->LcdEnabled() || !machine.soc->PowerSleeping();
            if (!resumedWithoutKey)
                require(cpuGetRegExternal(machine.soc->GetCpu(), 15) == sleepingPc, "CPU executed during sleep");
            prematureResume |= resumedWithoutKey;
            if (!resumedWithoutKey) { machine.pulse(); machine.run(10000); }
            require(machine.soc->LcdEnabled() && machine.nonblankFrame(), "ROM resume failed or blank LCD");
            saved = machine.snapshot(); machine.restore(saved); machine.run(1000);
            require(machine.soc->LcdEnabled() && machine.nonblankFrame(), "awake session did not persist");
            std::printf("TX_POWER cycle=%u sleep_restore=ok resume=ok awake_restore=ok unsolicited_wake=%u\n",
                        cycle + 1, unsigned(resumedWithoutKey));
            std::fflush(stdout);
        }
        if (prematureResume) {
            std::fputs("FAIL: ROM resumes, but the power manager wakes without a programmed event; unsolicited wake detected\n", stderr);
            return 2;
        }
        std::puts("PASS: TX sleep/wake and full-session persistence");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
