#include "device_configuration.h"

bool deviceConfigurationSupportsRamSize(DeviceType5 deviceType, uint32_t ramSize) {
    return deviceProfileSupportsRam5(deviceType, ramSize);
}

uint32_t deviceConfigurationGetNandSize(DeviceType5 deviceType) {
    const auto* profile = deviceProfile5(deviceType);
    return profile ? profile->nandSize : 0;
}

bool deviceConfigurationDeviceSupportsDisplayMode(DeviceType5 deviceType, DisplayMode mode) {
    return deviceProfileSupportsDisplay5(deviceType, int32_t(mode));
}

DisplayMode deviceConfigurationDefaultDisplayMode(DeviceType5 deviceType) {
    const auto* profile = deviceProfile5(deviceType);
    return profile ? profile->defaultDisplay : DisplayMode::mode_320x320;
}

DisplayConfiguration deviceConfigurationDisplayConfigForMode(DisplayMode mode) {
    switch (mode) {
        case DisplayMode::mode_160x160:
            return {.width = 160, .height = 160, .graffitiHeight = 60, .density = 72};

        case DisplayMode::mode_160x220:
            return {.width = 160, .height = 220, .graffitiHeight = 0, .density = 72};

        case DisplayMode::mode_240x240:
            return {.width = 240, .height = 240, .graffitiHeight = 90, .density = 108};

        case DisplayMode::mode_240x320:
            return {.width = 240, .height = 320, .graffitiHeight = 0, .density = 108};

        case DisplayMode::mode_320x320:
            return {.width = 320, .height = 320, .graffitiHeight = 120, .density = 144};

        case DisplayMode::mode_320x480:
            return {.width = 320, .height = 480, .graffitiHeight = 0, .density = 144};

        case DisplayMode::mode_480x320:
            return {.width = 480, .height = 320, .graffitiHeight = 0, .density = 144};

        case DisplayMode::mode_480x480:
            return {.width = 480, .height = 480, .graffitiHeight = 180, .density = 216};

        case DisplayMode::mode_480x720:
            return {.width = 480, .height = 720, .graffitiHeight = 0, .density = 216};

        case DisplayMode::mode_560x160:
            return {.width = 560, .height = 160, .graffitiHeight = 0, .density = 72};

        default:
            return DisplayConfiguration{};
    }
}

DisplayConfiguration deviceConfigurationDisplayConfigForDevice(DeviceType5 type, DisplayMode mode) {
    auto config = deviceConfigurationDisplayConfigForMode(mode);
    const auto *profile = deviceProfile5(type);
    if (profile && profile->frontend.width > 0 && profile->frontend.width == config.width &&
        profile->frontend.height == config.height)
        config.graffitiHeight = profile->frontend.silkscreenHeight;
    return config;
}
