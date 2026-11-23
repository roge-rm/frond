#include "InstallBridge.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string_view>
#include <vector>

using namespace frondInstaller;

namespace {

enum Error {
    invalidIndex = -1001, deleteFailed = -1002, noConduit = -1003, noMemory = -1004,
    exists = -1005, storageFailure = -1006, createFailed = -1007, writeFailed = -1008,
    tooSmall = -1010, noValue = -1011, invalidCreator = -1012, invalidArgument = -1013,
    invalidType = -1015, noNotifier = -1021
};

using Fields = std::map<std::string, std::string>;

class RegistrationLock {
   public:
    RegistrationLock() : handle(CreateMutexW(nullptr, FALSE, L"Local\\Frond.PalmConduitRegistration")) {
        if (!handle) return;
        const auto status = WaitForSingleObject(handle, 10000);
        held = status == WAIT_OBJECT_0 || status == WAIT_ABANDONED;
    }
    ~RegistrationLock() {
        if (held) ReleaseMutex(handle);
        if (handle) CloseHandle(handle);
    }
    explicit operator bool() const { return held; }

   private:
    HANDLE handle;
    bool held{};
};

fs::path RegistrationDirectory() { return Environment(L"FROND_PALM_CONDUIT_STAGE"); }

std::string CreatorKey(const char* creator) {
    if (!creator || strnlen(creator, 5) != 4) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string key;
    for (size_t i = 0; i < 4; ++i) {
        const auto byte = static_cast<unsigned char>(creator[i]);
        if (byte < 0x20 || byte > 0x7e) return {};
        key += hex[byte >> 4];
        key += hex[byte & 15];
    }
    return key;
}

std::string FromAnsi(const char* value) {
    if (!value || strnlen(value, 4097) > 4096) return {};
    const int count = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, value, -1, nullptr, 0);
    if (count <= 1) return {};
    std::wstring wide(count, L'\0');
    if (!MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, value, -1, wide.data(), count)) return {};
    wide.pop_back();
    return fs::path(wide).u8string();
}

std::string ToAnsi(const std::string& value) {
    const auto wide = fs::u8path(value).wstring();
    const int count = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (!count) return {};
    std::string result(count, '\0');
    if (!WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, result.data(), count, nullptr, nullptr)) return {};
    result.pop_back();
    return result;
}

bool Read(const fs::path& path, Fields& fields) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > 65536) return false;
    std::ifstream input(path, std::ios::binary);
    std::string line, key, value;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        if (!(row >> key >> std::quoted(value)) || (row >> std::ws).peek() != EOF ||
            !fields.emplace(key, value).second) return false;
    }
    return input.eof();
}

bool Write(const fs::path& path, const Fields& fields) {
    const auto temporary = path.wstring() + L".tmp";
    std::ofstream output(temporary.c_str(), std::ios::binary | std::ios::trunc);
    for (const auto& [key, value] : fields) output << key << ' ' << std::quoted(value) << '\n';
    output.close();
    if (output && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
    DeleteFileW(temporary.c_str());
    return false;
}

template <typename Operation>
int WithRegistration(const char* creator, Operation operate, bool create = false) {
    try {
        const auto key = CreatorKey(creator);
        if (key.empty()) return invalidCreator;
        RegistrationLock lock;
        const auto root = RegistrationDirectory();
        if (!lock || root.empty()) return storageFailure;
        const auto file = root / (key + ".conf");
        Fields fields;
        std::error_code error;
        const bool present = fs::exists(file, error);
        if (error || (present && !Read(file, fields))) return storageFailure;
        if (create && present) return exists;
        if (!create && !present) return noConduit;
        return operate(file, fields);
    } catch (...) {
        return storageFailure;
    }
}

int Set(const char* creator, const char* field, std::string value) {
    if (value.size() > 1024 || value.find_first_of("\r\n") != std::string::npos) return invalidArgument;
    return WithRegistration(creator, [&](const auto& path, auto& fields) -> int {
        fields[field] = std::move(value);
        fields["pending"] = "1";
        return Write(path, fields) ? 0 : writeFailed;
    });
}

int SetText(const char* creator, const char* field, const char* value) {
    if (!value) return invalidArgument;
    try {
        auto text = FromAnsi(value);
        if (*value && text.empty()) return invalidArgument;
        return Set(creator, field, std::move(text));
    } catch (...) {
        return invalidArgument;
    }
}

int Copy(const std::string& value, char* output, int* size) {
    if (!size || *size < 0) return invalidArgument;
    const int needed = static_cast<int>(value.size()) + 1;
    const bool fits = *size >= needed;
    *size = needed;
    if (!output || !fits) return tooSmall;
    std::memcpy(output, value.c_str(), needed);
    return 0;
}

int GetText(const char* creator, const char* field, char* output, int* size) {
    return WithRegistration(creator, [&](const auto&, const auto& fields) -> int {
        const auto found = fields.find(field);
        if (found == fields.end()) return noValue;
        return Copy(ToAnsi(found->second), output, size);
    });
}

std::vector<std::string> Creators() {
    std::vector<std::string> result;
    constexpr std::string_view hex = "0123456789abcdef";
    for (const auto& entry : fs::directory_iterator(RegistrationDirectory())) {
        if (!fs::is_regular_file(entry.symlink_status()) || entry.path().extension() != ".conf") continue;
        const auto key = entry.path().stem().string();
        if (key.size() != 8 || key.find_first_not_of(hex) != std::string::npos) continue;
        std::string creator;
        for (size_t i = 0; i < key.size(); i += 2)
            creator += static_cast<char>(16 * hex.find(key[i]) + hex.find(key[i + 1]));
        result.push_back(std::move(creator));
    }
    std::sort(result.begin(), result.end());
    return result;
}

fs::path ExistingDll(fs::path path) {
    if (path.empty()) return {};
    if (path.extension().empty()) path.replace_extension(L".dll");
    if (_wcsicmp(path.extension().c_str(), L".dll")) return {};
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return {};
    return fs::absolute(path).lexically_normal();
}

fs::path ResolveDll(const Fields& fields) {
    const auto found = fields.find("name");
    if (found == fields.end() || found->second.empty()) return {};
    const auto name = fs::u8path(found->second);
    if (name.is_absolute()) return ExistingDll(name);
    if (name.has_parent_path() || name.has_root_path()) return {};

    fs::path selected;
    const auto include = [&](const fs::path& directory) {
        if (!directory.is_absolute()) return true;
        auto file = ExistingDll(directory / name);
        if (file.empty()) return true;
        if (!selected.empty() && _wcsicmp(selected.c_str(), file.c_str())) return false;
        selected = std::move(file);
        return true;
    };
    if (!include(Environment(L"FROND_INSTALL_SHIMS"))) return {};
    const auto module = fields.find("module");
    if (module != fields.end()) {
        const auto path = fs::u8path(module->second);
        const auto directory = path.has_extension() ? path.parent_path() : path;
        if (!include(directory)) return {};
    }
    const auto titleField = fields.find("title");
    if (titleField != fields.end() && !titleField->second.empty()) {
        const auto title = fs::u8path(titleField->second);
        if (!title.has_parent_path() && !title.has_root_path() && title != "." && title != "..") {
            for (const auto* variable : {L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432"}) {
                const fs::path programs = Environment(variable);
                if (programs.is_absolute() && !include(programs / title)) return {};
            }
        }
    }
    return selected;
}

// CondMgr returns a movable HGLOBAL with string offsets after this header.
struct ConduitDescription {
    int version, size, type;
    char creator[8];
    DWORD priority;
    int offsets[7];
};
static_assert(sizeof(ConduitDescription) == 52);

}  // namespace

extern "C" {

__declspec(dllexport) int WINAPI FrondCompleteRegistrations() {
    try {
        RegistrationLock lock;
        if (!lock || RegistrationDirectory().empty()) return storageFailure;
        for (const auto& creator : Creators()) {
            const auto result = WithRegistration(creator.c_str(), [](const auto& path, auto& fields) -> int {
                const auto name = ResolveDll(fields);
                if (name.empty()) return noValue;
                const auto resolved = name.u8string();
                if (!fields.count("pending") && fields.at("name") == resolved) return 0;
                fields["name"] = resolved;
                fields.erase("pending");
                return Write(path, fields) ? 0 : writeFailed;
            });
            if (result != 0) return result;
        }
        return 0;
    } catch (...) {
        return storageFailure;
    }
}

__declspec(dllexport) int WINAPI CmInstallCreator(const char* creator, int type) {
    if (type != 1) return invalidType;
    return WithRegistration(creator, [&](const auto& path, auto& fields) -> int {
        fields = {{"type", std::to_string(type)}, {"priority", "2"}, {"integrate", "0"}, {"pending", "1"}};
        return Write(path, fields) ? 0 : createFailed;
    }, true);
}

__declspec(dllexport) int WINAPI CmRemoveConduitByCreatorID(const char* creator) {
    const auto result = WithRegistration(creator, [](const auto& path, const auto&) -> int {
        return DeleteFileW(path.c_str()) ? 1 : deleteFailed;
    });
    return result == noConduit ? 0 : result;
}

__declspec(dllexport) int WINAPI CmGetConduitCount() {
    try {
        RegistrationLock lock;
        return !lock || RegistrationDirectory().empty() ? storageFailure : static_cast<int>(Creators().size());
    } catch (...) {
        return storageFailure;
    }
}

__declspec(dllexport) int WINAPI CmGetConduitCreatorID(int index, char* creator, int* size) {
    if (index < 0) return invalidIndex;
    try {
        RegistrationLock lock;
        if (!lock || RegistrationDirectory().empty()) return storageFailure;
        const auto creators = Creators();
        if (static_cast<size_t>(index) >= creators.size()) return invalidIndex;
        return Copy(creators[index], creator, size);
    } catch (...) {
        return storageFailure;
    }
}

__declspec(dllexport) int WINAPI CmGetConduitByCreator(const char* creator, HANDLE* output) {
    if (!output) return invalidArgument;
    *output = nullptr;
    return WithRegistration(creator, [&](const auto&, const auto& fields) -> int {
        ConduitDescription description{};
        description.version = 100;
        description.type = std::stoi(fields.at("type"));
        description.priority = std::stoul(fields.at("priority"));
        std::memcpy(description.creator, creator, 4);
        const std::array keys{"name", "directory", "file", "remote", "user", "title", "info"};
        std::string strings;
        for (size_t i = 0; i < keys.size(); ++i) {
            const auto found = fields.find(keys[i]);
            if (found == fields.end()) continue;
            description.offsets[i] = sizeof(description) + strings.size();
            strings += ToAnsi(found->second);
            strings += '\0';
        }
        description.size = sizeof(description) + strings.size();
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, description.size);
        if (!memory) return noMemory;
        void* buffer = GlobalLock(memory);
        if (!buffer) {
            GlobalFree(memory);
            return noMemory;
        }
        std::memcpy(buffer, &description, sizeof(description));
        std::memcpy(static_cast<char*>(buffer) + sizeof(description), strings.data(), strings.size());
        GlobalUnlock(memory);
        *output = memory;
        return 0;
    });
}

__declspec(dllexport) int WINAPI CmGetCreatorName(const char* creator, char* output, int* size) {
    return GetText(creator, "name", output, size);
}
__declspec(dllexport) int WINAPI CmGetCreatorModule(const char* creator, char* output, int* size) {
    return GetText(creator, "module", output, size);
}
__declspec(dllexport) int WINAPI CmSetCreatorName(const char* creator, const char* value) {
    return SetText(creator, "name", value);
}
__declspec(dllexport) int WINAPI CmSetCreatorModule(const char* creator, const char* value) {
    return SetText(creator, "module", value);
}
__declspec(dllexport) int WINAPI CmSetCreatorDirectory(const char* creator, const char* value) {
    return SetText(creator, "directory", value);
}
__declspec(dllexport) int WINAPI CmSetCreatorFile(const char* creator, const char* value) {
    return SetText(creator, "file", value);
}
__declspec(dllexport) int WINAPI CmSetCreatorRemote(const char* creator, const char* value) {
    return SetText(creator, "remote", value);
}
__declspec(dllexport) int WINAPI CmSetCreatorTitle(const char* creator, const char* value) {
    return SetText(creator, "title", value);
}
__declspec(dllexport) int WINAPI CmSetCreatorPriority(const char* creator, DWORD priority) {
    return Set(creator, "priority", std::to_string(priority));
}
__declspec(dllexport) int WINAPI CmSetCreatorIntegrate(const char* creator, DWORD integrate) {
    return Set(creator, "integrate", std::to_string(integrate));
}

__declspec(dllexport) int WINAPI CmGetNotifierDll(int, char*, int*) { return noNotifier; }

__declspec(dllexport) int WINAPI CmSetNotifierDll(int, const char*) {
    // The conduit host starts work at HotSync, without Palm Desktop process notifications.
    return noNotifier;
}

}  // extern "C"
