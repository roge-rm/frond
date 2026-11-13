#include <stdexcept>
#pragma GCC diagnostic ignored "-Wmultichar"

#include <SDL.h>
#include <SDL_image.h>
#include <pthread.h>
#include <csignal>
#include <unistd.h>

#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <fstream>
#include <iterator>

#include "Cli.h"
#include "Commands.h"
#include "DesktopControl.h"
#include "WindowId.h"
#include "CardSwap.h"
#include "CardTool.h"
#include "FileUtil.h"
#include "FrondPerformance.h"
#include "HostSerial.h"
#include "HostInfrared.h"
#include "UsbHostBridge.h"
#include "Logging.h"
#include "MainLoop.h"
#include "InputReplay.h"
#include "RenderThread.h"
#include "Rotation.h"
#include "ZodiacSpeed.h"

#ifdef __ANDROID__
// In the app (android_main.cpp): the guest OS turned its screen, in SysGetOrientation's numbers. Guest thread.
void frondGuestOrientationChanged(int orientation);
#endif
#include "SdlAudioDriver.h"
#include "SdlMicrophone.h"
#include "SdlEventHandler.h"
#include "DeviceControls.h"
#include "DisplayChange.h"
#include "SdlRenderer.h"
#include "WindowLayout.h"
#include "WindowScale.h"
#include "argparse.h"
#include "audio_queue.h"
#include "buffer.h"
#include "cputil.h"
#include "device.h"
#include "deviceSonyTG50.h"
#include "device_configuration.h"
#include "device_type5.h"
#include "md5.h"
#include "rom_info5.h"
#include "sdcard.h"
#include "session/session_file5.h"
#include "soc_PXA.h"
#include "soc_pv.h"
#include "soc_IMX1.h"
#include "CPU.h"
#include "MachineSession.h"
#include "MsProCards.h"
#include "MemoryStickImage.h"

#include <filesystem>

using namespace std;

struct Options {
    string norOrSession;
    optional<string> nand;
    optional<string> sd;
    optional<unsigned int> gdbPort;
    optional<unsigned int> mips; // Unset runs at the device's Original MIPS.
    bool disableAudio;
    unsigned int audioPeriod;
    optional<string> script;
    optional<unsigned int> ramSize;
    bool smallWindow;
    optional<float> windowScale;
    bool smoothLcd;
    bool hideKeyboard;
    bool hideJogControls;
    optional<JogPlacement> jogPlacement;
    WindowFit windowFit;
    uint32_t windowBackground;
    optional<DisplayMode> displayMode;
    optional<string> device;
    bool experimentalFastAudio;
    bool lowLatencyQueue;
    optional<unsigned int> runFor;
    optional<string> framebufferOutput;
    optional<string> inputReplay;
    bool emulatedTime;  // --emulated-time
    optional<string> desktopSocket;
    optional<string> saveOnDisconnect;
    optional<string> workFolder;
    optional<string> serial;
    optional<string> infrared;
    optional<string> usb;
    optional<string> bluetooth;
    bool cardSwap;                  // the frontend changed the card files while the machine was off
    optional<string> cardIdentity;  // the card put in: 64 hex digits
    bool cardProtected;
    bool fastEngine;  // --engine fast
    bool viewFollowsWindow;  // --view-turn window
};

namespace {

// Set when --engine fast was asked for and this build has no Fast engine for the ARM9 machines.
bool gFastEngineMissing = false;
    // The session has a Memory Stick Pro, opened into the SD card store by applySession.
    bool msProCard = false;
    // Under --card-swap, a slot that holds another card than the one saved, the same card changed
    // elsewhere, or none. It resumes ejected and the card it holds now goes in once the guest has
    // run swapInsertDelay awake, as a hot swap would. The stick's write protection is set from
    // `msProtect`.
    bool sdSwapped = false, msSwapped = false, msProtect = false;
    // A v9 session's NAND, attached from nand.frondnand once the SoC is made. A NAND in RAM came
    // from malloc (a session) or new[] (--nand or a blank one), freed that way once a save moves
    // it onto a file.
    std::optional<frondMachine::Media> nandMedia;
    bool nandFromMalloc = false;
    // Guest time awake between the eject and the insert, so the card driver handles the removal
    // first. A guest the eject doesn't wake gets the card after swapInsertLimit.
    constexpr uint64_t swapInsertDelay = 500000000, swapInsertLimit = 5000000000;  // ns
    constexpr size_t AUDIO_QUEUE_SIZE = 44100 / MAIN_LOOP_FPS * 10;

    uint32_t ramSizeFromMemorySize(uint32_t memorySize) {
        return memorySize ? (1u << (31 - __builtin_clz(memorySize))) : 0;
    }

    int windowWidth(DisplayConfiguration& displayConfiguration, Rotation rotation) {
        switch (rotation) {
            case Rotation::landscape_90:
            case Rotation::landscape_270:
                return displayConfiguration.height + displayConfiguration.graffitiHeight;

            default:
                return displayConfiguration.width;
        }
    }

    int windowHeight(DisplayConfiguration& displayConfiguration, Rotation rotation) {
        switch (rotation) {
            case Rotation::landscape_90:
            case Rotation::landscape_270:
                return displayConfiguration.width;

            default:
                return displayConfiguration.height + displayConfiguration.graffitiHeight;
        }
    }

    SDL_Window* initSdl(const char* title, DisplayConfiguration& displayConfiguration, float& scale, Rotation rotation,
                        const DeviceControlProfile& controls, const DeviceControlView& controlView, bool resizable) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO) < 0) {
            cerr << "could not initialize SDL: " << SDL_GetError() << endl;
            return nullptr;
        }
        // Game controllers are for the desktop, and it runs without them if SDL can't start them.
        // On Android the app maps them to keys itself.
#ifndef __ANDROID__
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0) cerr << "no game controllers: " << SDL_GetError() << endl;
#endif

        IMG_Init(IMG_INIT_PNG);

        atexit(SDL_Quit);

        SDL_Rect usable{};
        if (scale == 0) {
            scale = SDL_GetDisplayUsableBounds(0, &usable) == 0
                ? fittingWindowScale(deviceControlsWidth(controls, windowWidth(displayConfiguration, rotation), controlView),
                                     windowHeight(displayConfiguration, rotation) + deviceControlsHeight(controls,windowWidth(displayConfiguration,rotation),controlView),
                                     usable.w, usable.h)
                : 1.f;
        }
        auto window = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                       scale * deviceControlsWidth(controls, windowWidth(displayConfiguration, rotation), controlView),
                                       scale * (windowHeight(displayConfiguration, rotation)+deviceControlsHeight(controls,windowWidth(displayConfiguration,rotation),controlView)),
                                       SDL_WINDOW_ALLOW_HIGHDPI | (resizable ? SDL_WINDOW_RESIZABLE : 0));

        return window;
    }

    void sdlResizeWindow(SDL_Window* window, DisplayConfiguration& displayConfiguration, float scale,
                         Rotation rotation, const DeviceControlProfile& controls, const DeviceControlView& controlView) {
        SDL_SetWindowSize(window, scale * deviceControlsWidth(controls, windowWidth(displayConfiguration, rotation), controlView),
                          scale * (windowHeight(displayConfiguration, rotation)+deviceControlsHeight(controls,windowWidth(displayConfiguration,rotation),controlView)));
    }

    void copy(Buffer& buffer, size_t size, const void* data) {
        if (size == 0 || data == nullptr) {
            buffer.data = nullptr;
            buffer.size = 0;

            return;
        }

        buffer.data = malloc(size);
        buffer.size = size;

        memcpy(buffer.data, data, size);
    }

    bool applySession(const frondSession::Contents& contents, const std::filesystem::path& folder,
                      Buffer& nor, Buffer& nand, Buffer& ram, Buffer& savestate, uint32_t& ramSize,
                      RomInfo5& romInfo, optional<DisplayMode>& displayMode, DeviceType5 requestedDevice,
                      DeviceType5& sessionDevice, bool& embeddedCard, const Options& options) {
        const auto* profile = deviceProfileNamed5(contents.deviceName.c_str());
        if (!profile || profile->deviceType == deviceTypeInvalid) {
            cerr << "this session is for " << contents.deviceName << ", which Frond can't run" << endl;
            return false;
        }
        if (deviceProfileRuns68k5(profile->deviceType)) {
            cerr << "this session is for " << contents.deviceName << ", which runs in frond-68k" << endl;
            return false;
        }
        sessionDevice = profile->deviceType;
        if (requestedDevice != deviceTypeInvalid && sessionDevice != requestedDevice) {
            cerr << "--device " << deviceName5(requestedDevice) << " doesn't match the session's device "
                 << profile->name << endl;
            return false;
        }
        if (options.nand) {
            cerr << "separate NAND image cannot be used with session file" << endl;
            return false;
        }
        ramSize = contents.ramSize;
        if (ramSize == 0) ramSize = ramSizeFromMemorySize(contents.memory.size());
        displayMode = static_cast<DisplayMode>(contents.displayMode);
        copy(nor, contents.nor.size(), contents.nor.data());
        copy(nand, contents.nand.size(), contents.nand.data());
        nandFromMalloc = true;
        for (const auto& media : contents.media) {
            if (media.kind != frondMachine::kindNand) continue;
            if (nandMedia || nand.data) {
                cerr << "session NAND is described twice" << endl;
                return false;
            }
            nandMedia = media;
            nand.size = media.physicalSize;
        }
        copy(ram, contents.memory.size(), contents.memory.data());
        copy(savestate, contents.savestate.size(), contents.savestate.data());
        bool hasSd = false;
        for (const auto& media : contents.media)
            if (media.kind == frondMachine::kindSd) hasSd = true;
        if (hasSd) {
            if (profile->sdMax == 0) {
                cerr << "session SD card is not supported by device profile" << endl;
                return false;
            }
            if (options.sd) {
                cerr << "session already contains an SD card" << endl;
                return false;
            }
            for (const auto& media : contents.media)
                if (media.kind == frondMachine::kindSd &&
                    !deviceProfileSdSizeValid5(profile->deviceType, media.physicalSize)) {
                    cerr << "session SD card is not supported by device profile" << endl;
                    return false;
                }
            if (!frondNative::attachFolderMedia(contents, folder)) {
                cerr << "session SD sidecar is missing or unreadable" << endl;
                return false;
            }
            embeddedCard = true;
        }
        // A classic stick's size must be one the device takes.
        for (const auto& media : contents.media) {
            if (media.kind != frondMachine::kindMs) continue;
            if (media.logicalSize & 0xfffff || !deviceProfileMsSizeValid5(profile->deviceType, media.logicalSize >> 20)) {
                cerr << "session Memory Stick is not supported by device profile" << endl;
                return false;
            }
        }
        // A Memory Stick Pro opens into the SD card store; the controller takes it after the SoC is made.
        for (const auto& media : contents.media) {
            if (media.kind != frondMachine::kindMsPro) continue;
            if (hasSd || !deviceProfileMsProSizeValid5(profile->deviceType, media.physicalSize >> 20)) {
                cerr << "session Memory Stick Pro is not supported by device profile" << endl;
                return false;
            }
            if (!frondNative::attachFolderMedia(contents, folder)) {
                cerr << "session Memory Stick Pro sidecar is missing or unreadable" << endl;
                return false;
            }
            embeddedCard = true;
            msProCard = true;
        }
        // What the slots carry outside the card store, kept to the next save.
        for (const auto& media : contents.media) {
            if (media.kind == frondMachine::kindSd) frondNative::slotCards().sdProtected = media.writeProtected;
            if (media.kind == frondMachine::kindMs) frondNative::msOpened(media.identity, media.contentGeneration);
            if (media.kind == frondMachine::kindMs || media.kind == frondMachine::kindMsPro) msProtect = media.writeProtected;
        }
        sdSwapped = contents.sdSwapped;
        msSwapped = contents.msSwapped;
        romInfo = RomInfo5(nor.data, nor.size);
        if (sessionDevice != deviceTypeTX && romInfo.GetDeviceType() != sessionDevice) {
            cerr << "session device profile does not match ROM" << endl;
            return false;
        }
        return true;
    }

    bool readSession(const Options& options, Buffer& nor, Buffer& nand, Buffer& ram,
                     Buffer& savestate, uint32_t& ramSize, RomInfo5& romInfo,
                     optional<DisplayMode>& displayMode, DeviceType5 requestedDevice,
                     DeviceType5& sessionDevice, bool& embeddedCard, std::string& sessionFolder) {
        const std::filesystem::path path(options.norOrSession);
        frondSession::Contents contents;
        std::filesystem::path folder;
        const std::filesystem::path work = options.workFolder ? *options.workFolder : std::string();
        frondSession::CardSwap swap;
        if (options.cardIdentity) {
            frondNative::CardIdentity identity{};
            if (!frondNative::parseIdentityHex(*options.cardIdentity, identity)) {
                cerr << "--card-identity takes the card's identity as 64 hex digits" << endl;
                return false;
            }
            swap.identityGiven = true;
            swap.identity = identity;
        }
        swap.writeProtected = options.cardProtected;
        if (frondSession::loadPath(path, contents, folder, frondSession::msPhysicalBytes, work,
                                   options.cardSwap ? &swap : nullptr)) {
            sessionFolder = folder.string();
            return applySession(contents, folder, nor, nand, ram, savestate, ramSize, romInfo,
                                displayMode, requestedDevice, sessionDevice, embeddedCard, options);
        }

        // A folder that didn't load is not read as a file. When it holds a card file, that is the
        // likely cause: a card the session doesn't describe.
        std::error_code folderEc;
        if (std::filesystem::is_directory(path, folderEc)) {
            bool cardFile = false;
            for (const auto& entry : std::filesystem::directory_iterator(path, folderEc)) {
                const auto extension = entry.path().extension().string();
                if (extension == ".frondsd" || extension == ".frondms" || extension == ".frondmsp") cardFile = true;
            }
            cerr << "session folder " << path.string() << " did not load"
                 << (cardFile ? "; a card file in it must be one the session has" : "") << endl;
            return false;
        }

        size_t norOrSessionLen{0};
        unique_ptr<uint8_t[]> norOrSessionData;
        if (!util::ReadFile(options.norOrSession, norOrSessionData, norOrSessionLen)) return false;

        if (SessionFile5::IsSessionFile(norOrSessionLen, norOrSessionData.get())) {
            const auto* bytes = norOrSessionData.get();
            const uint32_t version =
                uint32_t(bytes[4]) | (uint32_t(bytes[5]) << 8) | (uint32_t(bytes[6]) << 16) |
                (uint32_t(bytes[7]) << 24);
            if (version < SessionFile5::kVersionV6) {
                SessionFile5 sessionFile;
                sessionFile.Deserialize(norOrSessionLen, bytes);
                return false;
            }
            cerr << "invalid session image" << endl;
            return false;
        } else {
            romInfo = RomInfo5(norOrSessionData.get(), norOrSessionLen);
            // Devices without a card header are only started by name.
            if (!romInfo.IsValid() && (requestedDevice == deviceTypeInvalid ||
                                       deviceProfile5(requestedDevice)->detect != DeviceDetect5::norSize)) {
                cerr << "provided file is neither a supported ROM nor a session image" << endl;
                return false;
            }

            size_t nandLen{0};
            unique_ptr<uint8_t[]> nandData;

            if (options.nand) {
                if (!util::ReadFile(options.nand, nandData, nandLen)) return false;
            } else {
                nandLen = deviceConfigurationGetNandSize(romInfo.GetDeviceType());
                nandData = nandLen > 0 ? make_unique<uint8_t[]>(nandLen) : nullptr;
                memset(nandData.get(), 0xff, nandLen);
            }

            ramSize = 0;

            nor.size = norOrSessionLen;
            nor.data = norOrSessionData.release();

            nand.size = nandLen;
            nand.data = nandData.release();

            ram.size = savestate.size = 0;
            ram.data = savestate.data = nullptr;
        }

        return true;
    }

    bool run(const Options& requested) {
        Options options = requested;
        if (!options.cardSwap && (options.cardIdentity || options.cardProtected)) {
            cerr << "--card-identity and --card-protected go with --card-swap" << endl;
            return false;
        }
        if (options.gdbPort && (*options.gdbPort == 0 || *options.gdbPort > 65535)) {
            cerr << "GDB port must be 1..65535" << endl;
            return false;
        }

        if (options.framebufferOutput && !options.runFor) {
            cerr << "--framebuffer-output requires --run-for" << endl;
            return false;
        }
        if (options.runFor && (options.script || *options.runFor == 0)) {
            cerr << "--run-for requires a positive duration and cannot be combined with --script" << endl;
            return false;
        }
        if (options.desktopSocket && (options.runFor || options.script)) {
            cerr << "--desktop-socket cannot be combined with --run-for or --script" << endl;
            return false;
        }
        if (options.saveOnDisconnect && (!options.desktopSocket || options.saveOnDisconnect->empty() ||
                                         options.saveOnDisconnect->front() != '/')) {
            cerr << "--save-on-disconnect takes an absolute path and needs --desktop-socket" << endl;
            return false;
        }
        if (options.saveOnDisconnect) {
            string error;
            if (!LockSessionForCore(*options.saveOnDisconnect, error)) {
                cerr << error << endl;
                return false;
            }
        }
        HostSerial::Config serialConfig;
        if (options.serial && !HostSerial::Parse(*options.serial, serialConfig)) {
            cerr << "--serial must be off, pty, tcp:PORT (PORT 0..65535) or unix:PATH" << endl;
            return false;
        }
        HostSerial::Config infraredConfig;
        const bool infraredPhone = options.infrared && *options.infrared == "phone";
        const bool infraredPrinter = options.infrared && *options.infrared == "printer";
        if (options.infrared && !infraredPhone && !infraredPrinter && !HostSerial::Parse(*options.infrared,infraredConfig)) {
            cerr << "--irda must be off, pty, tcp:PORT, unix:PATH, phone or printer" << endl; return false;
        }
        HostSerial::Config usbConfig;
        if (options.usb && !HostSerial::Parse(*options.usb, usbConfig)) {
            cerr << "--usb must be off, pty, tcp:PORT (PORT 0..65535) or unix:PATH" << endl;
            return false;
        }
        if (options.audioPeriod < 64 || options.audioPeriod > 8192) {
            cerr << "--audio-period must be 64..8192 frames" << endl;
            return false;
        }
        if (options.mips && *options.mips == 0) {
            cerr << "MIPS must be finite" << endl;
            return false;
        }

        DeviceType5 requestedDevice = deviceTypeInvalid;
        if (options.device) {
            const auto* profile = deviceProfileNamed5(options.device->c_str());
            if (!profile) {
                cerr << "unknown device profile " << *options.device << endl;
                return false;
            }
            if (profile->deviceType == deviceTypeInvalid) {
                cerr << profile->name << " isn't supported yet" << endl;
                return false;
            }
            requestedDevice = profile->deviceType;
        }

        Buffer nor, nand, memory, savestate;
        RomInfo5 romInfo;
        optional<DisplayMode> displayMode = nullopt;
        uint32_t ramSize{0};

        bool embeddedCard = false;
        DeviceType5 sessionDevice = DeviceType5::deviceTypeInvalid;
        string sessionFolder;
        if (!readSession(options, nor, nand, memory, savestate, ramSize, romInfo, displayMode,
                         requestedDevice, sessionDevice, embeddedCard, sessionFolder))
            return false;
        const string sessionPath = sessionFolder.empty() ? string() : string(options.norOrSession);
        const DeviceType5 deviceType = sessionDevice != DeviceType5::deviceTypeInvalid
            ? sessionDevice : requestedDevice != deviceTypeInvalid ? requestedDevice
                                                                   : romInfo.GetDeviceType();
        if (romInfo.IsValid() && deviceProfile5(romInfo.GetDeviceType()) &&
            romInfo.GetDeviceType() != deviceType) {
            cerr << "selected device profile does not match ROM" << endl;
            return false;
        }
        if (romInfo.IsValid() && romInfo.GetDeviceType() == deviceTypeInvalid &&
            (romInfo.GetCompanyId() == 0x736f6e79 /* sony */ || !deviceProfile5(deviceType) ||
             deviceProfile5(deviceType)->detect != DeviceDetect5::norSize)) {
            cerr << romInfo << "unknown ROM device identity" << endl;
            return false;
        }
        const bool isTx = deviceType == DeviceType5::deviceTypeTX;
        // The desktop passes saved serial settings; unsupported devices skip them.
        if (options.desktopSocket && !isTx && !deviceProfileIsE2Board5(deviceType) && deviceType != deviceTypeTG50) {
            serialConfig = HostSerial::Config{};

        }
        if (options.experimentalFastAudio) {
            const auto digest=md5(static_cast<uint8_t*>(nor.data),nor.size);
            const auto* audioProfile = deviceProfile5(deviceType);
            const bool qualified = audioProfile && deviceProfileFastAudioQualified5(
                *audioProfile,digest.c_str(),romInfo.GetFirmwareVariant());
            if (!qualified) {
                cerr << "low-latency audio requires a qualified device NOR image" << endl;
                return false;
            }
        }
        if (isTx) {
            cerr << "EXPERIMENTAL Palm TX: reset and session testing" << endl;
            if (nor.size != deviceProfile5(deviceType)->norSize ||
                (sessionDevice == DeviceType5::deviceTypeInvalid && !options.nand)) {
                cerr << "TX requires a 16 MiB NOR image and an explicit --nand image" << endl;
                return false;
            }
        } else {
            cerr << romInfo;
        }

        if (!isTx && (!romInfo.IsValid() || deviceType == DeviceType5::deviceTypeInvalid))
            return false;

        if (options.ramSize) {
            if (ramSize == 0) {
                if (*options.ramSize == 0) {
                    cerr << "unsupported RAM size: zero MiB" << endl;
                    return false;
                }
                if (*options.ramSize > (UINT32_MAX >> 20)) {
                    cerr << "unsupported RAM size: MiB value overflows" << endl;
                    return false;
                }
                ramSize = *options.ramSize << 20;
            } else {
                cerr << "cannot specify RAM size for an existing session" << endl;
                return false;
            }
        }

        if (ramSize == 0) ramSize = isTx ? (32ul << 20) : romInfo.GetRecommendedRamSize();
        // The Zodiac 2 has one RAM size, which its ROM header doesn't give.
        if (!options.ramSize && sessionDevice == deviceTypeInvalid && deviceProfile5(deviceType)->board == DeviceBoard5::imx1Zodiac)
            ramSize = deviceProfile5(deviceType)->minRam;

        if (!deviceConfigurationSupportsRamSize(deviceType, ramSize)) {
            cerr << "unsupported RAM size: " << ramSize << " bytes" << endl;
            return false;
        }

        cerr << "using RAM size: " << ramSize << " bytes" << endl << endl;

        if (!displayMode.has_value())
            displayMode = options.displayMode.value_or(
                deviceConfigurationDefaultDisplayMode(deviceType));

        if (!deviceConfigurationDeviceSupportsDisplayMode(deviceType, *displayMode)) {
            cerr << "unsupported display mode for device" << endl;
            return false;
        }

        auto firmwareError = deviceProfileValidateFirmware5(deviceType, nor.size, nand.size,
            sessionDevice != deviceTypeInvalid, options.nand.has_value(), romInfo.GetFirmwareVariant());
        if (firmwareError == DeviceFirmwareError5::none && romInfo.IsValid())
            firmwareError = deviceProfileValidateNorLayout5(deviceType, romInfo.GetHeaderOffset(),
                                                            sessionDevice != deviceTypeInvalid,
                                                            romInfo.HasSmallAndBigCards());
        if (firmwareError != DeviceFirmwareError5::none) {
            cerr << "invalid firmware geometry for device profile";
            if (firmwareError == DeviceFirmwareError5::norSize && deviceProfile5(deviceType)->norSize)
                cerr << ": invalid NOR size; expected " << deviceProfile5(deviceType)->norSize << " bytes";
            if (firmwareError == DeviceFirmwareError5::norLayout) {
                if (deviceProfile5(deviceType)->norLayout == DeviceNorLayout5::smallAndBigCards)
                    cerr << ": invalid NOR layout; expected a small card at 0x0 and a big card at 0x20000";
                else
                    cerr << ": invalid NOR layout; expected the card header at 0x" << hex
                         << e2NorHeaderOffset5 << dec;
            }
            if (firmwareError == DeviceFirmwareError5::nandSize)
                cerr << ": invalid NAND size; expected " << deviceConfigurationGetNandSize(deviceType) << " bytes";
            cerr << endl;
            return false;
        }

        if (!deviceProfileRunnerEligible5(deviceType)) {
            cerr << deviceName5(deviceType) << ": native board is not implemented" << endl;
            return false;
        }

        // A ROM start has a machine folder only through --work-folder. Candidate cards a stopped
        // process left there are removed before anything else runs.
        if (sessionFolder.empty() && options.workFolder) {
            std::error_code ec;
            if (std::filesystem::is_directory(*options.workFolder, ec)) sessionFolder = *options.workFolder;
        }
        if (!sessionFolder.empty()) frondSdCandidate::cleanup(sessionFolder);

        if (options.sd) {
            const auto loaded = frondNative::loadSdImage(sessionFolder, deviceType, *options.sd);
            if (!loaded.ok) {
                if (loaded.code == "size") cout << "sd card image has bad size" << endl;
                else cerr << loaded.message << endl;
                return false;
            }
        }

        const int gdbPort = options.gdbPort ? static_cast<int>(*options.gdbPort) : -1;

        if (options.desktopSocket && !deviceProfileSupportsDesktop5(deviceType)) {
            cerr << "desktop sessions currently support Palm TX, Tungsten E2 and Zire 72 only" << endl;
            return false;
        }
        DisplayConfiguration displayConfiguration =
            deviceConfigurationDisplayConfigForDevice(deviceType, *displayMode);

        InputReplay replay;
        string replayError;
        if (options.inputReplay &&
            !replay.Load(*options.inputReplay, displayConfiguration.width,
                         displayConfiguration.height + displayConfiguration.graffitiHeight,
                         replayError)) {
            cerr << replayError << endl;
            return false;
        }
        if (options.runFor && !replay.Finished() &&
            replay.LastEventMs() >= uint64_t(*options.runFor) * 1000) {
            cerr << "input replay extends beyond --run-for duration" << endl;
            return false;
        }

        const auto board = deviceProfile5(deviceType)->board;
        const bool pxa270 = board == DeviceBoard5::pxa270Tx || board == DeviceBoard5::pxa270Zire72;
        if (sessionDevice == deviceTypeTG50 &&
            (memory.size != tg50RamBackingSize + 2 * MEMORY_BUFFER_GRANULARITY || !savestate.size)) {
            cerr << "TG50 session requires complete RAM and CPU/peripheral state" << endl;
            return false;
        }
        SoC* soc = nullptr;
        if (board == DeviceBoard5::imx1Zodiac) {
            // The Zodiac's machine has its own options and no GDB stub, fast audio or replay yet.
            if (options.gdbPort || options.experimentalFastAudio || options.lowLatencyQueue) {
                cerr << "--gdb and the low-latency audio options are not available on the Zodiac" << endl;
                return false;
            }
            // Its machine has an engine of its own, set below; the JIT isn't used.
            cpuSetFastEngine(false);
            string why;
            soc = SocIMX1::Create(static_cast<const uint8_t*>(nor.data), nor.size, why);
            if (!soc) {
                cerr << "Tapwave Zodiac 2: " << why << endl;
                return false;
            }
#ifdef FROND_ZODIAC_DEVELOPER
            // The developer build's way in while a cold boot takes this long: a bring-up state file to
            // start a ROM run from.
            if (const char* path = getenv("FROND_ZODIAC_STATE"); path && *path && !savestate.size) {
                ifstream file(path, ios::binary);
                vector<char> bytes((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
                if (!file || bytes.empty() ||
                    !static_cast<SocIMX1*>(soc)->LoadBringUpState(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())) {
                    cerr << "FROND_ZODIAC_STATE: " << path << " could not be loaded" << endl;
                    return false;
                }
            }
#endif
            if (options.fastEngine && !static_cast<SocIMX1*>(soc)->SetFastEngine(true))
                cerr << "Tapwave Zodiac 2: the Fast engine is not available here, running Exact" << endl;
        } else {
            if (gFastEngineMissing) cerr << "the Fast engine isn't built in here, running Exact" << endl;
            soc = (board == DeviceBoard5::paravirtual)
                ? static_cast<SoC*>(new SocPV(ramSize, nor.data, nor.size, *displayMode, gdbPort))
                : static_cast<SoC*>(new SocPXA(deviceType, ramSize, nor.data, nor.size,
                                               reinterpret_cast<uint8_t*>(nand.data), nand.size,
                                               gdbPort, pxa270 ? 2 : deviceGetSocRev()));
        }

        if (auto* chip = soc->GetNand()) {
            if (nandFromMalloc)
                nandSetDataRelease(chip, [](void* p) { free(p); });
            else
                nandSetDataRelease(chip, [](void* p) { delete[] static_cast<uint8_t*>(p); });
            if (nandMedia && !frondNand::attachFile(chip, sessionFolder, *nandMedia)) {
                cerr << "session NAND file is missing or unreadable" << endl;
                return false;
            }
        } else if (nandMedia) {
            cerr << "session NAND is not supported by device profile" << endl;
            return false;
        }

        if (memory.data && memory.size > soc->GetMemoryData().size) {
            cerr << "RAM size mismatch" << endl;
            return false;
        }
        if (sessionDevice == DeviceType5::deviceTypeTX &&
            (memory.size != soc->GetMemoryData().size || !savestate.size)) {
            cerr << "TX session requires complete RAM and CPU/peripheral state" << endl;
            return false;
        }

        if (memory.data) {
            memcpy(soc->GetMemoryData().data, memory.data, memory.size);
            free(memory.data);
        }

        auto* socPxa = dynamic_cast<SocPXA*>(soc);
        auto* memoryStick = socPxa ? socPxa->GetMemoryStick() : nullptr;
        if (msProCard) {
            if (!frondNative::attachMsPro(memoryStick, false) ||
                (msSwapped && !msCtrlrProtect(memoryStick, msProtect))) {
                cerr << "session Memory Stick Pro could not be attached" << endl;
                return false;
            }
        }
        if (!sessionFolder.empty() && !msProCard) {
            const auto msPath = std::filesystem::path(sessionFolder) / frondMachine::msSibling;
            std::error_code ec;
            if (std::filesystem::is_regular_file(msPath, ec)) {
                const bool attached = frondNative::attachClassicFile(memoryStick, msPath, frondNative::slotCards().msIdentity,
                                                                     !msSwapped, msSwapped && msProtect);
                if (!attached) {
                    cerr << "session Memory Stick sidecar is missing or unreadable" << endl;
                    return false;
                }
            }
        }

        // A changed stick resumes ejected, then goes in below.
        if (msSwapped) {
            if (!memoryStick) {
                cerr << "this device has no Memory Stick slot" << endl;
                return false;
            }
            msCtrlrSwapOnLoad(memoryStick, true);
        }
        if (!soc->Load(savestate.size, savestate.data)) {
            cerr << "failed to restore savestate" << endl;
            return false;
        }
        // A swapped slot is ejected now and its card goes in later, in the guest loop below. The
        // stick's detect line drops here, after the restore, so a guest asleep at the save wakes
        // on it as on a hot eject.
        bool sdInsertPending = false, msInsertPending = false;
        if (msSwapped) {
            msCtrlrSwapEject(memoryStick);
            msInsertPending = msCtrlrPresent(memoryStick);
        }

        // Physical host keys are up when a session starts. Complete any saved
        // make packets and send break codes, without altering export rollback.
        soc->ReleaseKeyboard();
        if (savestate.data) free(savestate.data);

        if (memoryStick) {
            // A Memory Stick device has no SD slot; its stick only shares the card store. An SD
            // flag an older save kept from that store is dropped instead of checked against it.
            soc->SdEject();
        } else if (sdSwapped) {
            // The saved card is gone: eject it as a hot eject would; the one there now goes in later.
            soc->SdEject();
            sdInsertPending = sdCardInitialized();
        } else if (soc->SdInserted()) {
            if (!soc->SdRemount()) {
                cerr << "failed to remount SD card: session and card image do not match" << endl;
                return false;
            }
        } else if (sdCardInitialized() && !embeddedCard) {
            soc->SdInsert();
        }
        const uint64_t ejectedAt = soc->GetTime();
        std::optional<uint64_t> awakeAt;

        AudioQueue* audioQueue = audioQueueCreate(AUDIO_QUEUE_SIZE);
        soc->SetAudioQueue(audioQueue);
        if (options.experimentalFastAudio && !static_cast<SocPXA*>(soc)->EnableFastAudio()) {
            cerr << "unable to enable low-latency audio" << endl;
            return false;
        }

        MainLoop mainLoop(soc);
        if (options.experimentalFastAudio) mainLoop.SetTimesliceUsec(1000000 / 120);
        mainLoop.SetCyclesPerSecondLimit(uint64_t(options.mips.value_or(deviceProfileOriginalMips5(deviceType))) * 1000000);
        mainLoop.SetEmulatedTime(options.emulatedTime);

        Rotation rotation = Rotation::portrait_0;
        // The view turns with the guest OS's orientation (the Zodiac, unless --view-turn window). The
        // chosen rotation (Rotate) goes on top. Until the guest's is known, landscape, which it starts in.
        const bool followGuest = soc->FrameQuarterTurn() && !options.viewFollowsWindow;
        unsigned guestOrientation = 0;
        int guestTurns = 0;

        float scale = options.windowScale.value_or(options.smallWindow ? 1.f : 0.f);

        const JogPlacement jog = options.hideJogControls ? JogPlacement::hidden
                                                         : options.jogPlacement.value_or(JogPlacement::screen);
        // The display settings, controlView and fitted belong to the SDL thread once the
        // guest thread starts; the display command changes them through it.
        DisplaySettings display{options.windowFit, options.smoothLcd, !options.hideKeyboard, jog};
        DeviceControlView controlView = deviceType == deviceTypeTG50
            ? deviceControlView(display.keyboard, display.jog) : DeviceControlView{};
        // A device with two arrangements starts with its controls around the screen; a fitted
        // window then puts them around when landscape and below when portrait, again after each resize.
        controlView = deviceControlViewForWindow(deviceControls(deviceType), controlView, 0, 0);
        bool fitted = options.windowFit != WindowFit::none;
        // The window is named after the machine: its session file's name, or the device without one.
        const string windowTitle = !sessionFolder.empty()
            ? std::filesystem::path(options.norOrSession).stem().string()
            : string(deviceProfile5(deviceType)->displayName);
        SDL_Window* window = initSdl(windowTitle.c_str(), displayConfiguration, scale, rotation, deviceControls(deviceType), controlView, fitted);
        if (!window) {
            cerr << "failed to init SDL" << endl;
            return false;
        }
        auto arrangeForWindow = [&] {
            if (!fitted) return;
            int w = 0, h = 0;
            SDL_GetWindowSize(window, &w, &h);
            controlView = deviceControlViewForWindow(deviceControls(deviceType), controlView, w, h);
        };
        arrangeForWindow();
        // The rotation drawn and mapped for a chosen one: the Zodiac's panel stands upright in a
        // portrait window. Belongs to the SDL thread; input gets it with each layout.
        auto rotationForWindow = [&](Rotation chosen) {
            int w = 0, h = 0;
            SDL_GetWindowSize(window, &w, &h);
            return windowRotation(chosen, soc->FrameQuarterTurn() && !followGuest, fitted, w, h);
        };
        Rotation drawnRotation = rotationForWindow(rotation);

        auto sdlRenderer =
            make_unique<SdlRenderer>(window, soc, scale, displayConfiguration, drawnRotation, options.smoothLcd,
                                         options.windowFit, options.windowBackground, controlView);
        SdlEventHandler sdlEventHandler(soc, scale, displayConfiguration, drawnRotation, controlView);
        sdlEventHandler.SetLayout(sdlRenderer->Layout(), fitted);
        // The guest thread's copy of fitted, for the layouts the SDL thread sends it.
        bool inputFitted = fitted;

        // --low-latency-queue gives any device the --fast-audio queue limits without the preview.
        const bool queueOnly = options.lowLatencyQueue && !options.experimentalFastAudio;
        const bool lowLatencyQueue = options.experimentalFastAudio || queueOnly;
        // Low-latency limits are counted in host periods: 512 frames on the desktop, more where
        // the host takes frames in bursts, as Android's mixer does. The preview may hold 12.
        const uint32_t audioPeriod = options.audioPeriod;
        if (lowLatencyQueue) audioQueueEnsureCapacity(audioQueue, size_t(audioPeriod) * 12 + 1024);
        if (options.experimentalFastAudio) static_cast<SocPXA*>(soc)->SetFastAudioPeriod(audioPeriod);
        SdlAudioDriver audioDriver(soc, audioQueue, lowLatencyQueue, audioPeriod);
        if (!options.disableAudio) audioDriver.Start();

        commands::Register();
        if (!options.runFor && !options.desktopSocket) cli::Start(options.script);
        const uint64_t startedAt = timestampUsec();
        auto* const zodiac = dynamic_cast<SocIMX1*>(soc);
        const Imx1MachineCheapStatus zodiacStart = zodiac ? zodiac->Status() : Imx1MachineCheapStatus{};
        // The Zodiac's speed figures over 5 s windows, made here on the SDL thread.
        std::unique_ptr<ZodiacSpeed> zodiacSpeed = zodiac ? std::make_unique<ZodiacSpeed>(5000000) : nullptr;
        uint64_t replayStartedAt = options.emulatedTime ? soc->GetTime() / 1000 : startedAt;
        commands::Context commandContext{
            .soc = soc, .mainLoop = mainLoop, .audioDriver = audioDriver, .rotation = rotation,
            .replay = replay, .replayStartedAt = replayStartedAt, .emulatedTime = options.emulatedTime,
            .screenWidth = displayConfiguration.width,
            .screenHeight = displayConfiguration.height + displayConfiguration.graffitiHeight};
        commandContext.sessionFolder = sessionFolder;
        commandContext.sessionPath = sessionPath;

        // The endpoint opens after the session loads, so a restore starts detached.
        SdlMicrophone microphone;
        commandContext.microphone = &microphone;
        const bool hasMicrophone = deviceProfile5(deviceType) && deviceProfile5(deviceType)->capabilities.microphone;
        microphone.detach = [&] { if (hasMicrophone) static_cast<SocPXA*>(soc)->SetMicrophoneSource(nullptr); };
        const auto microphoneSource = microphone.Source();
        microphone.prepare = [&] {
            if (!hasMicrophone) return false;
            auto* machine = static_cast<SocPXA*>(soc);
            machine->SetMicrophoneSource(&microphoneSource);
            return !machine->PowerSleeping();
        };
        microphone.awake = [&] { return hasMicrophone && !static_cast<SocPXA*>(soc)->PowerSleeping(); };
        // The camera's source starts as the test pattern; the frontend picks another.
        HostCamera camera;
        const bool hasCamera = deviceProfile5(deviceType) && deviceProfile5(deviceType)->frontend.camera;
        if (hasCamera) {
            commandContext.camera = &camera;
            camera.attach = [&](const CameraSource* source) {
                auto* machine = static_cast<SocPXA*>(soc);
                if (machine->PowerSleeping()) return false;
                machine->SetCameraSource(source);
                return true;
            };
            camera.detach = [&] { static_cast<SocPXA*>(soc)->SetCameraSource(nullptr); };
        }
        HostSerial serial;
        commandContext.serial = &serial;
        const auto& capabilities = deviceProfile5(deviceType)->capabilities;
        if (soc->GetDeviceType() == DeviceType5::deviceTypeTX || deviceProfileIsE2Board5(soc->GetDeviceType()) ||
            soc->GetDeviceType() == DeviceType5::deviceTypeTG50) {
            commandContext.serialUart = static_cast<SocPXA*>(soc)->GetSerialUart();
            commandContext.serialRoute = static_cast<SocPXA*>(soc)->GetSerialRoute();
        }
        if (serialConfig.kind != HostSerial::Kind::off) {
            if (!capabilities.serial) {
                cerr << "--serial is not available on this device" << endl;
                return false;
            }
            if (!serial.Start(commandContext.serialUart, serialConfig, commandContext.serialError, commandContext.serialRoute))
                cerr << "serial: " << commandContext.serialError << endl;
            cout << "serial: " << serial.Describe() << endl;
        }
        HostInfrared infrared;
        commandContext.infrared = &infrared;
        if (infraredPhone) {
            if (!capabilities.irPhone) commandContext.infraredError = "This device has no infrared phone.";
            if (!capabilities.irPhone || !infrared.StartPhone(static_cast<SocPXA*>(soc),commandContext.infraredError)) {
                cerr << "infrared: " << commandContext.infraredError << endl; return false;
            }
            cout << "infrared: " << infrared.Describe() << endl;
        } else if (infraredPrinter) {
            if (!capabilities.infrared) commandContext.infraredError = "This device has no infrared port.";
            if (!capabilities.infrared || !infrared.StartPrinter(static_cast<SocPXA*>(soc),commandContext.infraredError)) {
                cerr << "infrared: " << commandContext.infraredError << endl; return false;
            }
            cout << "infrared: " << infrared.Describe() << endl;
        } else if (infraredConfig.kind != HostSerial::Kind::off) {
            if (!capabilities.infrared ||
                !infrared.Start(static_cast<SocPXA*>(soc),infraredConfig,commandContext.infraredError)) {
                cerr << "infrared: " << commandContext.infraredError << endl; return false;
            }
            cout << "infrared: " << infrared.Describe() << endl;
        }
        if (options.bluetooth) {
            const auto& path=*options.bluetooth;
            if (!capabilities.bluetooth ||
                (path!="off" && (path.empty() || path.front()!='/' || path.find(char(0))!=string::npos))) {
                cerr << "--bluetooth needs off or an absolute virtual controller Unix socket path on a TX, E2, Zire 72 or TG50" << endl;return false;
            }
            if (!static_cast<SocPXA*>(soc)->SetBluetooth(path=="off"?string():path,commandContext.bluetoothError)) {
                cerr << "bluetooth: " << commandContext.bluetoothError << endl;return false;
            }
            cout << "bluetooth: " << static_cast<SocPXA*>(soc)->BluetoothState() << endl;
        }
        UsbHostBridge usb;
        commandContext.usb = &usb;
        if (usbConfig.kind != HostSerial::Kind::off) {
            if (!capabilities.usb) {
                cerr << "--usb needs a Palm TX, Tungsten E2 or Zire 72" << endl;
                return false;
            }
            if (!usb.Start(static_cast<SocPXA*>(soc), usbConfig, commandContext.usbError))
                cerr << "usb: " << commandContext.usbError << endl;
            cout << "usb: " << usb.Describe() << endl;
        } else if (capabilities.usb) {
            usb.Start(static_cast<SocPXA*>(soc), usbConfig, commandContext.usbError);  // off: cable as without USB
        }

        unique_ptr<DesktopControl> desktop;
        if (options.desktopSocket) {
            desktop = make_unique<DesktopControl>(*options.desktopSocket, !options.disableAudio,
                                                  options.saveOnDisconnect.value_or(string()));
            SDL_SetWindowTitle(window, windowTitle.c_str());
        }

        uint64_t lastSpeedDump = startedAt;
        bool waitForPreviewAudio = false;
        // Queue-only mode counts as playing while changing frames were pushed in the last
        // 100 ms (host time, usec). Silence or a constant level keeps the normal limits.
        uint64_t queueOnlySounding = audioQueueAccounting(audioQueue).nonSilent, queueOnlySoundAt = 0;
        bool queueOnlyHadSound = false;
        uint64_t lastAudioUnderruns = audioDriver.GetUnderrunCount();
        auto reportAudioUnderruns = [&]() {
            const auto underruns = audioDriver.GetUnderrunCount();
            if (underruns != lastAudioUnderruns && options.experimentalFastAudio)
                static_cast<SocPXA*>(soc)->NotifyFastAudioUnderrun();
            // Console output can block; keep it out of the audio callback.
            for (; lastAudioUnderruns < underruns; ++lastAudioUnderruns)
                cout << "audio underrun" << endl;
        };

        // Two threads from here on. This one, which made the window, owns SDL: events, the
        // renderer, the window and audio device calls. The guest thread owns the SoC and
        // runs the loop below; everything that reaches the guest, input included, happens
        // on it in the same order and at the same points as with one thread. RenderLink
        // carries SDL events to it in order and its LCD frames back.
        RenderLink link(soc->FrameQuarterTurn() ? displayConfiguration.height : displayConfiguration.width,
                        soc->FrameQuarterTurn() ? displayConfiguration.width : displayConfiguration.height);
        // Rows in a handed-over frame, for uploading a whole one again: the Zodiac's panel has 480.
        const uint32_t frameRows = soc->FrameQuarterTurn() ? displayConfiguration.width : displayConfiguration.height;
        link.beforeEvent = [&](const SDL_Event& event) {
            microphone.AppEventOnDeviceThread(event.type);
            camera.AppEvent(event.type);
#ifndef __ANDROID__
            if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_MINIMIZED || event.window.event == SDL_WINDOWEVENT_HIDDEN)
                    { microphone.AppEventOnDeviceThread(SDL_APP_WILLENTERBACKGROUND); camera.AppEvent(SDL_APP_WILLENTERBACKGROUND); }
                if (event.window.event == SDL_WINDOWEVENT_RESTORED || event.window.event == SDL_WINDOWEVENT_SHOWN)
                    microphone.AppEventOnDeviceThread(SDL_APP_DIDENTERFOREGROUND);
            }
#endif
        };
        sdlEventHandler.SetDirectCapture(false);
        if (desktop) sdlEventHandler.SetShortcutSink([&](const string& name) { desktop->event("shortcut", name); });
        commandContext.windowId = [&]() {
            string id;
            link.Call([&] { id = x11WindowId(window); });
            return id;
        };
        commandContext.windowLayout = [&]() {
            string text;
            link.Call([&] {
                int windowWidth = 0, windowHeight = 0;
                SDL_GetWindowSize(window, &windowWidth, &windowHeight);
                const auto& layout = sdlRenderer->Layout();
                if (windowWidth > 0 && windowHeight > 0)
                    text = to_string(layout.x) + " " + to_string(layout.y) + " " + to_string(layout.width) + " " +
                           to_string(layout.height) + " " + to_string(windowWidth) + " " + to_string(windowHeight);
            });
            return text;
        };
        // Display changes from the guest thread wait here for the SDL thread.
        mutex displayMutex;
        optional<DisplayChange> displayRequest;
        commandContext.changeDisplay = [&](const DisplayChange& change) {
            {
                lock_guard<mutex> lock(displayMutex);
                if (displayRequest) displayRequest->Merge(change);
                else displayRequest = change;
            }
            link.Wake();
        };
        audioDriver.SetDeviceThread([&](const function<void()>& work) { link.Call(work); });

        microphone.SetDeviceThread([&](const function<void()>& work) { link.Call(work); });

        // SDL thread state: what it drew last, whether the app is hidden, and whether the
        // window needs drawing again (redraw) or the whole frame uploaded again (redrawAll).
        bool appBackground = false;
        DeviceKeyMask shownButtons = 0;
        DeviceStick shownStick{};
        bool shownLcd = true;
        bool redraw = false, redrawAll = false;
        // Draws what the guest thread published since the last draw. In the background the
        // draw stays pending, so the newest frame is drawn on return. A draw with no new
        // frame and nothing changed is skipped.
        auto drawPending = [&]() {
            if (appBackground || !sdlRenderer->Valid()) return;
            DrawState draw;
            const bool taken = link.TakeDraw(draw);
            if (!taken && !redraw && !redrawAll) return;
            if (!taken) {
                draw.buttons = shownButtons;
                draw.stick = shownStick;
                draw.lcdEnabled = shownLcd;
            }
            if (!draw.pixels && !draw.redraw && !redraw && !redrawAll && draw.buttons == shownButtons && draw.stick == shownStick &&
                draw.lcdEnabled == shownLcd)
                return;
            shownButtons = draw.buttons;
            shownStick = draw.stick;
            shownLcd = draw.lcdEnabled;
            if (redrawAll) {
                draw.pixels = link.FrontFrame();
                draw.firstDirtyLine = 0;
                draw.lastDirtyLine = frameRows - 1;
            }
            redraw = redrawAll = false;
            const uint64_t renderStart = zodiacSpeed ? timestampUsec() : 0;
            sdlRenderer->Render(draw.pixels, draw.firstDirtyLine, draw.lastDirtyLine, draw.lcdEnabled,
                                draw.buttons, draw.stick);
            if (zodiacSpeed) zodiacSpeed->Drawn(draw.pixels != nullptr, timestampUsec() - renderStart);
        };

        auto guestLoop = [&]() {
            DrawGate gate;
            vector<RenderLink::Input> inputs;
            bool wasHeadless = false;
            FrondPerformance performance(mainLoop.GetTimesliceSizeUsec());
#ifdef __ANDROID__
            cout << "FrondCore scheduling: " << performance.Description() << endl;
#endif
            // Snapshots are taken on the SDL thread once it has drawn the newest frame.
            auto snapshot = [&](const char* path, bool& blank) {
                bool saved = false;
                link.Call([&] {
                    drawPending();
                    saved = sdlRenderer->SaveFramebuffer(path, blank);
                });
                return saved;
            };

            while (!link.Stopped()) {
                uint64_t now = timestampUsec();
                performance.BeginWork();
                if (options.runFor && now - startedAt >= uint64_t(*options.runFor) * 1000000) {
                    cout << "bounded run complete; PC=0x" << hex
                         << (soc->GetCpu() ? cpuGetRegExternal(soc->GetCpu(), 15) : 0) << dec
                         << ", LCD=" << soc->LcdEnabled() << endl;
                    if (zodiac) {
                        const auto status = zodiac->Status();
                        cout << "zodiac: " << status.retired - zodiacStart.retired << " instructions and "
                             << (status.time.ticks - zodiacStart.time.ticks) * 1000 / 32768 << " ms of guest time in "
                             << (now - startedAt) / 1000 << " ms" << (zodiac->Halted() ? ", stopped" : "") << endl;
                    }
                    break;
                }

                // A stopped Zodiac machine can't go on. It ends the run like any other fatal error,
                // and nothing is saved over the session.
                if (zodiac && zodiac->Halted())
                    throw runtime_error("Tapwave Zodiac 2: the machine stopped: " + zodiac->StopText());

                link.TakeInputs(inputs);
                for (const auto& input : inputs) {
                    if (!input.layout) {
                        if (input.event.type == SDL_AUDIODEVICEREMOVED && input.event.adevice.iscapture)
                            microphone.Removed(input.event.adevice.which);
                        sdlEventHandler.HandleEvent(input.event);
                        continue;
                    }
                    // The SDL thread laid the window out again after a resize or rotation. A
                    // rotation applies here, with the layout made for it.
                    if (input.rotation) sdlEventHandler.SetRotation(*input.rotation);
                    sdlEventHandler.TakeResized();
                    if (input.before) input.before();
                    sdlEventHandler.SetLayout(input.windowLayout, inputFitted);
                    soc->SetFramebufferDirty();
                }
                if (sdlEventHandler.QuitRequested()) {
                    if (!desktop) break;
                    desktop->event("close");
                    sdlEventHandler.ClearQuitRequested();
                }

                if (desktop) desktop->microphoneChanged(commandContext);
                // With a background panel, the device menu lets Palm OS finish card notifications
                // without drawing. Leaving that menu for another app still pauses every guest clock,
                // unless the IR printer has a job coming in or a link up: only the guest ends those,
                // so it runs without drawing or sound until they end.
                const bool printing = infrared.PrinterNeedsGuest();
                const bool headless = sdlEventHandler.InBackground() &&
                                      ((capabilities.backgroundPanel && desktop && desktop->PanelVisible()) || printing);
                audioDriver.SetHeadless((capabilities.backgroundPanel || printing) && sdlEventHandler.InBackground());
                if (sdlEventHandler.InBackground() && !headless) {
                    wasHeadless = false;
                    mainLoop.PauseAt(now);
                    if (desktop && desktop->poll(commandContext)) break;
                    usleep(50000);
                    continue;
                }
                if (headless != wasHeadless) mainLoop.PauseAt(now);
                wasHeadless = headless;

                // RPC and CLI controls can enable sound after --no-sound startup.
                // Backpressure must stop all guest clocks together. Suspending just
                // the codec lets DMA and the guest stream deadlines run ahead.
                const bool waitForAudio = !headless && soc->PcmOutputEnabled() && audioDriver.GetAudioBackpressure();
                // A paused input replay holds every guest clock until replay-continue.
                const bool replayPaused = replay.Paused();

                reportAudioUnderruns();
                // SIR peers need character-sized handoffs. A full display slice can
                // consume the original stack's entire discovery receive window.
                bool lowLatencyActive = options.experimentalFastAudio && static_cast<SocPXA*>(soc)->FastAudioActive();
                if (queueOnly) {
                    const auto sounding = audioQueueAccounting(audioQueue).nonSilent;
                    if (sounding != queueOnlySounding) {
                        queueOnlySounding = sounding; queueOnlySoundAt = now; queueOnlyHadSound = true;
                    }
                    lowLatencyActive = queueOnlyHadSound && soc->PcmOutputEnabled() &&
                                       audioQueuePendingSamples(audioQueue) > 0 && now - queueOnlySoundAt < 100000;
                }
                mainLoop.SetTimesliceUsec(infrared.Active() ? 1000 :
                    lowLatencyActive ? 1000000 / 120 : 1000000 / MAIN_LOOP_FPS);

                if (!headless && lowLatencyActive) {
                    const auto pending = audioQueuePendingSamples(audioQueue);
                    if (!waitForPreviewAudio && pending > 6 * audioPeriod) waitForPreviewAudio = true;
                    if (waitForPreviewAudio && pending < 4 * audioPeriod) waitForPreviewAudio = false;
                } else waitForPreviewAudio = false;
                if (hasMicrophone) microphone.Service(static_cast<SocPXA*>(soc)->MicrophoneDemand());
                if (hasCamera) camera.Service();
                serial.Service();
                usb.Service();
                infrared.Service();
                if (zodiacSpeed) {
                    const uint64_t cycleStart = timestampUsec();
                    mainLoop.Cycle(now, waitForAudio || waitForPreviewAudio || replayPaused);
                    zodiacSpeed->Emulated(timestampUsec() - cycleStart);
                } else {
                    mainLoop.Cycle(now, waitForAudio || waitForPreviewAudio || replayPaused);
                }
                // The insert half of a swapped slot. A card taken out meanwhile isn't there to insert.
                if (sdInsertPending || msInsertPending) {
                    const uint64_t time = soc->GetTime();
                    if (!awakeAt && !(socPxa && socPxa->PowerSleeping())) awakeAt = time;
                    if ((awakeAt && time - *awakeAt >= swapInsertDelay) || time - ejectedAt >= swapInsertLimit) {
                        bool inserted = false;
                        if (sdInsertPending) soc->SdInsert(), inserted = soc->SdInserted();
                        if (msInsertPending) inserted = msCtrlrInsert(memoryStick, true) || inserted;
                        sdInsertPending = msInsertPending = false;
                        // The launcher learns of a hot insert from its reply; this one gets an event.
                        if (inserted && desktop) desktop->event("card", "inserted");
                    }
                }
                serial.Service();
                usb.Service();
                infrared.Service();
                for (const auto& message : usb.TakeMessages()) cout << message << endl;


                // Hand the frame to the SDL thread; it uploads and presents it.
                if (!headless) {
                    const uint64_t handoffStart = zodiacSpeed ? timestampUsec() : 0;
                    const bool lcdEnabled = soc->LcdEnabled();
                    uint32_t firstDirtyLine = 0, lastDirtyLine = 0;
                    const uint32_t* frame = soc->GetPendingFrame(firstDirtyLine, lastDirtyLine);
                    const DeviceKeyMask buttons = sdlEventHandler.PressedButtons();
                    const DeviceStick stick = sdlEventHandler.Stick();
                    const bool redrawRequested = sdlEventHandler.RedrawRequested();
                    if (gate.Pass(frame != nullptr, redrawRequested, buttons, lcdEnabled, stick)) {
                        link.Publish(frame, firstDirtyLine, lastDirtyLine, lcdEnabled, buttons, redrawRequested, stick);
                        soc->ResetPendingFrame();
                    }
                    if (zodiacSpeed && frame) zodiacSpeed->HandedOver(timestampUsec() - handoffStart);
                }
                // A word from the display state, so once a slice. An unknown one (display off) keeps the last.
                if (followGuest) {
                    const unsigned orientation = soc->GuestOrientation();
                    if (orientation && orientation != guestOrientation) {
                        guestOrientation = orientation;
                        guestTurns = guestQuarterTurns(orientation);
                        cout << "guest orientation: " << guestOrientationName(orientation) << endl;
#ifdef __ANDROID__
                        frondGuestOrientationChanged(int(orientation));
#endif
                    }
                }
                // The Zodiac's speed line goes to the log on Android, and its figures to speed-state.
                if (zodiacSpeed && zodiacSpeed->Due(now)) {
                    const auto status = zodiac->Status();
                    if (zodiacSpeed->Sample(now, status.retired, status.time.ticks)) {
                        commandContext.speedIps = zodiacSpeed->Ips();
                        commandContext.speedDetail = zodiacSpeed->Line();
#ifdef __ANDROID__
                        cout << zodiacSpeed->Line() << endl;
#endif
                    }
                }
                if (!headless) {
                    sdlEventHandler.ClearRedrawRequested();
                    replay.Dispatch((commands::replayClockUsec(commandContext) - replayStartedAt) / 1000, *soc, snapshot);
                }

                if (now - lastSpeedDump > 1000000) {
                    const uint64_t currentIps = mainLoop.GetCurrentIps();
                    const uint64_t currentIpsMax = mainLoop.GetCurrentIpsMax();
                    lastSpeedDump = now;

                    ostringstream s;
                    s << "cp-uarm @ " << fixed << setprecision(2)
                      << static_cast<float>(currentIps) / 1000000 << " MIPS, limit "
                      << static_cast<float>(currentIpsMax) / 1000000 << " MIPS -> "
                      << (100 * currentIps) / currentIpsMax << "%" << endl
                      << flush;

                    if (!desktop) link.SetTitle(s.str());
                    if (options.runFor) cout << s.str();
                    if (options.runFor && zodiac) {
                        const auto status = zodiac->Status();
                        cout << "zodiac: at " << status.retired << " instructions, "
                             << status.time.ticks * 1000 / 32768 << " ms of guest time" << endl;
                    }
                }

                if (desktop) { desktop->microphoneChanged(commandContext); desktop->cameraChanged(commandContext); desktop->serialChanged(commandContext); }
                if (desktop) desktop->usbChanged(commandContext);
                if (desktop) desktop->infraredChanged(commandContext);
                if (desktop) desktop->bluetoothChanged(commandContext);
                if (desktop && desktop->poll(commandContext)) break;
                if (!options.runFor && !desktop && cli::Execute(&commandContext)) break;

                // The SDL thread resizes the window and sends the new layout back with the
                // rotation; input uses the old rotation until then.
                const Rotation wanted = Rotation((int(commandContext.rotation) + guestTurns) % 4);
                if (wanted != rotation) {
                    rotation = wanted;
                    link.RequestRotation(rotation);
                }

                performance.ReportWork(mainLoop.GetTimesliceSizeUsec());
                const auto delay = mainLoop.GetSuggestedDelayUsec(timestampUsec());
                if (delay > 10) usleep(delay);
            }
        };

        // Events go straight from SDL to the guest thread through the filter.
        link.InstallFilter();

        exception_ptr guestError;
        thread guest([&] {
#ifdef __linux__
            pthread_setname_np(pthread_self(), "FrondCore");
#endif
            try {
                guestLoop();
            } catch (...) {
                guestError = current_exception();
            }
            link.Finish();
        });

        // The SDL thread: pumps SDL events, draws handed-over frames and serves the guest
        // thread's requests until the guest thread leaves its loop.
        exception_ptr renderError;
        try {
            bool captured = false;
            Rotation shownRotation = rotation;
            // A new renderer or rotation waits while the app is in the background, as Android
            // may have no surface to draw on until it is back.
            bool rebuild = !sdlRenderer->Valid(), rendererFailed = false;
            optional<Rotation> rotationPending;
            optional<DisplayChange> displayPending;
            // A new renderer starts blank; draw the last frame into it at once. Returns false
            // when SDL couldn't make one, to try again on the next wake. Its layout carries the
            // shown controls and fit, so input on the guest thread matches what it draws.
            auto newRenderer = [&](optional<Rotation> rotated) {
                sdlRenderer.reset();
                arrangeForWindow();
                // A window that turned between portrait and landscape can turn the Zodiac's panel.
                const Rotation drawn = rotationForWindow(shownRotation);
                sdlRenderer = make_unique<SdlRenderer>(window, soc, scale, displayConfiguration, drawn,
                                                       display.smooth, display.fit, options.windowBackground, controlView);
                if (!sdlRenderer->Valid()) {
                    if (!rendererFailed) cerr << "failed to create renderer: " << SDL_GetError() << endl;
                    rendererFailed = true;
                    return false;
                }
                rendererFailed = false;
                if (rotated || drawn != drawnRotation) rotated = drawn;
                drawnRotation = drawn;
                link.PushLayout(sdlRenderer->Layout(), rotated,
                                [&sdlEventHandler, &inputFitted, view = controlView, scale, fitted] {
                                    sdlEventHandler.SetView(view, scale);
                                    inputFitted = fitted;
                                });
                sdlRenderer->Render(link.FrontFrame(), 0, frameRows - 1, shownLcd, shownButtons, shownStick);
                return true;
            };

            // SDL's event pump runs the filter for desktop events and, on Android, notices
            // pause and resume (which come as semaphores, not events) and checks AAudio.
            // On Android the filter gets touches and window events without a pump, so the
            // pump runs only after a window or app event and every 100 ms. X11 events only
            // come in while SDL pumps, so the desktop pumps every 8 ms.
#ifdef __ANDROID__
            constexpr uint64_t pumpUsec = 100000;
#else
            constexpr uint64_t pumpUsec = 8000;
#endif
            uint64_t lastPump = 0;
            while (!link.Finished()) {
                const uint64_t waitUsec = appBackground ? 50000 : pumpUsec;
                const uint64_t sincePump = timestampUsec() - lastPump;
                link.Wait(sincePump >= waitUsec ? 0 : int((waitUsec - sincePump + 999) / 1000));

                bool inBackground = appBackground;
                RenderNotices notices = link.TakeNotices(inBackground);
                if (notices.pump || timestampUsec() - lastPump >= waitUsec) {
                    SDL_PumpEvents();
                    lastPump = timestampUsec();
                    // Anything SDL queued without the filter (none expected) goes over too.
                    link.DrainQueue();
                    notices |= link.TakeNotices(inBackground);
                }
                appBackground = inBackground;
                redraw = redraw || notices.redraw;
                // Back from the background or textures lost: upload the whole newest frame.
                if (notices.foreground || notices.redrawAll) redrawAll = true;
                // A fitted window lays the device out again once per wake, after the resize
                // events so far, and a lost renderer is made again.
                if ((notices.resized && fitted) || notices.lost) rebuild = true;
                if (const auto requested = link.TakeRotation()) rotationPending = requested;
                {
                    lock_guard<mutex> lock(displayMutex);
                    if (displayRequest && displayPending) displayPending->Merge(*displayRequest);
                    else if (displayRequest) displayPending = displayRequest;
                    displayRequest.reset();
                }

                // A display change resizes a window that isn't fitted, like a rotation, and the
                // new renderer below lays the device out with the new controls.
                if (!appBackground && displayPending) {
                    display.Apply(*displayPending);
                    displayPending.reset();
                    if (deviceType == deviceTypeTG50) controlView = deviceControlView(display.keyboard, display.jog);
                    const bool wasFitted = fitted;
                    fitted = display.fit != WindowFit::none;
                    sdlRenderer.reset();
                    if (fitted != wasFitted) SDL_SetWindowResizable(window, fitted ? SDL_TRUE : SDL_FALSE);
                    if (!fitted)
                        sdlResizeWindow(window, displayConfiguration, scale, shownRotation, deviceControls(deviceType), controlView);
                    rebuild = true;
                }

                if (!appBackground && (rotationPending || rebuild)) {
                    if (rotationPending) {
                        shownRotation = *rotationPending;
                        sdlRenderer.reset();
                        // A fitted window keeps its size and lays the rotated device out inside it.
                        if (!fitted)
                            sdlResizeWindow(window, displayConfiguration, scale, shownRotation, deviceControls(deviceType), controlView);
                    }
                    if (newRenderer(rotationPending)) {
                        rotationPending.reset();
                        rebuild = false;
                    }
                }
                if (const auto title = link.TakeTitle()) SDL_SetWindowTitle(window, title->c_str());

                const bool capture = sdlEventHandler.WantsCapture();
                if (capture != captured) {
                    SDL_CaptureMouse(capture ? SDL_TRUE : SDL_FALSE);
                    captured = capture;
                }

                drawPending();
                link.RunCall();
            }
        } catch (...) {
            renderError = current_exception();
        }
        link.Stop();
        guest.join();
        SDL_SetEventFilter(nullptr, nullptr);
        microphone.SetDeviceThread({});
        microphone.CloseOnDeviceThread();
        if (hasMicrophone) static_cast<SocPXA*>(soc)->SetMicrophoneSource(nullptr);
        if (hasCamera) static_cast<SocPXA*>(soc)->SetCameraSource(nullptr);
        if (renderError) rethrow_exception(renderError);
        if (guestError) rethrow_exception(guestError);
        // The last frame the guest thread published, as the one-thread loop drew it.
        if (options.framebufferOutput) drawPending();

        infrared.Stop();
        serial.Stop();
        if (const auto* host = usb.Host()) {
            const auto& c = host->GetCounters();
            cout << "usb: " << c.ticks << " ticks, " << c.frames << " frames, " << c.transactions << " transactions, "
                 << c.setupRetries << " SETUP retries" << endl;
        }
        usb.Stop();
        if (sdlEventHandler.InBackground()) audioDriver.Abandon();
        else audioDriver.Pause();
        reportAudioUnderruns();
        if (!options.runFor && !options.desktopSocket) cli::Stop();
        // A candidate card no save adopted holds nothing the folder needs.
        sdCardReset();
        frondSdCandidate::releaseUnused();

        if (desktop && desktop->SaveFailed()) return false;

        if (options.framebufferOutput) {
            bool blank = false;
            if (!sdlRenderer->SaveFramebuffer(options.framebufferOutput->c_str(), blank)) {
                cerr << "failed to save framebuffer: " << SDL_GetError() << endl;
                return false;
            }
            if (blank) cout << "framebuffer output with the LCD off, saved a black frame" << endl;
        }

        return true;
    }

}  // namespace

int main(int argc, char** argv) {
    // A launcher that dies leaves stdout and stderr without a reader; the core still has to save.
    signal(SIGPIPE, SIG_IGN);
    // The card tool: make, import or export a card file without starting a machine.
    if (argc >= 2 && std::string(argv[1]) == "--card")
        return frondCardTool::cliMain(std::vector<std::string>(argv + 2, argv + argc));

    argparse::ArgumentParser program("frond-core", "0.15.0");

    program.add_description("Frond Palm OS emulator");

    program.add_argument("nor_or_session").help("NOR rom or saved session").required();

    program.add_argument("--nand", "-n").help("NAND rom file").metavar("<nand file>");

    program.add_argument("--device")
        .help("device profile name for raw firmware; detected from firmware when omitted")
        .metavar("<name>");

    program.add_argument("--run-for")
        .help("exit after this many wall-clock seconds and print execution diagnostics")
        .scan<'u', unsigned int>();

    program.add_argument("--framebuffer-output")
        .help("save the unrotated LCD framebuffer as PNG at the end of --run-for");

    program.add_argument("--engine")
        .help("CPU engine: exact (the interpreter) or fast (the JIT where it's built in, the Zodiac's own Fast path)")
        .metavar("<exact|fast>")
        .default_value(string("exact"))
        .action([](const string& value) {
            if (value != "exact" && value != "fast") throw runtime_error("--engine is exact or fast");
            return value;
        });

    program.add_argument("--view-turn")
        .help("what turns a device whose OS turns its screen (the Zodiac): program (upright for the running "
              "program) or window (upright for the window's shape)")
        .metavar("<program|window>")
        .default_value(string("program"))
        .action([](const string& value) {
            if (value != "program" && value != "window") throw runtime_error("--view-turn is program or window");
            return value;
        });

    program.add_argument("--fast-audio", "--experimental-fast-audio")
        .help("preview one qualified stereo stream before the firmware audio queue")
        .default_value(false).implicit_value(true);

    program.add_argument("--low-latency-queue")
        .help("use the low-latency audio queue limits without the preview, on any device")
        .default_value(false).implicit_value(true);

    program.add_argument("--input-replay")
        .help("replay timestamped pen/key events and snapshots from a text file");

    program.add_argument("--emulated-time")
        .help("give every guest second the full --mips, however slow the host, and time input replays in guest time")
        .default_value(false).implicit_value(true);

    program.add_argument("--sd", "-s").help("SD card file").metavar("<SD card file>");

    program.add_argument("--ram-size")
        .help("RAM size in MB (16 or 32)")
        .metavar("<size>")
        .scan<'u', unsigned int>();
    ;

    program.add_argument("--audio-period")
        .help("host audio period in frames (64..8192); default 512")
        .metavar("<frames>")
        .scan<'u', unsigned int>()
        .default_value(512u);

    program.add_argument("--no-sound", "-q")
        .help("start with audio off")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--gdb")
        .help("wait for native ARM/Thumb GDB on 127.0.0.1 (port 1..65535)")
        .metavar("<port>")
        .scan<'u', unsigned int>();

    program.add_argument("--mips")
        .help("target speed in MIPS; default: the device's Original speed")
        .metavar("<mips>")
        .scan<'u', unsigned int>();

    program.add_argument("--small-window")
        .help("use a 1x window (alias for --window-scale 1)")
        .default_value(false)
        .implicit_value(true);

    program.add_argument("--window-scale")
        .help("window scale: 1, 1.25, 1.5, 1.75 or 2; default: largest that fits")
        .action([](const string& value) {
            if (value == "1") return 1.f;
            if (value == "1.25") return 1.25f;
            if (value == "1.5") return 1.5f;
            if (value == "1.75") return 1.75f;
            if (value == "2") return 2.f;
            throw runtime_error("invalid window scale: " + value);
        });
    program.add_argument("--window-fit")
        .help("fit the device to a resizable window: fit, whole or fill")
        .action([](const string& value) {
            WindowFit fit;
            if (!parseWindowFit(value, fit)) throw runtime_error("invalid window fit: " + value);
            return fit;
        });
    program.add_argument("--window-background")
        .help("colour around a fitted device as RRGGBB; default: 000000")
        .action([](const string& value) {
            uint32_t colour;
            if (!parseWindowColour(value, colour)) throw runtime_error("invalid window background: " + value);
            return colour;
        });
    program.add_argument("--smooth-lcd")
        .help("smooth LCD scaling instead of sharp nearest-neighbour pixels")
        .default_value(false).implicit_value(true);

    program.add_argument("--hide-keyboard")
        .help("hide the TG50 keyboard")
        .default_value(false).implicit_value(true);
    program.add_argument("--hide-jog-controls")
        .help("hide the TG50 jog controls; same as --jog-controls hidden")
        .default_value(false).implicit_value(true);
    program.add_argument("--jog-controls")
        .help("TG50 jog controls: hidden, screen (left of the LCD) or keyboard (left of the keyboard); default: screen")
        .action([](const string& value) {
            JogPlacement placement;
            if (!parseJogPlacement(value, placement)) throw runtime_error("invalid jog controls: " + value);
            return placement;
        });

    program.add_argument("--display")
        .help(
            "display mode (if supported); valid are 160x160, 160x240, 240x240, 240x320, "
            "320x320, 320x480, 480x480, 480x720")
        .metavar("<resolution>")
        .action([](const string& value) {
            if (value == "160x160") return DisplayMode::mode_160x160;
            if (value == "160x220") return DisplayMode::mode_160x220;
            if (value == "240x240") return DisplayMode::mode_240x240;
            if (value == "240x320") return DisplayMode::mode_240x320;
            if (value == "320x320") return DisplayMode::mode_320x320;
            if (value == "320x480") return DisplayMode::mode_320x480;
            if (value == "480x480") return DisplayMode::mode_480x480;
            if (value == "480x720") return DisplayMode::mode_480x720;

            throw runtime_error("invalid display mode");
        });

    program.add_argument("--desktop-socket").help("private desktop control socket");

    program.add_argument("--save-on-disconnect")
        .help("save the session here if the desktop's control socket goes without a quit")
        .metavar("<session file>");

    program.add_argument("--work-folder")
        .help("run a session from elsewhere in this empty machine folder; its cards are copied there")
        .metavar("<folder>");

    program.add_argument("--card-swap")
        .help("the machine folder's card files may have changed since the save; a changed slot resumes as a hot swap")
        .default_value(false).implicit_value(true);
    program.add_argument("--card-identity")
        .help("with --card-swap, the identity of a card put in, as 64 hex digits")
        .metavar("<hex>");
    program.add_argument("--card-protected")
        .help("with --card-swap, a card put in is write protected")
        .default_value(false).implicit_value(true);

    program.add_argument("--serial")
        .help("connect the TX serial port to a host PTY (pty), a TCP listener on 127.0.0.1 "
              "(tcp:PORT) or a Unix socket (unix:PATH, in a 0700 folder you own), one client at a time; default off")
        .metavar("<off|pty|tcp:PORT|unix:PATH>");

    program.add_argument("--bluetooth")
        .help("connect the virtual Bluetooth radio to a local H4 controller Unix socket; default off")
        .metavar("<off|/controller/socket>");

    program.add_argument("--irda")
        .help("connect infrared SIR bytes to a host PTY, loopback TCP listener or Unix socket, phone for the core's mobile phone or printer for its IrDA printer; exclusive with keyboard; default off")
        .metavar("<off|pty|tcp:PORT|unix:PATH|phone|printer>");

    program.add_argument("--usb")
        .help("connect the TX USB HotSync port to a host PTY (pty) or a TCP listener on 127.0.0.1 "
              "(tcp:PORT, one client at a time) through a built-in USB host; default off")
        .metavar("<off|pty|tcp:PORT|unix:PATH>");

    program.add_argument("--script").help("execute script on startup").metavar("<script file>");

    try {
        program.parse_args(argc, argv);
    } catch (const invalid_argument& e) {
        cerr << "invalid argument" << endl << endl;
        cerr << program;

        exit(1);
    } catch (const runtime_error& e) {
        cerr << e.what() << endl << endl;
        cerr << program;

        exit(1);
    }

    Options options = {.norOrSession = program.get("nor_or_session"),
                       .nand = program.present("--nand"),
                       .sd = program.present("--sd"),
                       .gdbPort = program.present<unsigned int>("--gdb"),
                       .mips = program.present<unsigned int>("--mips"),
                       .disableAudio = program.get<bool>("--no-sound"),
                       .audioPeriod = program.get<unsigned int>("--audio-period"),
                       .script = program.present("--script"),
                       .ramSize = program.present<unsigned int>("--ram-size"),
                       .smallWindow = program.get<bool>("--small-window"),
                       .windowScale = program.present<float>("--window-scale"),
                       .smoothLcd = program.get<bool>("--smooth-lcd"),
                       .hideKeyboard = program.get<bool>("--hide-keyboard"),
                       .hideJogControls = program.get<bool>("--hide-jog-controls"),
                       .jogPlacement = program.present<JogPlacement>("--jog-controls"),
                       .windowFit = program.present<WindowFit>("--window-fit").value_or(WindowFit::none),
                       .windowBackground = program.present<uint32_t>("--window-background").value_or(0),
                       .displayMode = program.present<DisplayMode>("--display"),
                       .device = program.present("--device"),
                       .experimentalFastAudio = program.get<bool>("--fast-audio"),
                       .lowLatencyQueue = program.get<bool>("--low-latency-queue"),
                       .runFor = program.present<unsigned int>("--run-for"),
                       .framebufferOutput = program.present("--framebuffer-output"),
                       .inputReplay = program.present("--input-replay"),
                       .emulatedTime = program.get<bool>("--emulated-time"),
                       .desktopSocket = program.present("--desktop-socket"),
                       .saveOnDisconnect = program.present("--save-on-disconnect"),
                       .workFolder = program.present("--work-folder"),
                       .serial = program.present("--serial"),
                       .infrared = program.present("--irda"),
                       .usb = program.present("--usb"),
                       .bluetooth = program.present("--bluetooth"),
                       .cardSwap = program.get<bool>("--card-swap"),
                       .cardIdentity = program.present("--card-identity"),
                       .cardProtected = program.get<bool>("--card-protected"),
                       .fastEngine = program.get("--engine") == "fast",
                       .viewFollowsWindow = program.get("--view-turn") == "window"};

    logEnable();

    // Said once the machine is known: the Zodiac has its own Fast engine and doesn't need this one.
    gFastEngineMissing = !cpuSetFastEngine(program.get("--engine") == "fast");

    try {
        if (!run(options)) return 1;
    } catch (const std::exception& error) {
        cerr << error.what() << endl;
        return 1;
    }

    return 0;
}
