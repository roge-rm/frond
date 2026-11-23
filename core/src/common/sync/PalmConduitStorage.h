#pragma once

#include <string_view>

#include "Sha256.h"

namespace frondSync {

#ifdef _WIN32
inline constexpr int64_t PalmConduitNativeLockId = -1;
inline constexpr char PalmConduitNativeInstallMarker[] = ".palm-conduit-installing";
#endif

inline std::string PalmConduitDataKey(std::string_view deviceKey, uint32_t userId) {
    auto key = Sha256Hex(reinterpret_cast<const uint8_t*>(deviceKey.data()), deviceKey.size()).substr(0, 32);
    key += '-';
    constexpr char digits[] = "0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4) key += digits[(userId >> shift) & 15];
    return key;
}

}  // namespace frondSync
