#pragma once

#include <windows.h>

namespace frondInstaller {

inline constexpr wchar_t palmHotSyncClass[] = L"KittyHawk";
inline constexpr wchar_t palmHotSyncTitle[] = L"HotSync Manager";
inline constexpr UINT palmHotSyncStatusMessage = 0xbec5;
inline constexpr WPARAM palmHotSyncSyncing = 2;

inline HWND FindPalmHotSyncWindow() {
    if (const auto window = FindWindowW(palmHotSyncClass, nullptr)) return window;
    return FindWindowW(nullptr, palmHotSyncTitle);
}

}  // namespace frondInstaller
