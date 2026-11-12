#ifndef FROND_DESKTOP_CHANNEL_H
#define FROND_DESKTOP_CHANNEL_H

// The desktop control socket's framing, shared by the ARM and 68k runners. The launcher listens on
// a private Unix socket and the core connects to it.
// Requests: id<TAB>command<TAB>hex UTF-8 argument. Replies: id<TAB>ok/error<TAB>code<TAB>hex text.
// Events from the core: 0<TAB>event<TAB>name<TAB>hex text. Id 0 requests from the launcher
// (cancel-files, panel-visible) get no reply.
// Not thread safe: only the emulation thread uses it.

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Locks <session>.core for the rest of the run: the launcher leaves the session alone while it is
// held, as when this core saves it after the launcher died. The kernel frees it when the core exits.
// False with a message when another core holds it or the file can't be made.
inline bool LockSessionForCore(const std::string& session, std::string& error) {
    const auto path = session + ".core";
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) {
        error = "cannot create " + path + ": " + std::strerror(errno);
        return false;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int code = errno;
        close(fd);
        error = code == EWOULDBLOCK ? "another core is running " + session
                                    : "cannot lock " + path + ": " + std::strerror(code);
        return false;
    }
    return true;  // fd stays open until exit
}

class DesktopChannel {
 public:
    struct Request { std::string id, command, argument; };
    // closed: the launcher went or broke the protocol, stop the core. handled: an id 0 request
    // was taken. request: one to dispatch and Reply to.
    enum class Poll { idle, handled, request, closed };
    enum class Cancel { none, cancelled, gone };

    explicit DesktopChannel(const std::string& path) {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path.size() >= sizeof(address.sun_path)) throw std::runtime_error("desktop socket path too long");
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) throw std::runtime_error("cannot create desktop socket");
        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
            fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
            close(fd);
            fd = -1;
            throw std::runtime_error("cannot connect to desktop launcher");
        }
        Event("ready");
    }
    ~DesktopChannel() { if (fd >= 0) close(fd); }
    DesktopChannel(const DesktopChannel&) = delete;
    DesktopChannel& operator=(const DesktopChannel&) = delete;

    void Event(const char* name, const std::string& message = {}) {
        output += std::string("0\tevent\t") + name + "\t" + Encode(message) + "\n";
    }
    void Reply(const std::string& id, bool ok, const std::string& code, const std::string& message) {
        output += id + (ok ? "\tok\t" : "\terror\t") + code + "\t" + Encode(message) + "\n";
    }

    // Reads what has arrived and takes at most one line. A new request is only taken once the
    // last reply is sent, so a large reply stays the only thing queued and a slow reader can't
    // grow the output without limit.
    Poll Receive(Request& request) {
        char data[4096];
        const auto n = recv(fd, data, sizeof(data), 0);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            panelVisible = false;
            lost = true;
            return Poll::closed;
        }
        if (n > 0) input.append(data, n);
        if (input.size() > 32768 || !Flush()) return Poll::closed;
        const auto end = output.empty() ? input.find('\n') : std::string::npos;
        if (end == std::string::npos) return Poll::idle;
        const auto line = input.substr(0, end);
        input.erase(0, end + 1);
        const auto first = line.find('\t'), second = line.find('\t', first == std::string::npos ? 0 : first + 1);
        if (first == std::string::npos || second == std::string::npos) return Poll::closed;
        const auto id = line.substr(0, first), command = line.substr(first + 1, second - first - 1);
        if (id == "0" && command == "cancel-files") return Poll::handled;
        if (id == "0" && command == "panel-visible") {
            std::string visible;
            if (!Decode(line.substr(second + 1), visible) || (visible != "on" && visible != "off")) return Poll::closed;
            panelVisible = visible == "on";
            return Poll::handled;
        }
        if (id.empty() || id.size() > 20 || id == "0" || id.find_first_not_of("0123456789") != std::string::npos)
            return Poll::closed;
        std::string argument;
        if (!Decode(line.substr(second + 1), argument)) return Poll::closed;
        request = {id, command, std::move(argument)};
        return Poll::request;
    }

    // Sends what the socket takes now; false when the client has gone.
    bool Flush() {
        if (output.empty()) return true;
        const auto sent = send(fd, output.data(), output.size(), MSG_NOSIGNAL);
        if (sent > 0) output.erase(0, sent);
        const bool ok = sent >= 0 || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        if (!ok) lost = true;
        return ok;
    }
    bool OutputEmpty() const { return output.empty(); }
    // The launcher closed its end or went away, rather than breaking the protocol.
    bool Lost() const { return lost; }
    // A quit request that arrived but wasn't taken before the launcher went.
    bool QuitPending() const {
        size_t start = 0;
        for (auto end = input.find('\n'); end != std::string::npos; start = end + 1, end = input.find('\n', start)) {
            const auto first = input.find('\t', start);
            if (first < end && first + 6 <= end && input.compare(first + 1, 5, "quit\t") == 0) return true;
        }
        return false;
    }
    bool PanelVisible() const { return panelVisible; }

    // Looks for a cancel-files for this job among the waiting lines, during a long job. Gone when
    // the launcher closed the socket.
    Cancel CheckCancel(const std::string& job) {
        char bytes[4096];
        while (true) {
            const auto received = recv(fd, bytes, sizeof(bytes), MSG_DONTWAIT);
            if (received == 0) {
                lost = true;
                return Cancel::gone;
            }
            if (received < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                lost = true;
                return Cancel::gone;
            }
            input.append(bytes, received);
            if (input.size() > 32768) throw std::runtime_error("Control input exceeds its bounded queue");
        }
        const std::string matching = "0\tcancel-files\t" + Encode(job);
        bool cancelled = false;
        size_t start = 0;
        while (true) {
            const auto end = input.find('\n', start);
            if (end == std::string::npos) break;
            const auto line = input.substr(start, end - start);
            if (line == matching) {
                cancelled = true;
                input.erase(start, end - start + 1);
            } else start = end + 1;
        }
        return cancelled ? Cancel::cancelled : Cancel::none;
    }

    static std::string Encode(const std::string& value) {
        static const char digits[] = "0123456789abcdef";
        std::string out;
        for (unsigned char c : value) { out += digits[c >> 4]; out += digits[c & 15]; }
        return out;
    }
    // Lowercase hex only, and no NUL bytes.
    static bool Decode(const std::string& value, std::string& out) {
        if (value.size() % 2) return false;
        auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
        for (size_t i = 0; i < value.size(); i += 2) {
            const int a = digit(value[i]), b = digit(value[i + 1]);
            if (a < 0 || b < 0 || !(a || b)) return false;
            out += char(a * 16 + b);
        }
        return true;
    }

 private:
    int fd{-1};
    bool panelVisible{false}, lost{false};
    std::string input, output;
};

#endif
