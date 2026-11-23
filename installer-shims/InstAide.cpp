#include "InstallBridge.h"

#include <cstring>
#include <cstdint>
#include <cwchar>
#include <limits>

using namespace frondInstaller;

namespace {

constexpr int noSuchUserIndex = -500;
constexpr int smallBuffer = -502;
constexpr int invalidPalmUser = -505;
constexpr int invalidPathKind = -509;
constexpr int missingPath = -510;
constexpr int noCardFile = -507;
constexpr int missingOutput = -516;
constexpr int unknownPalmId = -522;

std::wstring FromAnsi(const char* value) {
    if (!value) return {};
    const int size = MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
    if (!size) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_ACP, 0, value, -1, result.data(), size);
    result.pop_back();
    return result;
}

fs::path ConduitDirectory() { return Environment(L"FROND_CONDUIT_USER_PATH"); }

bool RunningConduit() { return !ConduitDirectory().empty(); }

std::wstring User() {
    if (RunningConduit()) return Environment(L"FROND_CONDUIT_USER_NAME");
    return UserDirectory().empty() || WindowsMobile() ? std::wstring() : Environment(L"FROND_INSTALL_USER");
}

DWORD UserId() {
    const auto text = Environment(RunningConduit() ? L"FROND_CONDUIT_USER_ID" : L"FROND_INSTALL_USER_ID");
    if (text.empty() || text.find_first_not_of(L"0123456789") != std::wstring::npos) return 0;
    const auto id = std::wcstoull(text.c_str(), nullptr, 10);
    return id <= UINT32_MAX ? DWORD(id) : 0;
}

template <typename Size>
int CopyResult(const std::wstring& value, char* output, Size* size, bool countTerminatorOnFailure = false) {
    if (!size) return missingOutput;
    if (value.empty()) return invalidPalmUser;
    const int needed = WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return invalidPalmUser;
    if (needed > (std::numeric_limits<Size>::max)()) return smallBuffer;
    if (!output || *size < needed) {
        *size = Size(countTerminatorOnFailure ? needed : needed - 1);
        return smallBuffer;
    }
    if (!WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, output, *size, nullptr, nullptr)) return invalidPalmUser;
    *size = Size(needed - 1);
    return *size;
}

}  // namespace

extern "C" {

__declspec(dllexport) int WINAPI PltGetUserCount() { return !User().empty(); }

__declspec(dllexport) int WINAPI PltGetUser(int index, char* output, short* size) {
    return index == 0 ? CopyResult(User(), output, size, true) : noSuchUserIndex;
}

__declspec(dllexport) int WINAPI PltGetUserDirectory(const char* user, char* output, int* size) {
    const auto name = User();
    if (name.empty() || FromAnsi(user) != name) return invalidPalmUser;
    const auto directory = RunningConduit() ? ConduitDirectory() : UserDirectory();
    return CopyResult(directory.wstring(), output, size);
}

__declspec(dllexport) int WINAPI PltGetPath(int kind, char* output, short* size) {
    if (kind != 2) return invalidPathKind;
    auto program = Environment(RunningConduit() ? L"FROND_CONDUIT_SHIMS" : L"FROND_INSTALL_SHIMS");
    if (program.empty() || User().empty()) return missingPath;
    return CopyResult((fs::path(program) / L"HotSync.exe").wstring(), output, size);
}

__declspec(dllexport) int WINAPI PltGetFileCount(const char* user, const char* wildcard) {
    const auto name = User();
    const auto pattern = FromAnsi(wildcard);
    if (name.empty() || FromAnsi(user) != name || pattern.empty() ||
        pattern.find_first_of(L"\\/:") != std::wstring::npos)
        return invalidPalmUser;
    if (RunningConduit()) return 0;

    WIN32_FIND_DATAW entry{};
    const auto files = FindFirstFileW((UserDirectory() / L"Received" / pattern).c_str(), &entry);
    if (files == INVALID_HANDLE_VALUE) return 0;
    int count = 0;
    do {
        if (!(entry.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
            InstallFormat(entry.cFileName))
            ++count;
    } while (FindNextFileW(files, &entry));
    FindClose(files);
    return count;
}

__declspec(dllexport) int WINAPI PlmGetUserIDFromName(const char* user, DWORD* id) {
    const auto name = User();
    const auto known = UserId();
    if (!id || !known || name.empty() || FromAnsi(user) != name) return invalidPalmUser;
    *id = known;
    return ERROR_SUCCESS;
}

__declspec(dllexport) int WINAPI PlmSlotGetFileCount(DWORD userId, const void*, int* count) {
    if (!count) return missingOutput;
    *count = 0;
    if (User().empty() || !userId || userId != UserId()) return unknownPalmId;
    // Frond queues handheld installation files in main memory.
    return ERROR_SUCCESS;
}

__declspec(dllexport) int WINAPI PlmSlotGetFileInfo(DWORD userId, const void*, int, void* fileName, int* nameLength,
                                                     void* fileInfo) {
    if (!fileName || !nameLength || !fileInfo) return missingOutput;
    return !User().empty() && userId && userId == UserId() ? noCardFile : unknownPalmId;
}

__declspec(dllexport) int WINAPI PltIsUserProfile(const char*) { return 0; }

__declspec(dllexport) int WINAPI PltRemoveInstallFile(const char* user, const char* path) {
    if (RunningConduit()) return ERROR_NOT_READY;
    const auto name = User();
    if (name.empty() || FromAnsi(user) != name) return ERROR_NOT_FOUND;
    const auto file = fs::path(FromAnsi(path)).filename();
    if (!InstallFormat(file)) return ERROR_INVALID_PARAMETER;
    const auto staged = UserDirectory() / L"Received" / file;
    return DeleteFileW(staged.c_str()) ? ERROR_SUCCESS : GetLastError();
}

__declspec(dllexport) int WINAPI PltInstallFile(const char* user, const char* path) {
    if (RunningConduit()) return ERROR_NOT_READY;
    const auto name = User();
    if (name.empty() || FromAnsi(user) != name) return ERROR_NOT_FOUND;
    const auto file = FromAnsi(path);
    return !file.empty() && Stage(file) ? 0 : ERROR_OPEN_FAILED;
}

}
