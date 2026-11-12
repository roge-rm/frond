#ifndef FROND_UNIX_LISTENER_H
#define FROND_UNIX_LISTENER_H

// A listening Unix stream socket for the host serial and infrared endpoints. The socket's folder
// must already exist, be owned by the user and have mode 0700, so no other user can connect.

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace frondUnix {

// Opens a non-blocking listener (backlog 1) at path and returns its descriptor, or -1 with the
// reason in error. A leftover socket file with no listener is replaced; anything else at path
// is refused. inode identifies the new socket file for Remove.
inline int Listen(const std::string& path, std::string& error, ino_t& inode) {
    inode = 0;
    sockaddr_un address{};
    if (path.empty() || path[0] != '/' || path.find('\0') != std::string::npos) {
        error = "the socket path must be absolute: " + path;
        return -1;
    }
    if (path.size() >= sizeof(address.sun_path)) {
        error = "the socket path is too long (" + std::to_string(path.size()) + " bytes, at most " +
                std::to_string(sizeof(address.sun_path) - 1) + "): " + path;
        return -1;
    }
    const size_t slash = path.rfind('/');
    if (slash == path.size() - 1) {
        error = "the socket path has no file name: " + path;
        return -1;
    }
    const std::string folder = slash == 0 ? "/" : path.substr(0, slash);
    struct stat info{};
    if (stat(folder.c_str(), &info) != 0) {
        error = "the socket folder " + folder + ": " + strerror(errno);
        return -1;
    }
    if (!S_ISDIR(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 07777) != 0700) {
        error = "the socket folder " + folder + " must be a folder owned by you with mode 0700";
        return -1;
    }
    if (lstat(path.c_str(), &info) == 0) {
        if (!S_ISSOCK(info.st_mode)) {
            error = path + " exists and is not a socket";
            return -1;
        }
        // A socket someone still listens on is not stale.
        const int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path, path.c_str(), path.size() + 1);
        const bool live = probe >= 0 && connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
        if (probe >= 0) close(probe);
        if (live) {
            error = path + " is in use";
            return -1;
        }
        if (unlink(path.c_str()) != 0 && errno != ENOENT) {
            error = "could not remove the old socket " + path + ": " + strerror(errno);
            return -1;
        }
    } else if (errno != ENOENT) {
        error = path + ": " + strerror(errno);
        return -1;
    }
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    address = {};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (fd < 0 || bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        error = "could not listen on " + path + ": " + strerror(errno);
        if (fd >= 0) close(fd);
        return -1;
    }
    if (listen(fd, 1) != 0 || lstat(path.c_str(), &info) != 0) {
        error = "could not listen on " + path + ": " + strerror(errno);
        close(fd);
        unlink(path.c_str());
        return -1;
    }
    inode = info.st_ino;
    return fd;
}

// Removes the socket file made by Listen, unless something else has replaced it since.
inline void Remove(const std::string& path, ino_t inode) {
    struct stat info{};
    if (inode && lstat(path.c_str(), &info) == 0 && S_ISSOCK(info.st_mode) && info.st_ino == inode)
        unlink(path.c_str());
}

}  // namespace frondUnix
#endif
