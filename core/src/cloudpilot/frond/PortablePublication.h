#pragma once
#include <filesystem>
#include <fcntl.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <unistd.h>

enum class PortablePublication { refused, publishedUncertain, durable };

// The caller owns staging cleanup. Once renamed, the complete output stays in place.
template <typename Sync>
PortablePublication PublishPortable(const std::filesystem::path& staging,
                                    const std::filesystem::path& destination, Sync sync) {
    const auto parent = std::filesystem::absolute(destination).parent_path();
    if (!sync(staging)) return PortablePublication::refused;
#if defined(__APPLE__)
    if (renamex_np(staging.c_str(), destination.c_str(), RENAME_EXCL) != 0) return PortablePublication::refused;
#else
    if (syscall(SYS_renameat2, AT_FDCWD, staging.c_str(), AT_FDCWD,
                destination.c_str(), 1) != 0) return PortablePublication::refused;
#endif
    try {
        if (!sync(parent)) return PortablePublication::publishedUncertain;
    } catch (...) { return PortablePublication::publishedUncertain; }
    return PortablePublication::durable;
}
