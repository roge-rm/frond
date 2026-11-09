// Public model inventory. This tool never opens firmware or session files.
#include <iostream>
#include <iomanip>
#include "device_profile5.h"

static const char* cohortName(DeviceCohort5 cohort) {
    switch (cohort) {
        case DeviceCohort5::txNand: return "tx-nor-nand";
        case DeviceCohort5::e2Nand: return "e2-nor-nand";
        case DeviceCohort5::patchedE2: return "patched-e2";
        case DeviceCohort5::paravirtual: return "paravirtual";
        case DeviceCohort5::zire72Nor: return "zire72-nor";
        case DeviceCohort5::tg50Nor: return "tg50-nor";
        case DeviceCohort5::reserved: return "reserved";
        case DeviceCohort5::ez68k: return "ez68k-rom";
        case DeviceCohort5::vz68k: return "vz68k-rom";
    }
    return "unknown";
}

int main() {
    std::cout << "{\"validation\":\"geometry only; boot compatibility requires separate evidence\",\"profiles\":[";
    bool first = true;
    for (const auto& p : deviceProfiles5) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"name\":\"" << p.name << "\",\"display_name\":\"" << p.displayName
                  << "\",\"cohort\":\"" << cohortName(p.cohort)
                  << "\",\"status\":\""
                  << (p.support == DeviceSupport5::txDesktop ? "tx-desktop"
                      : p.support == DeviceSupport5::e2Desktop ? "e2-desktop"
                      : p.support == DeviceSupport5::zire72Desktop ? "zire72-desktop"
                      : p.support == DeviceSupport5::experimentalDesktop ? "experimental-desktop"
                      : p.support == DeviceSupport5::staged ? "staged"
                      : p.support == DeviceSupport5::reserved ? "reserved" : "inherited-unverified")
                  << "\",\"nor_bytes\":";
        if (p.norSize) std::cout << p.norSize; else std::cout << "null";
        std::cout << ",\"raw_nand_bytes\":" << p.nandSize
                  << ",\"ram_min_bytes\":" << p.minRam << ",\"ram_max_bytes\":" << p.maxRam
                  << ",\"default_display_mode\":" << int(p.defaultDisplay)
                  << ",\"display_mode_mask\":" << p.displayModes
                  << ",\"explicit_raw_nand\":" << (p.requiresExplicitNand ? "true" : "false");
        std::cout << ",\"runner_eligible\":" << (deviceProfileRunnerEligible5(p.deviceType) ? "true" : "false")
                  << ",\"runs_68k\":" << (deviceProfileRuns68k5(p.deviceType) ? "true" : "false")
                  << ",\"hot_sync_button\":" << (deviceProfileHasHotSyncButton5(p.deviceType) ? "true" : "false")
                  << ",\"cpu_hz\":" << p.cpuHz
                  << ",\"sound\":" << (p.capabilities.sound ? "true" : "false")
                  << ",\"storage_slot\":" << std::quoted(deviceProfileStorageSlot5(p) == DeviceStorageSlot5::memoryStick ? "memory-stick"
                        : deviceProfileStorageSlot5(p) == DeviceStorageSlot5::sd ? "sd" : "none");
        std::cout << ",\"storage_slots\":[";
        DeviceCardSlotInfo5 slot{};
        for (unsigned n=0; deviceProfileCardSlot5(p,n,slot); ++n) {
            if (n) std::cout << ',';
            std::cout << "{\"id\":" << int(slot.id) << ",\"label\":" << slot.label
                      << ",\"kind\":" << std::quoted(slot.kind == DeviceStorageSlot5::sd ? "sd" : "memory-stick")
                      ;
            if (slot.kind == DeviceStorageSlot5::memoryStick) {
                std::cout << ",\"legacy_size_fields\":[\"ms_sizes_mb\",\"ms_pro_max_mb\"]}";
                continue;
            }
            std::cout << ",\"media_families\":[";
            bool firstFamily = true;
            for (auto family : {DeviceCardFamily5::sdsc, DeviceCardFamily5::legacyMmc}) {
                DeviceCardRule5 rule;
                if (!deviceProfileCardRule5(p,slot.id,family,rule)) continue;
                if (!firstFamily) std::cout << ',';
                firstFamily = false;
                std::cout << "{\"family\":" << std::quoted(family == DeviceCardFamily5::sdsc ? "sdsc" : "legacy-mmc-spi")
                          << ",\"csd_format\":" << std::quoted(family == DeviceCardFamily5::sdsc ? "sd-v1" : "mmc-1.2-spec-3.3")
                          << ",\"min_bytes\":" << rule.minBytes << ",\"max_bytes\":" << rule.maxBytes
                          << ",\"block_bytes\":512,\"size_rule\":"
                          << std::quoted(rule.legacyCsd ? "legacy-csd-fixed-512" : "legacy-profile-aligned-512")
                          << ",\"runtime_sizes_bytes\":[";
                for (unsigned i=0;i<rule.sizeCount;++i) std::cout << (i ? "," : "") << rule.sizes[i];
                std::cout << "],\"guest_qualified_sizes_bytes\":[";
                for (unsigned i=0;i<rule.guestSizeCount;++i) std::cout << (i ? "," : "") << rule.guestSizes[i];
                std::cout << "]}";
            }
            std::cout << "]}";
        }
        std::cout << ']';
        const auto& f = p.frontend;
        std::cout << ",\"device_type\":" << int(p.deviceType)
                  << ",\"detect\":" << int(p.detect)
                  << ",\"company_id\":" << p.companyId << ",\"hal_id\":" << p.halId
                  << ",\"nor_layout\":" << int(p.norLayout)
                  << ",\"header_offset\":" << p.headerOffset << ",\"big_card_offset\":" << p.bigCardOffset
                  << ",\"sd_min_bytes\":" << p.sdMin << ",\"sd_max_bytes\":" << p.sdMax
                  << ",\"ms_sizes_mb\":[";
        for (uint32_t mb = 1, first = 1; mb <= 128; mb <<= 1)
            if (p.msSizes & mb) std::cout << (first ? "" : ",") << mb, first = 0;
        std::cout << "]" << ",\"ms_pro_max_mb\":" << p.msProMaxMib
                  << ",\"available\":" << (f.available ? "true" : "false")
                  << ",\"setup\":" << std::quoted(f.setup)
                  << ",\"launcher\":" << std::quoted(f.launcher)
                  << ",\"width\":" << f.width << ",\"height\":" << f.height
                  << ",\"silkscreen_height\":" << f.silkscreenHeight << ",\"strip_height\":" << f.stripHeight
                  << ",\"app_names\":" << std::quoted(f.appNames) << ",\"buttons\":" << std::quoted(f.buttons)
                  << ",\"memory_stick_supported\":" << (f.memoryStick ? "true" : "false")
                  << ",\"hold_present\":" << (f.holdPresent ? "true" : "false")
                  << ",\"hold_supported\":" << (f.holdSupported ? "true" : "false")
                  << ",\"side_tab_width\":" << f.sideTabWidth
                  << ",\"home_key\":" << f.homeKey
                  << ",\"home_x\":" << f.homeX << ",\"home_y\":" << f.homeY
                  << ",\"menu_x\":" << f.menuX << ",\"menu_y\":" << f.menuY
                  << ",\"rotating_status_bar\":" << (f.rotatingStatusBar ? "true" : "false")
                  << ",\"sd_slot\":" << (f.sdSlot ? "true" : "false")
                  << ",\"camera\":" << (f.camera ? "true" : "false")
                  << ",\"fast_audio_md5\":" << std::quoted(f.fastAudioNorMd5)
                  << ",\"network\":" << (p.capabilities.network ? "true" : "false")
                  << ",\"microphone\":" << (p.capabilities.microphone ? "true" : "false")
                  << ",\"serial\":" << (p.capabilities.serial ? "true" : "false")
                  << ",\"keyboard\":" << (p.capabilities.keyboard ? "true" : "false")
                  << ",\"usb\":" << (p.capabilities.usb ? "true" : "false")
                  << ",\"infrared\":" << (p.capabilities.infrared ? "true" : "false")
                  << ",\"bluetooth\":" << (p.capabilities.bluetooth ? "true" : "false")
                  << ",\"modem\":" << (p.capabilities.modem ? "true" : "false")
                  << ",\"ir_phone\":" << (p.capabilities.irPhone ? "true" : "false")
                  << ",\"usb_printer\":" << (p.capabilities.usbPrinter ? "true" : "false")
                  << ",\"usb_wifi\":" << (p.capabilities.usbWifi ? "true" : "false")
                  << ",\"built_in_wifi\":" << (p.capabilities.builtInWifi ? "true" : "false")
                  << ",\"firmware_variants\":[";
        bool firstVariant = true;
        for (const auto& v : deviceFirmwareVariants5) {
            if (v.deviceType != p.deviceType) continue;
            if (!firstVariant) std::cout << ',';
            firstVariant = false;
            std::cout << "{\"id\":" << std::quoted(v.id)
                      << ",\"nor_bytes\":" << v.norSize << ",\"header_offset\":" << v.headerOffset
                      << ",\"card_name\":" << std::quoted(v.cardName)
                      << ",\"manufacturer\":" << std::quoted(v.manufacturer)
                      << ",\"company_id\":" << v.companyId << ",\"hal_id\":" << v.halId
                      << ",\"card_ends_at_eof\":" << (v.cardEndsAtEof ? "true" : "false")
                      << ",\"fast_audio_md5\":" << std::quoted(v.fastAudioNorMd5)
                      << ",\"power_supported\":" << (v.disableSleep ? "false" : "true") << '}';
        }
        std::cout << "]}";
    }
    std::cout << "]}\n";
}
