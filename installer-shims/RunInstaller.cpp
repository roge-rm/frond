#include "InstallBridge.h"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <iterator>
#include <vector>

using namespace frondInstaller;

namespace {

constexpr wchar_t ownerValue[] = L"FrondInstallerOwner";
constexpr wchar_t ownerTag[] = L"{BF9A14F6-7967-4360-BF73-38DF9B82F449}";

struct RegistryValue {
    const wchar_t* name;
    std::wstring data;
};

class Handle {
   public:
    explicit Handle(HANDLE handle) : handle(handle) {}
    Handle(const Handle&) = delete;
    ~Handle() {
        if (handle) CloseHandle(handle);
    }

    const HANDLE handle;
};

class InstallerLock {
   public:
    DWORD Acquire() {
        if (!mutex.handle) return GetLastError();
        switch (WaitForSingleObject(mutex.handle, 0)) {
            case WAIT_OBJECT_0:
            case WAIT_ABANDONED:
                held = true;
                return ERROR_SUCCESS;
            case WAIT_TIMEOUT:
                return ERROR_BUSY;
            default:
                return GetLastError();
        }
    }

    ~InstallerLock() {
        if (held) ReleaseMutex(mutex.handle);
    }

   private:
    Handle mutex{CreateMutexW(nullptr, FALSE, L"Global\\Frond.InstallerRegistry.BF9A14F6-7967-4360-BF73-38DF9B82F449")};
    bool held{};
};

bool Owned(HKEY key) {
    wchar_t tag[std::size(ownerTag)]{};
    DWORD bytes = sizeof(tag);
    DWORD type{};
    return RegQueryValueExW(key, ownerValue, nullptr, &type, reinterpret_cast<BYTE*>(tag), &bytes) == ERROR_SUCCESS &&
           type == REG_SZ && bytes == sizeof(tag) && std::equal(std::begin(tag), std::end(tag), std::begin(ownerTag));
}

bool OnlyInstallerValues(HKEY key, const std::vector<RegistryValue>& values) {
    for (DWORD index = 0;; ++index) {
        wchar_t name[64];
        DWORD length = std::size(name);
        const auto result = RegEnumValueW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS) return true;
        if (result != ERROR_SUCCESS) return false;
        if (_wcsicmp(name, ownerValue) == 0) continue;
        const auto known = [&](const auto& value) { return _wcsicmp(name, value.name ? value.name : L"") == 0; };
        if (std::find_if(values.begin(), values.end(), known) == values.end()) return false;
    }
}

class RegistryEntry {
   public:
    RegistryEntry() = default;
    RegistryEntry(const RegistryEntry&) = delete;
    ~RegistryEntry() {
        if (!key) return;
        if (!published || (Owned(key) && OnlyInstallerValues(key, values)))
            RegDeleteKeyExW(hive, name.c_str(), view, 0);
        RegCloseKey(key);
    }

    LONG Register(HKEY root, const wchar_t* path, REGSAM registryView, std::initializer_list<RegistryValue> entries) {
        values = entries;
        HKEY existing{};
        const auto result = RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE | registryView, &existing);
        if (result == ERROR_SUCCESS) {
            const bool ours = Owned(existing) && OnlyInstallerValues(existing, values);
            RegCloseKey(existing);
            if (!ours) return ERROR_ALREADY_EXISTS;
            const auto removed = RegDeleteKeyExW(root, path, registryView, 0);
            if (removed != ERROR_SUCCESS) return removed;
        } else if (result != ERROR_FILE_NOT_FOUND && result != ERROR_PATH_NOT_FOUND) {
            return result;
        }

        // Rename only after marking the key so a stopped launcher leaves an identifiable registration.
        const std::wstring temporary = std::wstring(path) + L".FrondInstaller." + std::to_wstring(GetCurrentProcessId()) +
                                       L"." + std::to_wstring(GetTickCount64());
        std::wstring destination(path);
        const auto leaf = destination.c_str() + destination.find_last_of(L'\\') + 1;
        DWORD disposition{};
        HKEY opened{};
        const auto created = RegCreateKeyExW(root, temporary.c_str(), 0, nullptr, 0, KEY_WRITE | KEY_QUERY_VALUE | registryView, nullptr,
                                            &opened, &disposition);
        if (created != ERROR_SUCCESS) return created;
        if (disposition != REG_CREATED_NEW_KEY) {
            RegCloseKey(opened);
            return ERROR_ALREADY_EXISTS;
        }
        hive = root;
        key = opened;
        name = temporary;
        view = registryView;
        auto status = Write(ownerValue, ownerTag);
        for (const auto& value : values) {
            if (status != ERROR_SUCCESS) return status;
            status = Write(value.name, value.data);
        }
        if (status != ERROR_SUCCESS) return status;
        status = RegRenameKey(key, nullptr, leaf);
        if (status == ERROR_SUCCESS) {
            name.swap(destination);
            published = true;
        }
        return status;
    }

   private:
    LONG Write(const wchar_t* value, const std::wstring& data) {
        return RegSetValueExW(key, value, 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()),
                             (data.size() + 1) * sizeof(wchar_t));
    }

    HKEY hive{}, key{};
    std::wstring name;
    REGSAM view{};
    bool published{};
    std::vector<RegistryValue> values;
};

std::wstring Quote(const std::wstring& path) { return L"\"" + path + L"\""; }

DWORD Run(const fs::path& installer, HANDLE job) {
    const bool msi = !_wcsicmp(installer.extension().c_str(), L".msi");
    auto command = msi ? L"msiexec.exe /i " + Quote(installer.wstring()) : Quote(installer.wstring());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto program = msi ? (fs::path(Environment(L"SystemRoot")) / L"System32" / L"msiexec.exe") : installer;
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                        installer.parent_path().c_str(), &startup, &process)) return GetLastError();
    Handle child(process.hProcess), thread(process.hThread);
    if (!AssignProcessToJobObject(job, child.handle)) {
        const auto error = GetLastError();
        TerminateProcess(child.handle, error);
        return error;
    }
    if (ResumeThread(thread.handle) == DWORD(-1) || WaitForSingleObject(child.handle, INFINITE) != WAIT_OBJECT_0)
        return GetLastError();

    DWORD code{};
    if (!GetExitCodeProcess(child.handle, &code)) return GetLastError();
    if (code) return code;

    // A setup launcher can exit before the installer it started.
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    do {
        if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr))
            return GetLastError();
        if (accounting.ActiveProcesses) Sleep(100);
    } while (accounting.ActiveProcesses);
    return code;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || UserDirectory().empty()) return ERROR_INVALID_PARAMETER;
    const auto directory = Environment(L"FROND_INSTALL_SHIMS");
    if (directory.empty()) return ERROR_PATH_NOT_FOUND;

    InstallerLock lock;
    if (const auto status = lock.Acquire(); status != ERROR_SUCCESS) return status;
    const fs::path shims(directory);
    std::array<RegistryEntry, 4> entries;
    Handle job(CreateJobObjectW(nullptr, L"Global\\Frond.InstallerProcess.BF9A14F6-7967-4360-BF73-38DF9B82F449"));
    const auto jobStatus = GetLastError();
    if (!job.handle) return jobStatus;
    if (jobStatus == ERROR_ALREADY_EXISTS) return ERROR_BUSY;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.handle, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return GetLastError();

    if (WindowsMobile()) {
        constexpr auto appPaths = L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\CEAppMgr.exe";
        constexpr auto services = L"Software\\Microsoft\\Windows CE Services";
        const auto app = (shims / L"CEAppMgr.exe").wstring();
        const auto registerApp = [&](HKEY hive, REGSAM view, size_t offset) -> LONG {
            const auto status = entries[offset].Register(hive, appPaths, view, {{nullptr, app}, {L"Path", directory}});
            if (status != ERROR_SUCCESS) return status;
            return entries[offset + 1].Register(hive, services, view, {{L"InstalledDir", directory}});
        };
        const auto machine = registerApp(HKEY_LOCAL_MACHINE, sizeof(void*) == 4 ? KEY_WOW64_32KEY : KEY_WOW64_64KEY, 0);
        if (machine != ERROR_SUCCESS && machine != ERROR_ACCESS_DENIED) return machine;
        if (Environment(L"FROND_INSTALL_WINE") == L"1") {
            if (machine != ERROR_SUCCESS) return machine;
        } else {
            const auto user = registerApp(HKEY_CURRENT_USER, 0, 2);
            if (user != ERROR_SUCCESS) return user;
        }
    } else {
        constexpr auto pilot = L"Software\\U.S. Robotics\\Pilot Desktop";
        constexpr auto palm = L"Software\\Palm, Inc.\\Palm Desktop";
        const auto install = (UserDirectory() / L"Install").wstring();
        for (size_t i = 0; i < 2; ++i) {
            const auto status = entries[i].Register(HKEY_CURRENT_USER, i == 0 ? pilot : palm, 0,
                                                    {{L"Path", directory}, {L"DesktopPath", directory}, {L"InstallPath", install}});
            if (status != ERROR_SUCCESS) return status;
        }
    }
    const auto path = directory + L";" + Environment(L"PATH");
    if (!SetEnvironmentVariableW(L"PATH", path.c_str())) return GetLastError();

    return int(Run(fs::absolute(argv[1]), job.handle));
}
