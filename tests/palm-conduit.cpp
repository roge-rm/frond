#include "../core/src/common/sync/PalmConduit.h"
#include "../core/src/common/sync/PalmConduitStorage.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>

using namespace frondSync;

namespace {

class Handheld : public PacketLink {
   public:
    bool Send(const Bytes& message) override {
        std::vector<DlpArg> args;
        uint8_t command{};
        assert(DlpParseRequest(message, command, args));
        ++transactions;
        if (command == dlpOpenConduit) {
            assert(args.empty());
            response = DlpResponse(command, dlpErrNone, {});
            return true;
        }
        assert(args.size() == 1);
        const auto& arg = args.front();
        switch (command) {
            case dlpOpenDB:
                assert(recovery && !opened);
                assert(arg.id == 0x20 && arg.data == Bytes({0, 0xc0, 'F', 'i', 'x', 't', 'u', 'r', 'e', 'D', 'B', 0}));
                opened = true;
                response = DlpResponse(command, dlpErrNone, {{0x20, {9}}});
                break;
            case dlpFindDB: {
                assert(opened && arg.id == 0x20 && arg.data == Bytes({0xc0, 0, 'F', 'i', 'x', 't', 'u', 'r', 'e', 'D', 'B', 0}));
                Bytes basic(54, 0);
                basic[10] = 54;
                basic[18] = 0x46, basic[19] = 0x72, basic[20] = 0x6f, basic[21] = 0x6e;
                const Bytes name{'F', 'i', 'x', 't', 'u', 'r', 'e', 'D', 'B', 0};
                basic.insert(basic.end(), name.begin(), name.end());
                response = DlpResponse(command, dlpErrNone,
                    {{0x20, basic}, {0x21, {0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0}}});
                break;
            }
            case dlpReadAppPreference:
                assert(opened && arg.id == 0x20 && arg.data == Bytes({0x46, 0x72, 0x6f, 0x6e, 0, 99, 0, 4, 0x80, 0}));
                response = DlpResponse(command, dlpErrNone, {{0x20, {0, 4, 0, 3, 0, 3, 'a', 'b', 'c'}}});
                break;
            case dlpWriteAppPreference:
                assert(preferenceRecovery && opened && arg.id == 0x20 &&
                       arg.data == Bytes({0x46, 0x72, 0x6f, 0x6e, 0, 99, 0, 4, 0, 3, 0x80, 0, 'a', 'b', 'd'}));
                response = DlpResponse(command, dlpErrNone, {});
                break;
            case dlpReadOpenDBInfo:
                assert(opened && arg.data == Bytes({9}));
                response = DlpResponse(command, dlpErrNone, {{0x20, {0, 8}}});
                break;
            case dlpReadRecord:
                assert(opened && arg.id == 0x21 && arg.data == Bytes({9, 0, 0, 7, 0, 0, 0xf0, 0}));
                response = DlpResponse(command, dlpErrNone,
                    {{0x20, {0, 0, 2, 43, 0, 7, 0, 3, 0x10, 3, 'o', 'l', 'd'}}});
                break;
            case dlpWriteRecord:
                assert(opened && arg.data == Bytes({9, 0x80, 0, 0, 2, 43, 0x10, 3, 'n', 'e', 'w'}));
                response = DlpResponse(command, dlpErrNone, {{0x20, {0, 0, 2, 43}}});
                wrote = true;
                break;
            case dlpWriteResource:
                assert(opened && arg.data == Bytes({9, 0, 'd', 'a', 't', 'a', 0, 123, 0, 3, 'n', 'e', 'w'}));
                response = DlpResponse(command, dlpErrNone, {});
                break;
            case dlpCloseDB:
                assert(opened && arg.data == Bytes({9}));
                response = DlpResponse(command, dlpErrNone, {});
                opened = false;
                break;
            default: assert(false && "unexpected vendor DLP traffic");
        }
        return true;
    }
    bool Receive(Bytes& message, int) override {
        message = response;
        return true;
    }
    std::string Name() const override { return "fixture"; }

    Bytes response;
    unsigned transactions{};
    bool recovery{}, preferenceRecovery{}, opened{}, wrote{};
};

std::string WindowsPath(const std::filesystem::path& path) {
#ifdef _WIN32
    return path.string();
#else
    std::string result = "Z:" + path.string();
    for (char& c : result)
        if (c == '/') c = '\\';
    return result;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    assert(argc == 3);
    namespace fs = std::filesystem;
    auto profile = fs::absolute(argv[1]);
#ifdef _WIN32
    profile /= "fixture-profile";
#endif
    const auto registration = profile / "conduits" / "registered";
    fs::create_directories(registration);
    const auto dll = WindowsPath(fs::absolute(argv[2]));
    const auto path = registration / "46726f6e.conf";
    const UserInfo user{123, 0, 456, 0, 3600, "Frond", {}};
    const auto seed = profile / "conduits" / "seed";
    fs::create_directories(seed);
    std::ofstream(seed / "data.txt") << "new user data";
    const std::string deviceKey = "fixture-device";
    const auto data = profile / "conduits" / "data" / PalmConduitDataKey(deviceKey, user.userId);

    for (const std::string mode : {"good", "implicit", "deny", "deny-pref", "failed"}) {
        std::ofstream record(path, std::ios::trunc);
        record << "name " << std::quoted(dll) << "\nfile " << std::quoted(mode)
               << "\nremote \"FixtureDB\"\ndirectory \"FixtureData\"\n";
        record.close();
        assert(record);
        Handheld device;
        DlpClient dlp(device);
        std::string error;
        bool logSeen = false;
        bool recoveryAsked = false;
        bool preferenceAsked = false;
        const auto log = [&](const std::string& line) { logSeen |= line == "Fixture log"; };
        const auto recovery = [&](const std::string& name, uint8_t card, std::string& why) {
            assert(!card);
            if (name == "Saved Preferences") {
                assert(device.opened && !preferenceAsked);
                preferenceAsked = true;
                if (mode == "deny-pref") {
                    why = "Fixture refused the preferences backup.";
                    return false;
                }
                return device.preferenceRecovery = true;
            }
            assert(name == "FixtureDB" && !device.opened && !recoveryAsked);
            recoveryAsked = true;
            if (mode == "deny") {
                why = "Fixture refused this backup.";
                return false;
            }
            return device.recovery = true;
        };
        const auto fast = [](uint32_t creator) {
            assert(creator == 0x46726f6e);
            return false;
        };
        const bool ok = RunPalmConduits(dlp, profile.string(), deviceKey, user, nullptr, fast, recovery, log, error);
        if (mode != "failed" && !ok) std::cerr << mode << ": " << error << '\n';
        assert(ok == (mode != "failed"));
        assert(recoveryAsked && !device.opened && device.wrote == (mode != "deny" && mode != "deny-pref"));
        assert(preferenceAsked == (mode != "deny"));
        assert(logSeen == (mode == "good" || mode == "implicit"));
        assert(device.transactions == (mode == "deny" ? 1 : mode == "deny-pref" ? 5 : 10));
        if (mode == "failed") assert(error.find("0x00000017") != std::string::npos);
        std::cout << "Palm conduit " << mode << " fixture passed\n";
        if (mode == "good") std::ofstream(seed / "data.txt") << "changed seed";
    }

    assert(fs::file_size(data / "data.txt") == 13);
    assert(PalmConduitDataKey("cloned-device", user.userId) != PalmConduitDataKey(deviceKey, user.userId));
    Handheld interrupted;
    DlpClient interruptedDlp(interrupted);
    std::string interruptedError, interruptedLog;
    const auto marker = profile / "conduits" / "installing";
    std::ofstream(marker) << "installer\n";
    assert(RunPalmConduits(interruptedDlp, profile.string(), deviceKey, user, nullptr, {}, {},
                          [&](const std::string& line) { interruptedLog = line; }, interruptedError));
    assert(!interrupted.transactions && interruptedLog.find("did not finish") != std::string::npos);
    fs::remove(marker);
#ifdef _WIN32
    const auto nativeMarker = profile.parent_path() / PalmConduitNativeInstallMarker;
    std::ofstream(nativeMarker) << "installer\n";
    interruptedLog.clear();
    assert(RunPalmConduits(interruptedDlp, profile.string(), deviceKey, user, nullptr, {}, {},
                          [&](const std::string& line) { interruptedLog = line; }, interruptedError));
    assert(!interrupted.transactions && interruptedLog.find("Windows installer did not finish") != std::string::npos);
    fs::remove(nativeMarker);
    std::cout << "Interrupted native installation skipped the conduit\n";
#endif
    std::ofstream(profile / "conduits" / "disabled") << "disabled\n";
    Handheld device;
    DlpClient dlp(device);
    std::string error;
    assert(RunPalmConduits(dlp, profile.string(), deviceKey, user, nullptr, {}, {}, {}, error));
    assert(!device.transactions);
    std::cout << "Disabled registration skipped the conduit\n";
}
