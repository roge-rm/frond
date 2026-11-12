#ifndef _SOC_H_
#define _SOC_H_

#include <memory.h>

#include <cstdint>
#include <functional>

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
#ifndef __EMSCRIPTEN__
    NetworkSimulation networkSimulation;
    std::unique_ptr<LibertasCF> wirelessDevice;
#endif
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
    virtual bool HoldSwitchSupported() const { return false; }
    virtual bool HoldSwitchOn() const { return false; }
    virtual bool SetHoldSwitch(bool) { return false; }
    virtual bool KeyboardAttached() const { return false; }
    virtual void AttachKeyboard(bool) {}
    virtual bool KeyboardKey(uint8_t, bool) { return false; }
    virtual void ReleaseKeyboard() {}
    virtual void Reset();
    // Next Reset() is a power-on reset; the Zire uses it for a hard reset (clean boot).
    void RequestPowerOnReset() { powerOnReset = true; }
    // A power-on reset on the Zire; on the TX and E2 power held through a reset, which brings up
    // the erase prompt.
    void HardReset();

    virtual uint64_t Run(uint64_t maxCycles, uint64_t cyclesPerSecond) = 0;
    virtual bool RunToPaceSyscall(uint16_t syscall, uint64_t maxCycles,
                                  uint64_t cyclesPerSecond) = 0;
#ifndef __EMSCRIPTEN__
    virtual bool RunToNativePc(uint32_t, uint32_t, uint64_t, uint64_t) { return false; }
#endif
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
    void KeyDown(enum KeyId key);
    void KeyUp(enum KeyId key);
    void PenDown(int x, int y);
    void PenUp();
    // Analogue stick from the frontend, on the thread that owns the SoC: each axis -1 to 1 with
    // right and down positive, and 0,0 when let go. A board with a stick maps it to its own
    // hardware; others ignore it.
    virtual void AnalogStick(float, float) {}

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

    virtual void DumpMMU() = 0;

    struct ArmCpu *GetCpu() { return cpu; }
    struct SyscallDispatch *GetSyscallDispatch() { return syscallDispatch; };
    uint32_t GetRamSize() { return ramSize; };
    struct NAND *GetNand() { return nand; };
    struct SystemState *GetSystemState() { return systemState; };

    virtual void JamKey(enum KeyId key, uint32_t durationMsec) = 0;

    bool IsPacePatched();
    virtual bool PrepareM68kSupport() { return IsPacePatched(); }
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
    virtual void ResetPendingFrame() = 0;
    virtual DeviceType5 GetDeviceType() = 0;
    virtual DisplayMode GetDisplayMode() = 0;
    virtual void SuspendTimerInterrupts(bool suspendInterrupts) = 0;
    virtual bool LcdEnabled() = 0;

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
    bool powerOnReset{false};
    bool framebufferDirty{true};
    bool pcmEnabled{false};
    bool pcmSuspended{false};
    uint64_t injectedTimeNsec{0};

    DisplayConfiguration displayConfiguration{};

    uint64_t eventQueueTicks{0};
    uint64_t eventQueueTicksAtPenDown{0};
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
