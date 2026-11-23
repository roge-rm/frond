#include "InstallBridge.h"
#include "PalmHotSync.h"

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
        if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
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

class HotSyncWindow {
   public:
    ~HotSyncWindow() {
        if (window) DestroyWindow(window);
    }

    DWORD Start() {
        if (FindPalmHotSyncWindow()) return ERROR_ALREADY_EXISTS;
        WNDCLASSW definition{};
        definition.lpfnWndProc = OnMessage;
        definition.hInstance = GetModuleHandleW(nullptr);
        definition.lpszClassName = palmHotSyncClass;
        if (!RegisterClassW(&definition)) return GetLastError();
        window = CreateWindowExW(0, palmHotSyncClass, palmHotSyncTitle, WS_POPUP, 0, 0, 0, 0,
                                 nullptr, nullptr, definition.hInstance, nullptr);
        return window ? ERROR_SUCCESS : GetLastError();
    }

   private:
    static LRESULT CALLBACK OnMessage(HWND window, UINT message, WPARAM parameter, LPARAM data) {
        if (message == palmHotSyncStatusMessage && parameter == palmHotSyncSyncing) {
            // The managed installer holds the profile's sync lock.
            return FALSE;
        }
        return DefWindowProcW(window, message, parameter, data);
    }

    HWND window{};
};

DWORD WaitForInstaller(HANDLE process, DWORD timeout, bool hosted) {
    if (!hosted) {
        if (process) return WaitForSingleObject(process, timeout);
        Sleep(timeout);
        return WAIT_TIMEOUT;
    }
    const DWORD count = process ? 1 : 0;
    const auto state = MsgWaitForMultipleObjects(count, process ? &process : nullptr, FALSE, timeout, QS_ALLINPUT);
    if (state != WAIT_OBJECT_0 + count) return state;
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    return WAIT_TIMEOUT;
}

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

LONG ClearRegistration(HKEY root, const wchar_t* path, REGSAM view, const std::vector<RegistryValue>& values) {
    HKEY existing{};
    const auto result = RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE | view, &existing);
    if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) return ERROR_SUCCESS;
    if (result != ERROR_SUCCESS) return result;
    const bool ours = Owned(existing) && OnlyInstallerValues(existing, values);
    RegCloseKey(existing);
    return ours ? RegDeleteKeyExW(root, path, view, 0) : ERROR_ALREADY_EXISTS;
}

class RegistryEntry {
   public:
    RegistryEntry() = default;
    RegistryEntry(const RegistryEntry&) = delete;
    ~RegistryEntry() {
        if (!key) return;
        if (!published || (Owned(key) && OnlyInstallerValues(key, values))) {
            const auto removed = RegDeleteKeyExW(hive, name.c_str(), view, 0);
            if (preserveChildren && removed == ERROR_ACCESS_DENIED) {
                // Vendor subkeys stay in the owned container between installer runs.
                for (const auto& value : values) ClearValue(value);
            }
        }
        RegCloseKey(key);
    }

    LONG Register(HKEY root, const wchar_t* path, REGSAM registryView, std::initializer_list<RegistryValue> entries,
                  bool keepChildren = false) {
        values = entries;
        preserveChildren = keepChildren;
        if (preserveChildren) {
            HKEY existing{};
            if (RegOpenKeyExW(root, path, 0, KEY_WRITE | KEY_QUERY_VALUE | KEY_ENUMERATE_SUB_KEYS | registryView,
                              &existing) == ERROR_SUCCESS) {
                DWORD children{};
                const auto query = RegQueryInfoKeyW(existing, nullptr, nullptr, nullptr, &children, nullptr,
                                                    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                if (query == ERROR_SUCCESS && children && Owned(existing) && OnlyInstallerValues(existing, values)) {
                    hive = root;
                    key = existing;
                    name = path;
                    view = registryView;
                    published = true;
                    return WriteValues();
                }
                RegCloseKey(existing);
            }
        }
        const auto cleared = ClearRegistration(root, path, registryView, values);
        if (cleared != ERROR_SUCCESS) return cleared;

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
        if (status != ERROR_SUCCESS) return status;
        status = WriteValues();
        if (status != ERROR_SUCCESS) return status;
        status = RegRenameKey(key, nullptr, leaf);
        if (status == ERROR_SUCCESS) {
            name.swap(destination);
            published = true;
        }
        return status;
    }

   private:
    LONG WriteValues() {
        for (const auto& value : values) {
            const auto status = Write(value.name, value.data);
            if (status != ERROR_SUCCESS) return status;
        }
        return ERROR_SUCCESS;
    }

    void ClearValue(const RegistryValue& value) {
        std::vector<wchar_t> current(value.data.size() + 1);
        DWORD bytes = current.size() * sizeof(wchar_t), type{};
        if (RegQueryValueExW(key, value.name, nullptr, &type, reinterpret_cast<BYTE*>(current.data()), &bytes) == ERROR_SUCCESS &&
            type == REG_SZ && bytes == current.size() * sizeof(wchar_t) &&
            std::equal(current.begin(), current.end() - 1, value.data.begin()) && current.back() == L'\0')
            RegDeleteValueW(key, value.name);
    }

    LONG Write(const wchar_t* value, const std::wstring& data) {
        return RegSetValueExW(key, value, 0, REG_SZ, reinterpret_cast<const BYTE*>(data.c_str()),
                             (data.size() + 1) * sizeof(wchar_t));
    }

    HKEY hive{}, key{};
    std::wstring name;
    REGSAM view{};
    bool published{}, preserveChildren{};
    std::vector<RegistryValue> values;
};

std::wstring Quote(const std::wstring& path) { return L"\"" + path + L"\""; }

BOOL CALLBACK FoundManifest(HMODULE, LPCWSTR, LPWSTR, LONG_PTR found) {
    *reinterpret_cast<bool*>(found) = true;
    return FALSE;
}

bool Unmanifested32BitInstaller(const fs::path& installer) {
    if (_wcsicmp(installer.extension().c_str(), L".exe")) return false;
    DWORD type{};
    if (!GetBinaryTypeW(installer.c_str(), &type) || type != SCS_32BIT_BINARY) return false;
    const auto sidecar = installer.wstring() + L".manifest";
    if (GetFileAttributesW(sidecar.c_str()) != INVALID_FILE_ATTRIBUTES || GetLastError() != ERROR_FILE_NOT_FOUND)
        return false;

    const auto image = LoadLibraryExW(installer.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!image) return false;
    bool found = false;
    SetLastError(ERROR_SUCCESS);
    EnumResourceNamesW(image, MAKEINTRESOURCEW(24), FoundManifest, reinterpret_cast<LONG_PTR>(&found));
    const auto error = GetLastError();
    FreeLibrary(image);
    return !found && (error == ERROR_RESOURCE_TYPE_NOT_FOUND || error == ERROR_RESOURCE_DATA_NOT_FOUND);
}

DWORD Run(const fs::path& installer, HANDLE job, bool hosted) {
    const bool msi = !_wcsicmp(installer.extension().c_str(), L".msi");
    const bool wine = Environment(L"FROND_INSTALL_WINE") == L"1";
    const auto commandLine = msi ? L"msiexec.exe /i " + Quote(installer.wstring()) : Quote(installer.wstring());
    const auto program = msi ? (fs::path(Environment(L"SystemRoot")) / L"System32" / L"msiexec.exe") : installer;
    PROCESS_INFORMATION process{};
    const auto start = [&]() -> DWORD {
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        auto command = commandLine;
        if (CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr,
                           installer.parent_path().c_str(), &startup, &process)) return ERROR_SUCCESS;
        return GetLastError();
    };
    auto status = start();
    if (status == ERROR_ELEVATION_REQUIRED && !wine && !msi && Unmanifested32BitInstaller(installer)) {
        // Old installers can trigger filename detection. Retry with the caller's existing token.
        constexpr auto compatibility = L"__COMPAT_LAYER";
        const auto previous = Environment(compatibility);
        const auto layer = previous.empty() ? L"RunAsInvoker" : previous + L" RunAsInvoker";
        if (!SetEnvironmentVariableW(compatibility, layer.c_str())) return status;
        status = start();
        SetEnvironmentVariableW(compatibility, previous.empty() ? nullptr : previous.c_str());
    }
    if (status != ERROR_SUCCESS) return status;
    Handle child(process.hProcess), thread(process.hThread);
    if (!AssignProcessToJobObject(job, child.handle)) {
        const auto error = GetLastError();
        if (TerminateProcess(child.handle, error)) WaitForSingleObject(child.handle, 1000);
        return error;
    }
    // Only honour a stop once the suspended process belongs to the job.
    const auto stop = wine ? UserDirectory().parent_path() / L"stop" : fs::path{};
    const auto cancel = [&]() {
        if (stop.empty() || GetFileAttributesW(stop.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
        TerminateJobObject(job, ERROR_CANCELLED);
        WaitForSingleObject(child.handle, 1000);
        return true;
    };
    if (cancel()) return ERROR_CANCELLED;
    if (ResumeThread(thread.handle) == DWORD(-1)) return GetLastError();
    for (;;) {
        const auto state = WaitForInstaller(child.handle, stop.empty() ? INFINITE : 100, hosted);
        if (state == WAIT_OBJECT_0) break;
        if (state != WAIT_TIMEOUT) return GetLastError();
        if (cancel()) return ERROR_CANCELLED;
    }

    DWORD code{};
    if (!GetExitCodeProcess(child.handle, &code)) return GetLastError();
    if (code) return code;

    // A setup launcher can exit before the installer it started.
    const auto stage = UserDirectory().parent_path();
    const auto finish = stage / L"finish";
    bool offered = false;
    bool canFinish = false;
    bool finishing = false;
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    do {
        if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr))
            return GetLastError();
        if (accounting.ActiveProcesses) {
            if (cancel()) return ERROR_CANCELLED;
            if (hosted && !offered) {
                offered = true;
                Handle signal(CreateFileW((stage / L"finish-ready").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                          CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
                canFinish = signal.handle != INVALID_HANDLE_VALUE;
            }
            if (canFinish && !finishing) {
                const auto request = GetFileAttributesW(finish.c_str());
                if (request != INVALID_FILE_ATTRIBUTES &&
                    !(request & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
                    if (!TerminateJobObject(job, ERROR_CANCELLED)) return GetLastError();
                    finishing = true;
                }
            }
            if (WaitForInstaller(nullptr, 100, hosted) == WAIT_FAILED) return GetLastError();
        }
    } while (accounting.ActiveProcesses);
    return code;
}

DWORD PreparePalmApplication(const fs::path& program) {
    if (Environment(L"FROND_INSTALL_WINE") != L"1" || Environment(L"FROND_MANAGED_PALM_APP") != L"1")
        return ERROR_SUCCESS;
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll || !GetProcAddress(ntdll, "wine_get_version")) return ERROR_SUCCESS;
    const auto root = program.root_name().wstring();
    const auto name = program.filename().wstring();
    if (_wcsicmp(root.c_str(), L"C:") || !program.is_absolute() || _wcsicmp(program.extension().c_str(), L".exe"))
        return ERROR_INVALID_NAME;
    const auto attributes = GetFileAttributesW(program.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError();
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_INVALID_NAME;

    const auto key = std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + name;
    std::vector<wchar_t> current(name.size() + 1);
    DWORD size = current.size() * sizeof(wchar_t), type{};
    const auto read = RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, RRF_RT_REG_SZ,
                                   &type, current.data(), &size);
    if (read == ERROR_FILE_NOT_FOUND || read == ERROR_PATH_NOT_FOUND || read == ERROR_MORE_DATA ||
        read == ERROR_UNSUPPORTED_TYPE) return ERROR_SUCCESS;
    if (read != ERROR_SUCCESS) return read;
    if (size != current.size() * sizeof(wchar_t) || current.back() || _wcsicmp(current.data(), name.c_str()))
        return ERROR_SUCCESS;

    // Restore the private Wine entry when setup saved only the executable's name.
    const auto path = program.wstring();
    return RegSetKeyValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, REG_SZ, path.c_str(),
                           (path.size() + 1) * sizeof(wchar_t));
}

DWORD PreparePalmRuntime() {
    const auto source = Environment(L"FROND_PALM_MFC42_STAGE");
    if (source.empty() || WindowsMobile() || Environment(L"FROND_INSTALL_WINE") != L"1" ||
        Environment(L"FROND_PALM_CONDUIT_STAGE").empty()) return ERROR_SUCCESS;
    wchar_t system[MAX_PATH];
    const auto length = GetSystemDirectoryW(system, std::size(system));
    if (!length || length >= std::size(system)) return ERROR_PATH_NOT_FOUND;
    const auto target = fs::path(system) / L"mfc42.dll";
    const auto attributes = GetFileAttributesW(target.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
        return attributes & FILE_ATTRIBUTE_DIRECTORY ? ERROR_DIRECTORY : ERROR_SUCCESS;
    if (CopyFileW(source.c_str(), target.c_str(), TRUE)) return ERROR_SUCCESS;
    const auto result = GetLastError();
    return result == ERROR_FILE_EXISTS || result == ERROR_ALREADY_EXISTS ? ERROR_SUCCESS : result;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || UserDirectory().empty()) return ERROR_INVALID_PARAMETER;
    const auto directory = Environment(L"FROND_INSTALL_SHIMS");
    if (directory.empty()) return ERROR_PATH_NOT_FOUND;

    InstallerLock lock;
    if (const auto status = lock.Acquire(); status != ERROR_SUCCESS) return status;
    if (const auto status = PreparePalmRuntime(); status != ERROR_SUCCESS) return status;
    const fs::path shims(directory);
    const bool hosted = !WindowsMobile() && !Environment(L"FROND_PALM_CONDUIT_STAGE").empty();
    HotSyncWindow presence;
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
        constexpr auto core = L"Software\\U.S. Robotics\\Pilot Desktop\\Core";
        const auto install = (UserDirectory() / L"Install").wstring();
        const auto hotSync = shims / L"HotSync.exe";
        const std::vector<RegistryValue> coreValues{{L"HotSyncPath", hotSync.wstring()}};
        const auto cleared = ClearRegistration(HKEY_CURRENT_USER, core, 0, coreValues);
        if (cleared != ERROR_SUCCESS) return cleared;
        for (size_t i = 0; i < 2; ++i) {
            const auto status = entries[i].Register(HKEY_CURRENT_USER, i == 0 ? pilot : palm, 0,
                                                    {{L"Path", directory}, {L"DesktopPath", directory}, {L"InstallPath", install}}, hosted);
            if (status != ERROR_SUCCESS) return status;
        }
        if (hosted) {
            if (GetFileAttributesW(hotSync.c_str()) == INVALID_FILE_ATTRIBUTES ||
                GetFileAttributesW((shims / L"CondMgr.dll").c_str()) == INVALID_FILE_ATTRIBUTES)
                return ERROR_FILE_NOT_FOUND;
            const auto status = entries[2].Register(HKEY_CURRENT_USER, core, 0, {{L"HotSyncPath", hotSync.wstring()}});
            if (status != ERROR_SUCCESS) return status;
            if (const auto visible = presence.Start(); visible != ERROR_SUCCESS) return visible;
        }
    }
    const auto path = directory + L";" + Environment(L"PATH");
    if (!SetEnvironmentVariableW(L"PATH", path.c_str())) return GetLastError();

    const auto program = fs::absolute(argv[1]);
    if (hosted) {
        if (const auto status = PreparePalmApplication(program); status != ERROR_SUCCESS) return status;
    }
    const auto result = Run(program, job.handle, hosted);
    if (result != ERROR_SUCCESS || !hosted)
        return int(result);
    const auto manager = LoadLibraryW((shims / L"CondMgr.dll").c_str());
    if (!manager) return GetLastError();
    const auto complete = reinterpret_cast<int (WINAPI*)()>(GetProcAddress(manager, "FrondCompleteRegistrations"));
    const auto published = complete ? (complete() == 0 ? ERROR_SUCCESS : ERROR_INVALID_DATA) : ERROR_PROC_NOT_FOUND;
    FreeLibrary(manager);
    return published;
}
