#ifndef FROND_SOC_S3C2442_H
#define FROND_SOC_S3C2442_H

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "SoC.h"
#include "s3c2442_machine.h"
#include "s3c2442_pen.h"
#include "zodiac_audio.h"

// The HP iPAQ rx1950 in the normal runtime: the S3C2442 machine (s3c2442_machine.h) with the rx1950's
// board (rx1950_board.h, rx1950_power.h). Windows Mobile, so no PACE or guest calls. The bootloader
// (CEBL.nbf) is the session's NOR; the 64 MB NAND, which the loader and OS run from, is a uARM NAND, so
// sessions keep it as their NAND file. One per process, as the CPU is.

// The firmware Frond runs (rx1950Cebl5 and rx1950Ceos5) by size and CRC-32, as the core has no SHA-256;
// the desktop checks the SHA-256 at import.
bool rx1950IsCebl(const uint8_t* data, size_t size);
bool rx1950IsCeos(const uint8_t* data, size_t size);

class SocS3C2442 : public SoC {
   public:
    // cebl is CEBL.nbf. flash is the whole NAND, data then spare per page (rx1950NandImage), taken over
    // and given back with delete[]; null when a session's NAND file is attached after (frondNand::attachFile).
    // battery sets the AC and charge pins before the machine starts, so a start on the charger raises no EINT2.
    // nullptr with the reason in error when it can't run.
    static SocS3C2442* Create(const uint8_t* cebl, size_t size, uint8_t* flash, size_t flashSize, std::string& error,
                              const BatteryPower& battery = {});

    void Reset() override;
    uint64_t Run(uint64_t maxCycles, uint64_t cyclesPerSecond) override;
    void SetClockMultiplier(uint32_t num, uint32_t den) override;
    uint64_t CoreHz() const override;
    bool RunToPaceSyscall(uint16_t, uint64_t, uint64_t) override { return false; }
    bool ExecuteInjected(uint64_t, uint64_t) override { return false; }
    bool Save() override;
    bool Load(size_t savestateSize, void* savestateData) override;
    Buffer GetRomData() override { return {.size = rom.size(), .data = rom.data()}; }
    Buffer GetSavestate() override { return {.size = saved.size(), .data = saved.data()}; }
    void DumpMMU() override {}
    void JamKey(enum KeyId key, uint32_t durationMsec) override;
    bool PrepareM68kSupport() override { return false; }
    bool GuestServicesSupported() const override { return false; }
    bool HardResetSupported() const override { return false; }
    uint64_t GetTime() override { return machine->nanoseconds(); }
    uint32_t* GetPendingFrame(uint32_t& firstDirtyLine, uint32_t& lastDirtyLine) override;
    void ResetPendingFrame() override { framePending = false; }
    Attention GuestAttention() override;
    DeviceType5 GetDeviceType() override { return deviceTypeRx1950; }
    DisplayMode GetDisplayMode() override { return DisplayMode::mode_240x320; }
    void SuspendTimerInterrupts(bool) override {}
    bool LcdEnabled() override;
    // The battery reaches the guest through AIN0 and the AC and charge pins, set again after every load and reset.
    bool BatterySupported() const override { return true; }
    bool SetBattery(const BatteryPower& value) override;
    // Headphones on GPG12 (EINT20), low while they're in; put back after every load and reset like the battery.
    bool HeadphonesSupported() const override { return true; }
    bool HeadphonesIn() const override { return headphones; }
    bool SetHeadphones(bool in) override;
    bool SetPcmCapture(const std::string& path) override;
    std::string PcmCaptureSummary() const override;
    bool PowerSleeping() const override { return machine->asleep(); }
    // The microphone feeds the UDA1380's ADC. A sleep, reset or load leaves the source attached; the host
    // re-arms capture from MicrophoneDemand.
    AudioInputDemand MicrophoneDemand() override { return machine->microphoneDemand(); }
    void SetMicrophoneSource(const AudioInputSource* source) override { machine->microphone = source; }

    // Exact (the interpreter with the cache models) or Fast (the JIT and lean path). Between Run calls only.
    bool SetFastEngine(bool fast) { return machine->setFastEngine(fast); }
    bool FastEngine() const { return machine->fastEngine(); }
    // Run stops after about 12 ms of host time even short of the time asked for, so the app keeps up.
    void SetHostTimeLimit(bool on) { hostTimeLimit = on; }
    // How long the host held the pen or a key is taken from the guest's clock (emulated time, where replays
    // run in guest time) instead of the host's.
    void SetInputGuestTime(bool on) { inputGuestTime = on; }
    bool Halted() const { return halted; }
    std::string StopText() const { return halted ? machine->stopText() : std::string(); }
    S3c2442Machine& Machine() { return *machine; }

   protected:
    void OnSetFramebufferDirty() override { redraw = true; }
    bool OnSleep() override { return false; }
    void OnWakeup() override {}
    void OnSetAudioQueue(struct AudioQueue*) override {}
    void OnSetPcmOutputEnabled() override {}
    void OnSetPcmSuspended() override {}
    void OnSdInsert() override {}
    void OnSdRemount() override {}
    void OnSdEject() override {}

   private:
    SocS3C2442() = default;
    // The loader's work at power on and at a wake: eboot into SDRAM, the clocks and the pins it sets.
    void BootLoader();
    void PumpInput();
    void StateChanged();
    struct NandPages;

    std::vector<uint8_t> rom, saved;
    std::unique_ptr<S3c2442Machine> machine;
    std::unique_ptr<NandPages> pages;
    MemoryBuffer nandPageBuffer{};
    bool halted{false}, hostTimeLimit{true}, inputGuestTime{false};
    // Keys held through JamKey, released when the guest's time reaches untilNs.
    struct Jam { KeyId key; uint64_t untilNs; };
    std::vector<Jam> jams;
    S3cPenPacer pen;
    S3cKeyPacer keys;
    std::vector<uint32_t> view, scan;
    bool framePending{false}, redraw{true};
    // Guest ns Run still owes the main loop, as cycles.
    uint64_t creditRemainder{0};
    bool headphones{false};
    // The IIS frames as the speaker or headphones play them, to the host's 44100 Hz. Frames are made while
    // PCM output is off or suspended too, and only the push to the queue is skipped, so the stream is the same
    // either way.
    ZodiacAudioOutput sound;
    static void PushFrame(void* context, uint32_t frame);
    // pcm-capture: host frames as they're made, with counts for the summary.
    FILE* capture{nullptr};
    uint64_t captureFrames{0}, captureLoud{0};
    unsigned capturePeak{0}, captureRawPeak{0};
    uint32_t captureGainPeak{0};
    // The battery and headphones on the board's pins, after a start, load or reset.
    void ApplyHostInputs();
};

#endif
