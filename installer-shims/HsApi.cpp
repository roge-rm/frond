#include "PalmHotSync.h"

extern "C" __declspec(dllexport) LONG WINAPI HsGetSyncStatus(DWORD* syncing) {
    constexpr LONG noManager = 0x10000001;
    constexpr LONG invalidParameter = 0x1000000a;
    if (!syncing) return invalidParameter;
    const auto window = frondInstaller::FindPalmHotSyncWindow();
    DWORD_PTR result{};
    if (!window || !SendMessageTimeoutW(window, frondInstaller::palmHotSyncStatusMessage,
                                        frondInstaller::palmHotSyncSyncing, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result))
        return noManager;
    *syncing = result != 0;
    return 0;
}
