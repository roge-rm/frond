#ifndef _SOC_H_
#define _SOC_H_

#include <memory.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "AttentionState.h"
#include "audio_input.h"
#include "BatteryPower.h"
#include "NetworkSimulation.h"
#include "LibertasCF.h"
#include "buffer.h"
#include "device_configuration.h"
#include "device_type5.h"
#include "keys.h"
#include "memory_buffer.h"
#include "queue.h"
#include "savestate/ChunkTypeUarm.h"
#include "savestate/Savestate.h"
#include "sdcard.h"

struct ArmRam;
class HostUartPort;
class HostInfraredPort;
struct ArmRom;
struct ArmMem;
struct NAND;
struct ArmCpu;
struct SystemState;
struct SyscallDispatch;
struct PacePatch;
struct PatchDispatch;
struct AudioQueue;
struct PatchContext;

class SoC {
   public:
    NetworkSimulation networkSimulation;
    std::unique_ptr<LibertasCF> wirelessDevice;
    class InputCheckpoint;
    virtual void SetAudioOutputFenced(bool) {}
    bool RestoringCheckpoint() const { return restoringCheckpoint; }

    class SerialCheckpoint {
       public:
        explicit SerialCheckpoint(SoC& soc) : soc(soc) { soc.BeginSerialCheckpoint(); }
        ~SerialCheckpoint() { Finish(); }
        void Finish() { if (active) { soc.EndSerialCheckpoint(); active = false; } }
        SerialCheckpoint(const SerialCheckpoint&) = delete;
        SerialCheckpoint& operator=(const SerialCheckpoint&) = delete;
       private:
        SoC& soc;
        bool active{true};
    };
    // The battery setting (machine setting, never saved with the session); see BatteryPower.h. Runs on the SoC's
    // thread. False where the board has no battery input modelled yet, which keeps its fixed full battery.
    virtual bool BatterySupported() const { return false; }
    virtual bool SetBattery(const BatteryPower&) { return false; }
    const BatteryPower& Battery() const { return battery; }
    virtual bool HoldSwitchSupported() const { return false; }
    virtual bool HoldSwitchOn() const { return false; }
    virtual bool SetHoldSwitch(bool) { return false; }
    // Headphones in the jack: a host setting like the battery, put back on the board after every load and
    // reset, never saved. Runs on the SoC's thread.
    virtual bool HeadphonesSupported() const { return false; }
    virtual bool HeadphonesIn() const { return false; }
    virtual bool SetHeadphones(bool) { return false; }
    // The cradle's HotSync button, held down or let go. A load or reset lets it go. Runs on the SoC's thread.
    virtual bool CradleButtonSupported() const { return false; }
    virtual bool CradleButtonDown() const { return false; }
    virtual bool SetCradleButton(bool) { return false; }
    // The cradle on the connector, a host setting like the headphones. Runs on the SoC's thread.
    virtual bool CradleSupported() const { return false; }
    virtual bool CradleAttached() const { return false; }
    virtual bool CradleSetting() const { return false; }
    virtual bool SetCradle(bool) { return false; }
    // A short press of the HotSync button, let go by itself. False without one or while one is held.
    virtual bool PressHotSync() { return false; }
    // Asleep, and the wakes counted since the session started (not saved).
    virtual bool PowerSleeping() const { return false; }
    // The host's microphone for boards with one: what the guest asks of it now, and the source it pulls from
    // (nullptr to detach). Current-session only, never armed by loading portable state.
    virtual AudioInputDemand MicrophoneDemand() { return {}; }
    virtual void SetMicrophoneSource(const AudioInputSource *) {}
    // Every host frame the board's sound makes (44100 Hz, stereo, signed 16-bit) to a raw file, whether PCM
    // output is on or not, for checks; an empty path stops it. False where the board has no capture.
    virtual bool SetPcmCapture(const std::string &) { return false; }
    // Frames, frames that weren't silent and the peak since the capture started.
    virtual std::string PcmCaptureSummary() const { return ""; }
    virtual uint64_t PowerWakes() const { return 0; }
    // The serial port on the connector and the infrared port for HostSerial and HostInfrared, on the
    // boards that give them this way (null elsewhere; the PXA boards give their UARTs). Owned by the SoC.
    virtual HostUartPort* SerialHostPort() { return nullptr; }
    virtual HostInfraredPort* InfraredHostPort() { return nullptr; }
    // A virtual Bluetooth controller on a local H4 Unix socket, empty for none.
    virtual bool SetBluetooth(const std::string &, std::string &error) {
        error = "No Bluetooth controller on this board.";
        return false;
    }
    virtual std::string BluetoothState() { return "off"; }
    virtual bool KeyboardAttached() const { return false; }
    virtual void AttachKeyboard(bool) {}
    virtual bool KeyboardKey(uint8_t, bool) { return false; }
    virtual void ReleaseKeyboard() {}
    // The CLIÉ game controller on the interface connector (TG50 only). Attach and detach run on
    // the SoC's thread while it runs, like plugging the pad in; identify false skips the plug-in
    // handshake (the pad then identifies on its first press). False if the connector is taken.
    virtual bool GamepadSupported() const { return false; }
    virtual bool GamepadAttached() const { return false; }
    virtual bool AttachGamepad(bool, bool = true) { return false; }
    virtual std::string GamepadState() const { return "unavailable"; }
    // The pad's Hold slide is on (attached only).
    virtual bool GamepadHold() const { return false; }
    virtual void Reset();
    // Next Reset() is a power-on reset; the Zire uses it for a hard reset (clean boot).
    void RequestPowerOnReset() { powerOnReset = true; }
    // A power-on reset on the Zire; on the TX and E2 power held through a reset, which brings up
    // the erase prompt. The Zodiac holds power from its own reset (SocIMX1).
    virtual void HardReset();

    virtual uint64_t Run(uint64_t maxCycles, uint64_t cyclesPerSecond) = 0;
    // Set by the host while a press or release waits for the guest thread. Run then ends at its next
    // slice end, so the press goes in within about a millisecond. Only live input sets it, so replays and
    // runs without live input don't change.
    void SetHostInputPending(const std::atomic<bool>* pending) { hostInputPending = pending; }
    // The core clock in Hz as the guest has set it, 0 where the board has none to follow.
    virtual uint64_t CoreHz() const { return 0; }
    // Original speed: a run call whose cyclesPerSecond is at least CoreHz() runs at CoreHz() as it
    // changes, on the instruction that changes it. maxCycles stays in cyclesPerSecond units, and so does
    // what the call returns. A slower cyclesPerSecond (a host that can't keep up) runs at that rate.
    // Fast charges the data cache and region model at Original only (cpuSetJitMemoryCost).
    void SetFollowCoreClock(bool follow);
    bool FollowsCoreClock() const { return followCoreClock; }
    // Faster speed: the CPU runs num / den times the core clock (CoreHz on PXA, FCLK on the Zodiac), with guest
    // time, timers and the cost model's charges as at Original. 1 / 1 is Original.
    virtual void SetClockMultiplier(uint32_t num, uint32_t den);
    uint32_t ClockMultiplierNum() const { return clockMultiplierNum; }
    uint32_t ClockMultiplierDen() const { return clockMultiplierDen; }
    // The rate a following run takes: CoreHz times the clock multiplier, 0 where there is no clock to follow.
    uint64_t FollowHz() const {
        const uint64_t hz = CoreHz();
        if (clockMultiplierNum == clockMultiplierDen) return hz;
        return uint64_t(__uint128_t(hz) * clockMultiplierNum / clockMultiplierDen);
    }
    virtual bool RunToPaceSyscall(uint16_t syscall, uint64_t maxCycles,
                                  uint64_t cyclesPerSecond) = 0;
    virtual bool RunToNativePc(uint32_t, uint32_t, uint64_t, uint64_t) { return false; }
    virtual bool ExecuteInjected(uint64_t maxCycles, uint64_t cyclesPerSecond) = 0;

    void AccountInjectedCycles(uint64_t cycles, uint64_t cyclesPerSecond) {
        if (!cyclesPerSecond) return;
        const __uint128_t total = __uint128_t(cycles) * 1000000000 / cyclesPerSecond + injectedTimeNsec;
        injectedTimeNsec = total > UINT64_MAX ? UINT64_MAX : uint64_t(total);
    }
    uint64_t GetInjectedTimeNsec() { return injectedTimeNsec; }
    void ResetInjectedTimeNsec() { injectedTimeNsec = 0; }

    virtual bool PowerButtonSupported() const { return true; }
    // False on a board with no PACE or guest calls: installs, exports, launches and the database list.
    virtual bool GuestServicesSupported() const { return true; }
    virtual bool HardResetSupported() const { return true; }
    // Install copies the file onto the SD card for the guest's own tools (Windows CE, frondNative::installToSdCard).
    virtual bool InstallsToCard() const { return false; }
    void KeyDown(enum KeyId key);
    void KeyUp(enum KeyId key);
    // A power press from the host the guest may not have had yet: held, queued or jammed.
    bool PowerKeyPending();
    void PenDown(int x, int y);
    void PenUp();
    // The clock key and pen events are stamped with, in milliseconds; null is the host's steady clock.
    // A headless replay passes guest time so the holds the Zodiac times from it repeat.
    static void SetEventClock(uint64_t (*clock)());
    // Analogue stick from the frontend, on the thread that owns the SoC: each axis -1 to 1 with
    // right and down positive, and 0,0 when let go. A board with a stick maps it to its own
    // hardware; others ignore it.
    virtual void AnalogStick(float, float) {}
    // Quarter turns the host shows the view by, on top of the guest's own; the stick follows the total.
    virtual void SetStickHostTurns(int) {}

    void SetFramebufferDirty(uint32_t pa = 0, uint32_t size = ~0u);
    bool SetFramebuffer(uint32_t start, uint32_t size);
    void ClearFramebufferDirty();

    void Sleep();
    virtual void Wakeup(uint8_t wakeupSource);

    void SetAudioQueue(struct AudioQueue *audioQueue);
    void SetPcmOutputEnabled(bool pcmOutputEnabled);
    bool PcmOutputEnabled() const { return pcmEnabled; }
    void SetPcmSuspended(bool pcmSuspended);

    virtual Buffer GetRomData();

    struct Buffer GetNandData();
    struct Buffer GetNandDirtyPages();
    bool IsNandDirty();
    void SetNandDirty(bool isDirty);

    Buffer GetMemoryData();
    Buffer GetMemoryDirtyPages();

    virtual bool Save() = 0;
    virtual bool Load(size_t savestateSize, void *savestateData) = 0;
    virtual struct Buffer GetSavestate();

    void SdInsert();
    bool SdRemount();
    void SdEject();
    bool SdInserted() { return cardInserted; }
    // After an SD eject: whether the guest's card driver has handled the removal, on boards that can
    // tell (the PXA SD devices: the guest stopped the MMC clock with the slot empty), else nullopt.
    virtual std::optional<bool> SdRemovalHandled() { return std::nullopt; }
    // The card's write-protect tab, from the host's card descriptor.
    virtual void SetSdWriteProtect(bool) {}
    // A second SD slot, the Zodiac's only, with its card in sdCardStore(1). Sd2Insert puts that card
    // in and is false when it can't go in.
    virtual bool HasSecondSdSlot() const { return false; }
    virtual bool Sd2Insert() { return false; }
    virtual void Sd2Eject() {}
    virtual bool Sd2Inserted() const { return false; }
    virtual void SetSd2WriteProtect(bool) {}

    virtual void DumpMMU() = 0;

    struct ArmCpu *GetCpu() { return cpu; }
    // Cycles the CPU ran in the run calls, sleep left out: one per guest instruction on the ARM
    // cores. A counter for tools like benchmark-replay, not saved.
    uint64_t GetRunInstructions() const { return runInstructions; }
    struct SyscallDispatch *GetSyscallDispatch() { return syscallDispatch; };
    uint32_t GetRamSize() { return ramSize; };
    struct NAND *GetNand() { return nand; };
    struct SystemState *GetSystemState() { return systemState; };

    virtual void JamKey(enum KeyId key, uint32_t durationMsec) = 0;

    bool IsPacePatched();
    virtual bool PrepareM68kSupport() { return IsPacePatched(); }
    // Guest RAM, which sessions save, holds PACE dispatch hooks (the T|X's PACE runs from RAM).
    virtual bool PaceHooksInRam() { return false; }
    virtual uint64_t GetTime() = 0;

    // Actual SoC needs to implement those, the other virtuals are taken
    // care of in soc_generic.h
    virtual uint32_t *GetPendingFrame(uint32_t &firstDirtyLine, uint32_t &lastDirtyLine) = 0;
    // Whether GetPendingFrame gives the panel as it is wired, the display's height wide and its
    // width high, for the frontend to turn a quarter clockwise as it draws.
    virtual bool FrameQuarterTurn() const { return false; }
    // The orientation the guest OS shows its screen in, as SysGetOrientation numbers it (1 portrait,
    // 2 landscape, 3 reverse portrait, 4 reverse landscape), or 0 when unknown or the device has none.
    // Guest thread only.
    virtual unsigned GuestOrientation() { return 0; }
    // The LED and vibrate motor as the guest drives them (AttentionState.h). All off on a device without them.
    // Guest thread only.
    using Attention = AttentionState;
    static constexpr uint8_t ledGreen = AttentionState::green, ledRed = AttentionState::red;
    virtual Attention GuestAttention() { return {}; }
    virtual void ResetPendingFrame() = 0;
    virtual DeviceType5 GetDeviceType() = 0;
    virtual DisplayMode GetDisplayMode() = 0;
    virtual void SuspendTimerInterrupts(bool suspendInterrupts) = 0;
    virtual bool LcdEnabled() = 0;

   protected:
    bool HostInputPending() const {
        return hostInputPending && hostInputPending->load(std::memory_order_relaxed);
    }
    const std::atomic<bool>* hostInputPending{nullptr};

   protected:
    // Actual SoC needs to implement those, the other virtuals are taken
    // care of in soc_generic.h
    std::shared_ptr<int> checkpointLifetime{std::make_shared<int>(0)};
    virtual std::function<void()> CapturePeripheralInput() { return [] {}; }
    virtual void BeforeStateReplacement() {}
    virtual void DiscardMemoryStickSaveImage() {}
    // After a reset, or a load that succeeded.
    virtual void StateReplaced() {}
    // A load was refused; restore loads the previous state back.
    virtual void StateReplacementRefused(const std::function<void()>& restore) { restore(); }
    virtual void BeginSerialCheckpoint() {}
    virtual void EndSerialCheckpoint() {}
    virtual void OnSetFramebufferDirty() = 0;
    virtual bool OnSleep() = 0;
    virtual void OnWakeup() = 0;
    virtual void OnSetAudioQueue(struct AudioQueue *audioQueue) = 0;
    virtual void OnSetPcmOutputEnabled() = 0;
    virtual void OnSetPcmSuspended() = 0;
    virtual void OnSdInsert() = 0;
    virtual void OnSdRemount() = 0;
    virtual void OnSdEject() = 0;

   protected:
    struct PenEvent {
        bool penDown;
        int x, y;
        // When the host made the event, steady clock milliseconds.
        uint64_t hostMsec{0};

        static PenEvent PenDown(int x, int y);
        static PenEvent PenUp();
    };

    struct KeyEvent {
        bool keyDown;
        enum KeyId key;
        uint64_t hostMsec{0};

        static KeyEvent KeyDown(enum KeyId key);
        static KeyEvent KeyUp(enum KeyId key);
    };

    // True keeps the next queued key event, and those after it, for a later pump.
    virtual bool HoldKeyEvent(const KeyEvent&) { return false; }

   protected:
    SoC();

   protected:
    ArmRom *rom{nullptr};
    ArmRam *ram{nullptr};
    ArmCpu *cpu{nullptr};
    ArmMem *mem{nullptr};

    NAND *nand{nullptr};

    SystemState *systemState{nullptr};
    SyscallDispatch *syscallDispatch{nullptr};
    PacePatch *pacePatch{nullptr};
    PatchDispatch *patchDispatch{nullptr};
    PatchContext *patchContext{nullptr};

    MemoryBuffer bufferMemory{};

    std::unique_ptr<Savestate<ChunkType>> savestate;
    std::unique_ptr<Savestate<ChunkType>> powerOnState;

    std::unique_ptr<Queue<PenEvent>> penEventQueue;
    std::unique_ptr<Queue<KeyEvent>> keyEventQueue;
    struct AudioQueue *audioQueue;

    bool sleeping{false};
    uint64_t runInstructions{0};
    bool followCoreClock{false};
    uint32_t clockMultiplierNum{1}, clockMultiplierDen{1};
    bool powerOnReset{false};
    BatteryPower battery{};
    bool framebufferDirty{true};
    bool pcmEnabled{false};
    bool pcmSuspended{false};
    uint64_t injectedTimeNsec{0};

    DisplayConfiguration displayConfiguration{};

    uint64_t eventQueueTicks{0};
    uint64_t eventQueueTicksAtPenDown{0};
    bool powerPopped{false};  // a power event reached the guest; powerPoppedAt is its time
    uint64_t powerPoppedAt{0};
    bool powerHeld{false};  // the host holds power, from KeyDown to KeyUp
    KeyId jammedKey{keyInvalid};
    uint64_t releaseJammedKeyAt{0};
    // Time of the last JamKey; only a key jammed right before Reset() (no emulated time between)
    // is held through it, so a reset during a hard reset's power hold cancels the hold.
    uint64_t jammedKeyAt{~0ull};
    bool penDown{false};

    bool cardInserted{false};
    char cardId[SD_CARD_ID_MAX_LEN + 1]{};

    uint32_t ramBase{0};
    uint32_t ramSize{0};
    // PXA27x internal SRAM; the Zire 72 keeps its frame buffer there.
    ArmRam *sramFb{nullptr};
    uint32_t sramBase{0};
    uint32_t sramSize{0};

    uint32_t framebufferAccessLowWatermark{0};
    uint32_t framebufferAccessHighWatermark{~0u};

   private:
    // The JIT stores straight to RAM pages it has marked dirty; once the frame buffer is
    // clean again, or moves, its pages go back through the callbacks.
    void WriteProtectFramebuffer();
    bool SetFramebufferRange(uint32_t start, uint32_t size);
    bool restoringCheckpoint{false};
    SoC(const SoC &) = delete;
    SoC(SoC &&) = delete;
    SoC &operator=(const SoC &) = delete;
    SoC &operator=(SoC &&) = delete;
};

// Host input is deliberately outside the session format. A temporary rollback
// must restore it together with the guest, including events consumed speculatively.
class SoC::InputCheckpoint {
 public:
    explicit InputCheckpoint(SoC& soc)
        : soc(soc), lifetime(soc.checkpointLifetime), pens(*soc.penEventQueue), keys(*soc.keyEventQueue),
          ticks(soc.eventQueueTicks), penTicks(soc.eventQueueTicksAtPenDown),
          jammed(soc.jammedKey), releaseAt(soc.releaseJammedKeyAt), jammedAt(soc.jammedKeyAt), down(soc.penDown),
          restorePeripheral(soc.CapturePeripheralInput()) {}
    // A committed transaction discards restoration ownership immediately.
    void Finish() { restorePeripheral = {}; }
    bool Restore(size_t size, void* data) {
        if (lifetime.expired()) return false;
        struct Guard {
            SoC& soc;
            bool previous;
            ~Guard() { soc.restoringCheckpoint = previous; }
        } guard{soc, soc.restoringCheckpoint};
        soc.restoringCheckpoint = true;
        // Restore external stimuli before loading GPIO/device registers, so load
        // callbacks see the original inputs without synthesizing new edges.
        restorePeripheral();
        const bool ok = soc.Load(size, data);
        *soc.penEventQueue = pens;
        *soc.keyEventQueue = keys;
        soc.eventQueueTicks = ticks;
        soc.eventQueueTicksAtPenDown = penTicks;
        soc.jammedKey = jammed;
        soc.releaseJammedKeyAt = releaseAt;
        soc.jammedKeyAt = jammedAt;
        soc.penDown = down;
        return ok;
    }
 private:
    SoC& soc;
    std::weak_ptr<int> lifetime;
    Queue<PenEvent> pens;
    Queue<KeyEvent> keys;
    uint64_t ticks, penTicks;
    KeyId jammed;
    uint64_t releaseAt, jammedAt;
    bool down;
    std::function<void()> restorePeripheral;
};

#endif  // _SOC_H_
