#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif !defined(__ANDROID__)
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#include <mach-o/dyld.h>
#else
extern char** environ;
#endif
#endif

namespace frondSync {

namespace fs = std::filesystem;

#if !defined(__ANDROID__)
class ConduitProcess {
   public:
    explicit ConduitProcess(const std::atomic<bool>* stop) : stop(stop) {}
    ConduitProcess(const ConduitProcess&) = delete;
    ConduitProcess& operator=(const ConduitProcess&) = delete;

    static fs::path ExecutableDirectory() {
#ifdef _WIN32
        std::wstring name(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, name.data(), DWORD(name.size()));
        if (!length || length >= name.size()) return {};
        name.resize(length);
        return fs::path(name).parent_path();
#elif defined(__APPLE__)
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string name(size, '\0');
        if (_NSGetExecutablePath(name.data(), &size)) return {};
        return fs::path(name.c_str()).parent_path();
#else
        std::error_code error;
        return fs::read_symlink("/proc/self/exe", error).parent_path();
#endif
    }

    bool Stopping() const {
        return (stop && stop->load()) || std::chrono::steady_clock::now() >= deadline;
    }

    bool Read(void* bytes, size_t length) { return Transfer(bytes, length, false); }
    bool Write(const void* bytes, size_t length) { return Transfer(const_cast<void*>(bytes), length, true); }

#ifdef _WIN32
    bool Start(const fs::path& host, const std::string& dll, const fs::path&) {
        static std::atomic<unsigned> sequence{};
        const auto pipeName = L"\\\\.\\pipe\\Frond.Conduit." + std::to_wstring(GetCurrentProcessId()) +
                              L"." + std::to_wstring(++sequence);
        pipe = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 8192, 8192, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return false;
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE childPipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, &inherit, OPEN_EXISTING, 0, nullptr);
        if (childPipe == INVALID_HANDLE_VALUE) return false;
        HANDLE errors = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                    OPEN_EXISTING, 0, nullptr);
        if (errors == INVALID_HANDLE_VALUE) {
            CloseHandle(childPipe);
            return false;
        }
        auto spawn = [&]() {
            OVERLAPPED connection{};
            if (!ConnectNamedPipe(pipe, &connection) && GetLastError() != ERROR_PIPE_CONNECTED) return false;
            job = CreateJobObjectW(nullptr, nullptr);
            if (!job) return false;
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return false;

            SIZE_T bytes = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
            std::vector<uint8_t> attributes(bytes);
            auto* list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
            if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes)) return false;
            HANDLE childHandles[] = {childPipe, errors};
            const bool configured = UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                               childHandles, sizeof(childHandles), nullptr, nullptr);
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = startup.StartupInfo.hStdOutput = childPipe;
            startup.StartupInfo.hStdError = errors;
            startup.lpAttributeList = list;
            PROCESS_INFORMATION info{};
            const auto target = fs::u8path(dll);
            auto command = L"\"" + host.wstring() + L"\" \"" + target.wstring() + L"\"";
            const bool created = configured && CreateProcessW(host.c_str(), command.data(), nullptr, nullptr, TRUE,
                CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, target.parent_path().c_str(),
                &startup.StartupInfo, &info);
            DeleteProcThreadAttributeList(list);
            if (!created) return false;
            child = info.hProcess;
            const bool ready = AssignProcessToJobObject(job, child) && ResumeThread(info.hThread) != DWORD(-1);
            if (!ready) TerminateProcess(child, 1);
            CloseHandle(info.hThread);
            return ready;
        };
        const bool ready = spawn();
        CloseHandle(errors);
        CloseHandle(childPipe);
        return ready;
    }

    bool Finished() {
        while (!Stopping() && WaitForSingleObject(child, 100) == WAIT_TIMEOUT) {}
        DWORD exitCode = 0;
        return GetExitCodeProcess(child, &exitCode) && exitCode == 0;
    }

    ~ConduitProcess() {
        if (job) CloseHandle(job);
        if (child) CloseHandle(child);
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    }

   private:
    bool Transfer(void* bytes, size_t length, bool write) {
        auto* at = static_cast<uint8_t*>(bytes);
        while (length && !Stopping()) {
            OVERLAPPED transfer{};
            transfer.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!transfer.hEvent) return false;
            DWORD count = 0;
            const DWORD requested = DWORD(std::min<size_t>(length, 8192));
            bool ready = write ? WriteFile(pipe, at, requested, &count, &transfer) != FALSE
                               : ReadFile(pipe, at, requested, &count, &transfer) != FALSE;
            if (!ready && GetLastError() == ERROR_IO_PENDING) {
                while (!Stopping() && WaitForSingleObject(transfer.hEvent, 100) == WAIT_TIMEOUT) {}
                if (Stopping()) CancelIoEx(pipe, &transfer);
                ready = GetOverlappedResult(pipe, &transfer, &count, TRUE) != FALSE;
            }
            CloseHandle(transfer.hEvent);
            if (!ready || !count) return false;
            at += count;
            length -= count;
        }
        return !length;
    }

    HANDLE pipe{INVALID_HANDLE_VALUE}, child{}, job{};
#else
    bool Start(const fs::path& host, const std::string& dll, const fs::path& prefix) {
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) return false;
        for (int& fd : sockets) {
            if (fd <= STDERR_FILENO) {
                const int duplicate = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
                close(fd);
                fd = duplicate;
            }
        }
        pipe = sockets[0];
        const auto closeChild = [&] { if (sockets[1] >= 0) close(sockets[1]); };
        if (pipe < 0 || sockets[1] < 0 || fcntl(pipe, F_SETFD, FD_CLOEXEC) ||
            fcntl(sockets[1], F_SETFD, FD_CLOEXEC) || fcntl(pipe, F_SETFL, O_NONBLOCK)) {
            closeChild();
            return false;
        }
#ifdef SO_NOSIGPIPE
        int enabled = 1;
        if (setsockopt(pipe, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled))) {
            closeChild();
            return false;
        }
#endif
        std::string hostArgument = host.string();
        std::string dllArgument = dll;
        char wine[] = "wine";
        char* command[] = {wine, hostArgument.data(), dllArgument.data(), nullptr};
        std::vector<std::string> variables;
#ifdef __APPLE__
        auto inherited = *_NSGetEnviron();
#else
        auto inherited = environ;
#endif
        for (auto item = inherited; item && *item; ++item) {
            const std::string_view variable(*item);
            if (variable.find("WINEPREFIX=") != 0 && variable.find("WINEDEBUG=") != 0)
                variables.emplace_back(variable);
        }
        variables.push_back("WINEPREFIX=" + prefix.string());
        variables.emplace_back("WINEDEBUG=-all");
        std::vector<char*> environment;
        for (auto& variable : variables) environment.push_back(variable.data());
        environment.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions)) {
            closeChild();
            return false;
        }
        posix_spawnattr_t attributes;
        const bool haveAttributes = !posix_spawnattr_init(&attributes);
        const bool configured = haveAttributes && !posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) &&
                !posix_spawnattr_setpgroup(&attributes, 0) &&
                !posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO) &&
                !posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO) &&
                !posix_spawn_file_actions_addclose(&actions, sockets[1]) &&
                !posix_spawn_file_actions_addclose(&actions, pipe);
        pid_t pid;
        const bool ready = configured &&
            !posix_spawnp(&pid, command[0], &actions, &attributes, command, environment.data());
        if (haveAttributes) posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        closeChild();
        if (ready) child = pid;
        return ready;
    }

    bool Finished() {
        int status = 0;
        while (!Stopping()) {
            const auto result = waitpid(child, &status, WNOHANG);
            if (result == child) {
                child = -1;
                return WIFEXITED(status) && !WEXITSTATUS(status);
            }
            if (result < 0 && errno != EINTR) return false;
            poll(nullptr, 0, 25);
        }
        return false;
    }

    ~ConduitProcess() {
        if (pipe >= 0) close(pipe);
        if (child > 0) {
            kill(-child, SIGKILL);
            while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
        }
    }

   private:
    bool Transfer(void* bytes, size_t length, bool write) {
        auto* at = static_cast<uint8_t*>(bytes);
        while (length && !Stopping()) {
            pollfd descriptor{pipe, short(write ? POLLOUT : POLLIN), 0};
            const auto ready = poll(&descriptor, 1, 100);
            if (!ready || (ready < 0 && errno == EINTR)) continue;
            if (ready < 0) return false;
#ifdef MSG_NOSIGNAL
            constexpr auto sendFlags = MSG_NOSIGNAL;
#else
            constexpr auto sendFlags = 0;
#endif
            auto amount = write ? send(pipe, at, length, sendFlags) : recv(pipe, at, length, 0);
            if (amount < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (amount <= 0) return false;
            at += amount;
            length -= amount;
        }
        return !length;
    }

    int pipe{-1};
    pid_t child{-1};
#endif

    const std::atomic<bool>* stop;
    const std::chrono::steady_clock::time_point deadline{std::chrono::steady_clock::now() + std::chrono::minutes(5)};
};
#endif

}  // namespace frondSync
