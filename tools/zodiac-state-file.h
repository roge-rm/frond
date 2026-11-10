#ifndef FROND_ZODIAC_STATE_FILE_H
#define FROND_ZODIAC_STATE_FILE_H

// Zodiac machine state files for the boot probe and the private driver. Link with -lz.
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

#include "imx1_boot_probe.h"

namespace zodiac_state {
// The build the state came from, kept in the file. A load from another build is only flagged.
inline const char* buildNote() {
#ifdef FROND_STATE_BUILD_NOTE
    return FROND_STATE_BUILD_NOTE;
#else
    return "unknown build";
#endif
}

// A state is about 138 MiB with 128 MiB of RAM.
inline constexpr size_t maximumBytes = size_t(1) << 30;

// Copies from to a new file at to. Fails with EEXIST when to is already there.
inline bool copyExclusive(const std::string& from, const std::string& to) {
    const int in = open(from.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return false;
    const int out = open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (out < 0) { const int error = errno; close(in); errno = error; return false; }
    std::vector<char> buffer(size_t(1) << 20);
    int error = 0;
    while (!error) {
        const ssize_t got = read(in, buffer.data(), buffer.size());
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) error = errno;
        if (got <= 0) break;
        for (ssize_t at = 0; !error && at < got;) {
            const ssize_t put = write(out, buffer.data() + at, size_t(got - at));
            if (put < 0 && errno != EINTR) error = errno;
            if (put > 0) at += put;
        }
    }
    if (!error && fsync(out) && errno != EINVAL && errno != EROFS) error = errno;
    if (close(out) && !error) error = errno;
    close(in);
    // Only this call made the file, so a half copy is its own to remove.
    if (error) unlink(to.c_str());
    errno = error;
    return !error;
}

// Writes gzip level 1 to a file beside the target, then moves it into place, so a reader never
// sees half a state. An existing file is kept unless overwrite is set. Where the filesystem has no
// hard links the file is copied into a new one instead, which a reader can see while it grows.
// The file beside the target is gone when this returns.
// Returns an empty string, or what went wrong.
inline std::string writeFile(const std::string& path, const std::vector<uint8_t>& state, bool overwrite = false) {
    if (path.empty()) return "no state file name";
    if (!overwrite && !access(path.c_str(), F_OK)) return path + " exists";
    const std::string temporary = path + ".tmp." + std::to_string(long(getpid()));
    const gzFile file = gzopen(temporary.c_str(), "wb1");
    if (!file) return "cannot create " + temporary + ": " + std::strerror(errno);
    bool written = true;
    for (size_t at = 0; written && at < state.size();) {
        const unsigned chunk = unsigned(std::min<size_t>(state.size() - at, size_t(1) << 26));
        written = gzwrite(file, state.data() + at, chunk) == int(chunk);
        at += chunk;
    }
    if (gzclose(file) != Z_OK || !written) {
        unlink(temporary.c_str());
        return "cannot write " + temporary;
    }
    bool moved;
    if (overwrite) moved = !std::rename(temporary.c_str(), path.c_str());
    else {
        // link refuses an existing target where rename would replace it.
        moved = !link(temporary.c_str(), path.c_str());
#ifdef FROND_STATE_FILE_NO_LINK
        if (moved) { unlink(path.c_str()); moved = false; errno = EPERM; }
#endif
        if (!moved && errno != EEXIST) moved = copyExclusive(temporary, path);
    }
    const std::string reason = moved ? "" : path + (errno == EEXIST ? " exists" : std::string(": ") + std::strerror(errno));
    // After a rename there's nothing left under this name.
    if (!moved || !overwrite) unlink(temporary.c_str());
    return reason;
}

// Reads a state written by writeFile. Returns an empty string, or what went wrong.
inline std::string readFile(const std::string& path, std::vector<uint8_t>& state) {
    state.clear();
    const gzFile file = gzopen(path.c_str(), "rb");
    if (!file) return "cannot open " + path + ": " + std::strerror(errno);
    std::string reason;
    for (;;) {
        const size_t at = state.size(), chunk = size_t(1) << 24;
        if (at >= maximumBytes) { reason = path + " is too large"; break; }
        state.resize(at + chunk);
        const int got = gzread(file, state.data() + at, unsigned(chunk));
        state.resize(at + size_t(got > 0 ? got : 0));
        if (got < 0) { reason = "cannot read " + path; break; }
        if (size_t(got) < chunk) break;
    }
    // A cut-off gzip stream shows up here.
    int error = Z_OK;
    gzerror(file, &error);
    if (reason.empty() && error != Z_OK && error != Z_STREAM_END) reason = path + " is damaged";
    if (gzclose(file) != Z_OK && reason.empty()) reason = path + " is damaged";
    if (!reason.empty()) state.clear();
    return reason;
}

// What a check or a load said, for a log line.
inline std::string describe(const Imx1StateInfo& info) {
    std::string text = imx1StateStatusName(info.status);
    if (info.section[0]) text += std::string(" section=") + info.section;
    if (info.detail) text += std::string(" detail=") + info.detail;
    return text;
}

// Saves the machine to path. Returns an empty string, or what went wrong.
inline std::string save(Imx1Machine* machine, const std::string& path, bool overwrite = false) {
    if (!overwrite && !access(path.c_str(), F_OK)) return path + " exists";
    std::vector<uint8_t> state;
    const auto status = imx1MachineSaveState(machine, state, buildNote());
    if (status != Imx1StateStatus::ok) return std::string("save refused: ") + imx1StateStatusName(status);
    return writeFile(path, state, overwrite);
}

// Loads path into the machine, fresh or already run. A refused state leaves the machine as it was.
// flags are the imx1State* load flags. Returns an empty string, or what went wrong; info says
// whether the engine, the core revision or the build differ.
inline std::string load(Imx1Machine* machine, const std::string& path, uint32_t flags = 0, Imx1StateInfo* info = nullptr) {
    std::vector<uint8_t> state;
    const auto reason = readFile(path, state);
    if (!reason.empty()) return reason;
    const auto loaded = imx1MachineLoadState(machine, state.data(), state.size(), flags, buildNote());
    if (info) *info = loaded;
    return loaded.status == Imx1StateStatus::ok ? "" : "load refused: " + describe(loaded);
}
}  // namespace zodiac_state

#endif
