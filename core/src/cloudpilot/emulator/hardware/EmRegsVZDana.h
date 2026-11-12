#ifndef EM_REGS_VZ_DANA_H
#define EM_REGS_VZ_DANA_H

#include <array>
#include <cstring>
#include <deque>
#include <functional>
#include <vector>
#include "EmRegsVZ.h"
#include "EmSPISlave.h"
#include "EmSPISlaveADS784x.h"
#include "EmSPISlaveSD.h"

// Dana ST controller: version exchange, reset and physical-key snapshots.
class EmRegsVZDana : public EmRegsVZ, public EmSPISlave {
    EmSPISlaveSD sd0, sd1{EmHAL::Slot::sdcard2};
    class CardBus : public EmSPISlave {
        EmRegsVZDana& board;
      public:
        explicit CardBus(EmRegsVZDana& board) : board(board) {}
        uint16 DoExchange(uint16 control, uint16 data) override;
    } cardBus{*this};
    bool CardPowered(unsigned slot);
    bool CardSelected(unsigned slot);
    void SyncCards();
    void CardControlWrite(emuptr, int, uint32);
    unsigned sleepCommand=0;
    // Cold mask bytes are unknown on hardware; zero is the simulator default.
    unsigned wakeMask[2]{};
    unsigned stAsleep=0;
    // D8 and row IRQ groups are independent of the retained DA/DB masks.
    unsigned ebHold=0, ebReported=0, rowGroups=0, pa3Option=1, fallingOnly=0;
    unsigned SelectedRowGroups(uint8 extra=0) const;
    void EnterHalt(bool shortPath=false);
    bool StRequest();
    void ResetPowerProtocol();
    bool resetReply=true, resetAsserted=false;
    unsigned versionCommand=0, versionReply=0;
    // The guest accepts up to eight scan codes per snapshot. Keep replies immutable
    // while later transitions wait in a bounded FIFO.
    uint8 heldKeys=0, packetKeys=0;
    uint8 heldScan[8]{}, packets[32][8]{};
    uint8 presentedScan[8]{};
    unsigned pollIndex=0;
    bool hostPaced=false;
    uint64 inputLastCycle=0,inputDelay=0,inputClockHz=0;
    bool InputReady();
    // The ROM drops a press of a key it saw released less than about 60-70 ms of guest time
    // before, and around 100 ms it can take the press twice ("letter hello" typed "heello"),
    // so a snapshot that presses a key in regapScan again waits until that key's regapLeft
    // cycles pass. Meanwhile the held set is reported again from presentedScan (repeatReply).
    // Entries expire on their own; a ninth release evicts the oldest.
    static constexpr unsigned kStateVersion=11;
    uint8 regapScan[8]{};
    uint64 regapLeft[8]{};
    bool repeatReply=false;
    bool RegapBlocked() const;
    bool KeyQueueBlocked() const;
    void NoteRelease(const uint8* next);
    void ClearRegap() { std::memset(regapScan,0,8); std::memset(regapLeft,0,sizeof regapLeft); repeatReply=false; }
    void AgeRegap(uint64 elapsed);
    unsigned heldCount=0, queueHead=0, queueCount=0;
    void QueueKeys();
    // Host transitions that don't fit the 32-report FIFO yet wait here in order, each a scan code
    // with bit 7 set for a press. FeedBacklog moves them in as slots free; it runs only from
    // ReadKeyPacket and CycleSlowly, so a replay feeds at the same guest cycles every run.
    // Presses past kBacklogLimit are refused; releases always fit.
    std::deque<uint8> backlog;
    bool backlogDamaged=false;
    void FeedBacklog(bool direct=false);
    bool Wakes(uint8 code) const;
    bool PullWakingKey();
    bool PullRelease();
    // Not saved: after a load the next halted feed scans once and sets it again.
    bool noWakeValid=false;
    std::array<uint32,14> noWakeAt{};
    std::array<uint32,14> WakeInputs() const;
    bool AcceptTransitions(const std::vector<std::pair<uint8,bool>>& changes,bool paced=false);
    std::vector<uint8> BacklogHeld() const;
    void ApplyScanCode(uint8 code, bool down);
    unsigned packetIndex=0;
    bool dcIrqHigh=true, hcIrqHigh=true;
    bool keysDirty=false, irqPinHigh=true, irqLatched=false;
    void UpdateStIrq();
    void StIntControlWrite(emuptr,int,uint32);
    void StIntStatusWrite(emuptr,int,uint32);
    uint8 ReadKeyPacket();
    class AnalogueInputs : public EmSPISlaveADS784x {
      public:
        // Raw 12-bit ADC counts, independent of any physical voltage or divider.
        // The ROM halves supply then subtracts 25 as an unsigned word.
        // Fifty is the lowest virtual disconnected supply without underflow.
        uint16 mainBattery=3072, supply=50;
        bool packDetectHigh=true;
        AnalogueInputs() : EmSPISlaveADS784x(kChannelUnused, kChannelPenY,
            kChannelVirtualInput, kChannelUnused, kChannelUnused, kChannelPenX,
            kChannelVirtualInput, kChannelUnused) {}
        uint16 ReadVirtualInput(unsigned channel) const override {
            return channel==2 ? mainBattery : channel==6 ? supply : 0;
        }
    } adc;
   protected:
    void MigrateLegacyInput();
    // Queues the held keys again once the report pacing allows; called from CycleSlowly.
    void ReportHeldKeys();
    void DispatchPwmChange() override;
    void SoundControlWrite(emuptr, int, uint32);
    void SoundClockWrite(emuptr, int, uint32);
    void SoundMuxWrite(emuptr, int, uint32);
   public:
    // USB root port supply: PM5 high powers the external port (0), PK0 high the internal module's
    // port (1), as the ROM's helper drives them on boards with CSA size 0x0c or more (Frond's Dana).
    std::function<void(unsigned port, bool on)> usbSupplySink;
    // Called from ScaleCycles so the ISP's cycle stamps follow the session's.
    std::function<void(uint32 factor)> cycleScaleSink;
    void SyncUsbSupply();
    void SetHcIrqPin(bool high) { auto old=hcIrqHigh; hcIrqHigh=high; UpdateIRQ3(old ? 0x40 : 0); }
    void SetDcIrqPin(bool high) { auto old=dcIrqHigh; dcIrqHigh=high; UpdateIRQ2(old ? 0x20 : 0); }
    bool SetVirtualAnalogueInputs(uint16 mainBattery, uint16 supply, bool packDetectHigh) {
        if(mainBattery>4095 || supply>4095) return false;
        adc.mainBattery=mainBattery; adc.supply=supply; adc.packDetectHigh=packDetectHigh;
        return true;
    }
    void CycleSlowly(Bool sleeping) override;
    bool PortDPolarityInverts() const override { return true; }
    void ScaleCycles(uint32 factor) override;
    virtual uint64 InputCycle() const;
    virtual uint64 InputHz() const;
    // Free bytes in Palm OS's key queue, or ~0u when it can't be read.
    virtual uint32 KeyQueueRoom() const;
    bool DispatchDanaInput(const std::vector<DanaTransition5>&) override;
    void ReleaseDanaInput(bool cancel=true) override;
    void DispatchButtonEvent(ButtonEvent) override;
    // Scan codes are physical ST positions, independent of host text and KeyId. False only for an
    // invalid code, a ninth held key or a full backlog; a full FIFO makes the change wait.
    bool DispatchScanCode(uint8 code, bool down);
    static constexpr size_t kBacklogLimit=8192;
    // Free bytes in the guest key queue below which a new press waits (KeyQueueBlocked), and the
    // bytes the ROM puts in per key.
    static constexpr uint32 kKeyQueueReserve=16, kKeyQueueEntry=7;
    // Free bytes in the KeyQueueType that SysEvtMgrGlobalsType at globals points to (keyQP at
    // +10; start, end and size at +0, +2, +4, start==end empty). ~0u unless both lie in RAM
    // [ramStart, ramStart+ramSize) and the fields are in range, so registers are never read.
    template<class Read16,class Read32> static uint32 KeyQueueRoomIn(emuptr globals,emuptr ramStart,uint32 ramSize,Read16 read16,Read32 read32) {
        auto inRam=[&](emuptr a,uint32 n) { return !(a&1) && a>=ramStart && a-ramStart<=ramSize && n<=ramSize-(a-ramStart); };
        if(!globals || !inRam(globals,14)) return ~0u;
        emuptr queue=read32(globals+10);
        if(!queue || !inRam(queue,6)) return ~0u;
        uint32 start=read16(queue),end=read16(queue+2),size=read16(queue+4);
        // A queue too small to hold one key past the reserve isn't gated, or it would block for ever.
        if(size<=kKeyQueueReserve+kKeyQueueEntry || size>0x4000 || start>=size || end>=size || !inRam(queue,6+size)) return ~0u;
        return size-(end>=start ? end-start : size-start+end);
    }
    // 150 ms also passes on the guest; 200 leaves room for slow guest reads.
    static constexpr uint64 kRegapMs=200;
    size_t BacklogSize() const { return backlog.size(); }
    bool BacklogValid() const;
    // Backlog scans for a waking key, for tests; not saved.
    unsigned wakeScans=0;
    void SetSubBankHandlers() override;
    void Save(Savestate<ChunkType>&) override;
    void Save(SavestateProbe<ChunkType>&) override;
    void Load(SavestateLoader<ChunkType>&) override;
    template <typename T> void SaveState(T&);
    // The ST chunk alone, which Save and Load use; public so tests can run it without the VZ core.
    template <typename T> void SaveStChunk(T&);
    void LoadStChunk(SavestateLoader<ChunkType>&);
    template <typename T> void LegacyStateFields(T& h) { h.DoBool(resetReply).DoBool(resetAsserted).Do32(versionCommand).Do32(versionReply).Do8(heldKeys).Do8(packetKeys).Do32(packetIndex).DoBool(keysDirty).DoBool(irqPinHigh).DoBool(irqLatched).Do16(adc.mainBattery).Do16(adc.supply).DoBool(adc.packDetectHigh);
    }
    template <typename T> void StateFields(T& h) {
        LegacyStateFields(h);
        h.Do32(heldCount).Do32(queueHead).Do32(queueCount);
        for(auto& code:heldScan) h.Do8(code);
        for(auto& packet:packets) for(auto& code:packet) h.Do8(code); }
    template<class T> void InputStateFields(T& h) { h.DoBool(hostPaced).Do64(inputLastCycle).Do64(inputDelay).Do64(inputClockHz).Do32(pollIndex); for(auto& code:presentedScan) h.Do8(code); }
    template<class T> void PowerStateFields(T& h) {
        h.Do32(sleepCommand).Do32(wakeMask[0]).Do32(wakeMask[1]).Do32(stAsleep);
    }
    template<class T> void PowerModeFields(T& h) {
        h.Do32(ebHold).Do32(ebReported).Do32(rowGroups).Do32(pa3Option).Do32(fallingOnly);
    }
    template<class T> void RegapFields(T& h) {
        for(auto& code:regapScan) h.Do8(code);
        for(auto& left:regapLeft) h.Do64(left);
        h.DoBool(repeatReply);
    }
    template<class T> void BacklogFields(T& h) {
        uint32 count=uint32(backlog.size()); h.Do32(count);
        if(count>kBacklogLimit+8) { backlogDamaged=true; count=0; }
        backlog.resize(count);
        for(auto& entry:backlog) h.Do8(entry);
    }
    void Reset(Bool hardware) override;
    void TurnSoundOff() override;
    Bool GetLineDriverState(EmUARTDeviceType) override;
    EmUARTDeviceType GetUARTDevice(int) override;

    bool SupportsImageInSlot(EmHAL::Slot, uint32) override;
    void Mount(EmHAL::Slot, CardImage&) override;
    void Unmount(EmHAL::Slot) override;
    void Remount(EmHAL::Slot, CardImage&) override;
    void KDirWrite(emuptr, int, uint32);
    void PortDataChanged(int, uint8, uint8) override;
    uint8 GetPortInputValue(int port) override;
    EmSPISlave* GetSPI2Slave() override;
    EmSPISlave* GetSPI1Slave() override { return &cardBus; }
    uint16 DoExchange(uint16 control, uint16 data) override;
    bool CopyLCDFrame(Frame& frame, bool fullRefresh) override;
    Bool GetLCDScreenOn() override;
    Bool GetLCDBacklightOn() override;
    uint8 GetPortInternalValue(int port) override;
    void GetKeyInfo(int* rows, int* cols, uint16* map, Bool* active) override;
};

#endif
