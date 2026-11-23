#include "../installer-shims/InstallBridge.h"
#include "../installer-shims/PalmHotSync.h"

#include <cstdio>
#include <cstdlib>
#include <iterator>

using namespace frondInstaller;

namespace {

void Check(bool result, const char* message, int line) {
    if (result) return;
    std::fprintf(stderr, "line %d: %s (Win32 error %lu)\n", line, message, GetLastError());
    std::exit(1);
}
#define CHECK(expression) Check((expression), #expression, __LINE__)

constexpr DWORD deadline = 10000;
constexpr wchar_t startedName[] = L"FROND_TEST_INSTALLER_STARTED";
constexpr wchar_t exitName[] = L"FROND_TEST_INSTALLER_EXIT";
constexpr wchar_t childName[] = L"FROND_TEST_INSTALLER_CHILD";
constexpr wchar_t crashName[] = L"FROND_TEST_INSTALLER_CRASH";

HANDLE Launch(const fs::path& runner, const std::wstring& installer) {
    std::wstring command = L"\"" + runner.wstring() + L"\" \"" + installer + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    CHECK(CreateProcessW(runner.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                         nullptr, &startup, &process));
    CloseHandle(process.hThread);
    return process.hProcess;
}

DWORD Wait(HANDLE process) {
    CHECK(WaitForSingleObject(process, deadline) == WAIT_OBJECT_0);
    DWORD code{};
    CHECK(GetExitCodeProcess(process, &code));
    CloseHandle(process);
    return code;
}

struct Registration {
    HKEY root;
    const wchar_t* path;
    REGSAM view;

    bool Exists() const {
        HKEY key{};
        const auto status = RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE | view, &key);
        if (status == ERROR_SUCCESS) {
            RegCloseKey(key);
            return true;
        }
        CHECK(status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND);
        return false;
    }
};

int Installer() {
    const HANDLE started = OpenEventW(EVENT_MODIFY_STATE, FALSE, Environment(startedName).c_str());
    const HANDLE finish = OpenEventW(SYNCHRONIZE, FALSE, Environment(exitName).c_str());
    const HANDLE child = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, Environment(childName).c_str());
    CHECK(started && finish && child);
    CHECK(WaitForSingleObject(child, deadline) == WAIT_OBJECT_0);
    CHECK(SetEvent(started));
    CHECK(WaitForSingleObject(finish, deadline) == WAIT_OBJECT_0);
    if (Environment(crashName) == L"1") TerminateProcess(GetCurrentProcess(), 91);
    CHECK(ReleaseMutex(child));
    CloseHandle(child);
    CloseHandle(finish);
    CloseHandle(started);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 1) return Installer();
    CHECK(argc == 3 && !UserDirectory().empty());
    const fs::path runner = fs::absolute(argv[1]);
    const fs::path otherRunner = fs::absolute(argv[2]);
    wchar_t self[32768];
    const DWORD length = GetModuleFileNameW(nullptr, self, std::size(self));
    CHECK(length && length < std::size(self));

    const auto name = L"Local\\Frond.InstallerTest." + std::to_wstring(GetCurrentProcessId());
    const HANDLE started = CreateEventW(nullptr, FALSE, FALSE, (name + L".started").c_str());
    const HANDLE finish = CreateEventW(nullptr, FALSE, FALSE, (name + L".finish").c_str());
    const HANDLE child = CreateMutexW(nullptr, FALSE, (name + L".child").c_str());
    CHECK(started && finish && child);
    CHECK(SetEnvironmentVariableW(startedName, (name + L".started").c_str()));
    CHECK(SetEnvironmentVariableW(exitName, (name + L".finish").c_str()));
    CHECK(SetEnvironmentVariableW(childName, (name + L".child").c_str()));

    const bool wine = Environment(L"FROND_INSTALL_WINE") == L"1";
    const HKEY mobileHive = wine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    const REGSAM view = wine ? (sizeof(void*) == 4 ? KEY_WOW64_32KEY : KEY_WOW64_64KEY) : 0;
    const Registration pilot = WindowsMobile()
        ? Registration{mobileHive, L"Software\\Microsoft\\Windows CE Services", view}
        : Registration{HKEY_CURRENT_USER, L"Software\\U.S. Robotics\\Pilot Desktop", 0};
    const Registration companion = WindowsMobile()
        ? Registration{mobileHive, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\CEAppMgr.exe", view}
        : Registration{HKEY_CURRENT_USER, L"Software\\Palm, Inc.\\Palm Desktop", 0};
    const Registration core{HKEY_CURRENT_USER, L"Software\\U.S. Robotics\\Pilot Desktop\\Core", 0};
    const bool hosted = !WindowsMobile() && !Environment(L"FROND_PALM_CONDUIT_STAGE").empty();
    const auto active = [&] { return pilot.Exists() && companion.Exists() && (WindowsMobile() || core.Exists() == hosted); };
    const auto cleared = [&] { return !pilot.Exists() && !companion.Exists() && (WindowsMobile() || !core.Exists()); };
    CHECK(cleared());

    HKEY foreign{};
    DWORD disposition{};
    CHECK(RegCreateKeyExW(pilot.root, pilot.path, 0, nullptr, 0, KEY_SET_VALUE | pilot.view, nullptr,
                         &foreign, &disposition) == ERROR_SUCCESS && disposition == REG_CREATED_NEW_KEY);
    CHECK(RegSetValueExW(foreign, L"TestForeign", 0, REG_SZ, reinterpret_cast<const BYTE*>(L"Existing"), sizeof(L"Existing")) ==
          ERROR_SUCCESS);
    RegCloseKey(foreign);
    CHECK(Wait(Launch(runner, self)) == ERROR_ALREADY_EXISTS);
    CHECK(pilot.Exists() && !companion.Exists());
    CHECK(RegDeleteKeyExW(pilot.root, pilot.path, pilot.view, 0) == ERROR_SUCCESS);

    CHECK(SetEnvironmentVariableW(crashName, L"1"));
    auto process = Launch(runner, self);
    CHECK(WaitForSingleObject(started, deadline) == WAIT_OBJECT_0);
    CHECK(active());
    if (hosted) {
        const auto window = FindPalmHotSyncWindow();
        CHECK(window && !IsWindowVisible(window));
        const auto library = LoadLibraryW((runner.parent_path() / L"HSAPI.dll").c_str());
        CHECK(library);
        const auto status = reinterpret_cast<LONG (WINAPI*)(DWORD*)>(GetProcAddress(library, "HsGetSyncStatus"));
        DWORD syncing = 1;
        CHECK(status && status(&syncing) == 0 && !syncing);
        CHECK(status(nullptr) == 0x1000000a);
        CHECK(FreeLibrary(library));
    }
    CHECK(Wait(Launch(otherRunner, self)) == ERROR_BUSY);
    CHECK(active());
    CHECK(SetEvent(finish));
    CHECK(Wait(process) == 91);
    CHECK(cleared());
    if (hosted) CHECK(!FindPalmHotSyncWindow());
    CHECK(SetEnvironmentVariableW(crashName, nullptr));
    CHECK(WaitForSingleObject(child, deadline) == WAIT_ABANDONED);
    CHECK(ReleaseMutex(child));

    process = Launch(runner, self);
    CHECK(WaitForSingleObject(started, deadline) == WAIT_OBJECT_0);
    CHECK(TerminateProcess(process, 92));
    CHECK(Wait(process) == 92);
    if (hosted) CHECK(!FindPalmHotSyncWindow());
    CHECK(WaitForSingleObject(child, deadline) == WAIT_ABANDONED);
    CHECK(ReleaseMutex(child));
    CHECK(active());

    CHECK(RegOpenKeyExW(pilot.root, pilot.path, 0, KEY_SET_VALUE | pilot.view, &foreign) == ERROR_SUCCESS);
    CHECK(RegSetValueExW(foreign, L"TestForeign", 0, REG_SZ, reinterpret_cast<const BYTE*>(L"Existing"), sizeof(L"Existing")) ==
          ERROR_SUCCESS);
    CHECK(Wait(Launch(runner, self)) == ERROR_ALREADY_EXISTS);
    CHECK(pilot.Exists());
    CHECK(RegDeleteValueW(foreign, L"TestForeign") == ERROR_SUCCESS);
    RegCloseKey(foreign);

    process = Launch(runner, self);
    CHECK(WaitForSingleObject(started, deadline) == WAIT_OBJECT_0);
    CHECK(active());
    CHECK(SetEvent(finish));
    CHECK(Wait(process) == 0);
    CHECK(cleared());
    if (hosted) {
        const Registration vendor{HKEY_CURRENT_USER, L"Software\\U.S. Robotics\\Pilot Desktop\\TestVendor", 0};
        process = Launch(runner, self);
        CHECK(WaitForSingleObject(started, deadline) == WAIT_OBJECT_0);
        CHECK(active());
        HKEY settings{};
        CHECK(RegCreateKeyExW(vendor.root, vendor.path, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                             &settings, &disposition) == ERROR_SUCCESS && disposition == REG_CREATED_NEW_KEY);
        RegCloseKey(settings);
        CHECK(SetEvent(finish));
        CHECK(Wait(process) == 0);
        CHECK(vendor.Exists() && !core.Exists() && !companion.Exists());
        CHECK(RegOpenKeyExW(pilot.root, pilot.path, 0, KEY_QUERY_VALUE, &settings) == ERROR_SUCCESS);
        CHECK(RegQueryValueExW(settings, L"Path", nullptr, nullptr, nullptr, nullptr) == ERROR_FILE_NOT_FOUND);
        RegCloseKey(settings);
        process = Launch(runner, self);
        CHECK(WaitForSingleObject(started, deadline) == WAIT_OBJECT_0);
        CHECK(active() && vendor.Exists());
        CHECK(SetEvent(finish));
        CHECK(Wait(process) == 0);
        CHECK(vendor.Exists() && !core.Exists() && !companion.Exists());
        CHECK(RegDeleteKeyExW(vendor.root, vendor.path, 0, 0) == ERROR_SUCCESS);
        CHECK(RegDeleteKeyExW(pilot.root, pilot.path, 0, 0) == ERROR_SUCCESS);
    }
    CloseHandle(child);
    CloseHandle(finish);
    CloseHandle(started);
    std::puts("Installer registry recovery passed.");
    return 0;
}
