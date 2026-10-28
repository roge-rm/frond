// Public model inventory. This tool never opens firmware or session files.
#include <iostream>
#include "device_profile5.h"

static const char* cohortName(DeviceCohort5 cohort) {
    switch (cohort) {
        case DeviceCohort5::txNand: return "tx-nor-nand";
        case DeviceCohort5::e2Nand: return "e2-nor-nand";
        case DeviceCohort5::patchedE2: return "patched-e2";
        case DeviceCohort5::paravirtual: return "paravirtual";
    }
    return "unknown";
}

int main() {
    std::cout << "{\"validation\":\"geometry only; boot compatibility requires separate evidence\",\"profiles\":[";
    bool first = true;
    for (const auto& p : deviceProfiles5) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"id\":\"" << p.id << "\",\"session_device_id\":" << int(p.deviceType)
                  << ",\"name\":\"" << p.name << "\",\"cohort\":\"" << cohortName(p.cohort)
                  << "\",\"status\":\""
                  << (p.support == DeviceSupport5::txDesktop ? "tx-desktop"
                      : p.support == DeviceSupport5::e2Desktop ? "e2-desktop" : "inherited-unverified")
                  << "\",\"nor_bytes\":";
        if (p.norSize) std::cout << p.norSize; else std::cout << "null";
        std::cout << ",\"raw_nand_bytes\":" << p.nandSize
                  << ",\"ram_min_bytes\":" << p.minRam << ",\"ram_max_bytes\":" << p.maxRam
                  << ",\"default_display_mode\":" << int(p.defaultDisplay)
                  << ",\"display_mode_mask\":" << p.displayModes
                  << ",\"explicit_raw_nand\":" << (p.requiresExplicitNand ? "true" : "false") << '}';
    }
    std::cout << "]}\n";
}
