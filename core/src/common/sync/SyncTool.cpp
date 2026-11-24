// frond-sync: the sync manager's profiles, install queue, backups and log from the command line,
// for scripts and headless use. HotSyncs themselves run in the cores (--serial sync, --usb sync).

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "PalmDb.h"
#include "SyncManager.h"
#include "SyncStore.h"

using namespace frondSync;

namespace {

int Usage() {
    std::cerr << "usage: frond-sync [--folder DIR] COMMAND\n"
                 "  profiles                      the profiles and their folders\n"
                 "  create NAME | rename NAME NEW | delete NAME\n"
                 "  devices                       handhelds and the profile each syncs with\n"
                 "  assign DEVICE PROFILE | unassign DEVICE\n"
                 "  install PROFILE FILE... [--device DEVICE]   queue PRC or PDB files\n"
                 "  partnership PROFILE DEVICE [cancel]   request a Windows Mobile desktop partnership on the next sync\n"
                 "  files PROFILE                 enable Windows Mobile file exchange and print the folder\n"
                 "  queue PROFILE                 the install queue\n"
                 "  unqueue PROFILE ID\n"
                 "  backups PROFILE [DEVICE]\n"
                 "  export PROFILE DEVICE NAME FILE   write a backup out as a PDB or PRC\n"
                 "  restore PROFILE FROM TO       queue FROM's backups for installing on TO\n"
                 "  log PROFILE [DEVICE]\n"
                 "The folder defaults to "
              << DefaultSyncRoot() << std::endl;
    return 2;
}

std::string Time(int64_t seconds) {
    if (!seconds) return "-";
    const time_t t = time_t(seconds);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return text;
}

bool ReadAll(const std::string& path, Bytes& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

int Fail(const std::string& why) {
    std::cerr << "frond-sync: " << why << std::endl;
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    std::string root = DefaultSyncRoot();
    if (args.size() >= 2 && args[0] == "--folder") root = args[1], args.erase(args.begin(), args.begin() + 2);
    if (args.empty()) return Usage();
    const std::string command = args[0];
    std::string error;
    SyncLibrary library;
    if (!library.Open(root, error)) return Fail(error);

    // The named profile, opened.
    ProfileSyncLock lock;
    SyncProfile profile;
    const auto open = [&](const std::string& name, bool modify = false) -> bool {
        const auto info = library.FindProfile(name);
        if (!info) return Fail("There is no profile called " + name + "."), false;
        if (modify) {
            if (!lock.Acquire(root, info->id, error)) return Fail(error), false;
            const auto current = library.FindProfile(info->id);
            if (!current || current->folder != info->folder || current->created != info->created)
                return Fail("The sync profile was changed"), false;
        }
        if (!profile.Open(library.ProfilePath(*info), error)) return Fail(error), false;
        return true;
    };

    if (command == "profiles" && args.size() == 1) {
        for (const auto& p : library.Profiles()) std::cout << p.id << "\t" << p.name << "\t" << p.folder << std::endl;
        return 0;
    }
    if (command == "create" && args.size() == 2) {
        ProfileInfo info;
        if (!library.CreateProfile(args[1], info, error)) return Fail(error);
        std::cout << info.id << "\t" << info.name << "\t" << info.folder << std::endl;
        return 0;
    }
    if (command == "rename" && args.size() == 3) {
        const auto info = library.FindProfile(args[1]);
        if (!info) return Fail("There is no profile called " + args[1] + ".");
        return library.RenameProfile(info->id, args[2], error) ? 0 : Fail(error);
    }
    if (command == "delete" && args.size() == 2) {
        const auto info = library.FindProfile(args[1]);
        if (!info) return Fail("There is no profile called " + args[1] + ".");
        return library.DeleteProfile(info->id, error) ? 0 : Fail(error);
    }
    if (command == "devices" && args.size() == 1) {
        for (const auto& a : library.Assignments()) {
            const auto info = library.FindProfile(a.profileId);
            char id[16];
            std::snprintf(id, sizeof(id), "%08x", a.userId);
            std::cout << a.deviceKey << "\t" << (info ? info->name : "?") << "\t" << a.userName << "\t" << id << "\t"
                      << Time(a.lastSync) << (a.slowNext ? "\tslow next" : "") << std::endl;
        }
        return 0;
    }
    if (command == "assign" && args.size() == 3) {
        const auto info = library.FindProfile(args[2]);
        if (!info) return Fail("There is no profile called " + args[2] + ".");
        return library.Assign(args[1], info->id, error) ? 0 : Fail(error);
    }
    if (command == "unassign" && args.size() == 2) return library.Unassign(args[1], error) ? 0 : Fail(error);
    if (command == "install" && args.size() >= 3) {
        std::string device;
        std::vector<std::string> files;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--device" && i + 1 < args.size()) device = args[++i];
            else files.push_back(args[i]);
        }
        if (!open(args[1], true)) return 1;
        int failed = 0;
        for (const auto& path : files) {
            Bytes file;
            InstallItem item;
            const auto slash = path.find_last_of("/\\");
            const auto name = slash == std::string::npos ? path : path.substr(slash + 1);
            if (!ReadAll(path, file)) {
                std::cerr << path << ": can't be read" << std::endl, ++failed;
                continue;
            }
            if (!profile.QueueInstall(device, name, file, item, error, IsCabFileName(name) ? "cab" : "ram")) {
                std::cerr << path << ": " << error << std::endl, ++failed;
                continue;
            }
            std::cout << item.id << "\t" << item.dbName << std::endl;
        }
        return failed ? 1 : 0;
    }
    if (command == "partnership" && (args.size() == 3 || (args.size() == 4 && args[3] == "cancel"))) {
        if (!open(args[1], true)) return 1;
        const auto info = library.FindProfile(args[1]);
        const auto device = library.FindAssignment(args[2]);
        if (!info || !device || device->kind != "winmo" || device->profileId != info->id)
            return Fail("The Windows Mobile device needs to connect to this profile first.");
        return profile.RequestActiveSyncPairing(device->deviceKey, args.size() == 3, error) ? 0 : Fail(error);
    }
    if (command == "files" && args.size() == 2) {
        const auto info = library.FindProfile(args[1]);
        if (!info) return Fail("There is no profile called " + args[1] + ".");
        ProfileSyncLock filesLock;
        if (!filesLock.Acquire(library.Root(), info->id, error)) return Fail(error);
        const auto confirmed = library.FindProfile(info->id);
        if (!confirmed || confirmed->folder != info->folder) return Fail("The sync profile changed.");
        try {
            std::cout << ActiveSyncFilesFolder(library.ProfilePath(*confirmed), true) << std::endl;
        } catch (const std::exception& e) {
            return Fail(e.what());
        }
        return 0;
    }
    if (command == "queue" && args.size() == 2) {
        if (!open(args[1])) return 1;
        for (const auto& i : profile.InstallQueue())
            std::cout << i.id << "\t" << i.state << "\t" << i.dbName << "\t" << (i.deviceKey.empty() ? "any" : i.deviceKey) << "\t"
                      << i.fileName << "\t" << i.size << (i.message.empty() ? "" : "\t" + i.message) << std::endl;
        return 0;
    }
    if (command == "unqueue" && args.size() == 3) {
        if (!open(args[1], true)) return 1;
        return profile.RemoveInstall(std::stoll(args[2]), error) ? 0 : Fail(error);
    }
    if (command == "backups" && (args.size() == 2 || args.size() == 3)) {
        if (!open(args[1])) return 1;
        std::vector<std::string> devices;
        if (args.size() == 3) devices.push_back(args[2]);
        else devices = profile.BackupDevices(true);
        for (const auto& device : devices)
            for (const auto& b : profile.Backups(device))
                std::cout << device << "\t" << b.name << "\t" << FourCc(b.type) << "\t" << FourCc(b.creator) << "\t" << b.size << "\t"
                          << Time(b.storedAt) << std::endl;
        return 0;
    }
    if (command == "export" && args.size() == 5) {
        if (!open(args[1])) return 1;
        Bytes file;
        if (!profile.LoadBackup(args[2], args[3], file)) return Fail("There is no backup of " + args[3] + ".");
        std::ofstream out(args[4], std::ios::binary);
        if (!out.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()))) return Fail("Can't write " + args[4] + ".");
        return 0;
    }
    if (command == "restore" && args.size() == 4) {
        if (!open(args[1], true)) return 1;
        const int queued = profile.QueueRestore(args[2], args[3], error);
        if (queued < 0) return Fail(error);
        std::cout << queued << " queued" << std::endl;
        return 0;
    }
    if (command == "log" && (args.size() == 2 || args.size() == 3)) {
        if (!open(args[1])) return 1;
        for (const auto& e : profile.LogEntries(args.size() == 3 ? args[2] : ""))
            std::cout << Time(e.time) << "\t" << e.deviceKey << "\t" << e.text << std::endl;
        return 0;
    }
    return Usage();
}
