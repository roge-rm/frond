#ifndef FROND_STORAGE_IO_H
#define FROND_STORAGE_IO_H

#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// File helpers for the storage library: whole reads and writes at an offset, folder fsync, temps.
namespace frondStorage {

inline bool writeAllAt(int fd, const void* data, size_t n, uint64_t offset) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (n) {
        const auto w = pwrite(fd, p, n, off_t(offset));
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= size_t(w);
        offset += uint64_t(w);
    }
    return true;
}

inline bool readAllAt(int fd, void* data, size_t n, uint64_t offset) {
    auto* p = static_cast<uint8_t*>(data);
    while (n) {
        const auto r = pread(fd, p, n, off_t(offset));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r;
        n -= size_t(r);
        offset += uint64_t(r);
    }
    return true;
}

inline bool syncDir(const std::filesystem::path& dir) {
    const int fd = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const int rc = fsync(fd);
    ::close(fd);
    return rc == 0;
}

inline std::filesystem::path folderOf(const std::filesystem::path& file) {
    auto dir = file.parent_path();
    return dir.empty() ? std::filesystem::path(".") : dir;
}

inline bool createTemp(const std::filesystem::path& dir, const char* prefix, std::filesystem::path& out) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return false;
    std::string tmpl = (dir / prefix).string();
    tmpl += "XXXXXX";
    std::vector<char> name(tmpl.begin(), tmpl.end());
    name.push_back(0);
    const int fd = mkstemp(name.data());
    if (fd < 0) return false;
    close(fd);
    out = name.data();
    return true;
}

inline void removePath(const std::filesystem::path& p) {
    std::error_code ec;
    if (!p.empty()) std::filesystem::remove(p, ec);
}

inline int64_t sizeOf(int fd) {
    struct stat st {};
    return fstat(fd, &st) == 0 ? int64_t(st.st_size) : -1;
}

// An open file description lock on the whole file, as the apps hold on a card open in Files.
// It conflicts with any other descriptor's, in this process or another.
inline bool tryLock(int fd) {
    struct flock l {};
    l.l_type = F_WRLCK;
    l.l_whence = SEEK_SET;
    return fcntl(fd, F_OFD_SETLK, &l) == 0;
}

// Closes on scope exit.
struct Fd {
    int fd{-1};
    explicit Fd(int f) : fd(f) {}
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() {
        if (fd >= 0) ::close(fd);
    }
};

}  // namespace frondStorage

#endif
