// Installer files unpacked for the install queue: PRC/PDB/CAB payloads inside setup programs,
// ZIPs and the like.
#include <fuzzer/FuzzedDataProvider.h>

#include "sync/InstallerFiles.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    using namespace frondSync;
    static const char* const names[] = {"setup.exe", "app.zip", "app.cab", "app.prc", "app.pdb", "app.msi", "app.sit", "x"};
    FuzzedDataProvider data(bytes, size);
    const std::string name = names[data.ConsumeIntegralInRange(0, 7)];
    const bool nested = data.ConsumeBool();
    const auto file = data.ConsumeRemainingBytes<uint8_t>();
    std::vector<InstallerFile> files;
    std::string error;
    UnpackInstaller(name, file, files, error, nested);
    for (const auto& f : files) IsInstallPayload(f);
    return 0;
}
