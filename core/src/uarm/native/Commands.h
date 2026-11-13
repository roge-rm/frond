#ifndef _CLI_UARM_COMMANDS_H_
#define _CLI_UARM_COMMANDS_H_

#include <functional>
#include <string>
#include <vector>

#include "DisplayChange.h"
#include "cputil.h"
#include "MainLoop.h"
#include "InputReplay.h"
#include "Rotation.h"
#include "SdlAudioDriver.h"
#include "SdlMicrophone.h"
#include "HostCamera.h"
#include "SoC.h"
#include "SerialHostRoute.h"

class HostSerial;
class HostInfrared;
class UsbHostBridge;
struct PxaUart;

namespace commands {
    struct Context {
        SoC* soc;
        MainLoop& mainLoop;
        SdlAudioDriver& audioDriver;
        Rotation rotation;
        InputReplay& replay;
        // When the input replay started, in replayClockUsec's time.
        uint64_t& replayStartedAt;
        // --emulated-time: input replay times are guest time.
        bool emulatedTime{false};
        int screenWidth;
        int screenHeight;
        HostInfrared* infrared{nullptr};
        std::string infraredError{};
        std::string bluetoothError{};
        SdlMicrophone* microphone{nullptr};
        HostCamera* camera{nullptr};      // the Zire 72 camera's host source, never session state
        HostSerial* serial{nullptr};      // live host endpoint, never session state
        SerialHostRoute serialRoute{};
        PxaUart* serialUart{nullptr};     // TX FFUART for serial-mode changes
        std::string serialError{};        // why the last endpoint could not open
        UsbHostBridge* usb{nullptr};      // live USB host bridge, never session state
        std::string usbError{};           // why the last USB endpoint could not open
        std::string sessionFolder{};      // machine folder, empty for a ROM start with no --work-folder
        std::string sessionPath{};        // the session file or folder loaded into sessionFolder, else empty
        // Queues a display change for the SDL thread; unset where there is no window to change.
        std::function<void(const DisplayChange&)> changeDisplay{};
        // The window's X11 id as "x11 ID" for the desktop app to embed, empty without one.
        std::function<std::string()> windowId{};
        // The fitted device's rectangle in the window as "x y width height windowWidth windowHeight", empty without one.
        std::function<std::string()> windowLayout{};
        // The Zodiac's speed over its last window: instructions per second of real time and the
        // whole figures line. 0 and empty on other devices and before the first window.
        uint64_t speedIps{0};
        std::string speedDetail{};
    };

    // The input replay's clock in us: guest time under --emulated-time, else host time.
    inline uint64_t replayClockUsec(const Context& ctx) {
        return ctx.emulatedTime ? ctx.soc->GetTime() / 1000 : timestampUsec();
    }

    void Register();
}  // namespace commands

#endif  // _CLI_UARM_COMMANDS_H_
