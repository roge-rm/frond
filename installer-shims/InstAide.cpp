#include "InstallBridge.h"

#include <cstring>

using namespace frondInstaller;

namespace {

std::wstring FromAnsi(const char* value) {
    if (!value) return {};
    const int size = MultiByteToWideChar(CP_ACP, 0, value, -1, nullptr, 0);
    if (!size) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_ACP, 0, value, -1, result.data(), size);
    result.pop_back();
    return result;
}

std::wstring User() {
    return UserDirectory().empty() || WindowsMobile() ? std::wstring() : Environment(L"FROND_INSTALL_USER");
}

int CopyResult(const std::wstring& value, char* output, int* size) {
    if (value.empty() || !size) return ERROR_INVALID_PARAMETER;
    const int needed = WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return ERROR_INVALID_DATA;
    const int capacity = *size;
    *size = needed;
    if (!output || capacity < needed) return ERROR_INSUFFICIENT_BUFFER;
    return WideCharToMultiByte(CP_ACP, 0, value.c_str(), -1, output, capacity, nullptr, nullptr) ? 0 : ERROR_INVALID_DATA;
}

}  // namespace

extern "C" {

__declspec(dllexport) int WINAPI PltGetUserCount() { return !User().empty(); }

__declspec(dllexport) int WINAPI PltGetUser(int index, char* output, int* size) {
    return index == 0 ? CopyResult(User(), output, size) : ERROR_NO_MORE_ITEMS;
}

__declspec(dllexport) int WINAPI PltGetUserDirectory(const char* user, char* output, int* size) {
    const auto name = User();
    if (name.empty() || FromAnsi(user) != name) return ERROR_NOT_FOUND;
    return CopyResult(UserDirectory().wstring(), output, size);
}

__declspec(dllexport) int WINAPI PltGetPath(int kind, char* output, int* size) {
    if (kind != 2 || User().empty()) return ERROR_INVALID_PARAMETER;
    auto program = Environment(L"FROND_INSTALL_SHIMS");
    if (program.empty()) return ERROR_NOT_FOUND;
    return CopyResult((fs::path(program) / L"HotSync.exe").wstring(), output, size);
}

__declspec(dllexport) int WINAPI PltInstallFile(const char* user, const char* path) {
    const auto name = User();
    if (name.empty() || FromAnsi(user) != name) return ERROR_NOT_FOUND;
    const auto file = FromAnsi(path);
    return !file.empty() && Stage(file) ? 0 : ERROR_OPEN_FAILED;
}

}
