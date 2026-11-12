#include "SdlEventHandler.h"

#include <SDL.h>

#include <cmath>
#include <algorithm>

#include "keys.h"
#include "ShortcutKeys.h"
#include "IrKeyboardMapping.h"
#include "BuiltInKeyboardMapping.h"

namespace {
    enum KeyId mapKey(SDL_Keycode keysym) {
        switch (keysym) {
            case SDLK_F1:
                return keyIdHard1;

            case SDLK_F2:
                return keyIdHard2;

            case SDLK_F3:
                return keyIdHard3;

            case SDLK_F4:
                return keyIdHard4;

            // Boards without a voice button ignore it.
            case SDLK_F5:
                return keyIdVoice;

            case SDLK_F6:
                return keyIdHome;

            // Zodiac Function, shoulder triggers and stick press, only on devices with those keys.
            case SDLK_F7:
                return keyIdFunction;

            case SDLK_F8:
                return keyIdTriggerLeft;

            case SDLK_F9:
                return keyIdTriggerRight;

            case SDLK_F10:
                return keyIdStickPress;

            case SDLK_LEFT:
                return keyIdLeft;

            case SDLK_RIGHT:
                return keyIdRight;

            case SDLK_UP:
                return keyIdUp;

            case SDLK_DOWN:
                return keyIdDown;

            case SDLK_RETURN:
            case SDLK_SELECT:
                return keyIdSelect;

            case SDLK_ESCAPE:
                return keyIdPower;

            default:
                return keyInvalid;
        }
    }

    bool hasKey(const DeviceControlProfile& controls, KeyId key) {
        for (size_t i = 0; i < controls.count; ++i) if (controls.buttons[i].key == key) return true;
        return false;
    }

    bool hasStick(const DeviceControlProfile& controls) {
        for (size_t i = 0; i < controls.count; ++i)
            if (controls.buttons[i].icon == DeviceControlIcon::stick) return true;
        return false;
    }

    // Arrow keys stand in for the stick on devices that have one: bits for up, down, left, right.
    uint8_t stickKeyBit(KeyId key) {
        switch (key) {
            case keyIdUp: return 1;
            case keyIdDown: return 2;
            case keyIdLeft: return 4;
            case keyIdRight: return 8;
            default: return 0;
        }
    }
}  // namespace

SdlEventHandler::SdlEventHandler(SoC* soc, float scale,
                                 DisplayConfiguration& deviceDisplayConfiguration,
                                 Rotation rotation, DeviceControlView controlView)
    : SdlEventHandler(soc, deviceControls(soc->GetDeviceType()), scale, deviceDisplayConfiguration, rotation, controlView) {}

SdlEventHandler::SdlEventHandler(SoC* soc, const DeviceControlProfile& controls, float scale,
                                 DisplayConfiguration& deviceDisplayConfiguration, Rotation rotation, DeviceControlView controlView)
    : soc(soc), controls(controls), controlView(controlView), scale(scale), rotation(rotation) {
    width = deviceDisplayConfiguration.width;
    height = deviceDisplayConfiguration.height + deviceDisplayConfiguration.graffitiHeight;
}

void SdlEventHandler::HandleEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) HandleEvent(event);
}

void SdlEventHandler::HandleEvent(const SDL_Event& polled) {
    SDL_Event event = polled;
    {
        switch (event.type) {
            case SDL_QUIT:
                ReleaseInputs();
                quitRequested = true;
                break;

            case SDL_FINGERDOWN:
            case SDL_FINGERMOTION:
            case SDL_FINGERUP:
                if (DirectTouch(event.tfinger.touchId)) HandleFinger(event.tfinger);
                break;

            case SDL_MOUSEBUTTONDOWN:
                if (event.button.which == SDL_TOUCH_MOUSEID) break;
                if (event.button.button != SDL_BUTTON_LEFT) break;
                if (std::any_of(fingers.begin(), fingers.end(), [](const Finger& f) { return f.active && f.pen; })) break;
                MapPointer(event.button.x, event.button.y);
                ReleasePointer();
                if (OnScreen(event.button.x,event.button.y)) {
                    penDown = true;
                    soc->PenDown(RotateX(event.button.x,event.button.y),
                                 RotateY(event.button.x,event.button.y));
                } else if (deviceStickGrab(StickWell(), (event.button.x + 0.5f)/scale, (event.button.y + 0.5f)/scale)) {
                    pointerStick = true;
                    pointerDownX = (event.button.x + 0.5f)/scale;
                    pointerDownY = (event.button.y + 0.5f)/scale;
                    pointerPressPending = deviceStickKnobAt(StickWell(), pointerDownX, pointerDownY);
                    pointerPosition = pointerPressPending ? DeviceStick{0, 0, true}
                                                          : TouchStick(event.button.x, event.button.y);
                    UpdateStick();
                } else if (event.button.x>=0 && event.button.y>=0) {
                    pointerKey=deviceControlAt(controls,ScreenWidth(),ScreenHeight(),
                                                int((event.button.x + 0.5f)/scale),int((event.button.y + 0.5f)/scale),controlView);
                    if (pointerKey && !hardKeys[pointerKey] && !builtInHolds[pointerKey]) soc->KeyDown(pointerKey);
                    redrawRequested=true;
                }
                if (penDown || pointerKey || pointerStick) Capture(true);

                break;

            case SDL_MOUSEBUTTONUP:
                if (event.button.which == SDL_TOUCH_MOUSEID) break;
                if (event.button.button != SDL_BUTTON_LEFT) break;
                if (pointerStick && pointerPressPending) {
                    int x = event.button.x, y = event.button.y;
                    MapPointer(x, y);
                    if (HoldStill(pointerPressPending, pointerDownX, pointerDownY, x, y)) TapStickPress();
                }
                ReleasePointer();

                break;

            case SDL_MOUSEMOTION:
                if (event.motion.which == SDL_TOUCH_MOUSEID) break;
                MapPointer(event.motion.x, event.motion.y);
                if (pointerStick) {
                    if (HoldStill(pointerPressPending, pointerDownX, pointerDownY, event.motion.x, event.motion.y)) break;
                    pointerPosition = TouchStick(event.motion.x, event.motion.y);
                    UpdateStick();
                    break;
                }
                if (pointerKey) {
                    if (event.motion.x<0 || event.motion.y<0 ||
                        deviceControlAt(controls,ScreenWidth(),ScreenHeight(),
                                        int((event.motion.x + 0.5f)/scale),int((event.motion.y + 0.5f)/scale),controlView)!=pointerKey)
                        ReleasePointer();
                    break;
                }
                if (!penDown) break;
                if (!OnScreen(event.motion.x,event.motion.y)) { ReleasePointer(); break; }

                soc->PenDown(RotateX(event.motion.x, event.motion.y),
                             RotateY(event.motion.x, event.motion.y));

                break;

            case SDL_KEYDOWN: {
                if (shortcutSink) {
                    if (const auto name = shortcutName(event.key.keysym.sym, event.key.keysym.mod, soc->KeyboardAttached())) {
                        if (!event.key.repeat) shortcutSink(*name);
                        break;
                    }
                }
                if (event.key.repeat) break;
                const auto scan = event.key.keysym.scancode;
                const auto code = irKeyboardCode(scan);
                if (scan > SDL_SCANCODE_UNKNOWN && size_t(scan) < builtInHeld.size() &&
                    (builtInHeld[scan] || hostKeys[scan])) break;
                if (soc->KeyboardAttached() && code) {
                    hostKeys[scan] = true;
                    if (++codeHolds[code] == 1) soc->KeyboardKey(code, true);
                    break;
                }
                const KeyId builtIn = builtInKeyboardKey(controls, scan);
                if (scan > SDL_SCANCODE_UNKNOWN && size_t(scan) < builtInHeld.size() && builtIn) {
                    if (!builtInHeld[scan]) {
                        builtInHeld[scan] = builtIn;
                        if (++builtInHolds[builtIn] == 1 && !hardKeys[builtIn] && pointerKey != builtIn)
                            soc->KeyDown(builtIn);
                        redrawRequested = true;
                    }
                    break;
                }
                enum KeyId key = mapKey(event.key.keysym.sym);
                if (key >= keyIdFunction && !hasKey(controls, key)) break;
                if (stickKeyBit(key) && hasStick(controls)) {
                    stickKeys |= stickKeyBit(key);
                    UpdateStick();
                    break;
                }
                if (key == keyIdPower) {
                    if (!soc->PowerButtonSupported()) break;
                    const auto* profile = deviceProfile5(soc->GetDeviceType());
                    if (profile && profile->frontend.available &&
                        ("|" + std::string(profile->frontend.buttons) + "|").find("|power|") == std::string::npos)
                        break;
                }
                if (key && !hardKeys[key]) {
                    hardKeys[key]=true;
                    if (pointerKey!=key && !builtInHolds[key]) soc->KeyDown(key);
                    redrawRequested=true;
                }
                break;
            }

            case SDL_KEYUP: {
                const auto scan = event.key.keysym.scancode;
                if (scan > SDL_SCANCODE_UNKNOWN && size_t(scan) < builtInHeld.size() && builtInHeld[scan]) {
                    const KeyId key = builtInHeld[scan];
                    builtInHeld[scan] = keyInvalid;
                    if (--builtInHolds[key] == 0 && !hardKeys[key] && pointerKey != key) soc->KeyUp(key);
                    redrawRequested = true;
                    break;
                }
                const auto code=irKeyboardCode(event.key.keysym.scancode);
                if (code && hostKeys[scan]) {
                    hostKeys[scan] = false;
                    if (--codeHolds[code] == 0) soc->KeyboardKey(code, false);
                    break;
                }
                enum KeyId key = mapKey(event.key.keysym.sym);
                if (stickKeyBit(key) && hasStick(controls)) {
                    stickKeys &= uint8_t(~stickKeyBit(key));
                    UpdateStick();
                    break;
                }
                if (key && hardKeys[key]) {
                    hardKeys[key]=false;
                    if (pointerKey!=key && !builtInHolds[key]) soc->KeyUp(key);
                    redrawRequested=true;
                }
                break;
            }

            // A game controller, for the Zodiac only. It is opened so SDL reports its buttons and axes.
            case SDL_CONTROLLERDEVICEADDED:
                if (controls.style == DeviceControlStyle::zodiac) SDL_GameControllerOpen(event.cdevice.which);
                break;

            case SDL_CONTROLLERDEVICEREMOVED:
                if (auto* controller = SDL_GameControllerFromInstanceID(event.cdevice.which)) SDL_GameControllerClose(controller);
                pads.erase(event.cdevice.which);
                if (lastStickPad == event.cdevice.which) lastStickPad = -1;
                UpdateGamepad();
                break;

            case SDL_CONTROLLERAXISMOTION: {
                GamepadState& one = pads[event.caxis.which];
                if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) one.leftX = event.caxis.value;
                else if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) one.leftY = event.caxis.value;
                else break;
                lastStickPad = event.caxis.which;
                UpdateGamepad();
                break;
            }

            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP: {
                GamepadState& pad = pads[event.cbutton.which];
                const bool down = event.type == SDL_CONTROLLERBUTTONDOWN;
                switch (event.cbutton.button) {
                    case SDL_CONTROLLER_BUTTON_A: pad.a = down; break;
                    case SDL_CONTROLLER_BUTTON_B: pad.b = down; break;
                    case SDL_CONTROLLER_BUTTON_X: pad.x = down; break;
                    case SDL_CONTROLLER_BUTTON_Y: pad.y = down; break;
                    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: pad.leftShoulder = down; break;
                    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: pad.rightShoulder = down; break;
                    case SDL_CONTROLLER_BUTTON_LEFTSTICK: pad.leftStick = down; break;
                    case SDL_CONTROLLER_BUTTON_BACK: pad.back = down; break;
                    case SDL_CONTROLLER_BUTTON_START: pad.start = down; break;
                    case SDL_CONTROLLER_BUTTON_GUIDE: pad.guide = down; break;
                    default: break;
                }
                UpdateGamepad();
                break;
            }

            case SDL_APP_WILLENTERBACKGROUND:
                ReleaseInputs();
                break;

            // Mobile systems only: the app is hidden or shown again.
            case SDL_APP_DIDENTERBACKGROUND:
                ReleaseInputs();
                background = true;
                break;

            case SDL_APP_DIDENTERFOREGROUND:
                background = false;
                padFocused = true;
                UpdateGamepad();
                redrawRequested = true;
                break;

            case SDL_WINDOWEVENT:
                if (event.window.event==SDL_WINDOWEVENT_FOCUS_LOST ||
                    event.window.event==SDL_WINDOWEVENT_HIDDEN ||
                    event.window.event==SDL_WINDOWEVENT_MINIMIZED) ReleaseInputs();
                if (event.window.event==SDL_WINDOWEVENT_FOCUS_GAINED ||
                    event.window.event==SDL_WINDOWEVENT_SHOWN ||
                    event.window.event==SDL_WINDOWEVENT_RESTORED) { padFocused = true; UpdateGamepad(); }
                if (event.window.event==SDL_WINDOWEVENT_SIZE_CHANGED) { ReleaseFingers(); ReleasePointer(); resized=true; }
                if (event.window.event==SDL_WINDOWEVENT_LEAVE) ReleasePointer();
                if (event.window.event==SDL_WINDOWEVENT_EXPOSED) redrawRequested=true;
                break;

        }
    }
}

bool SdlEventHandler::RedrawRequested() const { return redrawRequested; }

bool SdlEventHandler::QuitRequested() const { return quitRequested; }

void SdlEventHandler::ClearRedrawRequested() { redrawRequested = false; }

void SdlEventHandler::SetRotation(Rotation rotation) {
    ReleaseFingers(); ReleasePointer(); this->rotation = rotation; redrawRequested=true;
}
void SdlEventHandler::SetView(DeviceControlView controlView, float scale) {
    ReleaseFingers(); ReleasePointer(); this->controlView = controlView; this->scale = scale; redrawRequested=true;
}
void SdlEventHandler::SetLayout(const WindowLayout& layout, bool fitted) {
    ReleaseFingers(); ReleasePointer(); this->layout = layout; this->fitted = fitted;
    if (fitted) scale = 1;
    redrawRequested=true;
}
bool SdlEventHandler::TakeResized() { const bool was=resized; resized=false; return was; }
// A fitted window maps each pointer to the device pixel under its centre, so the rest
// works at scale 1.
void SdlEventHandler::MapPointer(int& x, int& y) const {
    if (!fitted) return;
    x = int(std::floor((x + 0.5f - layout.x) / layout.scaleX));
    y = int(std::floor((y + 0.5f - layout.y) / layout.scaleY));
}
int SdlEventHandler::ScreenWidth() const {
    return rotation==Rotation::landscape_90 || rotation==Rotation::landscape_270 ? height : width;
}
int SdlEventHandler::ScreenHeight() const {
    return rotation==Rotation::landscape_90 || rotation==Rotation::landscape_270 ? width : height;
}
bool SdlEventHandler::OnScreen(int x,int y) const {
    const int left=int(scale*deviceScreenLeft(controls,controlView));
    return x>=left && y>=0 && x<left+int(scale*ScreenWidth()) && y<int(scale*ScreenHeight());
}
void SdlEventHandler::ReleasePointer() {
    if (pointerStick) {
        pointerStick = false;
        pointerPressPending = false;
        pointerPosition = {};
        UpdateStick();
    }
    if (pointerKey) {
        if (!hardKeys[pointerKey] && !builtInHolds[pointerKey]) soc->KeyUp(pointerKey);
        pointerKey=keyInvalid; redrawRequested=true;
    }
    if (penDown) { soc->PenUp();penDown=false; }
    Capture(false);
}
void SdlEventHandler::Capture(bool on) {
    capture.store(on, std::memory_order_relaxed);
    if (directCapture) SDL_CaptureMouse(on ? SDL_TRUE : SDL_FALSE);
}
// Touch screens: each finger is the pen (one at most) or one held button. Track at most sixteen
// accepted contacts; excess contacts wait for a new down. SDL's touch-to-mouse copies are ignored.
void SdlEventHandler::HandleFinger(const SDL_TouchFingerEvent& event) {
    Finger* finger = nullptr;
    for (auto& item : fingers)
        if (item.active && item.touch == event.touchId && item.id == event.fingerId) finger = &item;
    int windowWidth = int(scale * deviceControlsWidth(controls,ScreenWidth(),controlView));
    int windowHeight = int(scale * (ScreenHeight() + deviceControlsHeight(controls,ScreenWidth(),controlView)));
    if (auto* window = SDL_GetWindowFromID(event.windowID)) SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    int x = int(std::floor(event.x * windowWidth)), y = int(std::floor(event.y * windowHeight));
    MapPointer(x, y);
    auto release = [&] {
        if (finger->key && --builtInHolds[finger->key] == 0 && !hardKeys[finger->key] && pointerKey != finger->key)
            soc->KeyUp(finger->key);
        if (finger->pen) soc->PenUp();
        const bool stickFinger = finger->stick;
        *finger = {}; redrawRequested = true;
        if (stickFinger) UpdateStick();
    };
    if (event.type == SDL_FINGERUP) {
        if (!finger) return;
        if (finger->stick && HoldStill(finger->pressPending, finger->downX, finger->downY, x, y)) TapStickPress();
        release();
        return;
    }
    if (event.type == SDL_FINGERDOWN) {
        if (finger) return;
        for (auto& item : fingers) if (!item.active) { finger = &item; break; }
        if (!finger) return;
        const bool pen = OnScreen(x,y);
        // One finger at a time holds the stick; it keeps it until it lifts, even off the well.
        if (!pen && deviceStickGrab(StickWell(), (x + 0.5f)/scale, (y + 0.5f)/scale)) {
            for (const auto& item : fingers) if (item.active && item.stick) return;
            const float downX = (x + 0.5f)/scale, downY = (y + 0.5f)/scale;
            const bool knob = deviceStickKnobAt(StickWell(), downX, downY);
            *finger = {true, false, true, event.touchId, event.fingerId, keyInvalid,
                       knob ? DeviceStick{0, 0, true} : TouchStick(x, y), knob, downX, downY};
            UpdateStick();
            return;
        }
        const KeyId key = x >= 0 && y >= 0 ? deviceControlAt(controls, ScreenWidth(), ScreenHeight(),
                                  int((x + 0.5f)/scale), int((y + 0.5f)/scale),controlView) : keyInvalid;
        if (!key && !pen) return;
        if (pen) {
            if (penDown) return;
            for (const auto& item : fingers) if (item.active && item.pen) return;
        }
        *finger = {true, pen, false, event.touchId, event.fingerId, key};
        if (key && ++builtInHolds[key] == 1 && !hardKeys[key] && pointerKey != key) soc->KeyDown(key);
        redrawRequested = true;
    }
    if (!finger || !finger->active) return;
    if (finger->stick) {
        if (HoldStill(finger->pressPending, finger->downX, finger->downY, x, y)) return;
        finger->position = TouchStick(x, y);
        UpdateStick();
        return;
    }
    if (finger->pen) {
        if (OnScreen(x,y)) soc->PenDown(RotateX(x,y), RotateY(x,y));
        else release();
    } else {
        // A button finger rolling onto another button lets go of the old key and presses the new
        // one; off any button it just lets go.
        const KeyId key = x >= 0 && y >= 0 ? deviceControlAt(controls, ScreenWidth(), ScreenHeight(),
                              int((x + 0.5f)/scale), int((y + 0.5f)/scale),controlView) : keyInvalid;
        if (key == finger->key) return;
        const KeyId old = finger->key;
        if (!key) { release(); return; }
        if (--builtInHolds[old] == 0 && !hardKeys[old] && pointerKey != old) soc->KeyUp(old);
        finger->key = key;
        if (++builtInHolds[key] == 1 && !hardKeys[key] && pointerKey != key) soc->KeyDown(key);
        redrawRequested = true;
    }
}
void SdlEventHandler::ReleaseFingers() {
    for (auto& finger : fingers) {
        if (!finger.active) continue;
        if (finger.key && --builtInHolds[finger.key] == 0 && !hardKeys[finger.key] && pointerKey != finger.key)
            soc->KeyUp(finger.key);
        if (finger.pen) soc->PenUp();
        finger = {};
    }
    UpdateStick();
    redrawRequested = true;
}

void SdlEventHandler::ReleaseInputs() {
    ReleaseFingers();
    ReleasePointer();
    soc->ReleaseKeyboard();
    stickKeys = 0;
    // The controllers' buttons are let go with the built-in holds below. Their sticks are only
    // taken off the device: the axes stay, so a stick still pushed is back when focus returns.
    for (auto& entry : pads) { GamepadState kept; kept.leftX = entry.second.leftX; kept.leftY = entry.second.leftY; entry.second = kept; }
    padFocused = false;
    padKeys = 0;
    padStick = {};
    UpdateStick();
    hostKeys.fill(false); codeHolds.fill(0);
    for (unsigned key=1;key<hardKeys.size();++key) {
        if (hardKeys[key] || builtInHolds[key]) soc->KeyUp(static_cast<KeyId>(key));
        hardKeys[key]=false;
    }
    builtInHolds.fill(0);
    builtInHeld.fill(keyInvalid);
    redrawRequested=true;
}
SDL_Rect SdlEventHandler::StickWell() const {
    return deviceStickRect(controls, ScreenWidth(), ScreenHeight(), controlView);
}
// x and y are window pixels after MapPointer, as for the keys.
DeviceStick SdlEventHandler::TouchStick(int x, int y) const {
    return deviceStickAt(StickWell(), (x + 0.5f) / scale, (y + 0.5f) / scale);
}
bool SdlEventHandler::HoldStill(bool& pending, float downX, float downY, int x, int y) const {
    if (pending && deviceStickDragged(StickWell(), downX, downY, (x + 0.5f)/scale, (y + 0.5f)/scale)) pending = false;
    return pending;
}
// The press goes down and up at once; holding it in needs F10. A key already held for it stays held.
void SdlEventHandler::TapStickPress() {
    if (!hardKeys[keyIdStickPress] && !builtInHolds[keyIdStickPress] && pointerKey != keyIdStickPress) {
        soc->KeyDown(keyIdStickPress);
        soc->KeyUp(keyIdStickPress);
    }
    redrawRequested = true;
}
void SdlEventHandler::UpdateStick() {
    DeviceStick now{};
    const Finger* held = nullptr;
    for (const auto& finger : fingers) if (finger.active && finger.stick) held = &finger;
    if (held) now = held->position;
    else if (pointerStick) now = pointerPosition;
    else if (padStick.held) now = padStick;
    else if (stickKeys) {
        float x = float((stickKeys & 8) != 0) - float((stickKeys & 4) != 0);
        float y = float((stickKeys & 2) != 0) - float((stickKeys & 1) != 0);
        const float length = std::hypot(x, y);
        if (length > 1) { x /= length; y /= length; }
        now = {x, y, true};
    }
    if (now == stick) return;
    stick = now;
    soc->AnalogStick(stick.x, stick.y);
    redrawRequested = true;
}
// A key the controller holds counts with the built-in holds, so a key held some other way too goes
// down once and comes up when the last of them lets go.
// A button is down if any controller holds it. The stick is the one moved last, or while that one
// is at rest or gone, the first controller with its stick pushed.
GamepadState SdlEventHandler::CombinedPads() const {
    GamepadState all;
    const GamepadState* stickPad = nullptr;
    if (auto it = pads.find(lastStickPad); it != pads.end() && (it->second.leftX || it->second.leftY)) stickPad = &it->second;
    for (const auto& entry : pads) {
        const GamepadState& one = entry.second;
        all.a |= one.a; all.b |= one.b; all.x |= one.x; all.y |= one.y;
        all.leftShoulder |= one.leftShoulder; all.rightShoulder |= one.rightShoulder; all.leftStick |= one.leftStick;
        all.back |= one.back; all.start |= one.start; all.guide |= one.guide;
        if (!stickPad && (one.leftX || one.leftY)) stickPad = &one;
    }
    if (stickPad) { all.leftX = stickPad->leftX; all.leftY = stickPad->leftY; }
    return all;
}
void SdlEventHandler::UpdateGamepad() {
    if (controls.style != DeviceControlStyle::zodiac) return;
    if (!padFocused) return;
    const GamepadInput input = zodiacGamepadInput(CombinedPads());
    for (unsigned key = 1; key < keyIdCount; ++key) {
        const DeviceKeyMask bit = deviceKeyBit(KeyId(key));
        if ((input.keys & bit) == (padKeys & bit)) continue;
        const bool heldElsewhere = hardKeys[key] || pointerKey == KeyId(key);
        if (input.keys & bit) {
            if (++builtInHolds[key] == 1 && !heldElsewhere) soc->KeyDown(KeyId(key));
        } else if (--builtInHolds[key] == 0 && !heldElsewhere) {
            soc->KeyUp(KeyId(key));
        }
        redrawRequested = true;
    }
    padKeys = input.keys;
    padStick = input.stick;
    UpdateStick();
}
DeviceKeyMask SdlEventHandler::PressedButtons() const {
    DeviceKeyMask pressed=pointerKey ? deviceKeyBit(pointerKey) : 0;
    for (unsigned key=1;key<hardKeys.size();++key) if(hardKeys[key] || builtInHolds[key])pressed|=deviceKeyBit(KeyId(key));
    return pressed;
}

// Map pixel centres before truncation so fractional scales keep taps on the drawn pixel.
int SdlEventHandler::RotateX(int x, int y) {
    x-=int(scale*deviceScreenLeft(controls,controlView));
    switch (rotation) {
        case Rotation::landscape_90:
            return int((scale * width - 0.5f - y) / scale);

        case Rotation::portrait_180:
            return int((scale * width - 0.5f - x) / scale);

        case Rotation::landscape_270:
            return int((y + 0.5f) / scale);

        default:
            return int((x + 0.5f) / scale);
    }
}

int SdlEventHandler::RotateY(int x, int y) {
    x-=int(scale*deviceScreenLeft(controls,controlView));
    switch (rotation) {
        case Rotation::landscape_90:
            return int((x + 0.5f) / scale);

        case Rotation::portrait_180:
            return int((scale * height - 0.5f - y) / scale);

        case Rotation::landscape_270:
            return int((scale * height - 0.5f - x) / scale);

        default:
            return int((y + 0.5f) / scale);
    }
}
