// Firmware-free E2 board/input and complete-session regression.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "CPU.h"
#include "mem.h"
#include "md5.h"
#include "audio_queue.h"
#include "session/session_file5.h"
#include "soc_PXA.h"

static uint32_t read(SocPXA& soc, uint32_t address) {
    uint32_t value = 0;
    assert(memAccess(cpuGetMem(soc.GetCpu()), address, 4, false, &value));
    return value;
}
static void write(SocPXA& soc, uint32_t address, uint32_t value) {
    assert(memAccess(cpuGetMem(soc.GetCpu()), address, 4, true, &value));
}
static void run(SocPXA& soc, unsigned ms = 100) {
    assert(soc.Run(ms * 1000, 1000000) > 0);
}
static void idle(SocPXA& soc) {
    write(soc, 0xa0000000, 0xeafffffe); // B . with IRQ/FIQ masked
    cpuInvalidateCodeCache(soc.GetCpu());
    cpuSetReg(soc.GetCpu(), ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC | ARM_SR_I | ARM_SR_F);
    cpuSetReg(soc.GetCpu(), 15, 0xa0000000);
}
static std::vector<uint8_t> state(SocPXA& soc) {
    assert(soc.Save());
    auto data = soc.GetSavestate();
    return {static_cast<uint8_t*>(data.data), static_cast<uint8_t*>(data.data) + data.size};
}
int main() {
    for (uint32_t ram : {16u << 20, 32u << 20}) {
        std::vector<uint8_t> rom(8u << 20), nand(34603008, 0xff);
        SocPXA soc(deviceTypeE2, ram, rom.data(), rom.size(), nand.data(), nand.size(), -1, 0);
        idle(soc);
        assert(soc.GetDeviceType() == deviceTypeE2 && soc.GetDisplayMode() == DisplayMode::mode_320x320);
        // GPIO hardware keys must reach the board through the public event queues.
        for (auto [key, pin] : {std::pair<KeyId, unsigned>{keyIdHard1,11}, {keyIdHard2,13},
                {keyIdHard3,7}, {keyIdHard4,5}, {keyIdUp,22}, {keyIdDown,21},
                {keyIdLeft,19}, {keyIdRight,20}, {keyIdSelect,14}, {keyIdPower,2}}) {
            assert(read(soc, 0x40e00000) & (1u << pin));
            soc.KeyDown(key); run(soc);
            assert(!(read(soc, 0x40e00000) & (1u << pin)));
            soc.KeyUp(key); run(soc);
            assert(read(soc, 0x40e00000) & (1u << pin));
        }
        // E2 direct PENDOWN is GPIO50, independent of the TX IRQ pin.
        constexpr uint32_t codec = 0x40500200;
        write(soc, codec + 0x4c * 2, 0xf836);
        write(soc, codec + 0x56 * 2, 0xf836);
        for (auto [x,y] : {std::pair<int,int>{0,0}, {319,319}, {160,439}}) {
            soc.PenDown(x,y); run(soc);
            assert(read(soc, 0x40e00004) & (1u << 18));
            soc.PenUp(); run(soc);
            assert(!(read(soc, 0x40e00004) & (1u << 18)));
        }
        // Pending host events belong to the old execution, not the restored session.
        auto withoutHostInput = state(soc);
        soc.PenDown(20,20); soc.KeyDown(keyIdHard1);
        assert(soc.Load(withoutHostInput.size(),withoutHostInput.data()));
        run(soc);
        assert(read(soc,0x40e00000) & (1u << 11));
        assert(!(read(soc,0x40e00004) & (1u << 18)));
        write(soc, 0xa0002000, 0x12345678);
        assert(read(soc, 0xa0002000 + ram) == 0x12345678); // RAM probe mirror
        const uint32_t descriptor = 0xa0003000, framebuffer = 0xa0100000;
        for (unsigned i = 0; i < 320 * 320 * 2; i += 4) write(soc, framebuffer + i, 0xf800f800);
        write(soc, descriptor, descriptor); write(soc, descriptor + 4, framebuffer);
        write(soc, descriptor + 8, 0); write(soc, descriptor + 12, 320 * 320 * 2);
        write(soc, 0x44000200, descriptor); write(soc, 0x4400000c, 4u << 24);
        write(soc, 0x44000000, 1); run(soc);
        uint32_t first, last;
        auto frame = soc.GetPendingFrame(first,last);
        assert(frame && frame[0] == 0xff0000ff && frame[320*320-1] == frame[0]);
        // Preserve embedded card contents and insertion state in both envelope versions.
        for (bool card : {false,true}) {
            if (card) {
                sdCardInitialize(65536, "synthetic-e2-card");
                uint8_t sector[512] = {0x5a}; assert(sdCardWrite(65535,sector)); soc.SdInsert();
            }
            if (card) {
                const auto data = sdCardData();
                const auto id = md5(static_cast<uint8_t*>(data.data),data.size);
                sdCardRekey(id.c_str());
            }
            auto saved = state(soc);
            // A rejected partial load must roll back register state.
            write(soc, 0x40f00008, 0x87654321);
            assert(!soc.Load(saved.size()-4, saved.data()));
            assert(read(soc, 0x40f00008) == 0x87654321);
            // Missing device state must fail atomically, as must a truncated payload.
            auto missingCodec = saved;
            auto* toc = reinterpret_cast<uint32_t*>(missingCodec.data());
            bool removed = false;
            for (unsigned i = 0; i < toc[0]; ++i) {
                if (toc[1 + 2*i] == uint32_t(ChunkType::wm9712L)) {
                    toc[1 + 2*i] = 0x7fffffff; removed = true;
                }
            }
            assert(removed && !soc.Load(missingCodec.size(),missingCodec.data()));
            assert(read(soc, 0x40f00008) == 0x87654321);
            auto audio = audioQueueCreate(1024);
            soc.SetAudioQueue(audio); audioQueuePush(audio,0x12345678);
            assert(soc.Load(saved.size(), saved.data()));
            assert(audioQueuePendingSamples(audio) == 0);
            soc.SetAudioQueue(nullptr); // queues live for this short test process
            auto memory = soc.GetMemoryData(), flash = soc.GetNandData();
            SessionFile5 envelope;
            envelope.SetDeviceType(0).SetDisplayMode(5).SetRamSize(ram)
                .SetNor(rom.size(),rom.data()).SetNand(flash.size,flash.data)
                .SetMemory(memory.size,memory.data).SetSavestate(saved.size(),saved.data());
            if (card) envelope.SetSd(sdCardData().size,sdCardData().data);
            assert(envelope.Serialize());
            SessionFile5 decoded;
            assert(decoded.Deserialize(envelope.GetSerializedSessionSize(),envelope.GetSerializedSession()));
            assert(decoded.GetVersion() == (card ? 5u : 4u));
            assert(decoded.GetDeviceType() == 0 && decoded.GetDisplayMode() == 5 && decoded.GetRamSize() == ram);
            std::vector<uint8_t> freshNor(decoded.GetNorSize()), freshNand(decoded.GetNandSize());
            memcpy(freshNor.data(),decoded.GetNor(),freshNor.size());
            memcpy(freshNand.data(),decoded.GetNand(),freshNand.size());
            if (card) {
                // Recreate the backing from the envelope, as the fresh CLI process does.
                sdCardReset();
                auto* image = static_cast<uint8_t*>(malloc(decoded.GetSdSize()));
                assert(image); memcpy(image,decoded.GetSd(),decoded.GetSdSize());
                const auto id = md5(image,decoded.GetSdSize());
                sdCardInitializeWithData(decoded.GetSdSize()/512,image,id.c_str());
            }
            SocPXA fresh(deviceTypeE2, ram, freshNor.data(),freshNor.size(),freshNand.data(),freshNand.size(),-1,0);
            memcpy(fresh.GetMemoryData().data,decoded.GetMemory(),decoded.GetMemorySize());
            std::vector<uint8_t> freshState(decoded.GetSavestateSize());
            memcpy(freshState.data(),decoded.GetSavestate(),freshState.size());
            assert(fresh.Load(freshState.size(),freshState.data()));
            assert(read(fresh,0xa0002000) == 0x12345678);
            assert(fresh.SdInserted() == card);
            assert(bool(read(fresh,0x40e00000) & (1u << 10)) == !card);
            if (card) {
                assert(decoded.GetSdSize() == sdCardData().size);
                assert(memcmp(decoded.GetSd(),sdCardData().data,decoded.GetSdSize()) == 0);
                assert(fresh.SdRemount());
                uint8_t sector[512] = {}; assert(sdCardRead(65535,sector));
                assert(sector[0] == 0x5a);
            }
            run(fresh);
            frame = fresh.GetPendingFrame(first,last);
            assert(frame && frame[0] == 0xff0000ff && frame[320*320-1] == frame[0]);
            fresh.Reset();
            assert(read(fresh,0xa0002000) == 0x12345678 && fresh.SdInserted() == card);
            assert(bool(read(fresh,0x40e00000) & (1u << 10)) == !card);
            if (card) { soc.SdEject(); sdCardReset(); }
        }
    }
    puts("PASS: E2 16/32 MiB GPIO keys, pen, RAM mirror, LCD, v4/v5 fresh-SoC restore, rollback and reset/card retention");
}
