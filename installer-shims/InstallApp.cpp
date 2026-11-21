#include "InstallBridge.h"

#include <vector>

using namespace frondInstaller;

namespace {

std::wstring Ini(const fs::path& ini, const wchar_t* section, const wchar_t* key) {
    std::wstring value(32768, L'\0');
    const auto size = GetPrivateProfileStringW(section, key, L"", value.data(), value.size(), ini.c_str());
    if (size >= value.size() - 1) return {};
    value.resize(size);
    return value;
}

std::wstring Trim(const std::wstring& item) {
    const auto first = item.find_first_not_of(L" \t");
    if (first == std::wstring::npos) return {};
    auto trimmed = item.substr(first, item.find_last_not_of(L" \t") - first + 1);
    if (trimmed.size() >= 2 && trimmed.front() == L'"' && trimmed.back() == L'"')
        trimmed = trimmed.substr(1, trimmed.size() - 2);
    return trimmed;
}

bool StageCabList(const fs::path& ini) {
    const auto version = Ini(ini, L"CEAppManager", L"Version");
    const auto component = Ini(ini, L"CEAppManager", L"Component");
    if (version != L"1.0" || component.empty()) return false;
    const auto directory = Ini(ini, component.c_str(), L"InstallDir");
    const auto base = directory.empty() ? ini.parent_path() : ini.parent_path() / directory;
    const auto files = Ini(ini, component.c_str(), L"CabFiles");
    bool staged = false;
    size_t first = 0;
    while (first < files.size()) {
        const auto comma = files.find(L',', first);
        const auto file = Trim(files.substr(first, comma - first));
        if (!file.empty() && Stage(base / file)) staged = true;
        if (comma == std::wstring::npos) break;
        first = comma + 1;
    }
    return staged;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (UserDirectory().empty()) return ERROR_PATH_NOT_FOUND;
    if (argc == 1 && !WindowsMobile()) return 0;  // The desktop collects User/Install on exit.
    bool staged = false;
    for (int i = 1; i < argc; ++i) {
        const fs::path file = fs::absolute(argv[i]);
        if (WindowsMobile() && !_wcsicmp(file.extension().c_str(), L".ini")) {
            if (StageCabList(file)) staged = true;
        } else if (Stage(file)) {
            staged = true;
        }
    }
    return staged ? 0 : ERROR_FILE_NOT_FOUND;
}
