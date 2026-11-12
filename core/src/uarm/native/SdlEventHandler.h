#ifndef _SDL_EVENT_HANDLER_
#define _SDL_EVENT_HANDLER_

#include <SDL.h>

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include "DeviceControls.h"
#include "Gamepad.h"
#include "Rotation.h"
#include "WindowLayout.h"
#include "SoC.h"
#include "device_configuration.h"

class SdlEventHandler {
   public:
    SdlEventHandler(SoC* soc, float scale, DisplayConfiguration& deviceDisplayConfiguration,
                    Rotation rotation, DeviceControlView controlView = {});
    virtual ~SdlEventHandler() = default;
    // With a strip profile given directly, for art whose device has no DeviceType5 yet.
    SdlEventHandler(SoC* soc, const DeviceControlProfile& controls, float scale,
                    DisplayConfiguration& deviceDisplayConfiguration, Rotation rotation, DeviceControlView controlView = {});

    // Polls SDL and handles every event; for a single thread that owns SDL and the SoC.
    void HandleEvents();
    // Handles one event on the thread that owns the SoC, when another thread polls SDL.
    void HandleEvent(const SDL_Event& event);
    // With direct capture off, the SDL thread applies WantsCapture with SDL_CaptureMouse.
    void SetDirectCapture(bool direct) { directCapture = direct; }
    bool WantsCapture() const { return capture.load(std::memory_order_relaxed); }

    DeviceKeyMask PressedButtons() const;
    // Where the analogue stick is held, for drawing; centred when let go.
    DeviceStick Stick() const { return stick; }
    bool RedrawRequested() const;
    bool QuitRequested() const;
    void ClearQuitRequested() { quitRequested = false; }
    void ClearRedrawRequested();
    // Called with the shortcut name for the keys the desktop app owns (see ShortcutKeys.h).
    void SetShortcutSink(std::function<void(const std::string&)> sink) { shortcutSink = std::move(sink); }

    void SetRotation(Rotation rotation);
    // A new set of shown controls and window scale; releases pointers and touches held on
    // the old ones. A SetLayout with the new layout follows.
    void SetView(DeviceControlView controlView, float scale);
    // Pointer positions map through the layout's offset and scale when fitted is set.
    void SetLayout(const WindowLayout& layout, bool fitted);
    // True once after the window changed size.
    bool TakeResized();
    // True while a mobile system has the app in the background.
    bool InBackground() const { return background; }

    // Only touch screens count as fingers on the device, not touchpads.
    virtual bool DirectTouch(SDL_TouchID touch) const {
        return SDL_GetTouchDeviceType(touch) == SDL_TOUCH_DEVICE_DIRECT;
    }

   private:
    void ReleasePointer();
    void HandleFinger(const SDL_TouchFingerEvent& event);
    void ReleaseFingers();
    void Capture(bool on);
    void ReleaseInputs();
    bool OnScreen(int x, int y) const;
    int ScreenWidth() const;
    int ScreenHeight() const;
    int RotateX(int x, int y);
    int RotateY(int x, int y);
    void MapPointer(int& x, int& y) const;
    // The stick well in window pixels, w 0 for none.
    SDL_Rect StickWell() const;
    // Sends the stick's position to the SoC when it changed: a finger or the pointer on it wins
    // over a game controller's stick, and that over the arrow keys.
    void UpdateStick();
    DeviceStick TouchStick(int x, int y) const;
    // A finger or the pointer put down on the knob holds the stick still until it moves past
    // deviceStickDragged; lifted before that it is a tap that pushes the stick in. Returns true
    // while that tap is still possible.
    bool HoldStill(bool& pending, float downX, float downY, int x, int y) const;
    void TapStickPress();
    // Sends what the game controller holds now: its keys that changed and the stick.
    void UpdateGamepad();
    GamepadState CombinedPads() const;

   private:
    SoC* soc;

    std::array<bool,512> hostKeys{};
    std::array<uint8_t,128> codeHolds{};
    std::array<bool,keyIdCount> hardKeys{};
    std::array<KeyId,512> builtInHeld{};
    std::array<unsigned,keyIdCount> builtInHolds{};
    struct Finger {
        bool active{false}, pen{false}, stick{false};
        SDL_TouchID touch{};
        SDL_FingerID id{};
        KeyId key{keyInvalid};
        DeviceStick position{};
        // Put down on the knob and not yet moved, in window pixels.
        bool pressPending{false};
        float downX{0}, downY{0};
    };
    std::array<Finger,16> fingers{};
    const DeviceControlProfile& controls;
    DeviceControlView controlView;
    KeyId pointerKey{keyInvalid};
    // The pointer holding the stick, and arrow keys standing in for it (up, down, left, right).
    bool pointerStick{false};
    DeviceStick pointerPosition{};
    bool pointerPressPending{false};
    float pointerDownX{0}, pointerDownY{0};
    uint8_t stickKeys{0};
    // Each game controller as last reported (by instance id), the one whose stick moved last, and
    // the keys and stick they hold on the device together. Off while the window has lost focus.
    std::map<SDL_JoystickID, GamepadState> pads;
    SDL_JoystickID lastStickPad{-1};
    bool padFocused{true};
    DeviceKeyMask padKeys{0};
    DeviceStick padStick{};
    DeviceStick stick{};
    bool penDown{false};
    float scale{1};
    bool redrawRequested{false};
    bool quitRequested{false};
    bool resized{false};
    bool background{false};
    bool fitted{false};
    bool directCapture{true};
    std::function<void(const std::string&)> shortcutSink;
    std::atomic<bool> capture{false};
    WindowLayout layout;

    int width;
    int height;
    Rotation rotation;

   private:
    SdlEventHandler();
    SdlEventHandler(const SdlEventHandler&);
    SdlEventHandler(SdlEventHandler&&);
    SdlEventHandler& operator=(const SdlEventHandler&);
    SdlEventHandler& operator=(SdlEventHandler&&);
};

#endif  // _SDL_EVENT_HANDLER_
