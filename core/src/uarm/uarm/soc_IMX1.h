#ifndef FROND_SOC_IMX1_H
#define FROND_SOC_IMX1_H

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "SoC.h"
#include "guest_orientation.h"
#include "imx1_boot_probe.h"
#include "imx1_i2c_read_window.h"
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

class SocIMX1 : public SoC {
   public:
    // nullptr with the reason in error when the ROM can't run.
    static SocIMX1* Create(const uint8_t* rom, size_t size, std::string& error);

    void Reset() override;
    uint64_t Run(uint64_t maxCycles, uint64_t cyclesPerSecond) override;
    bool RunToPaceSyscall(uint16_t, uint64_t, uint64_t) override { return false; }
    bool ExecuteInjected(uint64_t, uint64_t) override { return false; }
    void AnalogStick(float x, float y) override;
    bool Save() override;
    bool Load(size_t savestateSize, void* savestateData) override;
    // A state file from the bring-up driver (gzip or plain), made with its boot pin levels.
    bool LoadBringUpState(const uint8_t* data, size_t size);
    Buffer GetRomData() override { return {.size = rom.size(), .data = rom.data()}; }
    Buffer GetSavestate() override { return {.size = saved.size(), .data = saved.data()}; }
    void DumpMMU() override {}
    void JamKey(enum KeyId key, uint32_t durationMsec) override;
    bool PrepareM68kSupport() override { return false; }
    bool GuestServicesSupported() const override { return false; }
    bool HardResetSupported() const override { return false; }
    uint64_t GetTime() override;
    uint32_t* GetPendingFrame(uint32_t& firstDirtyLine, uint32_t& lastDirtyLine) override;
    void ResetPendingFrame() override { framePending = false; }
    bool FrameQuarterTurn() const override { return true; }
    unsigned GuestOrientation() override;
    DeviceType5 GetDeviceType() override { return deviceTypeZodiac2; }
    DisplayMode GetDisplayMode() override { return DisplayMode::mode_480x320; }
    void SuspendTimerInterrupts(bool) override {}
    bool LcdEnabled() override;

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
    bool LoadState(const uint8_t* data, size_t size, uint32_t flags);
    // Puts the card in slot 1 or takes it out to match cardIn, card detect pin included. A change the
    // machine refuses just now is tried again on the next Run.
    void SyncCard();

    std::vector<uint8_t> rom, saved, powerOn;
    std::unique_ptr<Imx1I2cReadWindow> eeprom;
    Imx1Machine* machine{nullptr};
    bool halted{false};
    bool hostTimeLimit{true};

    // The panel frame handed to the frontend, a copy, so the machine's frame is acknowledged at
    // once. The frontend turns it on the GPU.
    std::vector<uint32_t> view = std::vector<uint32_t>(size_t(zodiacPanelWidth) * zodiacPanelHeight);
    bool framePending{false}, redraw{true};
    GuestOrientationTracker orientationTracker;

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
