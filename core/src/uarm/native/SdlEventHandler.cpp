#include "SdlEventHandler.h"

#include <SDL.h>

#include <cmath>
#include <cstdlib>
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

    KeyId zaurusKeyboardKey(SDL_Scancode scan) {
        constexpr std::string_view letters[] = {
            "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
            "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z"
        };
        constexpr std::string_view digits[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"};
        std::string_view name;
        if (scan >= SDL_SCANCODE_A && scan <= SDL_SCANCODE_Z) name = letters[scan - SDL_SCANCODE_A];
        else if (scan >= SDL_SCANCODE_1 && scan <= SDL_SCANCODE_0) name = digits[scan - SDL_SCANCODE_1];
        else switch (scan) {
            case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: name = "enter"; break;
            case SDL_SCANCODE_ESCAPE: name = "cancel"; break;
            case SDL_SCANCODE_BACKSPACE: name = "backspace"; break;
            case SDL_SCANCODE_TAB: name = "tab"; break;
            case SDL_SCANCODE_SPACE: name = "space"; break;
            case SDL_SCANCODE_MINUS: name = "minus"; break;
            case SDL_SCANCODE_COMMA: case SDL_SCANCODE_SLASH: name = "comma"; break;
            case SDL_SCANCODE_PERIOD: name = "dot"; break;
            case SDL_SCANCODE_LSHIFT: name = "lshift"; break;
            case SDL_SCANCODE_RSHIFT: name = "rshift"; break;
            case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: name = "ctrl"; break;
            case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: name = "fn"; break;
            case SDL_SCANCODE_LANG1: case SDL_SCANCODE_LANG3: name = "kana"; break;
            case SDL_SCANCODE_LANG5: case SDL_SCANCODE_GRAVE: name = "zenhan"; break;
            case SDL_SCANCODE_F1: name = "calendar"; break;
            case SDL_SCANCODE_F2: name = "address"; break;
            case SDL_SCANCODE_F3: name = "mail"; break;
            case SDL_SCANCODE_F4: case SDL_SCANCODE_APPLICATION: name = "menu"; break;
            case SDL_SCANCODE_F6: case SDL_SCANCODE_HOME: name = "home"; break;
            case SDL_SCANCODE_F10: return keyIdPower;
            case SDL_SCANCODE_UP: name = "up"; break;
            case SDL_SCANCODE_DOWN: name = "down"; break;
            case SDL_SCANCODE_LEFT: name = "left"; break;
            case SDL_SCANCODE_RIGHT: name = "right"; break;
            case SDL_SCANCODE_PAGEUP: name = "exjogup"; break;
            case SDL_SCANCODE_PAGEDOWN: name = "exjogdown"; break;
            case SDL_SCANCODE_INSERT: name = "exok"; break;
            case SDL_SCANCODE_DELETE: name = "excancel"; break;
            default: return keyInvalid;
        }
        const int index = zaurusKeyIndex(name);
        return index < 0 ? keyInvalid : zaurusArtKey(zaurusArt().keys[index]);
    }

    KeyId zaurusControllerKey(const SDL_Keysym& key) {
        if (key.scancode != SDL_SCANCODE_UNKNOWN) return keyInvalid;
        switch (key.sym) {
            case SDLK_PAGEUP: return keyIdJogUp;
            case SDLK_PAGEDOWN: return keyIdJogDown;
            case SDLK_END: return keyIdJogPress;
            case SDLK_HOME: return keyIdBack;
            default: return mapKey(key.sym);
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

    // Enter is Select, or on a device with an analogue stick (the Zodiac, which has no Select) the stick's press.
    KeyId hostKey(SDL_Keycode keysym, const DeviceControlProfile& controls) {
        const KeyId key = mapKey(keysym);
        return key == keyIdSelect && hasStick(controls) ? keyIdStickPress : key;
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
    : SdlEventHandler(soc, deviceControls(soc->GetDeviceType(), controlView), scale, deviceDisplayConfiguration, rotation, controlView) {
    followsDevice = true;
    inputControls = &deviceControls(deviceArt(soc->GetDeviceType()), controlView.gamepad);
}

SdlEventHandler::SdlEventHandler(SoC* soc, const DeviceControlProfile& controls, float scale,
                                 DisplayConfiguration& deviceDisplayConfiguration, Rotation rotation, DeviceControlView controlView)
    : soc(soc), shownControls(&controls), inputControls(&controls), controlView(controlView), scale(scale), rotation(rotation) {
    if (const auto* profile = deviceProfile5(soc->GetDeviceType())) controllerMap = controllerDefaults(*profile);
    padAttachedSeen = soc->GamepadAttached();
    width = deviceDisplayConfiguration.width;
    height = deviceDisplayConfiguration.height + deviceDisplayConfiguration.graffitiHeight;
}

void SdlEventHandler::HandleEvents() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) HandleEvent(event);
    FollowGamepadAttach();
    ServiceHolds();
}

// The Zaurus wants Power held: the PROM's power-on waits about 2.75 s for it, so a tap holds it
// that long in guest time, as the desktop and Android toolbars' Power does.
constexpr uint64_t zaurusPowerHoldMs = 2750;

bool SdlEventHandler::TapZaurusPower(KeyId key) {
    if (key != keyIdPower || !controlView.zaurus || controlView.bare) return false;
    if (!powerTapHeld) {
        powerTapHeld = true;
        if (++builtInHolds[key] == 1 && !hardKeys[key] && pointerKey != key) soc->KeyDown(key);
    }
    powerTapAt = soc->GetTime();
    redrawRequested = true;
    return true;
}

void SdlEventHandler::ServiceHolds() {
    const uint64_t now = soc->GetTime(), seen = holdsSeenAt;
    holdsSeenAt = now;
    // A reset puts guest time back: it lets go of the timed holds, so Power isn't down while it boots.
    if (now < seen) {
        ReleaseTimedPower();
        return;
    }
    if (!powerTapHeld || now - powerTapAt < zaurusPowerHoldMs * 1000000) return;
    powerTapHeld = false;
    if (builtInHolds[keyIdPower] && --builtInHolds[keyIdPower] == 0 && !hardKeys[keyIdPower] && pointerKey != keyIdPower)
        soc->KeyUp(keyIdPower);
    redrawRequested = true;
}

bool SdlEventHandler::ReplayKey(KeyId key, bool down) {
    if (key != keyIdPower) return false;
    if (down) {
        if (!replayPower && ++builtInHolds[key] == 1 && !hardKeys[key] && pointerKey != key) soc->KeyDown(key);
        replayPower = true;
    } else if (replayPower) {
        replayPower = false;
        if (--builtInHolds[key] == 0 && !hardKeys[key] && pointerKey != key) soc->KeyUp(key);
    } else if (!builtInHolds[key] && !hardKeys[key] && pointerKey != key) {
        soc->KeyUp(key);
    }
    redrawRequested = true;
    return true;
}

void SdlEventHandler::ReleaseReplayKeys() {
    if (replayPower) ReplayKey(keyIdPower, false);
}

void SdlEventHandler::ReleaseTimedPower() {
    const unsigned timed = unsigned(powerTapHeld) + unsigned(replayPower);
    if (!timed) return;
    powerTapHeld = replayPower = false;
    builtInHolds[keyIdPower] -= std::min(builtInHolds[keyIdPower], timed);
    if (!builtInHolds[keyIdPower] && !hardKeys[keyIdPower] && pointerKey != keyIdPower) soc->KeyUp(keyIdPower);
    redrawRequested = true;
}

void SdlEventHandler::FollowGamepadAttach() {
    if (soc->GamepadAttached() == padAttachedSeen) return;
    padAttachedSeen = soc->GamepadAttached();
    UpdateGamepad();
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
                if (MovePen(event.button.x, event.button.y)) {
                    penDown = true;
                } else if (deviceStickGrab(StickWell(), (event.button.x + 0.5f)/scale, (event.button.y + 0.5f)/scale)) {
                    pointerStick = true;
                    pointerDownX = (event.button.x + 0.5f)/scale;
                    pointerDownY = (event.button.y + 0.5f)/scale;
                    pointerPressPending = deviceStickKnobAt(StickWell(), pointerDownX, pointerDownY);
                    pointerPosition = pointerPressPending ? DeviceStick{0, 0, true}
                                                          : TouchStick(event.button.x, event.button.y);
                    UpdateStick();
                } else if (event.button.x>=0 && event.button.y>=0) {
                    const KeyId key = ControlAt(event.button.x, event.button.y);
                    pointerKey = TapZaurusPower(key) ? keyInvalid : key;
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
                    if (ControlAt(event.motion.x, event.motion.y) != pointerKey)
                        ReleasePointer();
                    break;
                }
                if (penDown && !MovePen(event.motion.x, event.motion.y)) ReleasePointer();

                break;

            case SDL_KEYDOWN: {
                if (shortcutSink) {
                    if (const auto name = shortcutName(event.key.keysym.sym, event.key.keysym.mod, soc->KeyboardAttached(),
                                                       controlView.zaurus.has_value())) {
                        if (!event.key.repeat) shortcutSink(*name);
                        break;
                    }
                } else if (controlView.zaurus) {
                    if (event.key.keysym.scancode == SDL_SCANCODE_F11) {
                        if (!event.key.repeat) soc->SetSpitzLid(!soc->SpitzLidClosed(), false);
                        break;
                    }
                    if (event.key.keysym.scancode == SDL_SCANCODE_F12) {
                        if (!event.key.repeat) soc->SetSpitzLid(false, !soc->SpitzSwivelled());
                        break;
                    }
                }
                if (event.key.repeat) break;
                // Android's controller keys for the pad (KMOD_RESERVED): none once the pad is detached.
                if ((event.key.keysym.mod & KMOD_RESERVED) && !soc->GamepadAttached()) break;
                const auto scan = event.key.keysym.scancode;
                const auto code = irKeyboardCode(scan);
                if (scan > SDL_SCANCODE_UNKNOWN && size_t(scan) < builtInHeld.size() &&
                    (builtInHeld[scan] || hostKeys[scan])) break;
                if (soc->KeyboardAttached() && code) {
                    hostKeys[scan] = true;
                    if (++codeHolds[code] == 1) soc->KeyboardKey(code, true);
                    break;
                }
                KeyId builtIn = soc->GamepadAttached() ? clieGamepadHostKey(scan) : keyInvalid;
                if (!builtIn) builtIn = controlView.zaurus ? zaurusKeyboardKey(scan) : builtInKeyboardKey(*inputControls, scan);
                if (scan > SDL_SCANCODE_UNKNOWN && size_t(scan) < builtInHeld.size() && builtIn) {
                    if (!builtInHeld[scan]) {
                        builtInHeld[scan] = builtIn;
                        if (++builtInHolds[builtIn] == 1 && !hardKeys[builtIn] && pointerKey != builtIn)
                            soc->KeyDown(builtIn);
                        redrawRequested = true;
                    }
                    break;
                }
                const KeyId key = controlView.zaurus ? zaurusControllerKey(event.key.keysym) : hostKey(event.key.keysym.sym, *inputControls);
                if (key >= keyIdFunction && !hasKey((*inputControls), key)) break;
                if (stickKeyBit(key) && hasStick(*inputControls)) {
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
                const KeyId key = controlView.zaurus ? zaurusControllerKey(event.key.keysym) : hostKey(event.key.keysym.sym, *inputControls);
                if (stickKeyBit(key) && hasStick(*inputControls)) {
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

            // A game controller, for the Zodiac, the CLIÉ game controller and a controller map with keys. It is
            // opened so SDL reports its buttons and axes.
            case SDL_CONTROLLERDEVICEADDED:
                ScanControllers();
                break;

            case SDL_CONTROLLERDEVICEREMOVED:
                ScanControllers();
                pads.erase(event.cdevice.which);
                if (lastStickPad == event.cdevice.which) lastStickPad = -1;
                UpdateGamepad();
                break;

            case SDL_CONTROLLERAXISMOTION: {
                PadReading& one = pads[event.caxis.which];
                const int16_t value = event.caxis.value;
                PadAxis moved = PadAxis::none;
                switch (event.caxis.axis) {
                    case SDL_CONTROLLER_AXIS_LEFTX: one.leftX = value; moved = PadAxis::leftX; break;
                    case SDL_CONTROLLER_AXIS_LEFTY: one.leftY = value; moved = PadAxis::leftY; break;
                    case SDL_CONTROLLER_AXIS_RIGHTX: one.rightX = value; moved = PadAxis::rightX; break;
                    case SDL_CONTROLLER_AXIS_RIGHTY: one.rightY = value; moved = PadAxis::rightY; break;
                    case SDL_CONTROLLER_AXIS_TRIGGERLEFT: one.leftTrigger = value; moved = PadAxis::leftTrigger; break;
                    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: one.rightTrigger = value; moved = PadAxis::rightTrigger; break;
                    default: break;
                }
                if (controllerMap.usesAxis(moved)) lastStickPad = event.caxis.which;
                UpdateGamepad();
                break;
            }

            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP: {
                // PadKey numbers the buttons as SDL does.
                static_assert(SDL_CONTROLLER_BUTTON_A == int(PadKey::a) && SDL_CONTROLLER_BUTTON_BACK == int(PadKey::back) &&
                              SDL_CONTROLLER_BUTTON_RIGHTSHOULDER == int(PadKey::rightShoulder) &&
                              SDL_CONTROLLER_BUTTON_DPAD_RIGHT == int(PadKey::dpadRight), "SDL's button numbers");
#if SDL_VERSION_ATLEAST(2, 0, 14)
                static_assert(SDL_CONTROLLER_BUTTON_TOUCHPAD == int(PadKey::touchpad), "SDL's button numbers");
#endif
                PadReading& pad = pads[event.cbutton.which];
                const int button = event.cbutton.button;
                if (button < 0 || button >= padButtonCount) break;
                if (event.type == SDL_CONTROLLERBUTTONDOWN) pad.buttons |= uint32_t(1) << button;
                else pad.buttons &= ~(uint32_t(1) << button);
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
                    event.window.event==SDL_WINDOWEVENT_MINIMIZED) {
                    ReleaseInputs(event.window.event==SDL_WINDOWEVENT_FOCUS_LOST);
                    // Focus moved to the desktop app's own window: controllers still count.
                    if (appActive && event.window.event==SDL_WINDOWEVENT_FOCUS_LOST) padFocused = true;
                }
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
    if (followsDevice) {
        shownControls = &deviceControls(soc->GetDeviceType(), controlView);
        inputControls = &deviceControls(deviceArt(soc->GetDeviceType()), controlView.gamepad);
    }
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
    const auto screen = ScreenRect();
    const SDL_Point point{x, y};
    return SDL_PointInRect(&point, &screen);
}
SDL_Rect SdlEventHandler::ScreenRect() const {
    if (controlView.zaurus && !controlView.bare)
        return zaurusDeviceLayout(ScreenWidth(), ScreenHeight(), *controlView.zaurus, scale).screen;
    return {int(scale * deviceScreenLeft(*shownControls, controlView)),
            int(scale * deviceScreenTop(*shownControls, controlView)),
            int(scale * ScreenWidth()), int(scale * ScreenHeight())};
}
KeyId SdlEventHandler::ControlAt(int x, int y, KeyId* also) const {
    if (also) *also = keyInvalid;
    if (x < 0 || y < 0) return keyInvalid;
    if (controlView.zaurus && !controlView.bare) {
        const int index = zaurusKeyAt(zaurusDeviceLayout(ScreenWidth(), ScreenHeight(), *controlView.zaurus, scale), x, y);
        return index < 0 ? keyInvalid : zaurusArtKey(zaurusArt().keys[index]);
    }
    return deviceControlAt(*shownControls, ScreenWidth(), ScreenHeight(),
                           int((x + 0.5f) / scale), int((y + 0.5f) / scale), controlView, also);
}
int SdlEventHandler::TabletKeyAt(int x, int y) const {
    if (!controlView.zaurus || controlView.bare) return -1;
    const auto layout = zaurusDeviceLayout(ScreenWidth(), ScreenHeight(), *controlView.zaurus, scale);
    const int index = zaurusKeyAt(layout, x, y);
    if (index < 0 || zaurusArt().keys[index].part != ZaurusPart::icons) return -1;
    return index - zaurusKeyIndex("icon-dictionary");
}
bool SdlEventHandler::MovePen(int x, int y) {
    if (OnScreen(x, y)) {
        soc->PenDown(RotateX(x, y), RotateY(x, y));
        return true;
    }
    const int tabletKey = TabletKeyAt(x, y);
    return tabletKey >= 0 && soc->SpitzTabletKeyDown(unsigned(tabletKey));
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
    const auto size = deviceControlSize(*shownControls, ScreenWidth(), ScreenHeight(), controlView);
    int windowWidth = int(scale * size.x), windowHeight = int(scale * size.y);
    if (auto* window = SDL_GetWindowFromID(event.windowID)) SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    int x = int(std::floor(event.x * windowWidth)), y = int(std::floor(event.y * windowHeight));
    MapPointer(x, y);
    auto press = [&](KeyId key) {
        if (key && ++builtInHolds[key] == 1 && !hardKeys[key] && pointerKey != key) soc->KeyDown(key);
    };
    auto unpress = [&](KeyId key) {
        if (key && --builtInHolds[key] == 0 && !hardKeys[key] && pointerKey != key) soc->KeyUp(key);
    };
    auto keysAt = [&](int x, int y, KeyId& also) {
        return ControlAt(x, y, &also);
    };
    auto release = [&] {
        unpress(finger->key);
        unpress(finger->also);
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
        const bool pen = OnScreen(x, y) || TabletKeyAt(x, y) >= 0;
        // One finger at a time holds the stick; it keeps it until it lifts, even off the well.
        if (!pen && deviceStickGrab(StickWell(), (x + 0.5f)/scale, (y + 0.5f)/scale)) {
            for (const auto& item : fingers) if (item.active && item.stick) return;
            const float downX = (x + 0.5f)/scale, downY = (y + 0.5f)/scale;
            const bool knob = deviceStickKnobAt(StickWell(), downX, downY);
            *finger = {true, false, true, event.touchId, event.fingerId, keyInvalid, keyInvalid,
                       knob ? DeviceStick{0, 0, true} : TouchStick(x, y), knob, downX, downY};
            UpdateStick();
            return;
        }
        KeyId also = keyInvalid;
        const KeyId key = keysAt(x, y, also);
        if (!key && !pen) return;
        // The tap holds Power by itself; the finger isn't tracked.
        if (!pen && TapZaurusPower(key)) return;
        if (pen) {
            if (penDown) return;
            for (const auto& item : fingers) if (item.active && item.pen) return;
            also = keyInvalid;
        }
        *finger = {true, pen, false, event.touchId, event.fingerId, key, also};
        press(key);
        press(also);
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
        if (!MovePen(x, y)) release();
    } else {
        // A button finger rolling onto another button lets go of the old key and presses the new
        // one; off any button it just lets go. On the pad's arrows it slides through diagonals.
        KeyId also = keyInvalid;
        const KeyId key = keysAt(x, y, also);
        if (key == finger->key && also == finger->also) return;
        if (!key) { release(); return; }
        // Rolling onto the Zaurus Power taps it: the finger lets go and the tap holds it.
        if (controlView.zaurus && !controlView.bare && key == keyIdPower) {
            TapZaurusPower(key);
            release();
            return;
        }
        const KeyId oldKey = finger->key, oldAlso = finger->also;
        finger->key = key;
        finger->also = also;
        // Press the new keys before letting go of the old so a key kept across the move stays down.
        if (key != oldKey && key != oldAlso) press(key);
        if (also && also != oldKey && also != oldAlso) press(also);
        if (oldKey != key && oldKey != also) unpress(oldKey);
        if (oldAlso && oldAlso != key && oldAlso != also) unpress(oldAlso);
        redrawRequested = true;
    }
}
void SdlEventHandler::ReleaseFingers() {
    for (auto& finger : fingers) {
        if (!finger.active) continue;
        for (const KeyId key : {finger.key, finger.also})
            if (key && --builtInHolds[key] == 0 && !hardKeys[key] && pointerKey != key) soc->KeyUp(key);
        if (finger.pen) soc->PenUp();
        finger = {};
    }
    UpdateStick();
    redrawRequested = true;
}

void SdlEventHandler::SetAppActive(bool active) {
    appActive = active;
    if (active) { padFocused = true; UpdateGamepad(); }
    else if (!SDL_GetKeyboardFocus()) ReleaseInputs(true);
}

void SdlEventHandler::ReleaseInputs(bool keepTimedPower) {
    if (!keepTimedPower) ReleaseTimedPower();
    ReleaseFingers();
    ReleasePointer();
    soc->ReleaseKeyboard();
    stickKeys = 0;
    // The controllers' buttons are let go with the built-in holds below. Their sticks are only
    // taken off the device: the axes stay, so a stick still pushed is back when focus returns.
    for (auto& entry : pads) {
        PadReading kept;
        for (const PadAxis axis : {controllerMap.stickX.axis, controllerMap.stickY.axis}) {
            switch (axis) {
                case PadAxis::leftX: kept.leftX = entry.second.leftX; break;
                case PadAxis::leftY: kept.leftY = entry.second.leftY; break;
                case PadAxis::rightX: kept.rightX = entry.second.rightX; break;
                case PadAxis::rightY: kept.rightY = entry.second.rightY; break;
                case PadAxis::leftTrigger: kept.leftTrigger = entry.second.leftTrigger; break;
                case PadAxis::rightTrigger: kept.rightTrigger = entry.second.rightTrigger; break;
                default: break;
            }
        }
        entry.second = kept;
    }
    padFocused = false;
    padKeys = 0;
    padStick = {};
    padDirections = {};
    UpdateStick();
    hostKeys.fill(false); codeHolds.fill(0);
    const unsigned timedPower = unsigned(powerTapHeld) + unsigned(replayPower);
    for (unsigned key=1;key<hardKeys.size();++key) {
        if ((hardKeys[key] || builtInHolds[key]) && !(key == keyIdPower && timedPower)) soc->KeyUp(static_cast<KeyId>(key));
        hardKeys[key]=false;
    }
    builtInHolds.fill(0);
    builtInHolds[keyIdPower] = timedPower;
    builtInHeld.fill(keyInvalid);
    redrawRequested=true;
}
SDL_Rect SdlEventHandler::StickWell() const {
    return deviceStickRect((*shownControls), ScreenWidth(), ScreenHeight(), controlView);
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
bool SdlEventHandler::Rumble(bool on, uint64_t nowUsec) {
    controllers->SetRumble(on);
    if (controllers == &ownControllers) controllers->Service(nowUsec);
    return controllers->CanRumble();
}

void SdlEventHandler::SetHostControllers(HostControllers* shared) {
    controllers = shared ? shared : &ownControllers;
    controllers->SetWanted(ControllersWanted());
}

void SdlEventHandler::ScanControllers() {
    controllers->SetWanted(ControllersWanted());
    controllers->RequestScan();
    if (controllers == &ownControllers) controllers->ServiceScan();
}

bool SdlEventHandler::ControllersWanted() const {
    return inputControls->style == DeviceControlStyle::zodiac || soc->GamepadSupported() || !controllerMap.keys.empty();
}
// A map with keys on a device that had none opens the controllers already connected.
void SdlEventHandler::SetControllerMap(const ControllerMap& map) {
    controllerMap = map;
    if (ControllersWanted()) ScanControllers();
    else controllers->SetWanted(false);
    UpdateGamepad();
}
// A controller key is held if any controller holds it. The Zodiac's stick (the map's stick axes) is the one moved last, or while that one is at rest or gone, the first controller with it pushed.
// A key the controller holds counts with the built-in holds, so a key held some other way too goes
// down once and comes up when the last of them lets go.
void SdlEventHandler::UpdateGamepad() {
    const bool zodiac = inputControls->style == DeviceControlStyle::zodiac;
    if (!zodiac && controllerMap.keys.empty() && !padKeys) return;
    if (!padFocused) return;
    PadKeyMask held = 0;
    int16_t stickX = 0, stickY = 0;
    auto pushed = [&](const PadReading& pad) {
        int16_t x, y;
        padStickAxes(pad, controllerMap, x, y);
        if (!x && !y) return false;
        stickX = x;
        stickY = y;
        return true;
    };
    bool found = false;
    if (auto it = pads.find(lastStickPad); it != pads.end()) found = pushed(it->second);
    for (auto& entry : pads) {
        held |= padKeysHeld(entry.second);
        if (!found) found = pushed(entry.second);
    }
    const GamepadInput input = gamepadInput(controllerMap, held, stickX, stickY,
                                            zodiac, soc->GamepadAttached(), &padDirections);
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
    const auto screen = ScreenRect();
    x -= screen.x;
    y -= screen.y;
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
    const auto screen = ScreenRect();
    x -= screen.x;
    y -= screen.y;
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
