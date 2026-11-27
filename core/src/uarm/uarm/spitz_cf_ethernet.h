#ifndef FROND_SPITZ_CF_ETHERNET_H
#define FROND_SPITZ_CF_ETHERNET_H

#include <array>
#include <cstddef>
#include <cstdint>

class SpitzCfEthernet {
   public:
    using Transmit = void (*)(void*, const uint8_t*, size_t);
    inline static constexpr std::array<uint8_t, 6> mac{0x02, 0x46, 0x52, 0x4f, 0x4e, 0x01};

    static uint8_t Attribute(uint32_t offset);
    void Reset();
    void Access(uint32_t port, bool write, uint8_t* data, unsigned size,
                Transmit transmit, void* context);
    bool Receive(const uint8_t* data, size_t size);
    bool Irq() const { return interruptStatus & interruptMask; }

    template <typename T>
    void SaveLoad(T& chunk) {
        chunk.Do8(command).Do8(pageStart).Do8(pageStop).Do8(boundary).Do8(currentPage)
            .Do8(transmitPage).Do8(transmitStatus).Do8(receiveStatus).Do8(receiveConfig)
            .Do8(transmitConfig).Do8(dataConfig).Do8(interruptStatus).Do8(interruptMask)
            .Do8(missed).Do16(transmitBytes).Do16(remoteAddress).Do16(remoteBytes)
            .DoBuffer(physical.data(), physical.size()).DoBuffer(multicast.data(), multicast.size())
            .DoBuffer(ram.data(), ram.size());
    }

   private:
    static constexpr uint16_t ramBase = 0x4000;
    static constexpr unsigned ramSize = 0x4000;

    uint8_t command = 0x21;
    uint8_t pageStart = 0, pageStop = 0, boundary = 0, currentPage = 0;
    uint8_t transmitPage = 0, transmitStatus = 0, receiveStatus = 0;
    uint8_t receiveConfig = 0, transmitConfig = 0, dataConfig = 0;
    uint8_t interruptStatus = 0x80, interruptMask = 0, missed = 0;
    uint16_t transmitBytes = 0, remoteAddress = 0, remoteBytes = 0;
    std::array<uint8_t, 6> physical{};
    std::array<uint8_t, 8> multicast{};
    std::array<uint8_t, ramSize> ram{};

    uint8_t ReadRegister(unsigned reg);
    void WriteRegister(unsigned reg, uint8_t value, Transmit transmit, void* context);
    void ResetRegisters();
    void RemoteAccess(bool write, uint8_t* data, unsigned size);
    void TransmitPacket(Transmit transmit, void* context);
    bool AddressMatches(const uint8_t* data) const;
    bool ValidRing() const;
};

#endif
