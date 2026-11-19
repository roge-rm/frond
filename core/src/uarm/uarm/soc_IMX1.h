#ifndef FROND_SOC_IMX1_H
#define FROND_SOC_IMX1_H

#include <atomic>
#include <functional>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "SoC.h"
#include "UsbDevicePort.h"
#include "UsbHostHooks.h"
#include "guest_orientation.h"
#include "imx1_boot_probe.h"
#include "imx1_i2c_read_window.h"
#include "pace_patch.h"
#include "zodiac_audio.h"
#include "zodiac_input.h"
#include "zodiac_panel.h"

// The Tapwave Zodiac 2 in the normal runtime. It runs the guarded i.MX1 machine (imx1_boot_probe.h)
// and nothing else: no PACE or guest calls, and input reaches the ROM as pen samples, pin levels and
// stick values. One per process, as the machine is.

// The machine's options. Every button is released from reset: the pins read high, A7 to A10 and
// D7 to D10 and D12 to D14. The other levels are the bring-up's. A saved state only loads into a
// machine made with these, or with imx1StateAllowBootInputs when only the button levels differ.
Imx1ProbeOptions zodiacMachineOptions(Imx1I2cBus* eeprom);

// How long a hard reset holds power, in ms of guest time. ROM facts: right after reset the boot ROM (20003948)
// makes A7, A10 and D12 inputs, waits, then reads Home (A7), power (A10) and Function (D12) in that order, low
// for pressed; power alone picks boot mode 1. The DAL reads them again at 20085b34 a little later, and once
// power is let go shows "Erase all data?", Function for yes and Home for no. 500 ms was enough on the 1.1 ROM.
inline constexpr uint32_t zodiacHardResetHoldMs = 2000;

// The ROM's address in the guest (file offset 0 there).
inline constexpr uint32_t zodiacRomBase = 0x20000000;

// Where an injected call returns to: never code, as the run stops before fetching it.
inline constexpr uint32_t zodiacInjectedReturn = 0xfffffff0;

class SocIMX1 : public SoC {
   public:
    // nullptr with the reason in error when the ROM can't run.
    static SocIMX1* Create(const uint8_t* rom, size_t size, std::string& error);

    void Reset() override;
    // A reset through RESET_IN with power held from it on for zodiacHardResetHoldMs of guest time.
    void HardReset() override { HardResetHolding(zodiacHardResetHoldMs); }
    void HardResetHolding(uint32_t holdMs);
    // Whether the hard reset's power hold is still to start, for tests.
    bool HardResetHoldPending() const { return input.jamPending(); }
    uint64_t Run(uint64_t maxCycles, uint64_t cyclesPerSecond) override;
    // Faster: FCLK times the multiplier in the machine's clock model; guest time still comes from its own clock.
    void SetClockMultiplier(uint32_t num, uint32_t den) override;
    // Guest calls through the ROM PACE (paceDiscoverZodiac), the ROM unchanged. RunToPaceSyscall runs until the
    // 68k code calls trap and leaves the machine on the syscall site's bl, with r0 the 68k state and r1 the trap.
    // The machine then stops there again after every injected call, until the next Run lets it go on. While a
    // call is pending the machine runs step by step; with none, Exact and Fast run as before.
    bool RunToPaceSyscall(uint16_t trap, uint64_t maxCycles, uint64_t cyclesPerSecond) override;
    // Runs the call set up in the registers (pc the entry, lr zodiacInjectedReturn) until it returns there.
    // With a TRAP #0 hook set it also stops on the ROM PACE's TRAP #0 frame and calls the hook there.
    bool ExecuteInjected(uint64_t maxCycles, uint64_t cyclesPerSecond) override;
    // Called with the machine on PaceSites().trap0Site (r0 the 68k state) during an injected call. True once it
    // has run the trap and moved pc past the bl (SkipTrap0Callout); false lets the ROM run it. nullptr for none.
    void SetTrap0Hook(std::function<bool()> hook) { trap0Hook = std::move(hook); }
    bool SkipTrap0Callout() { return SetGuestRegister(15, paceSites.trap0Site + 4); }
    // Wakes the UI task of an idle 68k app, as a nilEvent would: see WakeIdle in soc_IMX1.cpp.
    bool WakeIdle();
    const ZodiacPaceSites& PaceSites() const { return paceSites; }
    bool GuestCallPending() const { return guestCall; }
    bool GuestRegisters(uint32_t (&registers)[16], uint32_t* cpsr = nullptr) const {
        return imx1MachineRegisters(machine, registers, cpsr);
    }
    bool SetGuestRegister(unsigned reg, uint32_t value) { return imx1MachineSetRegister(machine, reg, value); }
    bool ReadGuest(uint32_t address, void* data, uint32_t size) {
        return imx1MachineAccessMemory(machine, address, data, size, false);
    }
    bool WriteGuest(uint32_t address, const void* data, uint32_t size) {
        return imx1MachineAccessMemory(machine, address, const_cast<void*>(data), size, true);
    }
    void AnalogStick(float x, float y) override;
    void SetStickHostTurns(int turns) override;
    bool Save() override;
    bool Load(size_t savestateSize, void* savestateData) override;
    // A state file from the bring-up driver (gzip or plain), made with its boot pin levels.
    bool LoadBringUpState(const uint8_t* data, size_t size);
    Buffer GetRomData() override { return {.size = rom.size(), .data = rom.data()}; }
    Buffer GetSavestate() override { return {.size = saved.size(), .data = saved.data()}; }
    void DumpMMU() override {}
    void JamKey(enum KeyId key, uint32_t durationMsec) override;
    // Guest calls go through the ROM PACE, found at create.
    bool PrepareM68kSupport() override { return paceSites.syscallSite && paceSites.trap0Site; }
    bool GuestServicesSupported() const override { return paceSites.syscallSite && paceSites.trap0Site; }
    bool HardResetSupported() const override { return true; }
    uint64_t GetTime() override;
    uint32_t* GetPendingFrame(uint32_t& firstDirtyLine, uint32_t& lastDirtyLine) override;
    void ResetPendingFrame() override { framePending = false; }
    bool FrameQuarterTurn() const override { return true; }
    unsigned GuestOrientation() override;
    Attention GuestAttention() override;
    DeviceType5 GetDeviceType() override { return deviceTypeZodiac2; }
    DisplayMode GetDisplayMode() override { return DisplayMode::mode_480x320; }
    void SuspendTimerInterrupts(bool) override {}
    bool LcdEnabled() override;
    // The battery level reaches the guest through the ASP's U input, set again after every load and reset.
    bool BatterySupported() const override { return true; }
    bool SetBattery(const BatteryPower& value) override;
    // The battery inputs the machine has now, for tests: the ASP U word and GPIO B14 (low on the charger).
    uint16_t BatterySample() const { return imx1MachineAspUInput(machine); }
    bool ChargerInputLow() const { return !(imx1MachineGpioInput(machine, 1) & (1u << 14)); }
    bool HeadphoneInputHigh() const { return imx1MachineGpioInput(machine, 1) & (1u << 16); }
    bool CradleInputLow() const { return !(imx1MachineGpioInput(machine, 3) & (1u << 22)); }
    // Headphones on GPIO B16, high while they are in (the DAL's 20097d40 then mutes the speaker path).
    bool HeadphonesSupported() const override { return true; }
    bool HeadphonesIn() const override { return headphones; }
    bool SetHeadphones(bool in) override;
    // The cradle's HotSync button on GPIO D22, low while held.
    bool CradleButtonSupported() const override { return true; }
    bool CradleButtonDown() const override { return cradleDown; }
    bool SetCradleButton(bool down) override;
    // The cradle: A11 (attach detect) and A1 high and the accessory ID line at the bottom of its ADC range.
    // It's in while the user put it in or the USB bridge's cable is in; each takes out only its own.
    bool CradleSupported() const override { return true; }
    bool CradleAttached() const override { return cradle || usbCable; }
    bool SetCradle(bool attached) override;
    // The USB bridge's cable, which comes in through the cradle.
    bool UsbCableAttached() const { return usbCable; }
    bool SetUsbCable(bool attached);
    // The accessory inputs the machine has now, for tests: A11 and A1 high and the aux ADC sample.
    bool AccessoryPinsHigh() const { return (imx1MachineGpioInput(machine, 0) & 0x802) == 0x802; }
    uint32_t AccessorySample() const { return imx1MachineGraphicsAux(machine); }
    // Holds D22 for 500 ms of guest time. The awake ROM only takes it with the cradle attached.
    bool PressHotSync() override;
    // Asleep is the i.MX1 in stop mode; a wake is leaving it, as Run sees it.
    bool PowerSleeping() const override { return imx1MachineSystemStopped(machine); }
    uint64_t PowerWakes() const override { return wakes; }
    // UART1 is the connector's serial port, UART2 the infrared port (ROM uMX1 driver, IREN on unit 1
    // only). With no peer both see RTS inactive.
    HostUartPort* SerialHostPort() override;
    HostInfraredPort* InfraredHostPort() override;
    // The W4200 USB function as a UsbDevicePort, for UsbHostBridge. The bus comes through the cradle, so the
    // bridge's cable puts the cradle in (SetUsbCable). Hooks tick between run slices on GetTime's clock; while
    // the host has work due, an idle guest's clock runs on to it instead of waiting.
    UsbDevicePort GetUsbPort();
    void SetUsbHostHooks(const UsbHostHooks* hooks) { usbHostHooks = hooks ? *hooks : UsbHostHooks{}; }
    bool UsbHostIoPaused() const { return halted || PowerSleeping(); }
    ~SocIMX1();

    // The engine, Exact (the full step with the cache models) or Fast (the lean path in quiet spans). It
    // changes between Run calls only, so always at a slice boundary, and stays through Load and Reset.
    // False on a stopped machine.
    bool SetFastEngine(bool fast) { return imx1MachineSetFastEngine(machine, fast); }
    bool FastEngine() const { return imx1MachineFastEngine(machine); }
    // Instructions retired and idle clocks skipped so far, for speed readouts.
    Imx1MachineCheapStatus Status() const { return imx1MachineGetStatus(machine); }
    bool Halted() const { return halted; }
    // Instructions the Fast engine ran in Dynarmic blocks so far.
    uint64_t JitRetired() const { return imx1MachineJitRetired(machine); }
    uint64_t JitRuns() const { return imx1MachineJitRuns(machine); }
    uint64_t DelaySkips() const { return imx1MachineDelaySkips(machine); }
    uint64_t DelayHaltSkips() const { return imx1MachineDelayHaltSkips(machine); }
    uint64_t YmfReadyReads() const { return imx1MachineYmfReadyReads(machine); }
    // The guest time of the last of them in ns, on GetTime's clock, for tests.
    uint64_t YmfLastReadyReadNs() const;
    // The RAM and graphics memory hashed, for comparing where two runs end.
    uint64_t MemoryHash() const { return imx1MachineMemoryHash(machine); }
    uint64_t PcmUnderrunWords() const { return imx1MachinePcmUnderrunWords(machine); }
    // The sound and graphics chips' registers as text (imx1MachineChipSummary).
    std::string ChipSummary() const { return imx1MachineChipSummary(machine); }
    // Run stops after about 12 ms of host time even short of the time asked for, so the app keeps
    // up. Off, a run always reaches its guest time and runs repeat however busy the host is (tools).
    void SetHostTimeLimit(bool on) { hostTimeLimit = on; }
    // Whether a redraw of the frame the machine already has is still owed.
    bool RedrawPending() const { return redraw; }
    // The sound path, for tests.
    ZodiacPcmSink& PcmSink() { return pcm; }
    // The last load found another card in the card store than the one saved in the slot, and left
    // it out. The session start puts it in as a new card.
    bool CardSwappedOnLoad() const { return cardSwapped; }
    // Whether slot 1 has the card attached, for tests.
    bool CardAttached() const { return imx1MachineMmcsdCard(machine) != nullptr; }
    // Slot 2, the W4200's SD slot, holds sdCardStore(1)'s card (Sd2Insert). Whether the machine has it
    // attached, for tests.
    bool SecondCardAttached() const { return imx1MachineGraphicsSdCard(machine) != nullptr; }
    bool HasSecondSdSlot() const override { return true; }
    bool Sd2Insert() override;
    void Sd2Eject() override;
    bool Sd2Inserted() const override { return card2In; }
    // Whether the machine has the pen down just now.
    bool PenContact() const { return imx1MachinePenInputs(machine).contactDown; }
    // Why a halted machine stopped, in one line. Empty while it runs.
    std::string StopText() const;

   protected:
    void OnSetFramebufferDirty() override { redraw = true; }
    bool OnSleep() override { return false; }
    void OnWakeup() override {}
    void OnSetAudioQueue(struct AudioQueue* queue) override { pcm.queue = queue; }
    void OnSetPcmOutputEnabled() override { pcm.enabled = pcmEnabled; }
    void OnSetPcmSuspended() override { pcm.suspended = pcmSuspended; }
    void OnSdInsert() override;
    void OnSdRemount() override;
    void OnSdEject() override;

   private:
    SocIMX1() = default;
    void PumpInput();
    void StateChanged();
    // Runs until the pc stop or for ticks of guest time. True on the stop.
    bool RunToStop(uint64_t ticks);
    void EndGuestCall();
    ZodiacPaceSites paceSites{};
    bool guestCall{false};
    std::function<bool()> trap0Hook;
    // Puts the battery, headphone, cradle and cradle button settings on the machine's inputs.
    bool ApplyBattery();
    bool ApplyHeadphones();
    bool ApplyCradle();
    bool SetCradleInputs(bool& setting, bool attached);
    bool headphones{false}, cradleDown{false}, cradle{false}, usbCable{false};
    // CLK32 ticks until a PressHotSync lets go of D22, 0 with none going.
    uint64_t hotSyncTicks{0};
    UsbHostHooks usbHostHooks{};
    void TickUsbHost();
    bool UsbHostEventPending();
    bool wasStopped{false};
    uint64_t wakes{0};
    struct SerialPort;
    struct InfraredPort;
    std::unique_ptr<SerialPort> serialPort;
    std::unique_ptr<InfraredPort> infraredPort;
    // Puts the UARTs' RTS inputs and host peers back on the machine after it was made, loaded or reset.
    void ApplyUartLines();
    bool LoadState(const uint8_t* data, size_t size, uint32_t flags);
    // Puts the card in slot 1 or takes it out to match cardIn, card detect pin included. A change the
    // machine refuses just now is tried again on the next Run.
    void SyncCard();
    // The same for slot 2, card2In and vsd2.
    void SyncSecondCard();

    std::vector<uint8_t> rom, saved, powerOn;
    // Slot 2's card on sdCardStore(1), made at its first insert.
    VSD* vsd2{nullptr};
    bool card2In{false}, card2Sync{false};
    std::unique_ptr<Imx1I2cReadWindow> eeprom;
    Imx1Machine* machine{nullptr};
    bool halted{false};
    bool hostTimeLimit{true};

    // The panel frame handed to the frontend, a copy, so the machine's frame is acknowledged at
    // once. The frontend turns it on the GPU.
    std::vector<uint32_t> view = std::vector<uint32_t>(size_t(zodiacPanelWidth) * zodiacPanelHeight);
    bool framePending{false}, redraw{true};
    GuestOrientationTracker orientationTracker;
    // The stick as the host pushed it, and the quarter turns the guest's screen has: the guest gets the
    // stick turned to match what the player sees.
    std::atomic<float> stickHostX{0}, stickHostY{0};
    std::atomic<int> stickTurns{0}, stickHostTurns{0};
    void PushStick();

    // Guest time in CLK32 ticks (32768 a second) that Run still owes the main loop, as cycles.
    uint64_t creditRemainder{0};

    ZodiacInput input;

    // The sound out, pushed to the host's queue while PCM output is on and not suspended.
    ZodiacPcmSink pcm;

    // The SD card in slot 1 over the shared card store. The ROM speaks SD 1.x only, so cards stay at 2 GB.
    struct VSD* vsd{nullptr};
    bool cardIn{false}, cardSync{false}, cardSwapped{false};
};

#endif
