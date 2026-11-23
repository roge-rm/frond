#include "PalmConduitAbi.h"
#include "PalmConduitProtocol.h"

#include <cstdio>
#include <cstring>
#include <io.h>
#include <filesystem>

namespace {

using namespace frondConduit;

long ProgressMessage(char* message) {
    const auto runtime = GetModuleHandleW(L"sync20.dll");
    const auto log = runtime ? GetProcAddress(runtime, "FrondConduitLog") : nullptr;
    if (!log) return disconnected;
    return reinterpret_cast<long (*)(const char*)>(log)(message);
}

struct Channel {
    HANDLE handle{};
    explicit Channel(HANDLE source) {
        DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    }
    ~Channel() { if (handle) CloseHandle(handle); }
};

// The DLL API predates Unicode Windows, while registration paths are on disk as UTF-8.
bool LegacyText(const std::string& text, char* out, size_t capacity) {
    if (text.empty()) return true;
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (size <= 0) return false;
    std::wstring wide(size, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), wide.data(), size))
        return false;
    BOOL replaced = FALSE;
    const auto wideCodePage = GetACP() == CP_UTF8;
    const auto converted = WideCharToMultiByte(CP_ACP, wideCodePage ? 0 : WC_NO_BEST_FIT_CHARS, wide.data(), size,
                                               out, int(capacity - 1), nullptr, wideCodePage ? nullptr : &replaced);
    return converted > 0 && !replaced;
}

bool PrepareUser(const SyncProperties& props, const std::string& userPath, uint32_t userId, const wchar_t* program) {
    const int size = MultiByteToWideChar(CP_ACP, 0, props.user, -1, nullptr, 0);
    if (size <= 1) return false;
    std::wstring name(size, L'\0');
    if (!MultiByteToWideChar(CP_ACP, 0, props.user, -1, name.data(), size)) return false;
    const auto directory = std::filesystem::u8path(userPath);
    const auto shims = std::filesystem::absolute(program).parent_path();
    if (!directory.is_absolute() || shims.empty()) return false;

    SetEnvironmentVariableW(L"FROND_INSTALL_STAGE", nullptr);
    SetEnvironmentVariableW(L"FROND_INSTALL_USER", nullptr);
    SetEnvironmentVariableW(L"FROND_INSTALL_USER_ID", nullptr);
    SetEnvironmentVariableW(L"FROND_INSTALL_KIND", nullptr);
    const auto id = std::to_wstring(userId);
    return SetEnvironmentVariableW(L"FROND_CONDUIT_USER_NAME", name.c_str()) &&
           SetEnvironmentVariableW(L"FROND_CONDUIT_USER_ID", id.c_str()) &&
           SetEnvironmentVariableW(L"FROND_CONDUIT_USER_PATH", directory.c_str()) &&
           SetEnvironmentVariableW(L"FROND_CONDUIT_SHIMS", shims.c_str());
}

bool Receive(HANDLE input, void* buffer, size_t length) {
    auto* bytes = static_cast<uint8_t*>(buffer);
    while (length) {
        DWORD read = 0;
        if (!ReadFile(input, bytes, DWORD(length), &read, nullptr) || !read) return false;
        bytes += read;
        length -= read;
    }
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return ERROR_INVALID_PARAMETER;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
    Bytes header;
    if (!ReadFrame([&](void* buffer, size_t length) { return Receive(input, buffer, length); }, header))
        return ERROR_INVALID_DATA;

    SyncProperties props{};
    Reader init{header};
    if (init.Get() != version) return ERROR_INVALID_DATA;
    props.type = init.Get();
    props.creator = init.Get();
    const auto userId = init.Get();
    props.firstDevice = init.Get();
    props.connection = init.Get();
    const auto conduitPath = init.Text(sizeof(props.userPath) - 1);
    const auto userPath = init.Text(sizeof(props.userPath) - 1);
    const auto localName = init.Text(sizeof(props.localName) - 1);
    const auto user = init.Text(sizeof(props.user) - 1);
    const auto remote = init.Text(31);
    const auto registry = init.Text(sizeof(props.registryPath) - 1);
    if (!init.End() || props.type > 1 || props.firstDevice > 2 || props.connection > 1 ||
        conduitPath.empty() || userPath.empty())
        return ERROR_INVALID_DATA;
    if (!LegacyText(conduitPath, props.userPath, sizeof(props.userPath)) ||
        !LegacyText(localName, props.localName, sizeof(props.localName)) ||
        !LegacyText(registry, props.registryPath, sizeof(props.registryPath)))
        return ERROR_NO_UNICODE_TRANSLATION;
    std::memcpy(props.user, user.data(), user.size());
    std::array<char, 32> remoteName{};
    if (!remote.empty()) {
        if (!LegacyText(remote, remoteName.data(), remoteName.size())) return ERROR_NO_UNICODE_TRANSLATION;
        props.remoteNames[0] = remoteName.data();
        props.remoteCount = 1;
    }
    props.registryRoot = HKEY_CURRENT_USER;
    if (!PrepareUser(props, userPath, userId, argv[0])) return ERROR_INVALID_DATA;

    // Keep a conduit's stdio separate from the request and reply pipe.
    const Channel incoming(input), outgoing(output);
    if (!incoming.handle || !outgoing.handle) return ERROR_INVALID_HANDLE;
    SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
    SetStdHandle(STD_INPUT_HANDLE, INVALID_HANDLE_VALUE);
    _dup2(_fileno(stderr), _fileno(stdout));
    _close(_fileno(stdin));
    const auto runtime = LoadLibraryExW(L"sync20.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    const auto logger = LoadLibraryExW(L"hslog20.dll", nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    if (!runtime || !logger) return ERROR_MOD_NOT_FOUND;
    const auto connect = reinterpret_cast<void (*)(HANDLE, HANDLE)>(GetProcAddress(runtime, "FrondConduitConnect"));
    const auto complete = reinterpret_cast<long (*)(long)>(GetProcAddress(runtime, "FrondConduitComplete"));
    if (!connect || !complete) return ERROR_PROC_NOT_FOUND;
    connect(incoming.handle, outgoing.handle);

    const std::filesystem::path path(argv[1]);
    if (!path.is_absolute() || _wcsicmp(path.extension().c_str(), L".dll") != 0) return ERROR_INVALID_NAME;
    const auto vendor = path.parent_path();
    // Some conduits load their own helper DLLs by name after changing directories.
    if (!SetCurrentDirectoryW(vendor.c_str()) || !SetDllDirectoryW(vendor.c_str())) {
        const auto failure = GetLastError();
        complete(HRESULT_FROM_WIN32(failure));
        return failure;
    }
    const auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        const auto failure = GetLastError();
        complete(HRESULT_FROM_WIN32(failure));
        return failure;
    }
    const auto run = reinterpret_cast<OpenConduit>(GetProcAddress(module, "OpenConduit"));
    if (!run) {
        complete(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND));
        return ERROR_PROC_NOT_FOUND;
    }
    const auto status = run(ProgressMessage, props);
    const auto sent = complete(status);
    return sent ? ERROR_BROKEN_PIPE : 0;
}
