#ifndef _SOC_PXA_H_
#define _SOC_PXA_H_

#include <cstdint>
#include <vector>

#include "CardBacking.h"
#include "CPU.h"
#include "ClieGamepad.h"
#include "IrKeyboard.h"
#include <string>
#include "fast_audio.h"
#include "device_type5.h"
#include "device_profile5.h"
#include "memory_buffer.h"
#include "SoC.h"
#include "soc_generic.h"
#include "pxa_RTC.h"

struct AudioInputSource;
struct CameraSource;
struct AudioInputDemand;
struct PxaUart;
#include "SerialHostRoute.h"
using IrHostHooks = SerialHostHooks;

struct PxaSsp;
struct PxaGpio;
struct PxaAC97;
struct PxaDma;
struct MemoryStickController;
struct PxaI2s;
struct PxaI2c;
struct PxaIc;
struct PxaMemCtrlr;
struct PxaPwrClk;
struct PxaI2c;
struct PxaPwm;
struct PxaTimr;
struct PxaMmc;
class PalmSdioWifi;
class Pxa270Ohci;
namespace frondNet { class WifiRouter; }
struct PxaRtc;
enum class SpitzCfNetwork { off, ethernet, wifi };
struct SpitzPowerState {
    unsigned batteryPercent;
    bool acConnected, batteryDoorLocked, chargeEnabled, chargeLed, diskLed;
};
struct PxaLcd;
struct Pxa270wmmx;
struct Pxa270Udc;
#include "UsbDevicePort.h"

#include "UsbHostHooks.h"
struct PxaImc;
struct PxaKpc;
struct Pxa255dsp;
struct Pxa255Udc;
struct Device;
struct Keypad;
struct VSD;

class SocPXA : public SocGeneric<SocPXA> {
    friend SocGeneric<SocPXA>;
    bool disableSleep{false};
    bool spitzDirectBoot{false};
    bool spitzUsbCableAttached{false};
    std::unique_ptr<Pxa270Ohci> spitzOhci;
    void RefreshSpitzUsbHost();
    std::unique_ptr<frondNet::WifiRouter> spitzCfRouter;
    uint64_t spitzCfPollAt{}, spitzCfTx{}, spitzCfRx{};
    uint8_t spitzRegulatorPhase{0};
    uint8_t spitzRegulatorVoltage{0};
    void ConfigureSpitzMemoryAndClocks();
    void BootSpitzKernel(bool rtcReset);
    std::vector<uint8_t> e3WifiRom;
    bool e3WifiCompatible = false;

   public:
    ~SocPXA();
    void SetAudioOutputFenced(bool) override;
    MemoryStickController* GetMemoryStick();
    void RestoreFastAudioRoute();
    FastAudio::EarlyDiagnostics AudioDiagnostics() const { return fastAudio?fastAudio->EarlyStatus():FastAudio::EarlyDiagnostics{}; }
    bool AudioOutputFenced() const { return fastAudio && fastAudio->OutputFenced(); }
    FastAudio::HostState AudioHostState() const { return fastAudio?fastAudio->CaptureHostState():FastAudio::HostState{}; }
    static constexpr int MEMORY_SYSTEM_KIND = ARM_MEMORY_SYSTEM_MMU;

   public:
    SocPXA(enum DeviceType5 deviceType, uint32_t ramSize, void *romData, const uint32_t romSize,
           uint8_t *nandContent, size_t nandSize, int gdbPort, uint_fast8_t socRev,
           bool deferSpitzBoot = false);

    bool MountMicrodrive(const char* path, bool writable, const char* mediaId = nullptr);
    bool FlushMicrodrive() const;
    // The guest is paused while the machine save journals and applies these pages.
    bool ForEachMicrodriveChange(const CardBacking::OverlayFn& fn) const;
    bool WriteMicrodriveBase(uint64_t offset, const void* data, size_t count);
    bool ResetMicrodriveOverlay();
    // The external CF socket uses the card in sdCardStore(1).
    bool InsertCompactFlash(bool writeProtected);
    void EjectCompactFlash();
    bool CompactFlashInserted() const;
    void SetCompactFlashWriteProtect(bool locked);
    void SetCompactFlashSwapOnLoad(bool allow);
    bool SetSpitzCfNetwork(SpitzCfNetwork mode);
    bool SetSpitzUsbDrive(const std::string& path);
    bool SpitzUsbDriveAttached() const;
    std::string SpitzUsbHostState() const;
    SpitzCfNetwork GetSpitzCfNetwork() const;
    std::string SpitzCfNetworkState() const;
    // Select the protocol of the next card in the SD/MMC slot.
    bool SetSpitzMmcCard(bool legacyMmc);
    bool SpitzMmcCard() const;
    uint64_t Run(uint64_t maxCycles, uint64_t cyclesPerSecond) override;
    uint64_t CoreHz() const override;
    std::optional<bool> SdRemovalHandled() override;
    bool InsertPalmWifi();
    void EjectPalmWifi();
    bool PalmWifiInserted() const { return bool(palmSdioWifi); }
    using SocGeneric<SocPXA>::Save;
    bool Save() override;
    bool SupportsPalmWifi();
    uint32_t *GetPendingFrame(uint32_t &firstDirtyLine, uint32_t &lastDirtyLine) override;
    void ResetPendingFrame() override;
    DeviceType5 GetDeviceType() override;
    DisplayMode GetDisplayMode() override;
    void SuspendTimerInterrupts(bool suspendInterrupts) override;
    bool LcdEnabled() override;
    bool HoldSwitchSupported() const override;
    bool BatterySupported() const override;
    bool SetBattery(const BatteryPower&) override;
    bool SetSpitzPower(unsigned batteryPercent, bool acConnected, bool batteryDoorLocked);
    bool SetSpitzBatteryDoorLocked(bool locked);
    bool GetSpitzPower(SpitzPowerState&) const;
    bool HeadphonesSupported() const override;
    bool HeadphonesIn() const override { return headphones || SpitzRemoteInserted(); }
    bool SetHeadphones(bool in) override;
    bool SpitzRemoteInserted() const;
    void SetSpitzRemote(bool inserted);
    bool SetSpitzRemoteKey(SpitzRemoteKey key, bool down);
    void SetSdWriteProtect(bool on) override;
    bool HoldSwitchOn() const override;
    bool SetHoldSwitch(bool on) override;
    bool SetSpitzLid(bool closed, bool swivelled) override;
    bool SpitzLidClosed() const override;
    bool SpitzSwivelled() const override;
    bool PowerButtonSupported() const override { return !disableSleep; }
    bool PowerSleeping() const override;
    // Wakes from PXA sleep since the process started, for tests. Not saved.
    uint64_t PowerWakes() const override { return powerWakes; }
    void Wakeup(uint8_t wakeupSource) override;
    bool PrepareM68kSupport() override;
    bool PaceHooksInRam() override;

    // Current-session host source, never armed by loading portable state.
    AudioInputDemand MicrophoneDemand() override;
    void SetMicrophoneSource(const AudioInputSource *source) override;
    // The camera's host picture source (Zire 72), same lifetime as the microphone's.
    void SetCameraSource(const CameraSource *source);
    uint32_t DispatchTicks(uint32_t clientType, uint32_t batchedTicks);

    bool KeyboardAttached() const override { return irKeyboard.Attached(); }
    void AttachKeyboard(bool value) override;
    bool KeyboardKey(uint8_t code, bool down) override;
    void ReleaseKeyboard() override;

    // PEGA-GC10 on the TG50 connector: owns the FFUART hooks, AUX and GPIO3 while attached.
    bool GamepadSupported() const override;
    bool GamepadAttached() const override { return gamepad.Attached(); }
    bool AttachGamepad(bool attach, bool identify = true) override;
    std::string GamepadState() const override;
    bool GamepadHold() const override { return gamepad.Attached() && gamepad.HoldOn(); }
    const ClieGamepad& Gamepad() const { return gamepad; }

    // One infrared accessory at a time. Hook ownership is host-only and is not
    // serialized. Reset/accepted load invalidate the peer; checkpoints do not.
    bool SetIrHostHooks(const IrHostHooks* hooks);
    bool IrHostAttached() const { return irHostHooks.context != nullptr; }
    bool IrPowerEnabled();
    bool SetBluetooth(const std::string& path,std::string& error) override;
    std::string BluetoothState() override;
    bool HostIoPaused() const { return usbHostPaused != 0; }
    PxaUart* GetIrUart() { return irUart; }

    // Model access for development probes; creates no host connection.
    PxaUart* GetSerialUart() { return (GetDeviceType() == deviceTypeTX || deviceProfileIsE2Board5(GetDeviceType()) || GetDeviceType() == deviceTypeTG50) ? ffUart : nullptr; }
    // Pulls the connector's HotSync line low for a moment; false where there is none or a key is already being held.
    bool PressHotSync() override;

    SerialHostRoute GetSerialRoute();
    bool SetSerialHostHooks(const SerialHostHooks* hooks);
    bool SerialEnabled();

    // Non-owning development-probe access; use only on this SoC's execution
    // thread. The cable method drives the board's detector input.
    Pxa270Udc* GetUsbController() {
        const auto type = GetDeviceType();
        return type == deviceTypeTX || type == deviceTypeZire72 || type == deviceTypeSpitz ? udc2 : nullptr;
    }
    UsbDevicePort GetUsbPort() {
        if(GetDeviceType()==deviceTypeTX || GetDeviceType()==deviceTypeZire72)return UsbDevicePort(udc2);
        if(deviceProfileIsE2Board5(GetDeviceType()))return UsbDevicePort(udc1);
        if(GetDeviceType()==deviceTypeTG50)return UsbDevicePort(udc1,
            [](void* context) { return static_cast<SocPXA*>(context)->UsbBoardListening(); }, this);
        return {};
    }
    bool UsbDefaultCableAttached() { return false; }
    bool SetUsbTestCable(bool connected);
    bool UsbBoardListening();
    bool UsbTestCableAttached();
    // TG50: the connector's AUX conversion result (12 bits), the accessory's ID.
    uint16_t Tg50ConnectorAux();
    // A USB host on emulated time (UsbHostBridge). `tick` runs in the peripheral batch
    // (about every 10 us) once the time `wake` asks for has come, except during a serial
    // checkpoint; `replaced` runs on a reset or after a load succeeds, but not for a
    // checkpoint restore. KickUsbHost runs the batch soon, for an earlier wake.
    // nullptr removes the hooks.
    void SetUsbHostHooks(const UsbHostHooks* hooks) { usbHostHooks = hooks ? *hooks : UsbHostHooks{}; }
    void KickUsbHost();
    bool UsbHostIoPaused() const { return usbHostPaused || irReplacementDepth || PowerSleeping(); }

    bool EnableFastAudio();
    bool FastAudioActive() const { return fastAudio && fastAudio->Active(); }
    void NotifyFastAudioUnderrun();
    void SetFastAudioPeriod(uint32_t frames);

   protected:
    std::function<void()> CapturePeripheralInput() override;
    void BeforeStateReplacement() override;
    void DiscardMemoryStickSaveImage() override;
    void StateReplaced(StateReplacement) override;
    void StateReplacementRefused(const std::function<void()>& restore) override;
    void BeginSerialCheckpoint() override;
    void EndSerialCheckpoint() override;
    void OnSetFramebufferDirty() override;
    bool OnSleep() override;
    void OnWakeup() override;
    void OnSetAudioQueue(struct AudioQueue *audioQueue) override;
    void OnSetPcmOutputEnabled() override;
    void OnSetPcmSuspended() override;
    std::unique_ptr<PalmSdioWifi> palmSdioWifi;
    std::unique_ptr<PalmSdioWifi> displacedPalmSdioWifi;
    bool reinsertPalmWifi = false;
    bool LoadPalmWifi(SavestateLoader<ChunkType>& loader);

    void OnSdInsert() override;
    void OnSdRemount() override;
    void OnSdEject() override;

    void OnTouch(int x, int y);
    void OnEngageKey(KeyId key, bool down);
    bool HoldKeyEvent(const KeyEvent& event) override;
    void OnBeforeReset();
    void OnReset();

    void OnLoad(SavestateLoader<ChunkType> &loader);

    template <typename T>
    void OnSave(T &savestate);

    // When the power key last went down; time of the first held-back power press, or 0 (Zire only).
    bool resetSpitzLidClosed{false}, resetSpitzSwivelled{false};
    std::optional<bool> resetSpitzMmcCard;
    uint64_t powerKeyDownAt{0};
    uint64_t powerPressHeldSince{0};

   private:
    PxaRtcRetained resetRtc{};
    bool resetHold{false};
    void ResumePowerSleep();
    void LoadSleepReset(SavestateLoader<ChunkType>& loader);
    void ResumePowerSleep25x();
    void LoadSleepReset25x(SavestateLoader<ChunkType>& loader);
    bool restoringPowerState{false};
    uint64_t powerWakes{0};
    void UpdateFastAudioRoute();
    std::unique_ptr<FastAudio> fastAudio;
    bool fastAudioRouted{false};
    uint32_t ramBackingSize{0};
    void AllocateBuffers();
    void SetupScheduler();
    void SchedulePcmTask();

    void CycleBatch0();
    bool Batch0Required();
    uint32_t Batch0Ticks();

    void UpdateSchedulePcm();

    static void RescheduleCB(void *ctx, uint32_t task);
    static uint64_t GetTimeCB(void *ctx);
    static uint32_t TimerElapsedCB(void* ctx);
    static void TimerRescheduleCB(void* ctx);
    static bool OscrPeekCB(void* ctx, uint32_t cycles, uint32_t* oscr);
    static bool GpioPeekCB(void* ctx, uint32_t pa, uint32_t* value);
    uint32_t TimerElapsedAt(uint32_t cycles) const;

   private:
    int HeadphoneDetectPin() const;
    void ApplyHeadphones();
    void ApplySdWriteProtect(bool inserted);
    void OnSpitzSdPowerChanged();
    bool headphones{false};
    bool sdProtected{false};

    IrKeyboard irKeyboard;
    ClieGamepad gamepad;
    bool gamepadLineLow{};  // GPIO3 as the pad last drove it; wiring, follows the loaded pad
    void SyncGamepadLine();
    bool GamepadPortReady();
    void InstallGamepadHooks();
    bool GamepadConnectorBusy() const;
    SerialHostHooks serialHostHooks{};
    IrHostHooks irHostHooks{};
    unsigned irReplacementDepth{};
    static void RefreshIrMode(void* context);
    static void RefreshIrConfiguration(void* context);
    static void RefreshTransportPins(void* context);
    uint8_t socRev{0};
    UsbHostHooks usbHostHooks{};
    unsigned usbHostPaused{0};
    // From the device profile: checkpointFence (the microphone capability) fences the microphone
    // and audio output during checkpointed guest work; backgroundPanel allows SetAudioOutputFenced.
    bool checkpointFence{}, backgroundPanel{};
    bool audioOutputFenced{};
    unsigned checkpointFenceDepth{0};
    bool microphoneInvalidatePending{};
    bool serialResumePending{false};

    PxaUart *ffUart{nullptr}, *hwUart{nullptr}, *stUart{nullptr}, *btUart{nullptr};
    PxaUart *irUart{nullptr};  // Board wiring; not session state.
    PxaSsp *ssp[3]{nullptr, nullptr, nullptr};
    PxaGpio *gpio{nullptr};
    PxaAC97 *ac97{nullptr};
    PxaDma *dma{nullptr};
    struct UartDmaLine { PxaDma *dma; uint8_t rxRequest; };  // PXA27x UART DMA wiring
    UartDmaLine uartDma[3]{};
    PxaI2s *i2s{nullptr};
    PxaI2c *i2c{nullptr};
    PxaIc *ic{nullptr};

    PxaMemCtrlr *memCtrl{nullptr};
    PxaPwrClk *pwrClk{nullptr};
    PxaI2c *pwrI2c{nullptr};
    PxaPwm *pwm[4]{nullptr, nullptr, nullptr, nullptr};
    PxaTimr *tmr{nullptr};
    PxaMmc *mmc{nullptr};
    PxaRtc *rtc{nullptr};
    PxaLcd *lcd{nullptr};
    ArmRam *sram{nullptr};
    ArmRam *ramMirror{nullptr};  // mirror for ram termination
    Device *dev{nullptr};
    Keypad *kp{nullptr};

    VSD *vSD{nullptr};

    MemoryBuffer bufferNand{};
    MemoryBuffer bufferLcd{};
    MemoryBuffer bufferSram{};

    union {
        // 25x/26x
        struct {
            Pxa255dsp *dsp;
            Pxa255Udc *udc1;
        };
        // PXA27x
        struct {
            Pxa270wmmx *wmmx;
            Pxa270Udc *udc2;
            PxaImc *imc;
            PxaKpc *kpc;
        };
        void *__zeroInit[4]{nullptr};
    };
};

#endif  // _SOC_PXA_H_
