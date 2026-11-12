#ifndef FROND_LIBERTAS_CF_H
#define FROND_LIBERTAS_CF_H
#ifndef __EMSCRIPTEN__
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "NetworkSimulation.h"
#include "get_emu_time.h"
#include "mem.h"
#include "pxa_GPIO.h"
#include "net/WifiRouter.h"
#include "reschedule.h"
class LibertasCF {
   public:
    LibertasCF(ArmMem*, PxaGpio*, Reschedule, GetEmuTime, NetworkSimulation*);
    void Poll();
    template <class S>
    void Save(S& state) {
        auto* chunk = state.GetChunk(ChunkType::libertasCF, 0);
        SaveChunkHelper helper(*chunk);
        Transfer(helper);
    }
    void Load(SavestateLoader<ChunkType>& loader);
    void Tick();
    bool NeedsTick() const { return irqDirty; }

   private:
    NetworkSimulation* network;
    bool enabled{true};
    bool Sync();
    PxaGpio* gpio;
    Reschedule schedule;
    GetEmuTime clock;
    bool associated{};
    std::unique_ptr<frondNet::WifiRouter> router;
    std::deque<std::vector<uint8_t>> rx;
    size_t rxPosition{};
    void Send();
    bool irqDirty{};
    void Dirty();
    bool power1{}, power2{}, reset{}, configured{};
    bool trace{};
    unsigned traced{};
    bool helper{}, running{}, bootAck{}, bootHeader{true};
    uint16_t firmwareRequest{16};
    uint16_t mask{0x1f}, cause{}, commandLength{}, txLength{};
    std::array<uint8_t, 1024> attribute{};
    std::vector<uint8_t> command, response, tx;
    size_t responsePosition{};
    void UpdateIrq();
    void Reset();
    bool valid{true};
    template <size_t capacity, class H>
    void Buffer(H& h, std::vector<uint8_t>& bytes) {
        std::array<uint8_t, capacity> storage{};
        uint32_t length = bytes.size();
        if (bytes.size() <= capacity) std::copy(bytes.begin(), bytes.end(), storage.begin());
        h.Do32(length).DoBuffer(storage.data(), storage.size());
        if (length <= capacity)
            bytes.assign(storage.begin(), storage.begin() + length);
        else {
            valid = false;
            bytes.clear();
        }
    }
    template <class H>
    void Transfer(H& h) {
        h.DoBool(enabled)
            .DoBool(power1)
            .DoBool(power2)
            .DoBool(reset)
            .DoBool(configured)
            .DoBool(associated)
            .DoBool(irqDirty)
            .DoBool(helper)
            .DoBool(running)
            .DoBool(bootAck)
            .DoBool(bootHeader)
            .Do16(firmwareRequest)
            .Do16(mask)
            .Do16(cause)
            .Do16(commandLength)
            .Do16(txLength)
            .DoBuffer(attribute.data(), attribute.size());
        uint32_t rsp = responsePosition, rxpos = rxPosition;
        h.Do32(rsp).Do32(rxpos);
        responsePosition = rsp;
        rxPosition = rxpos;
        Buffer<4096>(h, command);
        Buffer<4096>(h, response);
        Buffer<4096>(h, tx);
        uint32_t count = rx.size();
        h.Do32(count);
        if (count > 64) valid = false;
        std::deque<std::vector<uint8_t>> packets;
        for (unsigned i = 0; i < 64; ++i) {
            std::vector<uint8_t> bytes = i < rx.size() ? rx[i] : std::vector<uint8_t>{};
            Buffer<1560>(h, bytes);
            if (i < count) packets.push_back(std::move(bytes));
        }
        rx = std::move(packets);
    }
    void Command();
    bool Access(uint32_t, uint_fast8_t, bool, void*);
    static bool Memory(void* context, uint32_t pa, uint_fast8_t size, bool write, void* data) {
        return static_cast<LibertasCF*>(context)->Access(pa, size, write, data);
    }
    static void Power(void*, uint32_t, bool, bool);
};
#endif
#endif
