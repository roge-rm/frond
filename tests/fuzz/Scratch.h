// A scratch sync folder for targets that need a profile on disk. FROND_FUZZ_SCRATCH picks the
// parent folder (default the current one); the folder is wiped every so many runs so backups
// and logs don't pile up over a long run.
#pragma once

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>

inline std::string FuzzScratch(const char* name, unsigned& runs) {
    namespace fs = std::filesystem;
    const char* parent = std::getenv("FROND_FUZZ_SCRATCH");
    const fs::path folder = fs::path(parent ? parent : ".") / (std::string(name) + "-" + std::to_string(getpid()));
    if (runs++ % 256 == 0) {
        std::error_code ignored;
        fs::remove_all(folder, ignored);
    }
    fs::create_directories(folder);
    return folder.string();
}
