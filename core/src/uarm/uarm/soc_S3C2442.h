#ifndef FROND_SOC_S3C2442_H
#define FROND_SOC_S3C2442_H

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "SoC.h"
#include "UsbDevicePort.h"
#include "UsbHostHooks.h"
#include "net/WifiRouter.h"
#include "net/DesktopLink.h"
#include "s3c2442_machine.h"
#include "s3c2442_pen.h"
#include "zodiac_audio.h"

// The HP iPAQ rx1950 in the normal runtime: the S3C2442 machine (s3c2442_machine.h) with the rx1950's
// board (rx1950_board.h, rx1950_power.h). Windows Mobile, so no PACE or guest calls. The bootloader
// (CEBL.nbf) is the session's NOR; the 64 MB NAND, which the loader and OS run from, is a uARM NAND, so
// sessions keep it as their NAND file. One per process, as the CPU is.

// HP's firmware (rx1950Cebl5 and rx1950Ceos5) by size and CRC-32, as the core has no SHA-256. CEBL.nbf is required, CEOS.nbf only recognised;
// the desktop checks the SHA-256 at import.
bool rx1950IsCebl(const uint8_t* data, size_t size);
bool rx1950IsCeos(const uint8_t* data, size_t size);

class SocS3C2442 : public SoC {
   public:
    // cebl is CEBL.nbf. flash is the whole NAND, data then spare per page (rx1950NandImage), taken over
    // and given back with delete[]; null when a session's NAND file is attached after (frondNand::attachFile).
    // battery sets the AC and charge pins before the machine starts, so a start on the charger raises no EINT2.
    // anyFirmware accepts any CEBL that fits the reserved blocks, not only HP's.
    // nullptr with the reason in error when it can't run.
    static SocS3C2442* Create(const uint8_t* cebl, size_t size, uint8_t* flash, size_t flashSize, std::string& error,
                              const BatteryPower& battery = {}, bool anyFirmware = false);
    bool AnyFirmware() const override { return anyFirmware; }

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
    // A clean boot: the user store's NAND blocks erased (from the first block rx1950UserStoreBlockFromMbr finds), then a power-on reset with
    // SDRAM cleared. WM5 formats the empty store and starts as from a new import (welcome, alignment, setup).
    void HardReset() override;
    bool HardResetSupported() const override { return true; }
    bool InstallsToCard() const override { return true; }
    uint64_t GetTime() override { return machine->nanoseconds(); }
    uint32_t* GetPendingFrame(uint32_t& firstDirtyLine, uint32_t& lastDirtyLine) override;
    void ResetPendingFrame() override { lcdView.reset(); }
    Attention GuestAttention() override;
    DeviceType5 GetDeviceType() override { return deviceTypeRx1950; }
    DisplayMode GetDisplayMode() override { return DisplayMode::mode_240x320; }
    void SuspendTimerInterrupts(bool) override {}
    bool LcdEnabled() override;
    // The battery reaches the guest through AIN0 and the AC and charge pins, set again after every load and reset.
    bool BatterySupported() const override { return true; }
    bool SetBattery(const BatteryPower& value) override;
    // The RTC on by the time a session was closed (rx1950RtcForward).
    void ForwardClock(uint64_t seconds) override;
    // Headphones on GPG12 (EINT20), low while they're in; put back after every load and reset like the battery.
    bool HeadphonesSupported() const override { return true; }
    bool HeadphonesIn() const override { return headphones; }
    bool SetHeadphones(bool in) override;
    bool SetPcmCapture(const std::string& path) override;
    std::string PcmCaptureSummary() const override;
    bool PowerSleeping() const override { return machine->asleep(); }
    // The microphone feeds the UDA1380's ADC. A sleep, reset or load disarms the source (a fresh load stays
    // disarmed) and the host re-arms capture from MicrophoneDemand. An InputCheckpoint fences it meanwhile.
    AudioInputDemand MicrophoneDemand() override { return machine->microphoneDemand(); }
    void SetMicrophoneSource(const AudioInputSource* source) override;
    std::function<void()> CapturePeripheralInput() override;
    void SetSdWriteProtect(bool on) override;
    // A load found another card (or none) in the store than the one the state had in: the slot is empty and the
    // card in the store goes in later as a new one.
    bool CardSwappedOnLoad() const { return cardSwapped; }

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

    // The sync connector's serial port (UART0, rx1950SerialUart) and the infrared port (UART2 in IrDA mode)
    // for HostSerial and HostInfrared. A host on the serial port is a serial cable in: the cable sense (GPF6)
    // goes high while one is attached. Bytes from the host reach the UART at its bit rate, on guest time.
    HostUartPort* SerialHostPort() override;
    HostInfraredPort* InfraredHostPort() override;
    void BeginSerialCheckpoint() override { ++serialCheckpoints; }
    void EndSerialCheckpoint() override { if (serialCheckpoints) --serialCheckpoints; }
    // The USB device controller as a UsbDevicePort, for UsbHostBridge. The bridge's cable is VBUS on GPG5.
    // Hooks tick on GetTime's clock at their wake times, in the run loop (HostService).
    UsbDevicePort GetUsbPort();
    void SetUsbHostHooks(const UsbHostHooks* hooks) { usbHostHooks = hooks ? *hooks : UsbHostHooks{}; }
    bool SetUsbCable(bool attached);
    std::shared_ptr<frondSync::ActiveSyncSession> StartActiveSync(bool listApplications = false);
    bool ActiveSyncRunning() const;
    void StopActiveSync();
    bool StartSerialActiveSync();
    bool StopSerialActiveSync();
    bool UsbCableAttached() const { return machine->usbVbus(); }
    // The earliest guest time the cable may go in again after it went out (0: any time).
    uint64_t UsbPlugReadyAt() const;
    bool UsbHostIoPaused() const { return halted || serialCheckpoints || PowerSleeping(); }
    // The desktop behind the USB cable's RNDIS link (UsbHostBridge's ethernet hooks, a DesktopLink): made at the
    // first poll, dropped at a reset or load, left alone while the network is frozen for a checkpoint.
    void UsbEthernetOut(const uint8_t* frame, size_t size, uint64_t now);
    void UsbEthernetPoll(uint64_t now, const uint8_t* mac, uint32_t link);
    bool UsbEthernetIn(std::vector<uint8_t>& frame);

   protected:
    void OnSetFramebufferDirty() override { redraw = true; }
    bool OnSleep() override { return false; }
    void OnWakeup() override {}
    void OnSetAudioQueue(struct AudioQueue*) override {}
    void OnSetPcmOutputEnabled() override {}
    void OnSetPcmSuspended() override {}
    void OnSdInsert() override;
    void OnSdRemount() override {}
    void OnSdEject() override;

   private:
    SocS3C2442() = default;
    // The loader's work at power on and at a wake: eboot into SDRAM, the clocks and the pins it sets.
    void BootLoader();
    void PumpInput();
    // After a load or reset (a watchdog reset too, from its hook): host input up, sound and frames again, and the
    // host side (RestartHost).
    void StateChanged();
    struct NandPages;

    std::vector<uint8_t> rom, saved;
    bool anyFirmware = false;
    std::unique_ptr<S3c2442Machine> machine;
    std::unique_ptr<NandPages> pages;
    MemoryBuffer nandPageBuffer{};
    // Disarms now, or when the last checkpoint fence ends.
    void DisarmMicrophone();
    unsigned checkpointFenceDepth{0};
    bool microphoneInvalidatePending{false}, wasAsleep{false};
    bool halted{false}, hostTimeLimit{true}, inputGuestTime{false};
    // Keys held through JamKey, released when the guest's time reaches untilNs.
    struct Jam { KeyId key; uint64_t untilNs; };
    std::vector<Jam> jams;
    S3cPenPacer pen;
    S3cKeyPacer keys;
    // The frame handed out, and its dirty rows since the last ResetPendingFrame.
    S3cLcdView lcdView;
    bool redraw{true};
    uint64_t scannedNs{0};  // guest time of the last LCD scan, and whether the LCD was on then
    bool scannedOn{false};
    uint64_t frameCallNs{~uint64_t(0)};  // guest time of the last GetPendingFrame call
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
    // The battery, headphones and SD slot on the board's pins, after a start, load or reset.
    void ApplyHostInputs();
    void ApplyCardPins();
    // After a load: the saved card carries on when the store has it, or the slot is left empty.
    void LoadCard();
    struct VSD* vsd{nullptr};
    bool sdProtect{false}, cardSwapped{false};
    // Frond's host network behind the emulated access point (machine->accessPoint): made on the first frame out,
    // polled on guest time in HostService, dropped at a reset or load. Left alone while the network is frozen for a
    // checkpoint.
    std::unique_ptr<frondNet::WifiRouter> router;
    std::unique_ptr<frondNet::DesktopLink> usbDesktop;
    std::shared_ptr<frondSync::ActiveSyncSession> activeSync;
    uint32_t usbDesktopLink{0};  // the RNDIS link number usbDesktop was made for
    void PollNetwork();

    struct SerialPort;
    struct SerialSync;
    struct InfraredPort;
    std::unique_ptr<SerialPort> serialPort;
    std::unique_ptr<SerialSync> serialSync;
    std::unique_ptr<InfraredPort> infraredPort;
    unsigned serialCheckpoints{0};
    // Host bytes into UART0 and UART2 at their bit rates, the USB host's ticks and Wi-Fi frames from the router, on
    // guest time: the machine calls HostService at machine->hostNextNs, which ScheduleHost sets, asleep too. Each
    // UART's next look is machine->hostUartNextNs (UINT64_MAX with no host), the router's machine->hostNetNextNs. RestartHost starts the host side again after a
    // load or reset.
    void HostService();
    void ScheduleHost(uint64_t late);
    void RestartHost();
    bool UartHasHost(unsigned channel) const;
    void PumpUart(unsigned channel, uint64_t t);
    // A load or reset made a new board: the host peers start again; the cable sense follows the serial host.
    void ApplyUartLines();
    void UartSent(unsigned channel, uint8_t byte);
    UsbHostHooks usbHostHooks{};
    void (*usbNotify)(void*){nullptr};
    void* usbNotifyContext{nullptr};
    void TickUsbHost(uint64_t t);
};

#endif
