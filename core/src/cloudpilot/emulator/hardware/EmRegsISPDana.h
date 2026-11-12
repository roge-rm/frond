#pragma once
#include "EmRegs.h"
#include <array>
#include <functional>
#include <vector>

// ISP1161A nonisochronous device FIFOs and bounded host root ports.
class EmRegsISPDana : public EmRegs {
 public:
    enum class PacketResult { ack, nak, stall, noAnswer };
    void SetDeviceIrq(std::function<void(bool)> sink) { irqSink=std::move(sink); RefreshIrq(); }
    void SetVbus(bool);
    bool DeviceAttached() const { return vbus && (dcMode&1); }
    void BusReset();
    PacketResult HostOut(unsigned endpoint, const std::vector<uint8>&, bool data1, bool setup=false);
    PacketResult HostIn(unsigned endpoint, std::vector<uint8>&, bool& data1);
    void HostAckIn(unsigned endpoint);
    bool TryHostAckIn(unsigned endpoint);
    bool PacketIsIn(unsigned endpoint) const { return endpoint<16 && (dcEndpoints[endpoint]&64); }
    bool HostOutReady(unsigned endpoint, unsigned size) const {
        // Only a valid full FIFO defers host submission; other states reach HostOut's checks.
        return endpoint>=16 || PacketIsIn(endpoint) || size>Capacity(endpoint) || !DeviceAttached() ||
            !deviceEnabled || !allocated || !(dcEndpoints[endpoint]&128) || fifo[endpoint].stalled ||
            !fifo[endpoint].packet[fifo[endpoint].bus].full;
    }
    PacketResult PeekHostIn(unsigned endpoint, std::vector<uint8>&, bool& data1) const;
    uint64 PacketResetEpoch(unsigned endpoint) const { return packetResetEpoch.at(endpoint); }
    unsigned PacketCapacity(unsigned endpoint) const { return endpoint<16 ? Capacity(endpoint) : 0; }
    uint8 EffectiveAddress() const { return effectiveAddress; }
    // Codec never changes output on failure. PIO prefixes are validated separately.
    struct HostPtd {
        uint16 actual=0, maximum=0, total=0;
        uint8 completion=0, endpoint=0, direction=0, address=0;
        bool active=false, toggle=false, last=false, lowSpeed=false, interrupt=false;
    };
    static bool DecodeHostPtd(const uint8*, unsigned, unsigned, HostPtd&);
    static bool CompleteHostPtd(uint8*, unsigned, unsigned, uint16, uint8, bool);
    static bool ValidateHostList(const uint8*, unsigned, unsigned allocation=0);
    // HC logical request and level-mode pin output.
    bool HostInterruptPending();
    void SetHostIrq(std::function<void(bool)> sink) { hostIrqSink=std::move(sink); RefreshHostInterrupt(); }
    void SetHostConnection(unsigned port, bool attached, bool lowSpeed=false);
    struct HostPacket {
        uint8 address, endpoint, direction;
        bool toggle, lowSpeed;
        unsigned requested;
        std::vector<uint8> bytes;
    };
    struct HostReply {
        PacketResult result=PacketResult::noAnswer;
        bool toggle=false;
        std::vector<uint8> bytes;
    };
    // Synchronous, same-thread target. It must not re-enter the controller or throw.
    // ACK accepts an OUT irrevocably; IN bytes are accepted only with matching toggle/size.
    // Each root port's runtime owner, never saved. cancelled: this port's reset, disable, power or
    // disconnect, or a controller reset or load, ended its device's host work;
    // reattach: plug the device back in after a hardware reset; portReset: a reset of this port has
    // started; supplyChanged: this port's supply changed.
    struct HostOwner {
        std::function<void()> cancelled, reattach, portReset, supplyChanged;
    };
    std::array<HostOwner,2> hostOwner{};
    // Root port supply from the board's power lines (Dana: PM5 external, PK0 internal module).
    // Not saved: the board sets it again on reset and load.
    void SetPortSupply(unsigned port, bool on);
    bool PortSupply(unsigned port) const { return portSupply.at(port); }
    using HostTarget=std::function<HostReply(const HostPacket&)>;
    bool HasHostTarget(unsigned port) const { return bool(hostTargets.at(port)); }
    // Connected in a loaded state and waiting for its device to come back.
    bool PortLoaded(unsigned port) const { return loadedPorts.at(port); }
    // After a load a connected port stays as saved: an owner that plugs its device back in before the
    // guest runs keeps the connection; otherwise the guest's next access finds the port disconnected.
    void SetHostTarget(unsigned port, HostTarget target);
    // Disconnects a port kept by a load now, for an owner whose device isn't the saved one.
    void DropLoadedPort(unsigned port);
    // Multiplies the cycle stamps when the session's cycle unit changes.
    void ScaleCycles(uint32 factor);
    void Initialize() override;
    void Dispose() override;
    void Reset(Bool hardware) override;
    void Save(Savestate<ChunkType>&) override;
    void Save(SavestateProbe<ChunkType>&) override;
    void Load(SavestateLoader<ChunkType>&) override;
    template <typename T> void SaveState(T&);
    template <typename T> void StateFields(T&);

    void SetSubBankHandlers() override;
    uint8* GetRealAddress(emuptr) override { return nullptr; }
    emuptr GetAddressStart() override { return 0x1f000000; }
    uint32 GetAddressRange() override { return 8; }
 protected:
    virtual uint64 CurrentCycle() const;
    virtual uint64 ClockHz() const;
 private:
    std::array<uint64,16> packetResetEpoch{};
    struct Packet { uint16 length=0, words=0; bool full=false, writing=false; std::array<uint16,32> data{}; };
    struct Endpoint { std::array<Packet,2> packet{}; uint16 cpu=0, bus=0, error=0;
        bool statusPending=false, stalled=false, toggle=false, setup=false, overwrite=false, offered=false; };
    std::array<Endpoint,16> fifo{};
    unsigned configNext=0, cursor=0;
    uint32 flushed=0;
    bool accessCancelled=false;
    void CancelAccess(unsigned, int buffer=-1);
    template <typename T> void CancellationFields(T&);
    bool allocated=false, setupPending=false, vbus=false, deviceEnabled=false;
    uint16 effectiveAddress=0;
    std::function<void(bool)> irqSink, hostIrqSink;
    void RefreshIrq();
    void Transaction(unsigned, uint16);
    void DeviceCommand(uint16);
    unsigned Capacity(unsigned) const;
    void ValidateConfiguration() const;
    uint16 EndpointStatus(unsigned, bool);
    template <typename T> void PacketFields(T&);
    static void CycleThunk(void*, uint64, bool);
    void Tick();
    uint64 lastCycle=0, fractional=0, resetCycles=0;
    uint32 Read(emuptr, int);
    uint32 DeviceRead();
    void DeviceWrite(uint16);
    void ResetDevice();
    std::array<uint16, 16> dcEndpoints{};
    uint16 dcAddress=0;
    uint16 dcCommand=0, dcMode=0, dcHardware=0, dcScratch=0;
    bool dcUnlocked=true, dcSuspended=false;
    uint32 dcInterrupt=0, dcInterruptEnable=0, dcLatch=0;
    unsigned dcWord=0;
    void Write(emuptr, int, uint32);
    void Commit(uint32);
    std::array<HostTarget,2> hostTargets{};
    std::array<bool,2> portSupply{{true,true}};
    std::array<bool,2> loadedPorts{}; // Connected in the loaded state, waiting for its device.
    void SettleLoadedPorts();
    void HostCancelled(int port=-1);
    bool hostArmed=false;
    void ExecuteHostFrame();
    void FailHostList(std::array<uint8,4096>&, unsigned written, unsigned size);
    void CancelHostWork(int port=-1);
    std::array<bool,2> hostAttached{}, hostLowSpeed{};
    std::array<uint64,2> portResetCycles{};
    void HostPortPower(unsigned, bool);
    void RefreshHostInterrupt();
    std::array<uint16,2048> hostRam{};
    unsigned hostCursor=0;
    unsigned hostWritten=0;
    template <typename T> void HostFields(T&);
    uint16 HostWord(bool writing, uint16 value=0);
    std::array<uint32, 0x30> regs{};
    uint16 command = 0;
    unsigned word = 0;
    uint32 latch = 0;
};
