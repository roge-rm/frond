#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "Dlp.h"

namespace frondSync {

using ConduitLog = std::function<void(const std::string&)>;
using ConduitMark = std::function<bool(uint32_t creator)>;
using ConduitAsset = std::function<bool(const std::string& name, uint8_t card, std::string& error)>;

bool RunPalmConduits(DlpClient& dlp, const std::string& profile, const std::string& deviceKey,
                     const UserInfo& user, const std::atomic<bool>* stop,
                     const ConduitMark& wasFast, const ConduitAsset& saveRecovery, const ConduitLog& log,
                     std::string& error);

}  // namespace frondSync
