#pragma once
#include <array>
#include <cstdint>
#include <functional>

#include "EmRegs.h"
#include "UsbDevicePort.h"

// Philips PDIUSBD12 USB device controller as the Sony T400 series wires it: an 8-bit bus at
// 0x10900000 with data at +0/+1 and commands at +2/+3, INT_N on IRQ1 (shared with the cradle
// button), driven by the ROM's vdrvUSB PD driver. Endpoints by D12 index: 0 control OUT, 1 control
// IN, 2/3 endpoint 1 OUT/IN (16 bytes), 4/5 endpoint 2 OUT/IN (64 bytes, double buffered).
// The host side is Port(), for UsbBusHost; it runs on the emulator thread between slices.
class EmRegsUsbD12 : public EmRegs {
   public:
    explicit EmRegsUsbD12(emuptr base);

    // INT_N: true while any interrupt register bit is set.
    std::function<void(bool)> onInterrupt;
    // The cradle cable went in or out, for the cradle's dock ID.
    std::function<void(bool)> onCable;

    void Initialize() override;
    void Reset(Bool hardwareReset) override;
    void Save(Savestate<ChunkType>&) override;
    void Save(SavestateProbe<ChunkType>&) override;
    void Load(SavestateLoader<ChunkType>&) override;
    void Dispose() override {}

    void SetSubBankHandlers() override;
    uint8* GetRealAddress(emuptr address) override;
    emuptr GetAddressStart() override { return base; }
    uint32 GetAddressRange() override { return 4; }

    // The cradle's USB cable (VBUS). Not session state: the host bridge plugs it in again after a load.
    void SetCable(bool attached);
    // On the bus: cable in, SoftConnect on and the clock running (Set Mode 0x14 bits).
    bool Listening() const { return cable && (mode[0] & 0x14) == 0x14; }
    UsbDevicePort Port();

   private:
    static constexpr unsigned kEndpoints = 6;
    struct Endpoint {
        uint8 data[2][64]{};
        uint8 size[2]{};
        uint8 head{0}, count{0};  // FIFO of whole packets
        uint8 stage[64]{};        // IN: Write Buffer fills this until Validate Buffer
        uint8 stageSize{0};
        uint8 status{0};          // last transaction status
        bool statusUnread{false}, stalled{false}, toggle{false};
    };

    uint32 Read(emuptr address, int size);
    void Write(emuptr address, int size, uint32 value);
    uint8 ReadData();
    void WriteData(uint8 value);
    void Command(uint8 command);

    static unsigned Capacity(unsigned index) { return index < 4 ? 16 : 64; }
    static unsigned Buffers(unsigned index) { return index < 4 ? 1 : 2; }
    bool Full(unsigned index) const { return ep[index].count >= Buffers(index); }
    void Complete(unsigned index, bool setup);
    void BusReset();
    void Interrupt(uint16 bits);
    void UpdateLine();
    void ListeningChanged(bool wasListening);
    void ApplyAddress(uint8 value);

    using R = UsbPacketResult;
    static EmRegsUsbD12& Self(void* p) { return *static_cast<EmRegsUsbD12*>(p); }
    R HostSetup(const uint8* data);
    R HostOut(unsigned index, const uint8* data, unsigned size);
    R HostIn(unsigned index, uint8* data, unsigned* size, bool take);
    bool HostReset();

    template <typename T>
    void DoSave(T& savestate);
    template <typename T>
    void DoSaveLoad(T& helper);

    emuptr base;
    uint8 regs[4]{};

    // Chip state, saved with the session.
    std::array<Endpoint, kEndpoints> ep{};
    uint8 mode[2]{};
    uint8 dma{0};
    uint8 address{0}, pendingAddress{0};
    bool addressEnabled{true}, addressPending{false}, endpointsEnabled{false};
    bool setupLocked[2]{}, setupPacket{false}, controlActive{false};
    uint16 interrupts{0}, frame{0};
    uint8 command{0}, dataIndex{0}, selected{0};

    // Bus side, not saved.
    bool cable{false}, line{false};
    uint32 enableCount{0};
    void (*notify)(void*){nullptr};
    void* notifyContext{nullptr};
};
