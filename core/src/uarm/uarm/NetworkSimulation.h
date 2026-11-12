#ifndef FROND_NETWORK_SIMULATION_H
#define FROND_NETWORK_SIMULATION_H
#ifndef __EMSCRIPTEN__
#include <array>
#include <cstdint>
#include <string>

#include "HostNetwork.h"
#include "savestate/savestateAll.h"
struct ArmCpu;
// Network profile; wireless mode leaves the real Palm NetLib in control.
class NetworkSimulation {
   public:
    NetworkSimulation();
    ~NetworkSimulation();
    static constexpr uint16_t refnum = 0x7ffe;
    enum Mode : uint32_t { disabled, connected, timeout, disconnected, internet, wireless };
    Mode GetMode() const { return static_cast<Mode>(mode); }
    const char* GetModeName() const;
    bool SetMode(const std::string& name);
    void Reset();
    bool Frozen() const { return checkpoints != 0; }
    bool Handle(ArmCpu* cpu, uint16_t trap);
    static bool Dispatch(void* context, ArmCpu* cpu, uint16_t trap) {
        return static_cast<NetworkSimulation*>(context)->Handle(cpu, trap);
    }
    template <class S>
    void Save(S& state) {
        auto* chunk = state.GetChunk(ChunkType::networkSimulation, 1);
        SaveChunkHelper helper(*chunk);
        Transfer(helper, 1);
    }
    void Load(SavestateLoader<ChunkType>& loader);
    // Export checkpoints freeze external I/O and preserve live host descriptors.
    class Checkpoint {
       public:
        explicit Checkpoint(NetworkSimulation& owner) : owner(owner) { ++owner.checkpoints; }
        ~Checkpoint() { Finish(); }
        void Finish() { if (active) { --owner.checkpoints; active = false; } }
        Checkpoint(const Checkpoint&) = delete;
        Checkpoint& operator=(const Checkpoint&) = delete;

       private:
        NetworkSimulation& owner;
        bool active{true};
    };

   private:
    struct Socket {
        uint32_t used{}, connected{}, size{}, type{1};
        std::array<uint8_t, 4096> bytes{};
    };
    HostNetwork host;
    unsigned checkpoints{};
    uint32_t mode{}, opens{};
    std::array<Socket, 16> sockets{};
    template <class H>
    void Transfer(H& h, uint32_t version) {
        h.Do32(mode).Do32(opens);
        for (auto& s : sockets) {
            h.Do32(s.used).Do32(s.connected).Do32(s.size);
            if (version >= 1)
                h.Do32(s.type);
            else
                s.type = 1;
            h.DoBuffer(s.bytes.data(), s.bytes.size());
        }
    }
};
#endif
#endif
