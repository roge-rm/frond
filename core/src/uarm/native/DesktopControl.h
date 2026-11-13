#ifndef FROND_DESKTOP_CONTROL_H
#define FROND_DESKTOP_CONTROL_H

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <memory>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "Commands.h"
#include "DesktopChannel.h"
#include "device_profile5.h"
#include "ReadOnlyGuest.h"
#include "HostSerial.h"
#include "HostInfrared.h"
#include "UsbHostBridge.h"
#include "soc_PXA.h"
#include "pxa_UART.h"
#include "native_boundary.h"
#include "CPU.h"
#include "SdImage.h"
#include "MemoryStickImage.h"
#include "mmiodev_MemoryStickController.h"
#include "app_launcher.h"
#include "md5.h"
#include "GuestExport.h"
#include "GuestDatabaseOps.h"
#include "GuestFiles.h"
#include "FileUtil.h"
#include "db_backup.h"
#include "db_installer.h"
#include "session/session_file5.h"
#include "MachineSession.h"
#include "MsProCards.h"
#include "CardSwap.h"
#include "CardReinsert.h"
#include "CardTool.h"

// Private Unix socket, owned by the launcher; DesktopChannel has the framing. Only the emulation
// thread dispatches requests.
class DesktopControl {
 public:
    // saveOnLoss: the session the launcher saves on close, or empty to quit without saving when it goes.
    DesktopControl(const std::string& path, bool sound, std::string saveOnLoss = {})
        : channel(path), sound(sound), saveOnLoss(std::move(saveOnLoss)) {}
    void event(const char* name, const std::string& message = {}) { channel.Event(name, message); }
    void microphoneChanged(commands::Context& ctx) {
        const auto state = ctx.microphone ? ctx.microphone->State() : "off";
        if (state == lastMicrophone || (state == "off" && lastMicrophone.empty())) return;
        lastMicrophone = state; event("microphone", state);
    }
    // A camera event when the source changes, for example back to the pattern on sleep.
    void cameraChanged(commands::Context& ctx) {
        const auto state = ctx.camera ? ctx.camera->State() : "pattern";
        if (state == lastCamera || (state == "pattern" && lastCamera.empty())) return;
        lastCamera = state; event("camera", state);
    }
    // Pushes the serial state when it changes, for example on a peer connect or hangup.
    void serialChanged(commands::Context& ctx) {
        // Clients that never use serial never see the event.
        const auto state = SerialState(ctx);
        if (state == lastSerial || (state == "off" && lastSerial.empty())) return;
        lastSerial = state;
        event("serial", state);
    }
    // Same for the USB bridge.
    void usbChanged(commands::Context& ctx) {
        const auto state = UsbState(ctx);
        if (state == lastUsb || (state == "off" && lastUsb.empty())) return;
        lastUsb = state;
        event("usb", state);
    }

    void infraredChanged(commands::Context& ctx) {
        const auto state=ctx.infrared ? ctx.infrared->Describe() : "off";
        if (state==lastInfrared || (state=="off" && lastInfrared.empty())) return;
        lastInfrared=state;event("infrared",state);
    }

    static const DeviceCapabilities5& capabilities(commands::Context& ctx) {
        return deviceProfile5(ctx.soc->GetDeviceType())->capabilities;
    }

    void bluetoothChanged(commands::Context& ctx) {
        if(!capabilities(ctx).bluetooth)return;
        const auto state=static_cast<SocPXA*>(ctx.soc)->BluetoothState();
        if(state==lastBluetooth || (state=="off" && lastBluetooth.empty()))return;
        lastBluetooth=state;event("bluetooth",state);
    }

    bool PanelVisible() const { return channel.PanelVisible(); }

    // The save when the launcher went away failed; the core exits non-zero.
    bool SaveFailed() const { return saveFailed; }
    bool poll(commands::Context& ctx) {
        DesktopChannel::Request request;
        switch (channel.Receive(request)) {
            case DesktopChannel::Poll::closed: return lost(ctx);
            case DesktopChannel::Poll::handled: return false;
            case DesktopChannel::Poll::idle: break;
            case DesktopChannel::Poll::request: {
                const auto& command = request.command;
                const bool work = command == "guest-ready" || command == "save" || command == "install" || (command == "export" || command == "export-db") || command == "db-list" || command == "reset" || command == "hard-reset" || command == "launch-app" || command == "delete-db" || command == "input-replay" || (command.rfind("sd-", 0) == 0 && command != "sd-state") || (command.rfind("ms-",0)==0&&command!="ms-state"&&command!="ms-type"&&command!="ms-protection");
                if (work) {
                    // Host calls only drop capture for their length; the user's choice stays and capture comes back.
                    if (ctx.microphone) ctx.microphone->Suspend();
                    ctx.audioDriver.Pause();
                }
                if (command == "save" && ctx.camera) ctx.camera->Disarm();
                Reply result;
                try {
                    result = dispatch(ctx, command, request.argument, request.id);
                } catch (const GuestFiles::Cancelled& error) {
                    result = {false, "cancelled", error.what()};
                } catch (const InjectedCallError& error) {
                    result = {false, "timeout", error.what()};
                } catch (const std::exception& error) {
                    result = {false, "failed", error.what()};
                }
                if (work) {
                    // File I/O and Palm API injection deliberately pause playback; do not catch up that wall time.
                    if (sound) ctx.audioDriver.Start();
                    ctx.mainLoop.PauseAt(timestampUsec());
                }
                channel.Reply(request.id, result.ok, result.code, result.message);
                break;
            }
        }
        if (!channel.Flush()) return lost(ctx);
        return quit && channel.OutputEmpty();
    }

 private:
    // The socket closed. When the launcher went away without asking to quit, the session is saved
    // where its close would have saved it, here on the emulation thread like a save request.
    bool lost(commands::Context& ctx) {
        if (saveOnLoss.empty() || quit || !channel.Lost() || channel.QuitPending()) return true;
        if (ctx.microphone) ctx.microphone->Disarm();
        ctx.audioDriver.Pause();
        saveFailed = !frondNative::saveMachine(saveOnLoss, *ctx.soc, ctx.sessionPath, ctx.sessionFolder);
        if (saveFailed)
            std::cerr << "desktop: the launcher went away and the session could not be saved" << std::endl;
        else
            std::cout << "desktop: the launcher went away, session saved to " << saveOnLoss << std::endl;
        return true;
    }
    bool flush() { return channel.Flush(); }
    std::string lastMicrophone, lastCamera, lastSerial, lastUsb, lastInfrared, lastBluetooth;
    static std::string UsbState(commands::Context& ctx) {
        if (!ctx.usb) return "off";
        const auto state = ctx.usb->Describe();
        return state == "off" && !ctx.usbError.empty() ? "off " + ctx.usbError : state;
    }
    // Reads the TX display driver's rotation in degrees, the value SysGetOrientation converts, without
    // running guest code. The driver's globals are at [[0x1ff00ffc + 0x248] + 0x3c] on the TX 5.4 ROM; the
    // getter's code is checked first so another ROM gives "" (unknown).
    static std::string TxOrientation(ArmCpu* cpu) {
        const auto word = [cpu](uint32_t address, uint32_t& value) {
            return cpuMemOpExternal(cpu, &value, address, 4, false);
        };
        static const uint32_t getter[] = {0xe5999000, 0xe5999248, 0xe59f05a8, 0xe0800009, 0xe1d000b0};
        uint32_t value = 0;
        for (unsigned i = 0; i < 5; ++i)
            if (!word(0x2033a4a4 + 4 * i, value) || value != getter[i]) return "";
        uint32_t offset = 0, base = 0;
        if (!word(0x2033aa5c, offset) || offset != 0x3c || !word(0x1ff00ffc + 0x248, base) || !base || (base & 3))
            return "";
        uint16_t degrees = 0;
        if (!cpuMemOpExternal(cpu, &degrees, base + offset, 2, false)) return "";
        switch (degrees) {
            case 0: return "portrait";
            case 90: return "landscape";
            case 180: return "reverse-portrait";
            case 270: return "reverse-landscape";
            default: return "";
        }
    }
    // on: playing, muted: playing silence, off: no device open or paused by audio-off.
    std::string SoundState(commands::Context& ctx) const {
        if (!sound || !ctx.audioDriver.Running()) return "off";
        return ctx.audioDriver.Muted() ? "muted" : "on";
    }
    static std::string SerialState(commands::Context& ctx) {
        if (!ctx.serial) return "off";
        const auto state = ctx.serial->Describe();
        return state == "off" && !ctx.serialError.empty() ? "off " + ctx.serialError : state;
    }
    struct Reply { bool ok{false}; std::string code{"failed"}, message; };
    static std::string encode(const std::string& value) { return DesktopChannel::Encode(value); }
    static bool decode(const std::string& value, std::string& out) { return DesktopChannel::Decode(value, out); }
    bool cancelFiles(const std::string& job) {
        const auto state = channel.CheckCancel(job);
        if (state == DesktopChannel::Cancel::gone) throw GuestFiles::Cancelled();
        return state == DesktopChannel::Cancel::cancelled;
    }
    static void writeAtomic(const std::string& path, const void* bytes, size_t size, const GuestFiles::Cancel& cancel = {}) {
        std::string temporary = path + ".tmp-XXXXXX";
        const int file = mkstemp(temporary.data());
        if (file < 0) throw std::runtime_error("cannot create output file: " + std::string(strerror(errno)));
        bool open = true;
        try {
            const auto* data = static_cast<const uint8_t*>(bytes);
            while (size) {
                const auto n = write(file, data, size);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) throw std::runtime_error("cannot write output file");
                data += n; size -= n;
            }
            if (fsync(file) < 0) throw std::runtime_error("cannot flush output file");
            const auto closed = close(file); open = false;
            if (closed < 0) throw std::runtime_error("cannot close output file");
            GuestFiles::CheckCancel(cancel);
            if (rename(temporary.c_str(), path.c_str()) < 0) throw std::runtime_error("cannot replace output file");
            // Makes the rename itself durable; a failure here leaves the new file in place.
            const auto slash = path.rfind('/');
            const int folder = ::open(slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash).c_str(),
                                      O_RDONLY | O_DIRECTORY);
            if (folder >= 0) { fsync(folder); ::close(folder); }
        } catch (...) {
            if (open) close(file);
            unlink(temporary.c_str());
            throw;
        }
    }
    // Progress for SD create/import/export: an sd-progress event ("done total" in bytes) at each
    // whole percent, and a cancel-files for this job stops it.
    frondSdCandidate::Progress sdProgress(const std::string& job) {
        auto last = std::make_shared<int>(-1);
        return [this, job, last](uint64_t done, uint64_t total) {
            const int percent = total ? int(done * 100 / total) : 100;
            if (percent != *last) {
                *last = percent;
                event("sd-progress", std::to_string(done) + " " + std::to_string(total));
                flush();
            }
            try {
                return !cancelFiles(job);
            } catch (...) {
                return false;
            }
        };
    }
    Reply dispatch(commands::Context& ctx, const std::string& command, const std::string& path, const std::string& job) {
        // On a Memory Stick device the card store holds a Memory Stick Pro, reached through ms-*.
        // This comes before the argument checks so every sd-* command gets the same answer.
        if (command.rfind("sd-", 0) == 0) {
            auto* pxa = dynamic_cast<SocPXA*>(ctx.soc);
            if (pxa && pxa->GetMemoryStick())
                return command == "sd-state" && path.empty() ? Reply{true, "card", "none"}
                                                             : Reply{false, "unsupported", "This device has no SD slot."};
        }
        const bool msFilesCommand = command == "ms-files" || command == "ms-get" || command == "ms-put" ||
            command == "ms-mkdir" || command == "ms-delete" || command == "ms-rename";
        const bool fileCommand = command == "save" || command == "install" || (command == "export" || command == "export-db") || command == "sd-insert" || command == "sd-export" || command == "ms-insert" || command == "ms-export" || command == "input-replay";
        if (fileCommand && (path.empty() || path.front() != '/')) return {false, "argument", "An absolute file path is required."};
        if (!fileCommand && !msFilesCommand && command != "sd-create" && command != "ms-create" && command != "sd-put-in" && command != "ms-put-in" && command != "ms-read-only" && command != "launch-app" && command != "delete-db" && command != "network-mode" && command != "keyboard-attach" && command != "keyboard-key" && command != "serial-mode" && command != "usb-mode" && command != "irda-mode" && command != "irprinter" && command != "printer" && command != "bluetooth-mode" && command != "sound-mode" && command != "display" && command != "microphone-mode" && command != "camera-mode" && command != "hold" && !path.empty()) return {false, "argument", "This command takes no argument."};
        // A board with no guest calls takes none of the commands that need them, and has no hard reset yet.
        if (!ctx.soc->GuestServicesSupported()) {
            for (const char* needs : {"guest-ready", "launch-app", "delete-db", "install", "export", "export-db", "db-list",
                                      "guest-orientation", "power-state", "hotsync"})
                if (command == needs) return {false, "unsupported", "This isn't available on this device yet."};
        }
        if (command == "hard-reset" && !ctx.soc->HardResetSupported())
            return {false, "unsupported", "Hard reset isn't available on this device yet."};
        if (frondNative::insertsCard(command)) frondNative::settleEject(*ctx.soc);
        if (command == "guest-ready") {
            if (!GuestDatabaseOps::Ready(*ctx.soc))
                return {false, "not-ready", "Palm OS has not reached a supported application boundary."};
            return {true, "guest-ready", "Palm OS can accept application commands."};
        }
        if (command == "replay-state")
            return {true, ctx.replay.Paused() ? "paused" : ctx.replay.Finished() ? "finished" : "running",
                    std::to_string(ctx.replay.LastEventMs())};
        if (command == "replay-snapshots") {
            std::string results;
            for (const auto& result : ctx.replay.SnapshotResults()) results += (results.empty() ? "" : ",") + result;
            return {true, "snapshots", results};
        }
        if (command == "input-replay") {
            InputReplay replacement; std::string error;
            if (!replacement.Load(path, ctx.screenWidth, ctx.screenHeight, error)) return {false, "replay", error};
            ctx.replay = std::move(replacement); ctx.replayStartedAt = commands::replayClockUsec(ctx);
            return {true, "loaded", "Input replay loaded."};
        }
        if (command == "replay-continue") {
            if (!ctx.replay.Paused()) return {false, "not-paused", "The input replay isn't paused."};
            // Picks up at the pause's own time, however long it was held.
            ctx.replayStartedAt = commands::replayClockUsec(ctx) - ctx.replay.Continue() * 1000;
            return {true, "continued", "Input replay continued."};
        }
        if (command == "launch-app") {
            if (path.empty() || path.size() > 31) return {false, "argument", "Enter a Palm application database name (up to 31 bytes)."};
            const auto launched = GuestDatabaseOps::Launch(*ctx.soc, path);
            if (launched == GuestDatabaseOps::LaunchResult::notReady)
                return {false, "not-ready", "Wait for Palm OS to reach a supported application boundary."};
            if (launched != GuestDatabaseOps::LaunchResult::launched)
                return {false, "launch", "Could not launch that application. Check its database name and wait for Palm OS to finish starting."};
            return {true, "launched", "Application launched."};
        }
        if (command == "delete-db") {
            if (path.empty() || path.size() > 31) return {false, "argument", "Enter a Palm database name (up to 31 bytes)."};
            const auto deleted = GuestDatabaseOps::Delete(*ctx.soc, path);
            if (!deleted) return {false, "not-ready", "Wait for Palm OS to reach a supported application boundary."};
            switch (*deleted) {
                case DeleteDbResult::deleted: return {true, "deleted", "Database deleted."};
                case DeleteDbResult::missing: return {false, "missing", "There is no Palm database with that name."};
                case DeleteDbResult::rom: return {false, "rom", "That database is in ROM and can't be deleted."};
                case DeleteDbResult::running: return {false, "running", "That application is running. Switch to another one first."};
                case DeleteDbResult::open: return {false, "open", "That database is open. Close its application first."};
                default: return {false, "delete", "The database could not be deleted."};
            }
        }
        if (command == "device-state") return {true, "device", deviceProfile5(ctx.soc->GetDeviceType())->name};
        if (command == "power-state") {
            const auto* machine = static_cast<SocPXA*>(ctx.soc);
            return {true, "power", std::string(machine->PowerSleeping() ? "asleep" : "awake") +
                                       " wakes=" + std::to_string(machine->PowerWakes())};
        }
        const auto& caps = capabilities(ctx);
        if (!caps.serial && command == "serial-mode")
            return {false, "unsupported", std::string("The ") + deviceProfile5(ctx.soc->GetDeviceType())->displayName + " has no serial port."};
        if (!caps.network && command == "network-mode" && path != "disabled")
            return {false, "unsupported", "This device has no emulated network."};
        if(command=="bluetooth-state" || command=="bluetooth-mode") {
            if(!caps.bluetooth)
                return {false,"unsupported","This device has no Bluetooth radio."};
            auto* machine=static_cast<SocPXA*>(ctx.soc);
            if(command=="bluetooth-mode") {
                if(path!="off" && (path.empty() || path.front()!='/' || path.find(char(0))!=std::string::npos))
                    return {false,"argument","Use off or an absolute virtual controller Unix socket path."};
                if(!machine->SetBluetooth(path=="off"?std::string():path,ctx.bluetoothError))
                    return {false,"bluetooth",ctx.bluetoothError};
            }
            return {true,"bluetooth",machine->BluetoothState()};
        }
        if (command == "irda-stats") {
            if (!ctx.infrared || !caps.infrared)
                return {false,"unsupported","This device has no infrared endpoint."};
            auto* machine=static_cast<SocPXA*>(ctx.soc);const auto state=pxaUartIrState(machine->GetIrUart());
            if (ctx.infrared->PhoneMode() || ctx.infrared->PrinterMode()) return {true,"infrared",ctx.infrared->PhoneStats()};
            const auto c=ctx.infrared->GetCounters();
            return {true,"infrared", "rx="+std::to_string(c.received)+" tx="+std::to_string(c.sent)+
                " rx_drop="+std::to_string(ctx.infrared->DroppedReceive())+" tx_drop="+std::to_string(ctx.infrared->DroppedTransmit())+
                " pending="+std::to_string(ctx.infrared->PendingReceive())+" baud="+std::to_string(state.baud)+
                " rx_on="+std::to_string(state.receive)+" tx_on="+std::to_string(state.transmit)+
                " power="+std::to_string(machine->IrPowerEnabled())+" sleeping="+std::to_string(machine->PowerSleeping())};
        }
        if (command == "irda-state") return {true,"infrared",ctx.infrared ? ctx.infrared->Describe() : "off"};
        if (command == "printer") return {false,"unsupported","This device has no USB printer."};
        if (command == "irprinter") {
            if (!ctx.infrared || !caps.infrared) return {false,"unsupported","This device has no infrared port."};
            const auto split=path.find(' ');
            std::string result;
            const bool ok=ctx.infrared->PrinterCommand(path.substr(0,split),split==std::string::npos?"":path.substr(split+1),result);
            return {ok,"irprinter",result};
        }
        if (command == "irda-mode") {
            if (!caps.infrared)
                return {false,"unsupported","This device has no infrared port."};
            if (ctx.infrared && path == "phone") {
                if (!caps.irPhone) return {false,"unsupported","This device has no infrared phone."};
                const bool ok=ctx.infrared->StartPhone(static_cast<SocPXA*>(ctx.soc),ctx.infraredError);
                return {ok,"infrared",ok?ctx.infrared->Describe():ctx.infraredError};
            }
            if (ctx.infrared && path == "printer") {
                const bool ok=ctx.infrared->StartPrinter(static_cast<SocPXA*>(ctx.soc),ctx.infraredError);
                return {ok,"infrared",ok?ctx.infrared->Describe():ctx.infraredError};
            }
            HostSerial::Config config;
            if (!ctx.infrared || !HostSerial::Parse(path,config)) return {false,"argument","Use off, pty, tcp:PORT, unix:PATH, phone or printer."};
            const bool ok=ctx.infrared->Start(static_cast<SocPXA*>(ctx.soc),config,ctx.infraredError);
            return {ok,"infrared",ok?ctx.infrared->Describe():ctx.infraredError};
        }
        if (command == "hotsync") {
            if (!static_cast<SocPXA*>(ctx.soc)->PressHotSync())
                return {false, "unsupported", "This device has no HotSync button, or one is already being pressed."};
            return {true, "hotsync", "HotSync button pressed."};
        }
        if (command == "hold-state")
            return {true, "hold", !ctx.soc->HoldSwitchSupported() ? "unavailable" :
                    ctx.soc->HoldSwitchOn() ? "on" : "off"};
        if (command == "hold") {
            if (path != "on" && path != "off") return {false, "argument", "Use on or off."};
            if (!ctx.soc->SetHoldSwitch(path == "on"))
                return {false, "unsupported", "This device has no supported Hold switch."};
            return {true, "hold", ctx.soc->HoldSwitchOn() ? "on" : "off"};
        }
        if (command == "keyboard-state") return {true,"keyboard",ctx.soc->KeyboardAttached()?"attached":"detached"};
        if (command == "keyboard-attach") {
            if (path!="on" && path!="off") return {false,"argument","Use on or off."};
            ctx.soc->AttachKeyboard(path=="on");
            if (ctx.soc->KeyboardAttached()!=(path=="on"))
                return {false,"infrared","Infrared peer owns this UART; turn it off before attaching the keyboard."};
            return {true,"keyboard",ctx.soc->KeyboardAttached()?"attached":"detached"};
        }
        if (command == "keyboard-key") {
            unsigned code; char state,extra;
            if (sscanf(path.c_str(),"%x %c %c",&code,&state,&extra)!=2 || !code || code>=128 || (state!='d' && state!='u'))
                return {false,"argument","Use a hexadecimal scan code followed by d or u."};
            return {ctx.soc->KeyboardKey(code,state=='d'),"keyboard","Keyboard event."};
        }
        if (command == "serial-state") return {true, "serial", SerialState(ctx)};
        if (command == "serial-mode") {
            HostSerial::Config config;
            if (!ctx.serial || !HostSerial::Parse(path, config)) return {false, "argument", "Use off, pty, tcp:PORT or unix:PATH."};
            // A new endpoint detaches the old one first; queued bytes are dropped.
            ctx.serialError.clear();
            if (!ctx.serial->Start(ctx.serialUart, config, ctx.serialError, ctx.serialRoute)) return {false, "serial", ctx.serialError};
            return {true, "serial", SerialState(ctx)};
        }
        if (command == "usb-state") return {true, "usb", UsbState(ctx)};
        if (command == "usb-mode") {
            if (!caps.usb) return {false,"unsupported","USB is not supported on this device."};
            HostSerial::Config config;
            if (!ctx.usb || !HostSerial::Parse(path, config)) return {false, "argument", "Use off, pty, tcp:PORT or unix:PATH."};
            // A new endpoint unplugs the cable first; queued bytes are dropped.
            ctx.usbError.clear();
            if (!ctx.usb->Start(dynamic_cast<SocPXA*>(ctx.soc), config, ctx.usbError)) return {false, "usb", ctx.usbError};
            return {true, "usb", UsbState(ctx)};
        }
        if (command == "network-state") return {true, "network", ctx.soc->networkSimulation.GetModeName()};
        if (command == "network-mode") {
            if (!ctx.soc->networkSimulation.SetMode(path)) return {false, "argument", "Unknown network mode."};
            if (ctx.soc->wirelessDevice) ctx.soc->wirelessDevice->Poll();
            return {true, "network", ctx.soc->networkSimulation.GetModeName()};
        }
        if (command.rfind("ms-",0)==0) {
            auto* pxa=dynamic_cast<SocPXA*>(ctx.soc);
            auto* ms=pxa?pxa->GetMemoryStick():nullptr;
            if(!ms)return {false,"unsupported","This device has no Memory Stick slot."};
            if (msFilesCommand) {
                const GuestFiles::Cancel cancel = [this, job] { return cancelFiles(job); };
                GuestFiles::CheckCancel(cancel);
                if (!msCtrlrInserted(ms)) return {false, "empty", "Insert the Memory Stick before opening its files."};
                const bool writing = command == "ms-put" || command == "ms-mkdir" || command == "ms-delete" || command == "ms-rename";
                if (writing && msCtrlrReadOnly(ms)) return {false, "protected", "The Memory Stick is write protected."};
                std::string cardPath = path, other;
                if (command == "ms-get" || command == "ms-put" || command == "ms-rename") {
                    const auto split = path.find('\n');
                    if (split == std::string::npos || path.find('\n', split + 1) != std::string::npos)
                        return {false, "argument", "Two hexadecimal paths are required."};
                    cardPath.clear();
                    if (!decode(path.substr(0, split), other) || !decode(path.substr(split + 1), cardPath))
                        return {false, "argument", "Invalid file paths."};
                    if (command == "ms-rename") std::swap(cardPath, other);
                }
                GuestFiles::Path(cardPath, command == "ms-files");
                if (command == "ms-rename") GuestFiles::Name(other);
                if ((command == "ms-get" || command == "ms-put") && (other.empty() || other.front() != '/'))
                    return {false, "argument", "An absolute host file path is required."};
                std::vector<uint8_t> contents;
                if (command == "ms-put") {
                    std::ifstream input(other, std::ios::binary | std::ios::ate);
                    const auto size = input ? input.tellg() : std::streampos(-1);
                    if (size < 0 || uint64_t(size) > GuestFiles::maxFileBytes)
                        return {false, "size", "Choose a readable host file up to 8 MB."};
                    contents.resize(size_t(size)); input.seekg(0);
                    if ((!contents.empty() && !input.read(reinterpret_cast<char*>(contents.data()), contents.size())) || input.peek() != std::char_traits<char>::eof())
                        return {false, "read", "Could not read the complete host file."};
                }
                if (!GuestDatabaseOps::Ready(*ctx.soc))
                    return {false, "not-ready", "Wait for Palm OS to reach a supported application boundary."};
                if (command == "ms-files") {
                    const auto listing = GuestFiles::List(*ctx.soc, cardPath, cancel);
                    std::string text = "space\t" + std::to_string(listing.used) + "\t" + std::to_string(listing.total) + "\n";
                    for (const auto& entry : listing.entries)
                        text += std::string(entry.directory ? "d\t" : "f\t") + std::to_string(entry.size) + "\t" + encode(entry.name) + "\n";
                    return {true, "files", text};
                }
                if (command == "ms-get") {
                    contents = GuestFiles::Get(*ctx.soc, cardPath, cancel);
                    writeAtomic(other, contents.data(), contents.size(), cancel);
                    return {true, "exported", "Card file exported."};
                }
                if (command == "ms-put") GuestFiles::Put(*ctx.soc, cardPath, contents, cancel);
                else if (command == "ms-mkdir") GuestFiles::Mkdir(*ctx.soc, cardPath, cancel);
                else if (command == "ms-delete") GuestFiles::Delete(*ctx.soc, cardPath, cancel);
                else if (command == "ms-rename") GuestFiles::Rename(*ctx.soc, cardPath, other, cancel);
                return {true, "changed", "Memory Stick updated."};
            }
            if(command=="ms-state")return {true,"card",!msCtrlrPresent(ms)?"none":msCtrlrInserted(ms)?"inserted":"ejected"};
            // "none", "classic MB" or "pro MiB".
            if(command=="ms-type")
                return {true,"type",!msCtrlrPresent(ms)?"none":(msCtrlrPro(ms)?"pro ":"classic ")+std::to_string(msCtrlrMegabytes(ms))};
            if(command=="ms-protection")return {true,"protection",msCtrlrReadOnly(ms)?"read-only":"writable"};
            if(command=="ms-create"||command=="ms-insert") {
                if(msCtrlrInserted(ms))return {false,"busy","Eject the current Memory Stick before replacing it."};
                std::vector<uint8_t> image;
                const auto device=ctx.soc->GetDeviceType();
                // Memory Stick Pro: 256, 512, 1024 or 1886 MiB, or a raw image of one of those sizes.
                if(command=="ms-create"&&frondMsPro::validCardMib(path.size()<=4&&!path.empty()&&path.find_first_not_of("0123456789")==std::string::npos?unsigned(std::stoul(path)):0u)){
                    const auto result=frondNative::createMsProCard(ms,ctx.sessionFolder,device,unsigned(std::stoul(path)),sdProgress(job));
                    if(!result.ok)return {false,result.code,result.message};
                    return {true,"inserted","Memory Stick inserted."};
                }
                if(command=="ms-insert"){
                    struct stat st{};
                    if(::stat(path.c_str(),&st)==0&&S_ISREG(st.st_mode)&&frondMsPro::mibOfBytes(uint64_t(st.st_size))){
                        const auto result=frondNative::loadMsProImage(ms,ctx.sessionFolder,device,path,sdProgress(job));
                        if(!result.ok)return {false,result.code,result.message};
                        return {true,"inserted","Memory Stick inserted."};
                    }
                }
                if(command=="ms-create") {
                    // Sizes in MB: 4, 8, 16, 32, 64 or 128, as the device's profile allows.
                    const bool digits=!path.empty()&&path.size()<=3&&path.find_first_not_of("0123456789")==std::string::npos;
                    const auto megabytes=digits?std::stoul(path):0ul;
                    const auto* geometry=frondMsGeometry::byMegabytes(megabytes);
                    if(!geometry||!deviceProfileMsSizeValid5(device,megabytes))
                        return {false,"size","Choose a classic Memory Stick from 4 to 128 MB."};
                    image=frondMs::blank(*geometry);
                }else {
                    // The header names the geometry, so the size check comes before reading the image.
                    std::ifstream input(path,std::ios::binary|std::ios::ate);
                    const auto size=input?input.tellg():std::streampos(-1);
                    uint8_t header[frondMs::headerBytes];
                    const frondMsGeometry::Geometry* geometry=nullptr;
                    frondSdFile::Info info;
                    if(size>=std::streamoff(frondMs::headerBytes)){input.seekg(0);
                        if(input.read(reinterpret_cast<char*>(header),sizeof(header)))
                            geometry=frondMs::get32(header+8)==2?frondMs::v2Geometry(header,&info):frondMs::headerGeometry(header);}
                    const uint64_t expected=!geometry?0:frondMs::get32(header+8)==2?info.fileBytes():frondMs::fileBytes(*geometry);
                    if(!geometry||uint64_t(size)!=expected||!deviceProfileMsSizeValid5(device,geometry->megabytes))
                        return {false,"size","Use a Frond classic Memory Stick image from 4 to 128 MB."};
                    input.close();
                    if(!frondMs::readPhysical(path,image))return {false,"image","Use a valid Frond classic Memory Stick image."};
                }
                const bool wasPro=msCtrlrPro(ms);
                if(!msCtrlrAttach(ms,image))return {false,"image","Could not attach the Memory Stick image."};
                // A Pro stick it replaced leaves the card store.
                if(wasPro)frondNative::detachMsPro(nullptr);
                frondNative::msOpened({},0);
                return {true,"inserted","Memory Stick inserted."};
            }
            // Take-out leaves the ejected stick settled in the folder's card file for the frontend to
            // move; put-in inserts the one the frontend moved there. Put-in takes "IDENTITY [protected]".
            if(command=="ms-take-out"){
                frondNative::CardIdentity identity{};bool protect=false;
                const auto result=frondNative::takeOutMs(ctx.sessionFolder,ms,identity,protect);
                if(!result.ok)return {false,result.code,result.message};
                return {true,"taken-out",frondNative::takenOutReply(identity,protect)};
            }
            if(command=="ms-put-in"){
                frondNative::CardIdentity identity{};bool protect=false;
                if(!frondNative::parsePutIn(path,identity,protect))return {false,"argument","Use the card's 64-digit identity, then protected or writable."};
                const auto result=frondNative::putInMs(ctx.sessionFolder,ms,ctx.soc->GetDeviceType(),identity,protect);
                if(!result.ok)return {false,result.code,result.message};
                return {true,"inserted","Memory Stick inserted."};
            }
            if(command=="ms-eject"||command=="ms-reinsert") {
                if(!msCtrlrInsert(ms,command=="ms-reinsert"))return {false,"empty","No Memory Stick is available in that state."};
                if(command=="ms-eject")frondNative::noteEjected(*ctx.soc);
                return {true,command=="ms-eject"?"ejected":"inserted",command=="ms-eject"?"Memory Stick ejected. Its contents remain in this session.":"Memory Stick inserted."};
            }
            if(command=="ms-read-only") {
                if(path!="on"&&path!="off")return {false,"argument","Use on or off."};
                if(!msCtrlrProtect(ms,path=="on"))return {false,"busy","Eject the Memory Stick before changing write protection."};
                return {true,"protection",msCtrlrReadOnly(ms)?"read-only":"writable"};
            }
            if(command=="ms-export") {
                if(msCtrlrInserted(ms))return {false,"busy","Eject the Memory Stick before exporting its image."};
                // A Pro stick exports as a raw image of its sectors, like an SD card.
                if(msCtrlrPro(ms)){
                    const auto result=frondNative::exportSd(path,sdProgress(job));
                    if(!result.ok)return {false,result.code,result.message};
                    return {true,"exported","Memory Stick image exported."};
                }
                // A classic stick exports as a v2 .frondms, which Cards imports.
                std::vector<uint8_t> image;
                if(!msCtrlrExport(ms,image))return {false,"empty","This session has no Memory Stick."};
                const frondCardTool::Progress none;
                frondCardTool::Run run{none};
                const auto result=frondCardTool::writeMs(path,image,run);
                if(!result.ok)return {false,result.code,result.message};
                return {true,"exported","Memory Stick image exported."};
            }
            return {false,"command","Unknown Memory Stick command."};
        }
        if (command == "sd-state") return {true, "card", !sdCardInitialized() ? "none" : ctx.soc->SdInserted() ? "inserted" : "ejected"};
        if (command == "sd-create" || command == "sd-insert") {
            if (ctx.soc->SdInserted()) return {false, "busy", "Eject the current card before replacing it."};
            const auto device = ctx.soc->GetDeviceType();
            const auto progress = sdProgress(job);
            frondNative::SdResult result;
            if (command == "sd-create") {
                if (path.empty() || path.size() > 4 || path.find_first_not_of("0123456789") != std::string::npos)
                    return {false, "size", frondNative::sdCreateSizes(device)};
                result = frondNative::createSdCard(ctx.sessionFolder, device, unsigned(std::stoul(path)), progress);
            } else {
                struct stat st{};
                if (::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
                    (!frondSd::validSize(uint64_t(st.st_size)) || !frondNative::sdImageFits(device, uint64_t(st.st_size))))
                    return {false, "size", frondNative::sdImageSizes(device)};
                result = frondNative::loadSdImage(ctx.sessionFolder, device, path, progress);
            }
            if (!result.ok) return {false, result.code, result.message};
            frondNative::slotCards().sdProtected = false;
            ctx.soc->SdInsert();
            return {true, "inserted", "SD card inserted."};
        }
        if (command == "sd-take-out") {
            if (!sdCardInitialized()) return {false, "empty", "This session has no card."};
            if (ctx.soc->SdInserted()) return {false, "busy", "Eject the card before taking it out."};
            frondNative::CardIdentity identity{};
            const bool protect = frondNative::slotCards().sdProtected;
            const auto result = frondNative::takeOutFileCard(ctx.sessionFolder, frondMachine::kindSd, protect, identity);
            if (!result.ok) return {false, result.code, result.message};
            frondNative::slotCards().sdProtected = false;
            return {true, "taken-out", frondNative::takenOutReply(identity, protect)};
        }
        if (command == "sd-put-in") {
            frondNative::CardIdentity identity{};
            bool protect = false;
            if (!frondNative::parsePutIn(path, identity, protect))
                return {false, "argument", "Use the card's 64-digit identity, then protected or writable."};
            const auto result = frondNative::putInSd(ctx.sessionFolder, ctx.soc->GetDeviceType(), identity, protect);
            if (!result.ok) return {false, result.code, result.message};
            ctx.soc->SdInsert();
            return {true, "inserted", "SD card inserted."};
        }
        if (command == "sd-eject") {
            if (!sdCardInitialized() || !ctx.soc->SdInserted()) return {false, "empty", "No card is inserted."};
            ctx.soc->SdEject();
            frondNative::noteEjected(*ctx.soc);
            return {true, "ejected", "SD card ejected. Its contents remain in this session."};
        }
        if (command == "sd-reinsert") {
            if (!sdCardInitialized()) return {false, "empty", "This session has no card."};
            ctx.soc->SdInsert();
            return {true, "inserted", "SD card inserted."};
        }
        if (command == "sd-export") {
            if (!sdCardInitialized()) return {false, "empty", "This session has no card."};
            if (ctx.soc->SdInserted()) return {false, "busy", "Eject the card before exporting its image."};
            const auto result = frondNative::exportSd(path, sdProgress(job));
            if (!result.ok) return {false, result.code, result.message};
            return {true, "exported", "SD image exported."};
        }
        if (command == "save") {
            if (!frondNative::saveMachine(path, *ctx.soc, ctx.sessionPath, ctx.sessionFolder)) return {false, "save", "Could not save device state."};
            return {true, "saved", "Session saved."};
        }
        if (command == "install") {
            std::unique_ptr<uint8_t[]> data; size_t size;
            if (!util::ReadFile(path, data, size)) return {false, "read", "Could not read the application database."};
            const auto result = GuestDatabaseOps::Install(*ctx.soc, size, data.get());
            switch (result) {
                case DB_INSTALL_RESULT_OK: return {true, "installed", "Application installed."};
                case DB_INSTALL_RESULT_NEEDS_RESET: return {true, "needs-reset", "Installed. Reset the Palm to finish installation."};
                case DB_INSTALL_RESULT_ERR_CORRUPT: return {false, "corrupt", "The Palm database is corrupt."};
                case DB_INSTALL_RESULT_ERR_OOM: return {false, "memory", "The Palm does not have enough memory."};
                case DB_INSTALL_RESULT_ERR_IS_OPEN: return {false, "open", "Close this application on the Palm before replacing it."};
                case DB_INSTALL_RESULT_ERR_COULD_NOT_OVERWRITE: return {false, "overwrite", "The existing database could not be replaced."};
                case DB_INSTALL_RESULT_ERR_NOT_SUPPORTED: return {false, "unsupported", "Installation is not supported by this firmware."};
                case DB_INSTALL_RESULT_ERR_NOT_CURRENTLY_POSSIBLE: return {false, "not-ready", "Wait for Palm OS to finish starting, then try again."};
                default: return {false, "install", "Application installation failed."};
            }
        }
        if (command == "export" || command == "export-db") {
            std::string destination=path, database;
            if(command=="export-db"){
                const auto split=path.find('\n');
                if(split==std::string::npos || split==0 || split+1==path.size() ||
                   path.size()-split-1>31 || path.find('\n',split+1)!=std::string::npos)
                    return {false,"argument","Choose an absolute ZIP path and one Palm database name."};
                destination=path.substr(0,split);database=path.substr(split+1);
            }
            std::vector<uint8_t> archive;
            if (!GuestDatabaseOps::Export(*ctx.soc, database, archive))
                return {false, "not-ready", "Wait for Palm OS to reach a supported application boundary."};
            writeAtomic(destination, archive.data(), archive.size());
            return {true, "exported", database.empty()?"RAM databases exported.":"Database exported."};
        }
        if (command == "db-list") {
            // One line per RAM database: type, creator and attributes in hex, then the hex name.
            DbMetadataList list;
            if (!GuestDatabaseOps::List(*ctx.soc, list))
                return {false, "not-ready", "Wait for Palm OS to reach a supported application boundary."};
            std::string text;
            for (const auto& db : list) {
                char fields[32];
                std::snprintf(fields, sizeof(fields), "%08x %08x %04x ", unsigned(db.type), unsigned(db.creator),
                              unsigned(db.attr & 0xffff));
                text += fields + encode(db.name) + "\n";
            }
            std::cout << "db-list: " << list.size() << " RAM databases" << std::endl;
            return {true, "databases", text};
        }
        if (command == "guest-orientation") {
            // The Zodiac's comes from how its display scans the frame out, which the OS sets with it.
            if (ctx.soc->FrameQuarterTurn()) {
                const char* orientation = guestOrientationName(ctx.soc->GuestOrientation());
                if (!*orientation) return {false, "unknown", "The screen orientation can't be read right now."};
                return {true, "orientation", orientation};
            }
            if (ctx.soc->GetDeviceType() != DeviceType5::deviceTypeTX)
                return {false, "unsupported", "This device has no screen rotation."};
            const auto orientation = TxOrientation(ctx.soc->GetCpu());
            if (orientation.empty()) return {false, "unknown", "The screen orientation can't be read right now."};
            return {true, "orientation", orientation};
        }
        if (command == "speed-state") {
            // Same figures as the desktop window title, plus audio underruns since start. The Zodiac
            // gives the instructions it really ran, then its whole speed line.
            char text[128];
            const uint64_t ipsMax = ctx.mainLoop.GetCurrentIpsMax();
            const uint64_t ips = ctx.speedDetail.empty() ? ctx.mainLoop.GetCurrentIps() : ctx.speedIps;
            std::snprintf(text, sizeof(text), "%.2f MIPS, limit %.2f MIPS, %u%%, %llu underruns",
                          ips / 1e6, ipsMax / 1e6, ipsMax ? unsigned(100 * ips / ipsMax) : 0u,
                          static_cast<unsigned long long>(ctx.audioDriver.GetUnderrunCount()));
            if (!ctx.speedDetail.empty()) return {true, "speed", std::string(text) + "; " + ctx.speedDetail};
            return {true, "speed", text};
        }
        if (command == "microphone-epoch") return {true, "microphone", ctx.microphone ? std::to_string(ctx.microphone->Epoch()) : "0"};
        if (command == "microphone-state") return {true, "microphone", ctx.microphone ? ctx.microphone->State() : "off"};
        if (command == "microphone-mode") {
            if (!caps.microphone || !ctx.microphone)
                return {false, "unsupported", "This device has no emulated microphone."};
            if (path.rfind("on-return:", 0) == 0) {
                const auto number = path.substr(10);
                if (number.empty() || number.size() > 19 || number.find_first_not_of("0123456789") != std::string::npos)
                    return {false, "argument", "Invalid capture request."};
                if (!ctx.microphone->ArmOnForeground(std::stoull(number))) return {false, "expired", "Capture request expired."};
                return {true, "microphone", "pending"};
            }
            if (path != "on" && path != "off") return {false, "argument", "Use on or off."};
            if (path == "off") ctx.microphone->Disarm();
            else if (!ctx.microphone->Arm()) return {false, "microphone", ctx.microphone->State()};
            return {true, "microphone", ctx.microphone->State()};
        }
        if (command == "camera-state") return {true, "camera", ctx.camera ? ctx.camera->State() : "pattern"};
        if (command == "camera-mode") {
            // pattern, picture:PATH, webcam:DEVICE (V4L2, Linux) or frames (a file the frontend writes).
            if (!ctx.camera || !deviceProfile5(ctx.soc->GetDeviceType())->frontend.camera)
                return {false, "unsupported", "This device has no camera."};
            auto& camera = *ctx.camera;
            bool ok = true;
            if (path == "pattern") camera.Pattern();
            else if (path.rfind("picture:", 0) == 0 && path.size() > 9 && path[8] == '/') ok = camera.Picture(path.substr(8));
            else if (path.rfind("webcam:", 0) == 0 && path.size() > 8 && path[7] == '/') ok = camera.Webcam(path.substr(7));
            else if (path == "frames") {
                ok = camera.Frames(ctx.sessionFolder);
                if (ok) return {true, "camera", camera.FramesPath()};
            }
            else return {false, "argument", "Use pattern, picture:PATH, webcam:DEVICE or frames."};
            if (!ok) return {false, "camera", camera.State()};
            return {true, "camera", camera.State()};
        }
        if (command == "window-id") {
            const auto id = ctx.windowId ? ctx.windowId() : std::string();
            if (id.empty()) return {false, "unsupported", "The window is not an X11 window."};
            return {true, "window", id};
        }
        if (command == "window-layout") {
            const auto layout = ctx.windowLayout ? ctx.windowLayout() : std::string();
            if (layout.empty()) return {false, "unsupported", "There is no window."};
            return {true, "layout", layout};
        }
        if (command == "display") {
            // Queued for the SDL thread, which applies it once the app is in the foreground.
            if (!ctx.changeDisplay) return {false, "unsupported", "There is no window to change."};
            DisplayChange change;
            std::string error;
            if (!parseDisplayChange(path, ctx.soc->GetDeviceType() == deviceTypeTG50, change, error))
                return {false, "argument", error};
            ctx.changeDisplay(change);
            return {true, "display", "queued"};
        }
        if (command == "sound-state") return {true, "sound", SoundState(ctx)};
        if (command == "sound-mode") {
            // Off mutes a playing device; on unmutes it, or starts it after --no-sound or audio-off.
            if (path != "on" && path != "off") return {false, "argument", "Use on or off."};
            ctx.audioDriver.SetMuted(path == "off");
            if (path == "on" && !sound) { ctx.audioDriver.Start(); sound = true; }
            const auto state = SoundState(ctx);
            std::cout << "sound: " << state << std::endl;
            return {true, "sound", state};
        }
        if (command == "reset") {
            GuestDatabaseOps::SettleBeforeReset(*ctx.soc);
            ctx.soc->Reset();
        }
        else if (command == "hard-reset") ctx.soc->HardReset();
        else if (command == "rotate") ctx.rotation = Rotation((int(ctx.rotation) + 1) % 4);
        else if (command == "audio-on") { ctx.audioDriver.SetMuted(false); ctx.audioDriver.Start(); sound = true; }
        else if (command == "audio-off") { ctx.audioDriver.Pause(); sound = false; }
        else if (command == "quit") { if (ctx.microphone) ctx.microphone->Disarm(); if (ctx.camera) ctx.camera->Disarm(); quit = true; }
        else return {false, "command", "Unknown desktop command."};
        return {true, "done", "Done."};
    }
    DesktopChannel channel;
    bool sound, quit{false}, saveFailed{false};
    std::string saveOnLoss;
};
#endif
