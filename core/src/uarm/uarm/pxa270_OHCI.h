#ifndef FROND_PXA270_OHCI_H
#define FROND_PXA270_OHCI_H

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

template <typename T> class SavestateLoader;
enum class ChunkType : uint32_t;
struct ArmMem;

class Pxa270Ohci {
   public:
    Pxa270Ohci(ArmMem* memory, std::function<void(bool)> irq,
               std::function<void()> reschedule);
    ~Pxa270Ohci();

    bool OpenDrive(const std::string& path);
    void Eject();
    bool Attached() const { return bool(drive); }
    void SetBus(bool clockEnabled, bool portPowered, uint64_t now);
    void Tick(uint64_t now);
    uint64_t Wake() const;
    std::string Diagnostics() const;
    void MediaStateReplaced();
    std::function<void()> CaptureDriveState();
    void Load(SavestateLoader<ChunkType>& loader);
    template <typename T> void Save(T& state);

   private:
    struct UsbDrive;
    using RegisterArray = std::array<uint32_t, 28>;
    static bool Access(void*, uint32_t, uint_fast8_t, bool, void*);
    uint32_t Read(unsigned offset) const;
    void Write(unsigned offset, uint32_t value);
    void Reset();
    void SyncPort();
    void SyncIrq();
    void RunFrame();
    bool ServiceList(uint32_t& current, unsigned& budget, bool& foundTransfer);
    bool Dma(uint32_t addr, void* data, unsigned size, bool write);
    bool Transfer(uint32_t start, uint32_t end, uint8_t* data, unsigned count, bool write);

    ArmMem* memory;
    std::function<void(bool)> irq;
    std::function<void()> reschedule;
    RegisterArray registers{};
    std::unique_ptr<UsbDrive> drive;
    bool clockEnabled = false, portPowered = false;
    uint64_t now = 0, nextFrame = 0, portResetAt = 0, doneAt = 0;
    uint64_t setupPackets = 0, sectorsRead = 0;
};

#endif
