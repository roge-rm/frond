#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <vector>

// D-Link DCF-660W's Prism PC Card host interface. The socket handles card detect and power.
// Attribute addresses are PC Card bus addresses. I/O accesses are 16-bit words.
class Dcf660w {
   public:
    Dcf660w();
    void Reset();
    uint8_t ReadAttribute(uint32_t address) const;
    void WriteAttribute(uint32_t address, uint8_t value);
    uint16_t ReadIo(uint32_t address);
    void WriteIo(uint32_t address, uint16_t value);
    bool IrqAsserted() const;
    bool InReset() const { return option & 0x80; }
    bool IoConfigured() const;

    static constexpr size_t maxSnapshotBytes = 400 * 1024;
    std::vector<uint8_t> Save();
    bool Load(const uint8_t* bytes, size_t size);

    void SetNetworkAvailable(bool available);
    bool Associated() const { return IoConfigured() && associated; }
    bool ReceiveFrame(const std::vector<uint8_t>& frame);
    // Ethernet II without FCS. Runs on the socket's emulation thread.
    std::function<void(const std::vector<uint8_t>&)> transmitFrame;

   private:
    struct Bap {
        uint16_t select{0};
        uint16_t offset{0};
    };

    template <class Archive> void Transfer(Archive& archive);
    void InitializeFirmware();
    void CompleteCommand(uint16_t command);
    bool Allocate(uint16_t size);
    bool Transmit(uint16_t id, bool reclaim);
    void Acknowledge(uint16_t mask);
    void EvaluateLink();
    void Scan(const std::vector<uint8_t>& request);
    void QueueInfo(uint16_t type, const std::vector<uint8_t>& body);
    std::vector<uint8_t> Record(uint16_t id) const;
    bool AccessRecord(uint16_t id, bool write);
    std::vector<uint8_t>* Window(uint16_t id);
    uint16_t ReadBap(Bap& bap);
    void WriteBap(Bap& bap, uint16_t value);

    uint8_t option{0};
    bool enabled{false}, associated{false}, networkAvailable{false};
    uint16_t status{0}, events{0}, interruptMask{0}, nextFid{0};
    uint16_t recordId{0};
    std::array<uint16_t, 3> params{}, softwareSupport{};
    std::array<Bap, 2> baps{};
    std::map<uint16_t, std::vector<uint8_t>> buffers, records;
    std::vector<uint8_t> recordWindow;
    std::deque<uint16_t> allocations, transmissions;
    std::deque<std::vector<uint8_t>> receiveFrames, infoFrames;
};
