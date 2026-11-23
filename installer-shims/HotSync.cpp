#include "InstallBridge.h"

using namespace frondInstaller;

// The Frond desktop process already owns HotSync. A vendor installer can start this
// entry point while that process is hosting an installer session.
int wmain(int argc, wchar_t**) {
    const auto stage = Environment(L"FROND_PALM_CONDUIT_STAGE");
    std::error_code error;
    return argc == 1 && !WindowsMobile() && !UserDirectory().empty() && !stage.empty() &&
                   fs::is_directory(stage, error) ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
}
