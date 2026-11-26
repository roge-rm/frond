#ifndef FROND_DESKTOP_CONTROL_H
#define FROND_DESKTOP_CONTROL_H

#include <algorithm>
#include <chrono>
#include "AttentionState.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

#include "BackgroundJob.h"
#include "Commands.h"
#include "DesktopChannel.h"
#include "device_profile5.h"
#include "ReadOnlyGuest.h"
#include "HostSerial.h"
#include "HostInfrared.h"
#include "UsbHostBridge.h"
#include "../../common/sync/ActiveSync.h"
#include "../../common/sync/SyncManager.h"
#include "soc_PXA.h"
#include "soc_S3C2442.h"
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
#include "CardInstall.h"

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
    // The sync manager's state when it changes, and a HotSync press waiting on the USB bus.
    void syncChanged(commands::Context& ctx) {
        cancelReplacedSyncPress(ctx);
        const auto status = frondSync::SyncManager::Get().Status();
        const bool syncing = status.state == frondSync::SyncStatus::State::syncing;
        if (syncing) syncEnded = false;
        else if (lastSyncing) {
            syncEnded = true;
            syncEndedAt = std::chrono::steady_clock::now();
        }
        lastSyncing = syncing;
        if (syncPressBridge) {
            // A press after a sync waits for the device to leave the bus, a press for a new cable for it to come
            // up. Either goes in by syncPressBy anyway: a Zodiac's USB only comes up once HotSync runs.
            const bool up = ctx.usb->Host()->GetPhase() == UsbBusHost::Phase::running;
            if (syncPressAfterSync != up || std::chrono::steady_clock::now() > syncPressBy) {
                syncPressBridge = nullptr;
                if (!ctx.soc->PressHotSync()) event("sync", "failed usb: The HotSync button could not be pressed.");
            }
        }
        const auto state = status.Describe();
        if (state == lastSync || (state == "off" && lastSync.empty())) return;
        lastSync = state;
        event("sync", state);
    }
    // Same for the USB bridge.
    void usbChanged(commands::Context& ctx) {
        const auto state = UsbState(ctx);
        if (state == lastUsb || (state == "off" && lastUsb.empty())) return;
        lastUsb = state;
        event("usb", state);
    }

    // A cue event for the window: "on" while the guest's rumble is on and no game controller can play it.
    void rumbleChanged(bool cue) {
        if (cue == lastRumbleCue) return;
        lastRumbleCue = cue; event("rumble", cue ? "on" : "off");
    }

    // The guest's LED and vibrate motor (AttentionState.h), "led=green vibrate=1", sent on a change only.
    void attentionChanged(const AttentionState& state) {
        if (attentionSent && state == lastAttention) return;
        // Nothing to say until something first lights or buzzes.
        if (!attentionSent && state == AttentionState{}) return;
        attentionSent = true; lastAttention = state;
        event("attention", state.Describe());
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
        const auto state=ctx.soc->BluetoothState();
        if(state==lastBluetooth || (state=="off" && lastBluetooth.empty()))return;
        lastBluetooth=state;event("bluetooth",state);
    }

    bool PanelVisible() const { return channel.PanelVisible(); }

    // The save when the launcher went away failed; the core exits non-zero.
    bool SaveFailed() const { return saveFailed; }
    bool poll(commands::Context& ctx) {
        // No request is taken while a save writes in the background: it answers when it's done,
        // and everything after it, a quit or a card change too, waits behind it.
        if (pendingSave) {
            if (!saveJob.Finished()) return !channel.Flush() && lost(ctx);
            finishSave(ctx);
            // Step 3's file work paused the guest; don't catch up that wall time.
            ctx.mainLoop.PauseAt(timestampUsec());
        }
        DesktopChannel::Request request;
        switch (channel.Receive(request)) {
            case DesktopChannel::Poll::closed: return lost(ctx);
            case DesktopChannel::Poll::handled: return false;
            case DesktopChannel::Poll::idle:
                if (GuestDatabaseOps::FinishDue(timestampUsec())) finishInstalls(ctx);
                break;
            case DesktopChannel::Poll::request: {
                const auto& command = request.command;
                const bool work = command == "guest-ready" || command == "save" || command == "install" || (command == "export" || command == "export-db") || command == "db-list" || command == "reset" || command == "hard-reset" || command == "launch-app" || (command == "sync-start" && request.argument == "network") || command == "delete-db" || command == "input-replay" || (command.rfind("sd-", 0) == 0 && command != "sd-state") || (command.rfind("sd2-", 0) == 0 && command != "sd2-state") || (command.rfind("ms-",0)==0&&command!="ms-state"&&command!="ms-type"&&command!="ms-protection");
                // Installs sent one after another finish together once they stop coming; any other
                // guest work, or a quit, waits for that first.
                if (GuestDatabaseOps::FinishWaiting() && ((work && command != "install") || command == "quit"))
                    finishInstalls(ctx);
                if (work) {
                    // Host calls only drop capture for their length; the user's choice stays and capture comes back.
                    if (ctx.microphone) ctx.microphone->Suspend();
                    ctx.audioDriver.Pause();
                }
                if (command == "save" && ctx.camera) ctx.camera->Disarm();
                Reply result;
                bool deferred = false;
                try {
                    if (command == "save")
                        deferred = startSave(ctx, request.argument, request.id, result);
                    else
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
                if (!deferred) channel.Reply(request.id, result.ok, result.code, result.message);
                break;
            }
        }
        if (!channel.Flush()) return lost(ctx);
        return quit && channel.OutputEmpty();
    }

    // Waits for a save still writing and finishes it, for a core about to stop.
    void finishPendingSave(commands::Context& ctx) {
        if (pendingSave) finishSave(ctx);
    }

 private:
    // The socket closed. When the launcher went away without asking to quit, the session is saved
    // where its close would have saved it, here on the emulation thread like a save request.
    // The sync notifies and the launcher relaunch for installs sent one after another
    // (GuestDatabaseOps::InstallLater), with playback paused like any host call.
    void finishInstalls(commands::Context& ctx) {
        if (ctx.microphone) ctx.microphone->Suspend();
        ctx.audioDriver.Pause();
        GuestDatabaseOps::FinishPending(*ctx.soc, timestampUsec());
        if (sound) ctx.audioDriver.Start();
        ctx.mainLoop.PauseAt(timestampUsec());
    }

    bool lost(commands::Context& ctx) {
        finishPendingSave(ctx);
        if (GuestDatabaseOps::FinishWaiting() && !saveOnLoss.empty() && !quit && channel.Lost() && !channel.QuitPending())
            finishInstalls(ctx);
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

    bool lastRumbleCue{false};
    AttentionState lastAttention{};
    bool attentionSent{false};
    std::string lastMicrophone, lastCamera, lastSerial, lastUsb, lastInfrared, lastBluetooth, lastSync;
    const UsbHostBridge* syncPressBridge{nullptr};
    uint64_t syncPressRoute{0};
    bool syncPressAfterSync{false};
    std::chrono::steady_clock::time_point syncPressBy;
    // When the last sync ended, as syncChanged saw it.
    bool lastSyncing{false}, syncEnded{false};
    std::chrono::steady_clock::time_point syncEndedAt;
    // How long a press waits for a new cable's device to come up, and how long after a sync the guest may
    // still be closing it.
    static constexpr auto syncPressUsbWait = std::chrono::seconds(2);
    static constexpr auto syncCloseWait = std::chrono::seconds(10);

    void queueSyncPress(const commands::Context& ctx, bool afterSync) {
        syncPressBridge = ctx.usb;
        syncPressRoute = ctx.usb->RouteGeneration();
        syncPressAfterSync = afterSync;
        syncPressBy = std::chrono::steady_clock::now() + (afterSync ? syncCloseWait : syncPressUsbWait);
    }
    // A sync over the Sync Manager's USB route has just ended and the device is still on the bus: its HotSync
    // app is still closing the sync, and the Zodiac ROM loses a cradle press then (seen, not traced).
    bool guestClosingSync(const commands::Context& ctx) const {
        if (!ctx.usb || ctx.usb->GetConfig().kind != HostSerial::Kind::syncManager || !ctx.usb->Host() ||
            ctx.usb->Host()->GetPhase() != UsbBusHost::Phase::running)
            return false;
        const auto state = frondSync::SyncManager::Get().Status().state;
        if (state != frondSync::SyncStatus::State::done && state != frondSync::SyncStatus::State::failed) return false;
        // An end syncChanged hasn't seen yet is a new one.
        return lastSyncing || (syncEnded && std::chrono::steady_clock::now() - syncEndedAt < syncCloseWait);
    }

    void cancelReplacedSyncPress(const commands::Context& ctx) {
        if (!syncPressBridge) return;
        if (ctx.usb == syncPressBridge && ctx.usb->RouteGeneration() == syncPressRoute &&
            ctx.usb->GetConfig().kind == HostSerial::Kind::syncManager && ctx.usb->Host()) return;
        syncPressBridge = nullptr;
        event("sync", "failed usb: The Sync Manager USB route changed.");
    }
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
    // The sync manager's state, then the profile the next HotSync uses when one is set.
    static std::string SyncState() {
        auto& manager = frondSync::SyncManager::Get();
        const auto profile = manager.Options().profile;
        return manager.Status().Describe() + (profile.empty() ? "" : "; profile " + profile);
    }
    static std::string SerialState(commands::Context& ctx) {
        if (!ctx.serial) return "off";
        const auto state = ctx.serial->Describe();
        return state == "off" && !ctx.serialError.empty() ? "off " + ctx.serialError : state;
    }
    struct Reply { bool ok{false}; std::string code{"failed"}, message; };
    // A save request: the snapshot and any card or NAND journal here on the emulation thread, then
    // the .frond compressed and written on saveJob while the guest runs. True when the reply waits
    // for that write; false with `result` set when the save failed before it.
    bool startSave(commands::Context& ctx, const std::string& path, const std::string& id, Reply& result) {
        if (ctx.testPalmWifi) {
            result = {false, "test-mode", "Session saves are disabled in Palm Wi-Fi test runs."};
            return false;
        }
        if (!frondPosix::IsAbsolutePath(path)) {
            result = {false, "argument", "An absolute file path is required."};
            return false;
        }
        // Tests: the whole save on this thread to FROND_SAVE_REFERENCE first, at the same instant,
        // to compare with what the background write gives. Not for sessions with a card or NAND.
        if (const char* reference = std::getenv("FROND_SAVE_REFERENCE")) {
            const auto synchronous = std::chrono::steady_clock::now();
            const bool ok = frondNative::saveMachine(reference, *ctx.soc);
            std::cerr << "save timing: synchronous reference " << (ok ? "" : "failed ") << saveTimingMs(synchronous)
                      << " ms" << std::endl;
        }
        const auto started = std::chrono::steady_clock::now();
        auto save = std::make_unique<frondNative::MachineSave>();
        if (!frondNative::beginMachineSave(path, *ctx.soc, *save, ctx.sessionPath, ctx.sessionFolder)) {
            result = {false, "save", "Could not save device state."};
            return false;
        }
        pendingSave = std::move(save);
        pendingSaveId = id;
        saveStarted = started;
        saveGuestTime = ctx.soc->GetTime();
        // The job reads only *pendingSave, which nothing else touches until it finishes.
        auto* job = pendingSave.get();
        saveJob.Start([job] { frondNative::writeMachineSave(*job); });
        saveBlockedMs = saveTimingMs(started);
        if (std::getenv("FROND_SAVE_TEST_WRITES")) testWrites(ctx);
        return true;
    }
    // Tests: while the background write runs, writes "FROND-BG-MARK-n" (n counts saves) to the last
    // NAND page and the last SD sector, as the guest might. Only for test copies of sessions.
    void testWrites(commands::Context& ctx) {
        const std::string mark = "FROND-BG-MARK-" + std::to_string(++testMarks) + ".";
        if (auto* chip = ctx.soc->GetNand(); chip && nandRawBytes(chip)) {
            std::vector<uint8_t> page(nandPageBytes(chip), 0xff);
            std::memcpy(page.data(), mark.data(), mark.size());
            const bool ok = nandWriteRawPage(chip, uint32_t(nandRawBytes(chip) / page.size() - 1), page.data());
            std::cerr << "save test: NAND " << (ok ? "" : "not ") << "marked " << mark << std::endl;
        }
        if (sdCardInitialized() && sdCardSectorCount()) {
            std::vector<uint8_t> sector(SD_SECTOR_SIZE, 0);
            std::memcpy(sector.data(), mark.data(), mark.size());
            const bool ok = sdCardWrite(uint32_t(sdCardSectorCount() - 1), sector.data());
            std::cerr << "save test: SD " << (ok ? "" : "not ") << "marked " << mark << std::endl;
        }
        if (auto& second = sdCardStore(1); second.initialized() && second.sectorCount()) {
            std::vector<uint8_t> sector(SD_SECTOR_SIZE, 0);
            std::memcpy(sector.data(), mark.data(), mark.size());
            const bool ok = second.write(uint32_t(second.sectorCount() - 1), sector.data());
            std::cerr << "save test: SD2 " << (ok ? "" : "not ") << "marked " << mark << std::endl;
        }
    }
    unsigned testMarks{};
    // Emulation thread: waits for saveJob, finishes the card and NAND files and answers.
    void finishSave(commands::Context& ctx) {
        const auto waited = std::chrono::steady_clock::now();
        saveJob.Wait();
        const auto finishing = std::chrono::steady_clock::now();
        const bool ok = frondNative::finishMachineSave(*pendingSave, *ctx.soc);
        if (saveTimingWanted())
            std::cerr << "save timing: emulation thread " << saveBlockedMs + saveTimingMs(waited) << " ms (snapshot "
                      << saveBlockedMs << " ms, waited " << std::chrono::duration<double, std::milli>(finishing - waited).count()
                      << " ms, finish " << saveTimingMs(finishing) << " ms), whole save " << saveTimingMs(saveStarted)
                      << " ms, guest time " << saveGuestTime << " at snapshot, " << ctx.soc->GetTime() << " when written"
                      << std::endl;
        pendingSave.reset();
        if (ok) channel.Reply(pendingSaveId, true, "saved", "Session saved.");
        else channel.Reply(pendingSaveId, false, "save", "Could not save device state.");
        pendingSaveId.clear();
    }
    // Declared before saveJob, so saveJob joins first when this goes.
    std::unique_ptr<frondNative::MachineSave> pendingSave;
    BackgroundJob saveJob;
    std::string pendingSaveId;
    std::chrono::steady_clock::time_point saveStarted;
    double saveBlockedMs{};
    uint64_t saveGuestTime{};
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
    // The second SD slot's commands (the Zodiac's), as the sd- ones on sdCardStore(1).
    Reply dispatchSecondSd(commands::Context& ctx, const std::string& command, const std::string& path, const std::string& job) {
        auto& store = sdCardStore(1);
        auto& soc = *ctx.soc;
        if (command == "sd2-state") return {true, "card", !store.initialized() ? "none" : soc.Sd2Inserted() ? "inserted" : "ejected"};
        if (command == "sd2-create" || command == "sd2-insert") {
            if (soc.Sd2Inserted()) return {false, "busy", "Eject the current card before replacing it."};
            const auto device = soc.GetDeviceType();
            const auto progress = sdProgress(job);
            frondNative::SdResult result;
            if (command == "sd2-create") {
                if (path.empty() || path.size() > 4 || path.find_first_not_of("0123456789") != std::string::npos)
                    return {false, "size", frondNative::sdCreateSizes(device)};
                result = frondNative::createSdCard(ctx.sessionFolder, device, unsigned(std::stoul(path)), progress, 1);
            } else {
                struct stat st{};
                if (::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
                    (!frondSd::validSize(uint64_t(st.st_size)) || !frondNative::sdImageFits(device, uint64_t(st.st_size))))
                    return {false, "size", frondNative::sdImageSizes(device)};
                result = frondNative::loadSdImage(ctx.sessionFolder, device, path, progress, 1);
            }
            if (!result.ok) return {false, result.code, result.message};
            frondNative::slotCards().sd2Protected = false;
            soc.SetSd2WriteProtect(false);
            if (!soc.Sd2Insert()) return {false, "size", "The second slot can't take this card."};
            return {true, "inserted", "SD card inserted in the second slot."};
        }
        if (command == "sd2-take-out") {
            if (!store.initialized()) return {false, "empty", "This session has no card in the second slot."};
            if (soc.Sd2Inserted()) return {false, "busy", "Eject the card before taking it out."};
            frondNative::CardIdentity identity{};
            const bool protect = frondNative::slotCards().sd2Protected;
            const auto result = frondNative::takeOutFileCard(ctx.sessionFolder, frondMachine::kindSd2, protect, identity);
            if (!result.ok) return {false, result.code, result.message};
            frondNative::slotCards().sd2Protected = false;
            soc.SetSd2WriteProtect(false);
            return {true, "taken-out", frondNative::takenOutReply(identity, protect)};
        }
        if (command == "sd2-put-in") {
            frondNative::CardIdentity identity{};
            bool protect = false;
            if (!frondNative::parsePutIn(path, identity, protect))
                return {false, "argument", "Use the card's 64-digit identity, then protected or writable."};
            const auto result = frondNative::putInSd2(ctx.sessionFolder, soc.GetDeviceType(), identity, protect);
            if (!result.ok) return {false, result.code, result.message};
            soc.SetSd2WriteProtect(protect);
            if (!soc.Sd2Insert()) return {false, "size", "The second slot can't take this card."};
            return {true, "inserted", "SD card inserted in the second slot."};
        }
        if (command == "sd2-eject") {
            if (!store.initialized() || !soc.Sd2Inserted()) return {false, "empty", "No card is inserted in the second slot."};
            soc.Sd2Eject();
            frondNative::noteEjected(soc);
            return {true, "ejected", "SD card ejected from the second slot. Its contents remain in this session."};
        }
        if (command == "sd2-reinsert") {
            if (!store.initialized()) return {false, "empty", "This session has no card in the second slot."};
            if (!soc.Sd2Insert()) return {false, "size", "The second slot can't take this card."};
            return {true, "inserted", "SD card inserted in the second slot."};
        }
        if (command == "sd2-export") {
            if (!store.initialized()) return {false, "empty", "This session has no card in the second slot."};
            if (soc.Sd2Inserted()) return {false, "busy", "Eject the card before exporting its image."};
            const auto result = frondNative::exportSd(path, sdProgress(job), 1);
            if (!result.ok) return {false, result.code, result.message};
            return {true, "exported", "SD image exported."};
        }
        return {false, "command", "Unknown second SD slot command."};
    }
    // A host call refused: the Palm is asleep (a list, or still asleep after the power press), or
    // Palm OS isn't at a point that takes host calls.
    static Reply notReady(commands::Context& ctx) {
        if (frondNative::guestAsleep(*ctx.soc)) return {false, "asleep", "The Palm is asleep. Turn it on and try again."};
        return {false, "not-ready", "Wait for Palm OS to reach a supported application boundary."};
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
        if (command.rfind("sd2-", 0) == 0 && !ctx.soc->HasSecondSdSlot())
            return {false, "unsupported", "This device has no second SD slot."};
        const bool sdProtection = command == "sd-protection" || command == "sd2-protection";
        const bool sdReadOnly = command == "sd-read-only" || command == "sd2-read-only";
        if (sdProtection || sdReadOnly) {
            if (!deviceProfileSdWriteProtection5(ctx.soc->GetDeviceType()))
                return {false, "unsupported", "Write protection isn't available on this SD slot."};
            const bool second = command.rfind("sd2-", 0) == 0;
            if (!sdCardStore(second ? 1 : 0).initialized()) return {false, "empty", "This session has no card in that slot."};
            auto& slots = frondNative::slotCards();
            bool& protectedSlot = second ? slots.sd2Protected : slots.sdProtected;
            if (sdReadOnly) {
                if (path != "on" && path != "off") return {false, "argument", "Use on or off."};
                if (second ? ctx.soc->Sd2Inserted() : ctx.soc->SdInserted())
                    return {false, "busy", "Eject the card before changing write protection."};
                protectedSlot = path == "on";
                if (second) ctx.soc->SetSd2WriteProtect(protectedSlot);
                else ctx.soc->SetSdWriteProtect(protectedSlot);
            } else if (!path.empty()) return {false, "argument", "This command takes no argument."};
            return {true, "protection", protectedSlot ? "read-only" : "writable"};
        }
        const bool msFilesCommand = command == "ms-files" || command == "ms-get" || command == "ms-put" ||
            command == "ms-mkdir" || command == "ms-delete" || command == "ms-rename";
        const bool fileCommand = command == "save" || command == "install" || (command == "export" || command == "export-db") || command == "sd-insert" || command == "sd-export" || command == "sd2-insert" || command == "sd2-export" || command == "ms-insert" || command == "ms-export" || command == "input-replay";
        if (fileCommand && !frondPosix::IsAbsolutePath(path)) return {false, "argument", "An absolute file path is required."};
        if (!fileCommand && !msFilesCommand && command != "sd-create" && command != "ms-create" && command != "sd-put-in" && command != "ms-put-in" && command != "sd2-create" && command != "sd2-put-in" && command != "ms-read-only" && command != "launch-app" && command != "delete-db" && command != "db-list" && command != "network-mode" && command != "keyboard-attach" && command != "keyboard-key" && command != "gamepad" && command != "gamepad-key" && command != "serial-mode" && command != "usb-mode" && command != "irda-mode" && command != "irprinter" && command != "printer" && command != "bluetooth-mode" && command != "sound-mode" && command != "display" && command != "microphone-mode" && command != "camera-mode" && command != "hold" && command != "battery" && command != "headphones" && command != "cradle-button" && command != "voice-key" && command != "cradle" && command != "speed" && command != "sync-profile" && command != "sync-start" && command != "activesync-start" && command != "activesync-answer" && !path.empty()) return {false, "argument", "This command takes no argument."};
        if (command == "install" && ctx.soc->InstallsToCard()) {
            const auto result = frondNative::installToSdCard(*ctx.soc, ctx.sessionFolder, path);
            return {result.ok, result.code, result.message};
        }
        // A board with no guest calls takes none of the commands that need them.
        if (!ctx.soc->GuestServicesSupported()) {
            for (const char* needs : {"guest-ready", "launch-app", "delete-db", "install", "export", "export-db", "db-list",
                                      "guest-orientation"})
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
            if (launched == GuestDatabaseOps::LaunchResult::notReady) return notReady(ctx);
            if (launched != GuestDatabaseOps::LaunchResult::launched)
                return {false, "launch", "Could not launch that application. Check its database name and wait for Palm OS to finish starting."};
            return {true, "launched", "Application launched."};
        }
        if (command == "delete-db") {
            if (path.empty() || path.size() > 31) return {false, "argument", "Enter a Palm database name (up to 31 bytes)."};
            const auto deleted = GuestDatabaseOps::Delete(*ctx.soc, path);
            if (!deleted) return notReady(ctx);
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
            return {true, "power", std::string(ctx.soc->PowerSleeping() ? "asleep" : "awake") +
                                       " wakes=" + std::to_string(ctx.soc->PowerWakes())};
        }
        const auto& caps = capabilities(ctx);
        if (!caps.serial && command == "serial-mode")
            return {false, "unsupported", std::string("The ") + deviceProfile5(ctx.soc->GetDeviceType())->displayName + " has no serial port."};
        const bool palmWifi = deviceProfileHasPalmSdioWifi5(ctx.soc->GetDeviceType());
        if (!caps.network && command == "network-mode" && path != "disabled" &&
            !(path == "wireless" && (palmWifi || ctx.testPalmWifi)))
            return {false, "unsupported", "This device has no emulated network."};
        if(command=="bluetooth-state" || command=="bluetooth-mode") {
            if(!caps.bluetooth)
                return {false,"unsupported","This device has no Bluetooth radio."};
            auto* machine=ctx.soc;
            if(command=="bluetooth-mode") {
                if(path!="off" && (!frondPosix::IsAbsolutePath(path) || path.find(char(0))!=std::string::npos))
                    return {false,"argument","Use off or an absolute virtual controller Unix socket path."};
                if(!machine->SetBluetooth(path=="off"?std::string():path,ctx.bluetoothError))
                    return {false,"bluetooth",ctx.bluetoothError};
            }
            return {true,"bluetooth",machine->BluetoothState()};
        }
        if (command == "irda-stats") {
            if (!ctx.infrared || !caps.infrared)
                return {false,"unsupported","This device has no infrared endpoint."};
            if (ctx.infrared->PhoneMode() || ctx.infrared->PrinterMode()) return {true,"infrared",ctx.infrared->PhoneStats()};
            auto* port=ctx.infrared->Port();
            const auto state=port?port->Mode():HostUartMode{};
            const auto c=ctx.infrared->GetCounters();
            return {true,"infrared", "rx="+std::to_string(c.received)+" tx="+std::to_string(c.sent)+
                " rx_drop="+std::to_string(ctx.infrared->DroppedReceive())+" tx_drop="+std::to_string(ctx.infrared->DroppedTransmit())+
                " pending="+std::to_string(ctx.infrared->PendingReceive())+" baud="+std::to_string(state.baud)+
                " rx_on="+std::to_string(state.receive)+" tx_on="+std::to_string(state.transmit)+
                " power="+std::to_string(port && port->PowerEnabled())+" sleeping="+std::to_string(port && port->Sleeping())};
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
                const bool ok=(ctx.soc->InfraredHostPort()?ctx.infrared->StartPhone(ctx.soc->InfraredHostPort(),ctx.infraredError):
                    ctx.infrared->StartPhone(static_cast<SocPXA*>(ctx.soc),ctx.infraredError));
                return {ok,"infrared",ok?ctx.infrared->Describe():ctx.infraredError};
            }
            if (ctx.infrared && path == "printer") {
                const bool ok=(ctx.soc->InfraredHostPort()?ctx.infrared->StartPrinter(ctx.soc->InfraredHostPort(),ctx.infraredError):
                    ctx.infrared->StartPrinter(static_cast<SocPXA*>(ctx.soc),ctx.infraredError));
                return {ok,"infrared",ok?ctx.infrared->Describe():ctx.infraredError};
            }
            HostSerial::Config config;
            if (!ctx.infrared || !HostSerial::Parse(path,config) || config.kind==HostSerial::Kind::syncManager) return {false,"argument","Use off, pty, tcp:PORT, unix:PATH, phone or printer."};
            const bool ok=(ctx.soc->InfraredHostPort()?ctx.infrared->Start(ctx.soc->InfraredHostPort(),config,ctx.infraredError):
                ctx.infrared->Start(static_cast<SocPXA*>(ctx.soc),config,ctx.infraredError));
            return {ok,"infrared",ok?ctx.infrared->Describe():ctx.infraredError};
        }
        if (command == "sync-state") return {true, "sync", SyncState()};
        if (command == "sync-profile") {
            // The profile for the next HotSync; empty goes back to the handheld's assigned one.
            auto& manager = frondSync::SyncManager::Get();
            auto options = manager.Options();
            options.profile = path;
            manager.Configure(options);
            return {true, "sync", SyncState()};
        }
        if (command == "activesync-answer") {
            if (path != "set-up" && path != "guest") return {false, "argument", "Use set-up or guest."};
            if (!frondSync::SyncManager::Get().AnswerPartnership(path == "set-up"))
                return {false, "none", "No partnership question is open."};
            return {true, "sync", path == "set-up" ? "Setting up a partnership." : "Connecting as a guest."};
        }
        if (command == "activesync-start") {
            auto* device = dynamic_cast<SocS3C2442*>(ctx.soc);
            if (!device || !ctx.usb) return {false, "unsupported", "ActiveSync is unavailable for this device."};
            if (frondSync::SyncManager::Get().Status().state == frondSync::SyncStatus::State::syncing)
                return {false, "busy", "An ActiveSync is already running."};
            // A trailing "ask": the frontend answers the partnership question; without it the device pairs.
            const bool ask = path == "ask" || (path.size() > 4 && path.compare(path.size() - 4, 4, " ask") == 0);
            const auto transport = ask ? path.substr(0, path.size() == 3 ? 0 : path.size() - 4) : path;
            {
                auto& manager = frondSync::SyncManager::Get();
                auto options = manager.Options();
                options.askPartnership = ask;
                manager.Configure(options);
            }
            if (transport == "serial" || (transport.empty() && frondSync::ConfiguredActiveSyncUsesSerial())) {
                if (!ctx.serial || !ctx.serialPort) return {false, "unsupported", "Serial ActiveSync is unavailable."};
                ctx.usb->Stop();
                ctx.serial->Stop();
                ctx.serialError.clear();
                if (!device->StartSerialActiveSync()) return {false, "serial", "Couldn't attach the serial ActiveSync cable."};
                return {true, "sync", "Starting ActiveSync over serial."};
            }
            if (!transport.empty() && transport != "usb") return {false, "argument", "Use usb or serial."};
            ctx.usbError.clear();
            if (!ctx.usb->StartCable(ctx.soc, ctx.usbError)) return {false, "usb", ctx.usbError};
            device->StartActiveSync();
            return {true, "sync", "Starting ActiveSync over USB."};
        }
        if (command == "sync-start") {
            if (UsbHostBridge::NetworkLink(ctx.soc)) return {false, "unsupported", "Use activesync-start on Windows Mobile."};
            // Puts the HotSync port on the sync manager (USB where the device has it, else serial)
            // unless one already is, then presses HotSync as the cradle button does.
            using Kind = HostSerial::Kind;
            const bool usbSync = ctx.usb && ctx.usb->GetConfig().kind == Kind::syncManager;
            const bool serialSync = ctx.serial && ctx.serial->GetConfig().kind == Kind::syncManager;
            cancelReplacedSyncPress(ctx);
            if (syncPressBridge) return {false, "busy", "A HotSync is already starting."};
            if (frondSync::SyncManager::Get().Status().state == frondSync::SyncStatus::State::syncing)
                return {false, "busy", "A HotSync is already running."};
            if (path == "network") {
                const auto& profile = *deviceProfile5(ctx.soc->GetDeviceType());
                if (!deviceProfileNetworkHotSync5(profile))
                    return {false, "unsupported", "Network HotSync is unavailable on this device."};
                if (profile.deviceType == deviceTypeTX &&
                    std::string_view(ctx.soc->networkSimulation.GetModeName()) != "wireless")
                    return {false, "network", "Choose Virtual Wi-Fi in Network first."};
                const auto result = dispatch(ctx, "launch-app", "HotSync", job);
                if (!result.ok) return result;
                frondSync::SyncManager::Get().SetNetworkEnabled(true);
                return {true, "network", "Tap Network HotSync on the device."};
            }
            if (!path.empty() && path != "local") return {false, "argument", "Use local or network."};
            // The cable and the HotSync button are on the cradle: out of it there is nothing to sync over.
            if (ctx.soc->CradleSupported() && !ctx.soc->CradleAttached())
                return {false, "cradle", "Put the device in the cradle first (Cradle attached), or use network HotSync."};
            if (path == "local") frondSync::SyncManager::Get().SetNetworkEnabled(false);
            // A replug now would bring the closing sync's device back up, and the press would still be lost.
            if (usbSync && guestClosingSync(ctx)) {
                queueSyncPress(ctx, true);
                return {true, "sync", "Starting a HotSync over USB."};
            }
            // A cable already plugged in and up is not replugged: the press goes in as the cradle button's does.
            if (usbSync && ctx.usb->Host() && ctx.usb->Host()->GetPhase() == UsbBusHost::Phase::running) {
                if (!ctx.soc->PressHotSync())
                    return {false, "unsupported", "This device has no HotSync button, or one is already being pressed."};
                return {true, "sync", "HotSync button pressed."};
            }
            const HostSerial::Config config{Kind::syncManager, 0, {}};
            const bool usbFree = caps.usb && ctx.usb && !UsbHostBridge::NetworkLink(ctx.soc) &&
                                 ctx.usb->GetConfig().kind == Kind::off;
            if (usbSync || (!serialSync && usbFree)) {
                ctx.usbError.clear();
                if (!ctx.usb->Start(ctx.soc, config, ctx.usbError)) return {false, "usb", ctx.usbError};
                // Presses once the device comes up after plugging in the cable for this sync, or after syncPressUsbWait.
                queueSyncPress(ctx, false);
                return {true, "sync", "Starting a HotSync over USB."};
            }
            if (!serialSync) {
                if (!caps.serial || !ctx.serial || ctx.serial->GetConfig().kind != Kind::off)
                    return {false, "busy", "Serial and USB are in use. Set one of them to sync first."};
                ctx.serialError.clear();
                if (!(ctx.serialPort ? ctx.serial->Start(ctx.serialPort, config, ctx.serialError)
                                     : ctx.serial->Start(ctx.serialUart, config, ctx.serialError, ctx.serialRoute)))
                    return {false, "serial", ctx.serialError};
            }
            if (!ctx.soc->PressHotSync())
                return {false, "unsupported", "This device has no HotSync button, or one is already being pressed."};
            return {true, "sync", "HotSync button pressed."};
        }
        if (command == "hotsync") {
            if (ctx.soc->CradleSupported() && !ctx.soc->CradleAttached())
                return {false, "cradle", "Attach the cradle or USB cable first."};
            cancelReplacedSyncPress(ctx);
            if (syncPressBridge) return {false, "busy", "A HotSync is already starting."};
            if (guestClosingSync(ctx)) {
                queueSyncPress(ctx, true);
                return {true, "hotsync", "HotSync button pressed."};
            }
            if (!ctx.soc->PressHotSync())
                return {false, "unsupported", "This device has no HotSync button, or one is already being pressed."};
            return {true, "hotsync", "HotSync button pressed."};
        }
        if (command == "battery-state")
            return {true, "battery", ctx.soc->BatterySupported() ? ctx.soc->Battery().Describe() : "unavailable"};
        if (command == "battery") {
            if (!ctx.soc->BatterySupported()) return {false, "unsupported", "This device's battery can't be set yet."};
            BatteryPower battery;
            if (!BatteryPower::Parse(path, battery))
                return {false, "argument", "Use max, level N or host N (0-100), then charging or battery."};
            ctx.soc->SetBattery(battery);
            return {true, "battery", ctx.soc->Battery().Describe()};
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
        if (command == "headphones-state")
            return {true, "headphones", !ctx.soc->HeadphonesSupported() ? "unavailable" : ctx.soc->HeadphonesIn() ? "in" : "out"};
        if (command == "headphones") {
            if (path != "in" && path != "out") return {false, "argument", "Use in or out."};
            if (!ctx.soc->HeadphonesSupported() || !ctx.soc->SetHeadphones(path == "in"))
                return {false, "unsupported", "This device has no headphone jack that can be set."};
            return {true, "headphones", ctx.soc->HeadphonesIn() ? "in" : "out"};
        }
        if (command == "voice-key") {
            // d holds the voice or record key down, u lets it go; queued like other keys.
            if (path != "d" && path != "u") return {false, "argument", "Use d or u."};
            if (!deviceProfileHasVoiceKey5(ctx.soc->GetDeviceType())) return {false, "unsupported", "This device has no voice key."};
            if (path == "d") ctx.soc->KeyDown(keyIdVoice); else ctx.soc->KeyUp(keyIdVoice);
            return {true, "voice-key", path == "d" ? "down" : "up"};
        }
        if (command == "cradle-button-state")
            return {true, "cradle-button", !ctx.soc->CradleButtonSupported() ? "unavailable" : ctx.soc->CradleButtonDown() ? "down" : "up"};
        if (command == "cradle-button") {
            if (path != "down" && path != "up") return {false, "argument", "Use down or up."};
            if (!ctx.soc->CradleButtonSupported() || !ctx.soc->SetCradleButton(path == "down"))
                return {false, "unsupported", "This device has no cradle button that can be pressed."};
            return {true, "cradle-button", ctx.soc->CradleButtonDown() ? "down" : "up"};
        }
        if (command == "cradle-setting")
            return {true, "cradle", !ctx.soc->CradleSupported() ? "unavailable" : ctx.soc->CradleSetting() ? "attached" : "detached"};
        if (command == "cradle-state")
            return {true, "cradle", !ctx.soc->CradleSupported() ? "unavailable" : ctx.soc->CradleAttached() ? "attached" : "detached"};
        if (command == "cradle") {
            if (path != "attached" && path != "detached") return {false, "argument", "Use attached or detached."};
            if (!ctx.soc->CradleSupported() || !ctx.soc->SetCradle(path == "attached"))
                return {false, "unsupported", "This device has no cradle that can be attached."};
            return {true, "cradle", ctx.soc->CradleAttached() ? "attached" : "detached"};
        }
        if (command == "gamepad-state") return {true, "gamepad", ctx.soc->GamepadState()};
        if (command == "gamepad") {
            if (path != "on" && path != "off") return {false, "argument", "Use on or off."};
            if (!ctx.soc->GamepadSupported()) return {false, "unsupported", "This device takes no game controller."};
            if (!ctx.soc->AttachGamepad(path == "on"))
                return {false, "busy", "The connector is in use (serial, infrared or USB); turn that off first."};
            return {true, "gamepad", ctx.soc->GamepadState()};
        }
        if (command == "gamepad-key") {
            // NAME d|u, NAME one of up down left right a b x y home menu hold; queued like other keys.
            static const char* names[] = {"up", "down", "left", "right", "a", "b", "x", "y", "home", "menu", "hold"};
            const auto space = path.find(' ');
            const std::string name = path.substr(0, space), state = space == std::string::npos ? "" : path.substr(space + 1);
            const auto found = std::find(std::begin(names), std::end(names), name);
            if (found == std::end(names) || (state != "d" && state != "u")) return {false, "argument", "Use a pad button name followed by d or u."};
            if (!ctx.soc->GamepadAttached()) return {false, "detached", "The game controller isn't attached."};
            const auto key = KeyId(keyIdPadUp + (found - std::begin(names)));
            if (state == "d") ctx.soc->KeyDown(key); else ctx.soc->KeyUp(key);
            return {true, "gamepad", "Pad event queued."};
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
            if (!ctx.serial || !HostSerial::Parse(path, config)) return {false, "argument", "Use off, pty, tcp:PORT, unix:PATH or sync."};
            if (auto* device = dynamic_cast<SocS3C2442*>(ctx.soc)) {
                if (config.kind == HostSerial::Kind::syncManager)
                    return {false, "argument", "Use activesync-start serial."};
                device->StopSerialActiveSync();
            }
            // A new endpoint detaches the old one first; queued bytes are dropped.
            ctx.serialError.clear();
            if (!(ctx.serialPort ? ctx.serial->Start(ctx.serialPort, config, ctx.serialError)
                                : ctx.serial->Start(ctx.serialUart, config, ctx.serialError, ctx.serialRoute))) return {false, "serial", ctx.serialError};
            return {true, "serial", SerialState(ctx)};
        }
        if (command == "usb-state") return {true, "usb", UsbState(ctx)};
        if (command == "usb-mode") {
            if (!caps.usb) return {false,"unsupported","USB is not supported on this device."};
            HostSerial::Config config;
            // The rx1950's USB is a network link: on or off, no endpoint.
            const bool network = UsbHostBridge::NetworkLink(ctx.soc);
            if (!ctx.usb || (network && path != "on" && path != "off") || (!network && !HostSerial::Parse(path, config)))
                return {false, "argument", network ? "Use on or off." : "Use off, pty, tcp:PORT, unix:PATH or sync."};
            // A new endpoint unplugs the cable first; queued bytes are dropped.
            ctx.usbError.clear();
            const bool started = path == "on" ? ctx.usb->StartCable(ctx.soc, ctx.usbError)
                                              : ctx.usb->Start(ctx.soc, config, ctx.usbError);
            cancelReplacedSyncPress(ctx);
            if (!started) return {false, "usb", ctx.usbError};
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
                if ((command == "ms-get" || command == "ms-put") && !frondPosix::IsAbsolutePath(other))
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
                if (!GuestDatabaseOps::Wake(*ctx.soc) || !GuestDatabaseOps::Ready(*ctx.soc)) return notReady(ctx);
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
        if (command.rfind("sd-", 0) == 0) {
            auto* pxa = dynamic_cast<SocPXA*>(ctx.soc);
            const bool wifiInserted = pxa && pxa->PalmWifiInserted();
            if (command == "sd-wifi-insert") {
                if (!pxa || (!palmWifi && !ctx.testPalmWifi) || !pxa->SupportsPalmWifi())
                    return {false, "unsupported", "This device has no Palm Wi-Fi Card."};
                if (ctx.soc->SdInserted() || wifiInserted || !pxa->InsertPalmWifi())
                    return {false, "busy", "Eject the current SD card first."};
                return {true, "inserted", "Palm Wi-Fi Card inserted."};
            }
            if (command == "sd-wifi-eject" || (wifiInserted && command == "sd-eject")) {
                if (!wifiInserted) return {false, "empty", "No Palm Wi-Fi Card is inserted."};
                pxa->EjectPalmWifi();
                return {true, "ejected", "Palm Wi-Fi Card ejected."};
            }
            if (wifiInserted && command == "sd-state")
                return {true, "card", sdCardInitialized() ? "inserted wifi ejected" : "inserted wifi"};
            if (wifiInserted) return {false, "busy", "Eject the Palm Wi-Fi Card first."};
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
            ctx.soc->SetSdWriteProtect(false);
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
            ctx.soc->SetSdWriteProtect(false);
            return {true, "taken-out", frondNative::takenOutReply(identity, protect)};
        }
        if (command == "sd-put-in") {
            frondNative::CardIdentity identity{};
            bool protect = false;
            if (!frondNative::parsePutIn(path, identity, protect))
                return {false, "argument", "Use the card's 64-digit identity, then protected or writable."};
            const auto result = frondNative::putInSd(ctx.sessionFolder, ctx.soc->GetDeviceType(), identity, protect);
            if (!result.ok) return {false, result.code, result.message};
            ctx.soc->SetSdWriteProtect(protect);
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
        if (command.rfind("sd2-", 0) == 0) return dispatchSecondSd(ctx, command, path, job);
        if (command == "save") {
            if (!frondNative::saveMachine(path, *ctx.soc, ctx.sessionPath, ctx.sessionFolder)) return {false, "save", "Could not save device state."};
            return {true, "saved", "Session saved."};
        }
        if (command == "install") {
            std::unique_ptr<uint8_t[]> data; size_t size;
            if (!util::ReadFile(path, data, size)) return {false, "read", "Could not read the application database."};
            const auto result = GuestDatabaseOps::InstallLater(*ctx.soc, size, data.get(), timestampUsec());
            switch (result) {
                case DB_INSTALL_RESULT_OK: return {true, "installed", "Application installed."};
                case DB_INSTALL_RESULT_NEEDS_RESET: return {true, "needs-reset", "Installed. Reset the Palm to finish installation."};
                case DB_INSTALL_RESULT_ERR_CORRUPT: return {false, "corrupt", "The Palm database is corrupt."};
                case DB_INSTALL_RESULT_ERR_OOM: return {false, "memory", "The Palm does not have enough memory."};
                case DB_INSTALL_RESULT_ERR_RUNNING: return {false, "running", "Leave this application on the Palm before replacing it."};
                case DB_INSTALL_RESULT_ERR_IS_OPEN: return {false, "open", "Close this application on the Palm before replacing it."};
                case DB_INSTALL_RESULT_ERR_COULD_NOT_OVERWRITE: return {false, "overwrite", "The existing database could not be replaced."};
                case DB_INSTALL_RESULT_ERR_NOT_SUPPORTED: return {false, "unsupported", "Installation is not supported by this firmware."};
                case DB_INSTALL_RESULT_ERR_NOT_CURRENTLY_POSSIBLE:
                    if (frondNative::guestAsleep(*ctx.soc)) return notReady(ctx);
                    return {false, "not-ready", "Wait for Palm OS to finish starting, then try again."};
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
            if (!GuestDatabaseOps::Export(*ctx.soc, database, archive)) return notReady(ctx);
            writeAtomic(destination, archive.data(), archive.size());
            return {true, "exported", database.empty()?"RAM databases exported.":"Database exported."};
        }
        if (command == "db-list") {
            // One line per RAM database: type, creator and attributes in hex, then the hex name.
            if (!path.empty() && path != "wake") return {false, "argument", "db-list takes only wake."};
            DbMetadataList list;
            if (!GuestDatabaseOps::List(*ctx.soc, list, path == "wake")) return notReady(ctx);
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
            // Same figures as the desktop window title, plus audio underruns since start and the speed setting. The Zodiac
            // gives the instructions it really ran, then its whole speed line.
            char text[128];
            if (ctx.speedPending) {
                std::snprintf(text, sizeof(text), "- MIPS, limit - MIPS, 0%%, %llu underruns, %s",
                              static_cast<unsigned long long>(ctx.audioDriver.GetUnderrunCount()),
                              ctx.mainLoop.SpeedName().c_str());
                return {true, "speed", text};
            }
            // The rx1950 gives the clock its guest got against the clock it runs at.
            const bool clock = ctx.speedLimitHz != 0;
            const uint64_t ipsMax = clock ? ctx.speedLimitHz : ctx.mainLoop.GetCurrentIpsMax();
            const uint64_t ips = ctx.speedDetail.empty() && !clock ? ctx.mainLoop.GetCurrentIps() : ctx.speedIps;
            // The clock for the others; the Zodiac's figure is instructions a second.
            const char* unit = ctx.speedDetail.empty() || clock ? "MHz" : "MIPS";
            std::snprintf(text, sizeof(text), "%.2f %s, limit %.2f %s, %u%%, %llu underruns, %s",
                          ips / 1e6, unit, ipsMax / 1e6, unit, ipsMax ? unsigned(100 * ips / ipsMax) : 0u,
                          static_cast<unsigned long long>(ctx.audioDriver.GetUnderrunCount()),
                          ctx.mainLoop.SpeedName().c_str());
            if (!ctx.speedDetail.empty()) return {true, "speed", std::string(text) + "; " + ctx.speedDetail};
            return {true, "speed", text};
        }
        if (command == "speed") {
            if (path.empty()) return {true, "speed", ctx.mainLoop.SpeedName()};
            SpeedSetting5 speed;
            if (!SpeedSetting5::Parse(path, speed))
                return {false, "argument", "Use original, 1.5x, 2x, 3x, 4x, 8x or uncapped."};
            ctx.mainLoop.SetSpeed(speed, deviceProfileOriginalHz5(ctx.soc->GetDeviceType()));
            return {true, "speed", speed.Name()};
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
        if (command == "window-show") {
            // The window started with --window-hidden and the desktop couldn't embed it.
            if (!ctx.showWindow) return {false, "unsupported", "There is no window."};
            ctx.showWindow();
            return {true, "window", "shown"};
        }
        if (command == "window-layout") {
            const auto layout = ctx.windowLayout ? ctx.windowLayout() : std::string();
            if (layout.empty()) return {false, "unsupported", "There is no window."};
            return {true, "layout", layout};
        }
        if (command == "app-active") {
            if (path != "on" && path != "off") return {false, "invalid", "Use on or off."};
            if (ctx.setAppActive) ctx.setAppActive(path == "on");
            return {true, "app-active", path};
        }
        if (command == "controller-map") {
            // KEY:BUTTON,... or none; entries the device can't use are dropped.
            if (!ctx.setControllerMap) return {false, "unsupported", "This device takes no game controller."};
            return {true, "controller-map", ctx.setControllerMap(path)};
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
