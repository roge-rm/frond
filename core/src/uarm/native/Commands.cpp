#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

#include "Commands.h"
#include "GuestExport.h"
#include "GuestDatabaseOps.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "CPEndian.h"
#include "Cli.h"
#include "FileUtil.h"
#include "SoC.h"
#include "app_launcher.h"
#include "db_backup.h"
#include "db_installer.h"
#include "db_list.h"
#include "md5.h"
#include "nand.h"
#include "sdcard.h"
#include "device_profile5.h"
#include "session/session_file5.h"
#include "MachineSession.h"
#include "CardSwap.h"
#include "syscall_dispatch.h"
#include "cputil.h"

using namespace std;

namespace {
    Rotation rotate(Rotation rotation) {
        switch (rotation) {
            case Rotation::portrait_0:
                return Rotation::landscape_90;

            case Rotation::landscape_90:
                return Rotation::portrait_180;

            case Rotation::portrait_180:
                return Rotation::landscape_270;

            default:
                return Rotation::portrait_0;
        }
    }

    void CmdSetMips(vector<string> args, cli::CommandEnvironment& env, void* context) {
        if (args.size() != 1) return env.PrintUsage();

        uint32_t mips;
        istringstream s(args[0]);

        s >> mips;

        if (s.fail() || !s.eof()) {
            cout << "invalid argument" << endl;
            return;
        }

        static_cast<commands::Context*>(context)->mainLoop.SetCyclesPerSecondLimit(mips * 1000000);
    }

    void CmdEnableAudio(vector<string> args, cli::CommandEnvironment& env, void* context) {
        static_cast<commands::Context*>(context)->audioDriver.Start();
    }

    void CmdDisableAudio(vector<string> args, cli::CommandEnvironment& env, void* context) {
        static_cast<commands::Context*>(context)->audioDriver.Pause();
    }

    // On a Memory Stick device the card store holds the stick, so the SD commands refuse.
    bool refuseSd(void* context) {
        auto* pxa = dynamic_cast<SocPXA*>(reinterpret_cast<commands::Context*>(context)->soc);
        if (!pxa || !pxa->GetMemoryStick()) return false;
        cout << "This device has no SD slot." << endl;
        return true;
    }

    void CmdUnmount(vector<string> args, cli::CommandEnvironment& env, void* context) {
        if (refuseSd(context)) return;
        if (!sdCardInitialized()) {
            cout << "no sd card mounted" << endl;
            return;
        }

        auto ctx = reinterpret_cast<commands::Context*>(context);

        ctx->soc->SdEject();
        sdCardReset();
        frondSdCandidate::releaseUnused();
    }

    void CmdMount(vector<string> args, cli::CommandEnvironment& env, void* context) {
        if (refuseSd(context)) return;
        if (sdCardInitialized()) {
            cout << "sd card already mounted" << endl;
            return;
        }

        if (args.size() != 1) return env.PrintUsage();

        auto ctx = reinterpret_cast<commands::Context*>(context);
        const auto loaded = frondNative::loadSdImage(ctx->sessionFolder, ctx->soc->GetDeviceType(), args[0]);
        if (!loaded.ok) {
            cout << (loaded.code == "read" ? "failed to read " + args[0] : loaded.message) << endl;
            return;
        }
        ctx->soc->SdInsert();
    }

    // The same card moves as the control socket's sd-take-out, sd-put-in, ms-take-out and ms-put-in.
    MemoryStickController* memoryStick(commands::Context* ctx) {
        auto* pxa = dynamic_cast<SocPXA*>(ctx->soc);
        return pxa ? pxa->GetMemoryStick() : nullptr;
    }

    void CmdTakeOut(vector<string> args, cli::CommandEnvironment& env, void* context, bool stick) {
        if (!args.empty()) return env.PrintUsage();
        auto ctx = reinterpret_cast<commands::Context*>(context);
        auto* ms = memoryStick(ctx);
        frondNative::CardIdentity identity{};
        bool protect = false;
        frondNative::SdResult result;
        if (stick) {
            result = frondNative::takeOutMs(ctx->sessionFolder, ms, identity, protect);
        } else if (ms) {
            result = frondSdCandidate::fail("unsupported", "This device has no SD slot.");
        } else if (ctx->soc->SdInserted()) {
            result = frondSdCandidate::fail("busy", "Eject the card before taking it out.");
        } else {
            protect = frondNative::slotCards().sdProtected;
            result = frondNative::takeOutFileCard(ctx->sessionFolder, frondMachine::kindSd, protect, identity);
            if (result.ok) frondNative::slotCards().sdProtected = false;
        }
        cout << (result.ok ? frondNative::takenOutReply(identity, protect) : result.message) << endl;
    }

    void CmdPutIn(vector<string> args, cli::CommandEnvironment& env, void* context, bool stick) {
        frondNative::CardIdentity identity{};
        bool protect = false;
        if (args.empty() || args.size() > 2 ||
            !frondNative::parsePutIn(args.size() == 2 ? args[0] + " " + args[1] : args[0], identity, protect))
            return env.PrintUsage();
        auto ctx = reinterpret_cast<commands::Context*>(context);
        auto* ms = memoryStick(ctx);
        const auto device = ctx->soc->GetDeviceType();
        frondNative::SdResult result;
        if (stick) {
            result = frondNative::putInMs(ctx->sessionFolder, ms, device, identity, protect);
        } else if (ms) {
            result = frondSdCandidate::fail("unsupported", "This device has no SD slot.");
        } else {
            result = frondNative::putInSd(ctx->sessionFolder, device, identity, protect);
            if (result.ok) ctx->soc->SdInsert();
        }
        if (!result.ok) cout << result.message << endl;
    }

    void CmdSdTakeOut(vector<string> args, cli::CommandEnvironment& env, void* context) { CmdTakeOut(args, env, context, false); }
    void CmdMsTakeOut(vector<string> args, cli::CommandEnvironment& env, void* context) { CmdTakeOut(args, env, context, true); }
    void CmdSdPutIn(vector<string> args, cli::CommandEnvironment& env, void* context) { CmdPutIn(args, env, context, false); }
    void CmdMsPutIn(vector<string> args, cli::CommandEnvironment& env, void* context) { CmdPutIn(args, env, context, true); }

    void CmdHoldState(vector<string> args, cli::CommandEnvironment& env, void* context) {
        if (!args.empty()) return env.PrintUsage();
        auto *soc = reinterpret_cast<commands::Context*>(context)->soc;
        cout << "hold: " << (!soc->HoldSwitchSupported() ? "unavailable" :
                            soc->HoldSwitchOn() ? "on" : "off") << endl;
    }
    void CmdHold(vector<string> args, cli::CommandEnvironment& env, void* context) {
        if (args.size() != 1 || (args[0] != "on" && args[0] != "off")) return env.PrintUsage();
        auto *soc = reinterpret_cast<commands::Context*>(context)->soc;
        if (!soc->SetHoldSwitch(args[0] == "on")) cout << "Hold is unavailable for this device" << endl;
        else cout << "hold: " << (soc->HoldSwitchOn() ? "on" : "off") << endl;
    }

    void CmdReset(vector<string> args, cli::CommandEnvironment& env, void* context) {
        auto ctx = reinterpret_cast<commands::Context*>(context);

        ctx->soc->Reset();
    }

    void CmdResetNoExtensions(vector<string> args, cli::CommandEnvironment& env, void* context) {
        auto ctx = reinterpret_cast<commands::Context*>(context);

        ctx->soc->JamKey(keyIdUp, 2000);
        ctx->soc->Reset();
    }

    void CmdResetHard(vector<string> args, cli::CommandEnvironment& env, void* context) {
        auto* soc = reinterpret_cast<commands::Context*>(context)->soc;
        if (!soc->HardResetSupported()) {
            cout << "not available on this device" << endl;
            return;
        }
        soc->HardReset();
    }

    void CmdRotate(vector<string> args, cli::CommandEnvironment& env, void* context) {
        auto ctx = reinterpret_cast<commands::Context*>(context);

        ctx->rotation = rotate(ctx->rotation);
    }

    void CmdSaveSession(vector<string> args, cli::CommandEnvironment& env, void* context) {
        if (args.size() != 1 && args.size() != 2) return env.PrintUsage();

        auto ctx = reinterpret_cast<commands::Context*>(context);

        if (!frondNative::saveMachine(args[0], *ctx->soc, ctx->sessionPath, ctx->sessionFolder)) {
            cout << "failed to save session" << endl;
            return;
        }
        cout << "wrote session to " << args[0] << endl;

        if (args.size() == 2 && !refuseSd(context) && sdCardInitialized()) {
            if (!frondNative::exportSd(args[1]).ok) {
                cout << "failed to write SD image to " << args[1] << endl;
            } else {
                cout << "wrote SD image to " << args[1] << endl;
            }
        }
    }

    void CmdSaveSd(vector<string> args, cli::CommandEnvironment& env, void* context) {
        if (args.size() != 1) return env.PrintUsage();
        if (refuseSd(context)) return;

        if (!sdCardInitialized()) {
            cout << "no SD card mounted" << endl;
            return;
        }

        if (!frondNative::exportSd(args[0]).ok) {
            cout << "failed to write SD image to " << args[0] << endl;
        } else {
            cout << "wrote SD image to " << args[0] << endl;
        }
    }

    void CmdDumpMMU(vector<string> args, cli::CommandEnvironment& env, void* context) {
        if (args.size() != 0) return env.PrintUsage();
        auto ctx = reinterpret_cast<commands::Context*>(context);

        ctx->soc->DumpMMU();
    }

    void CmdOsVersion(vector<string> args, cli::CommandEnvironment& env, void* context) {
        if (args.size() != 0) return env.PrintUsage();
        auto ctx = reinterpret_cast<commands::Context*>(context);
        SyscallDispatch* sd = ctx->soc->GetSyscallDispatch();
        if (!sd) {
            cout << "not available on this device" << endl;
            return;
        }

        if (!syscallDispatchM68kSupport(sd)) {
            cout << "m68k syscalls not supported" << endl;
            return;
        }

        try {
            GuestExport guest(*ctx->soc);
            if (!guest.Ready()) {
                cout << "Palm OS is not ready for the OS version" << endl;
                return;
            }
            char version[32];
            const uint32_t ptr = syscall68k_SysGetOsVersionString(sd, SC_EXECUTE_FULL);
            if (!ptr || !syscallDispatch_strncpy_toHost(sd, version, ptr, sizeof(version)))
                throw std::runtime_error("could not read OS version");
            version[sizeof(version) - 1] = '\0';
            // The read-only checkpoint reclaims the version string on TG50.
            if (ctx->soc->GetDeviceType() != deviceTypeTG50 &&
                syscall68k_MemPtrFree(sd, SC_EXECUTE_PURE, ptr) != 0)
                throw std::runtime_error("could not free OS version string");
            guest.Finish();
            cout << version << endl;
        } catch (const std::exception& error) {
            cout << "OS version failed: " << error.what() << "; guest restored" << endl;
        }
    }

    void CmdSetAutoOff(vector<string> args, cli::CommandEnvironment& env, void* context) {
        if (args.size() != 1) return env.PrintUsage();

        uint32_t timeout;
        if (!(istringstream(args[0]) >> timeout)) {
            cout << "invalid timeout" << endl;
            return env.PrintUsage();
        }

        auto ctx = reinterpret_cast<commands::Context*>(context);
        if (ctx->soc->GetDeviceType() == deviceTypeTG50) {
            cout << "set-auto-off is not supported on TG50" << endl;
            return;
        }
        SyscallDispatch* sd = ctx->soc->GetSyscallDispatch();
        if (!sd) {
            cout << "not available on this device" << endl;
            return;
        }

        if (!syscallDispatchM68kSupport(sd)) {
            cout << "m68k syscalls not supported" << endl;
            return;
        }

        if (!syscallDispatchPrepare(sd)) {
            cout << "unable to prepare save environment for syscalls" << endl;
            return;
        }

        syscall_SysSetAutoOffTime(sd, SC_EXECUTE_PURE, timeout);
    }

    void CmdInstall(vector<string> args, cli::CommandEnvironment& env, void* context) {
        if (args.size() != 1) return env.PrintUsage();
        if (!reinterpret_cast<commands::Context*>(context)->soc->GuestServicesSupported()) {
            cout << "not available on this device" << endl;
            return;
        }

        size_t len;
        unique_ptr<uint8_t[]> data;
        if (!util::ReadFile(args[0], data, len)) {
            cout << "unable to read " << args[0] << endl;
            return;
        }

        auto ctx = reinterpret_cast<commands::Context*>(context);

        int result;
        try { result = GuestDatabaseOps::Install(*ctx->soc, len, data.get()); }
        catch (const std::exception& error) { cout << "installation failed: " << error.what() << endl; return; }

        switch (result) {
            case DB_INSTALL_RESULT_NEEDS_RESET:
                cout << "installation successful, needs reset" << endl;
                break;

            case DB_INSTALL_RESULT_OK:
                cout << "installation successful" << endl;
                break;

            case DB_INSTALL_RESULT_ERR_CORRUPT:
                cout << "database corrupt" << endl;
                break;

            case DB_INSTALL_RESULT_ERR_OOM:
                cout << "out of memory" << endl;
                break;

            case DB_INSTALL_RESULT_ERR_IS_OPEN:
                cout << "db cannot is open and cannot be deleted" << endl;
                break;

            case DB_INSTALL_RESULT_ERR_COULD_NOT_OVERWRITE:
                cout << "unable to overwrite db" << endl;
                break;

            case DB_INSTALL_RESULT_ERR_NOT_SUPPORTED:
                cout << "db installation not supported on this ROM" << endl;
                break;

            case DB_INSTALL_RESULT_ERR_NOT_CURRENTLY_POSSIBLE:
                cout << "db installation not currently possible" << endl;
                break;

            case DB_INSTALL_RESULT_ERR_UNKNOWN:
                cout << "unknown error during installation" << endl;
                break;
        }
    }

    void CmdDbList(vector<string> args, cli::CommandEnvironment& env, void* context) {
        if (args.size() != 0) return env.PrintUsage();
        auto ctx = reinterpret_cast<commands::Context*>(context);
        SyscallDispatch* sd = ctx->soc->GetSyscallDispatch();
        if (!sd) {
            cout << "not available on this device" << endl;
            return;
        }

        DbMetadataList metadataList;
        std::unique_ptr<GuestExport> guest;
        try {
            guest = std::make_unique<GuestExport>(*ctx->soc);
            if (!guest->Ready() || !dbListGet(sd, metadataList)) {
                cout << "Palm OS is not ready for the database list" << endl;
                return;
            }
            guest->Finish();
        } catch (const std::exception& error) {
            cout << "database list failed: " << error.what() << "; guest restored" << endl;
            return;
        }

        for (const auto& meta : metadataList) {
            union {
                uint32_t numeric;
                char text[5];
            } type, creator;

            type.text[4] = creator.text[4] = '\0';
            type.numeric = be32toh(meta.type);
            creator.numeric = be32toh(meta.creator);

            printf("ID 0x%08x cardNo %u index %u %s %s %s %s\n", meta.localID, meta.cardNo,
                   meta.index, meta.isRom ? "ROM" : "ram", type.text, creator.text, meta.name);
        }
    }

    void CmdDbExport(vector<string> args, cli::CommandEnvironment& env, void* context, int type) {
        if (args.size() != 1) return env.PrintUsage();
        if (!reinterpret_cast<commands::Context*>(context)->soc->GuestServicesSupported()) {
            cout << "not available on this device" << endl;
            return;
        }

        auto ctx = reinterpret_cast<commands::Context*>(context);
        try {
            std::vector<uint8_t> archive;
            if (!GuestDatabaseOps::Export(*ctx->soc, "", archive, type)) {
                cout << "Palm OS is not ready for database export; try again" << endl;
                return;
            }
            if (util::WriteFile(args[0], archive.data(), archive.size()))
                cout << "export written to " << args[0] << endl;
            else
                cout << "failed to write export to " << args[0] << endl;
        } catch (const std::exception& error) {
            cout << "export FAILED: " << error.what() << "; guest restored" << endl;
        }
    }

    void CmdDbExportAll(vector<string> args, cli::CommandEnvironment& env, void* context) {
        CmdDbExport(args, env, context, DbBackup::BACKUP_TYPE_EVERYTHING);
    }

    void CmdDbExportRamRom(vector<string> args, cli::CommandEnvironment& env, void* context) {
        CmdDbExport(args, env, context, DbBackup::BACKUP_TYPE_RAM_ROM);
    }

    void CmdDbExportRam(vector<string> args, cli::CommandEnvironment& env, void* context) {
        CmdDbExport(args, env, context, DbBackup::BACKUP_TYPE_RAM);
    }

    void CmdLaunch(vector<string> args, cli::CommandEnvironment& env, void* context) {
        if (args.size() != 1) return env.PrintUsage();
        if (!reinterpret_cast<commands::Context*>(context)->soc->GuestServicesSupported()) {
            cout << "not available on this device" << endl;
            return;
        }

        auto ctx = reinterpret_cast<commands::Context*>(context);
        try {
            if (GuestDatabaseOps::Launch(*ctx->soc, args[0]) != GuestDatabaseOps::LaunchResult::launched)
                cout << "application could not be launched at this time" << endl;
        } catch (const std::exception& error) { cout << "launch failed: " << error.what() << endl; }
    }

    void CmdSaveNand(vector<string> args, cli::CommandEnvironment& env, void* context) {
        if (args.size() != 1) return env.PrintUsage();
        auto* ctx = static_cast<commands::Context*>(context);
        auto* chip = ctx->soc->GetNand();
        if (!chip || !nandRawBytes(chip)) {
            cout << "this device has no NAND image" << endl;
            return;
        }
        // The raw image, main then spare bytes of each page, as --nand takes it.
        std::ofstream out(args[0], std::ios::binary | std::ios::trunc);
        std::vector<uint8_t> page(nandPageBytes(chip));
        const uint64_t pages = nandRawBytes(chip) / page.size();
        bool ok = bool(out);
        for (uint64_t p = 0; ok && p < pages; ++p)
            ok = nandReadRawPage(chip, uint32_t(p), page.data()) &&
                 bool(out.write(reinterpret_cast<const char*>(page.data()), std::streamsize(page.size())));
        out.close();
        if (ok && out)
            cout << "NAND image saved" << endl;
        else
            cout << "failed to save NAND image" << endl;
    }

    void CmdInputReplay(vector<string> args, cli::CommandEnvironment& env, void* context) {
        if (args.size() != 1) return env.PrintUsage();
        auto* ctx = static_cast<commands::Context*>(context);
        InputReplay replacement;
        string error;
        if (!replacement.Load(args[0], ctx->screenWidth, ctx->screenHeight, error)) {
            cout << error << endl;
            return;
        }
        ctx->replay = std::move(replacement);
        ctx->replayStartedAt = timestampUsec();
        cout << "input replay loaded" << endl;
    }

    const vector<cli::Command> commandList(
        {{.name = "set-mips",
          .usage = "set-mips <mips>",
          .description = "Set target MIPS.",
          .cmd = CmdSetMips},
         {.name = "input-replay",
          .usage = "input-replay <file>",
          .description = "Load pen/key/snapshot events relative to now.",
          .cmd = CmdInputReplay},
         {.name = "save-nand",
          .usage = "save-nand <new NAND image>",
          .description = "Export the current raw NAND, including spare bytes.",
          .cmd = CmdSaveNand},
         {.name = "hold-state", .description = "Show the Hold switch.", .cmd = CmdHoldState},
         {.name = "hold", .usage = "hold <on|off>", .description = "Set the Hold switch.", .cmd = CmdHold},
         {.name = "audio-on", .description = "Enable audio.", .cmd = CmdEnableAudio},
         {.name = "audio-off", .description = "Disable audio.", .cmd = CmdDisableAudio},
         {.name = "unmount", .description = "Unmount SD card.", .cmd = CmdUnmount},
         {.name = "mount",
          .usage = "mount <image>",
          .description = "Mount an SD card image.",
          .cmd = CmdMount},
         {.name = "sd-take-out",
          .description = "Leave the ejected SD card in the machine folder's card file and empty the slot.",
          .cmd = CmdSdTakeOut},
         {.name = "sd-put-in",
          .usage = "sd-put-in <identity> [protected|writable]",
          .description = "Insert the SD card file in the machine folder.",
          .cmd = CmdSdPutIn},
         {.name = "ms-take-out",
          .description = "Leave the ejected Memory Stick in the machine folder's card file and empty the slot.",
          .cmd = CmdMsTakeOut},
         {.name = "ms-put-in",
          .usage = "ms-put-in <identity> [protected|writable]",
          .description = "Insert the Memory Stick file in the machine folder.",
          .cmd = CmdMsPutIn},
         {.name = "reset", .description = "Reset Pilot.", .cmd = CmdReset},
         {.name = "reset-no-ext",
          .description = "Reset Pilot w/o loading extensions.",
          .cmd = CmdResetNoExtensions},
         {.name = "reset-hard", .description = "Hard reset Pilot.", .cmd = CmdResetHard},
         {.name = "rotate", .description = "Rotate 90° CCW", .cmd = CmdRotate},
         {.name = "save-session",
          .usage = "save-session <session file> [card image]",
          .description = "Save session.",
          .cmd = CmdSaveSession},
         {.name = "save-sd",
          .usage = "save-sd <card image>",
          .description = "Save SD card.",
          .cmd = CmdSaveSd},
         {.name = "dump-mmu", .description = "Dump MMU mappings.", .cmd = CmdDumpMMU},
         {.name = "os-version", .description = "Retrieve OS version. ", .cmd = CmdOsVersion},
         {.name = "set-auto-off",
          .usage = "set-auto-off <timeout>",
          .description = "Set auto off timeout (warning: wakeup will currently fail). ",
          .cmd = CmdSetAutoOff},
         {.name = "install",
          .usage = "install <database>",
          .description = "Install database.",
          .cmd = CmdInstall},
         {.name = "db-list", .description = "List databases.", .cmd = CmdDbList},
         {.name = "db-export-all",
          .usage = "db-export-all <file>",
          .description = "Export all databases.",
          .cmd = CmdDbExportAll},
         {.name = "db-export-rom",
          .usage = "db-export-rom <file>",
          .description = "Export RAM and ROM databases (excluding PACE cache).",
          .cmd = CmdDbExportRamRom},
         {.name = "db-export",
          .usage = "db-export <file>",
          .description = "Export all RAM databases (excluding PACE cache).",
          .cmd = CmdDbExportRam},
         {.name = "launch",
          .usage = "launch <name>",
          .description = "Launch app",
          .cmd = CmdLaunch}});
}  // namespace

void commands::Register() { cli::AddCommands(commandList); }
