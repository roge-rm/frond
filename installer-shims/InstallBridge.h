#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>

#include <filesystem>
#include <string>

namespace frondInstaller {

namespace fs = std::filesystem;

inline std::wstring Environment(const wchar_t* key) {
    const auto size = GetEnvironmentVariableW(key, nullptr, 0);
    if (!size || size > 32768) return {};
    std::wstring value(size, L'\0');
    const auto copied = GetEnvironmentVariableW(key, value.data(), size);
    if (!copied || copied >= size) return {};
    value.resize(copied);
    return value;
}

inline fs::path UserDirectory() {
    const auto root = Environment(L"FROND_INSTALL_STAGE");
    return root.empty() ? fs::path() : fs::path(root) / L"User";
}

inline bool WindowsMobile() { return Environment(L"FROND_INSTALL_KIND") == L"winmo"; }

inline bool InstallFormat(const fs::path& file) {
    auto extension = file.extension().wstring();
    for (auto& c : extension)
        if (c >= L'A' && c <= L'Z') c += L'a' - L'A';
    const bool cab = extension == L".cab";
    return WindowsMobile() ? cab : extension == L".prc" || extension == L".pdb" || extension == L".pqa";
}

inline bool Stage(const fs::path& file) {
    const auto user = UserDirectory();
    if (user.empty() || !InstallFormat(file)) return false;
    const auto attr = GetFileAttributesW(file.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return false;
    return CopyFileW(file.c_str(), (user / L"Received" / file.filename()).c_str(), TRUE) != 0;
}

}  // namespace frondInstaller
