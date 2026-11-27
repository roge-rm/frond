#include "ZaurusPcLink.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <sys/un.h>

#include "BackgroundJob.h"
#include "PosixCompat.h"
#include "UsbHostBridge.h"
#include "ZaurusPcLinkFiles.h"
#include "storage/Io.h"
#include "sync/vendor/arduinojson/ArduinoJson.h"

namespace {
using Endpoint = UsbHostBridge::ZaurusFilesEndpoint;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

class TransportError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

class SocketStream : public zaurusPcLink::Stream {
  public:
    SocketStream(const std::atomic<bool>& cancelled, const std::atomic<bool>& abort)
        : cancelled(cancelled), abort(abort) {}
    ~SocketStream() override { if (fd >= 0) close(fd); }

    void CheckCancelled() override {
        if (cancelled) throw TransportError("PC Link was closed.");
    }

    void Connect(const Endpoint& endpoint) {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, endpoint.path.c_str(), endpoint.path.size() + 1);
        fd = frondPosix::Socket(AF_UNIX, SOCK_STREAM | frondPosix::kCloexec | frondPosix::kNonblock, 0);
        if (fd < 0) throw TransportError("Couldn't open the PC Link connection.");
        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) return;
        if (errno != EINPROGRESS && errno != EAGAIN && errno != EINTR)
            throw TransportError("Couldn't connect to the Zaurus.");
        Wait(POLLOUT, Clock::now() + 5s);
        int error{};
        socklen_t size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) != 0 || error)
            throw TransportError("Couldn't connect to the Zaurus.");
    }

    void Send(const uint8_t* data, size_t size) override {
        Transfer(size, POLLOUT, [&](size_t offset) {
            return send(fd, reinterpret_cast<const char*>(data + offset), size - offset, MSG_NOSIGNAL);
        });
    }

    void Receive(uint8_t* data, size_t size) override {
        Transfer(size, POLLIN, [&](size_t offset) {
            return recv(fd, reinterpret_cast<char*>(data + offset), size - offset, 0);
        });
    }

  private:
    void Wait(short events, Clock::time_point deadline) {
        while (!abort && Clock::now() < deadline) {
            pollfd socket{fd, events, 0};
            const auto ready = poll(&socket, 1, 100);
            if (ready > 0) return;
            if (ready < 0 && errno != EINTR) throw TransportError("The PC Link connection closed.");
        }
        if (abort) throw TransportError("PC Link was closed.");
        throw TransportError("The Zaurus stopped responding over PC Link.");
    }

    template <typename Io>
    void Transfer(size_t size, short events, Io io) {
        for (size_t offset = 0; offset < size;) {
            const auto deadline = Clock::now() + 20s;
            while (true) {
                Wait(events, deadline);
                const auto count = io(offset);
                if (count > 0) {
                    offset += count;
                    break;
                }
                if (count == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
                    throw TransportError("The PC Link connection closed.");
            }
        }
    }

    int fd{-1};
    const std::atomic<bool>& cancelled;
    const std::atomic<bool>& abort;
};

void PrepareEndpoint(Endpoint& endpoint, const std::string& controlPath, std::string& directory) {
    auto pattern = (fs::path(controlPath).parent_path() / "pc-XXXXXX").string();
    sockaddr_un address{};
    if (pattern.size() + 2 >= sizeof(address.sun_path) || !mkdtemp(pattern.data()))
        throw std::runtime_error("Couldn't make a private PC Link socket.");
    directory = std::move(pattern);
    endpoint.path = directory + "/s";
}

struct CloseFile {
    void operator()(FILE* file) const { fclose(file); }
};
using File = std::unique_ptr<FILE, CloseFile>;

struct ReceivedFile {
    fs::path temporary;
    ~ReceivedFile() { frondStorage::removePath(temporary); }

    void Copy(zaurusPcLink::Client& client, const std::string& remote, const fs::path& target,
              const std::atomic<bool>& cancelled) {
        auto pattern = (target.parent_path() / ".frond-pc-XXXXXX").string();
        const int fd = mkstemp(pattern.data());
        if (fd < 0) throw std::runtime_error("Couldn't create a file in the selected folder.");
        temporary = pattern;
        File output(fdopen(fd, "wb"));
        if (!output) {
            close(fd);
            throw std::runtime_error("Couldn't open the destination file.");
        }
        client.Receive(remote, output.get());
        if (fclose(output.release()) != 0)
            throw std::runtime_error("Couldn't finish writing the destination file.");
        if (cancelled) throw std::runtime_error("PC Link was closed.");
        if (std::rename(temporary.string().c_str(), target.string().c_str()) != 0)
            throw std::runtime_error("Couldn't save the received file.");
        temporary.clear();
    }
};

std::string DirectoryJson(const std::vector<zaurusPcLink::Entry>& entries) {
    size_t capacity = JSON_ARRAY_SIZE(entries.size()) + JSON_OBJECT_SIZE(2) * entries.size();
    for (const auto& entry : entries) capacity += entry.name.size() + 1;
    if (capacity > 1024 * 1024) throw std::runtime_error("This folder has too many files to show.");
    ArduinoJson::DynamicJsonDocument document(capacity);
    auto array = document.to<ArduinoJson::JsonArray>();
    for (const auto& entry : entries) {
        auto item = array.createNestedObject();
        item["name"] = entry.name;
        item["directory"] = entry.directory;
    }
    // The desktop client's input limit includes the hex framing.
    if (document.overflowed() || ArduinoJson::measureJson(document) > 450000)
        throw std::runtime_error("This folder has too many files to show.");
    std::string json;
    ArduinoJson::serializeJson(document, json);
    return json;
}
}  // namespace

struct ZaurusPcLink::Impl {
    explicit Impl(std::string path) : controlPath(std::move(path)) {}

    void Run(const std::string& command, const std::string& remote, const std::string& local) {
        const auto waitUntil = Clock::now() + 75s;
        while (!cancelled && Clock::now() < waitUntil) {
            const auto status = endpoint->status.load();
            if (status < 0) throw std::runtime_error("Couldn't open the local PC Link socket.");
            if (status == 1) {
                SocketStream connection(cancelled, abort);
                std::unique_ptr<zaurusPcLink::Client> client;
                try {
                    connection.Connect(*endpoint);
                    client = std::make_unique<zaurusPcLink::Client>(connection);
                } catch (const TransportError&) {
                    // DHCP can finish while the stock guest is still starting the file service.
                }
                if (client) {
                    if (command == "zaurus-pc-link-list") result = DirectoryJson(client->List(remote));
                    else if (command == "zaurus-pc-link-send") {
                        const int fd = open(local.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                        if (fd < 0) throw std::runtime_error("Couldn't open the selected file.");
                        struct stat info{};
                        File input(fstat(fd, &info) == 0 && S_ISREG(info.st_mode) ? fdopen(fd, "rb") : nullptr);
                        if (!input) {
                            close(fd);
                            throw std::runtime_error("Couldn't read the selected file.");
                        }
                        client->Send(remote, input.get());
                    } else {
                        ReceivedFile{}.Copy(*client, remote, local, cancelled);
                    }
                    return;
                }
            }
            std::this_thread::sleep_for(100ms);
        }
        if (cancelled) throw std::runtime_error("PC Link was closed.");
        throw std::runtime_error("Couldn't reach the Zaurus. In PC Link on the Zaurus, select USB (TCP/IP).");
    }

    std::string controlPath, directory, result, error;
    UsbHostBridge* bridge{nullptr};
    uint64_t generation{0};
    std::shared_ptr<Endpoint> endpoint;
    std::atomic<bool> cancelled{false}, abort{false};
    Clock::time_point closeDeadline;
    BackgroundJob job;
};

ZaurusPcLink::ZaurusPcLink(std::string controlPath) : impl(std::make_unique<Impl>(std::move(controlPath))) {}
ZaurusPcLink::~ZaurusPcLink() { Close(); }

void ZaurusPcLink::BeginClose() {
    impl->cancelled = true;
    impl->closeDeadline = Clock::now() + 5s;
}

bool ZaurusPcLink::FinishClose() {
    if (impl->job.Busy() && !impl->job.Finished()) {
        if (Clock::now() >= impl->closeDeadline) impl->abort = true;
        return false;
    }
    Close();
    return true;
}

void ZaurusPcLink::Close() {
    impl->cancelled = true;
    impl->abort = true;
    impl->job.Wait();
    if (impl->bridge && impl->bridge->RouteGeneration() == impl->generation) impl->bridge->Stop();
    impl->bridge = nullptr;
    impl->endpoint.reset();
    frondStorage::removePath(impl->directory);
    impl->directory.clear();
}

ZaurusPcLink::Reply ZaurusPcLink::Command(UsbHostBridge& bridge, SocPXA& soc, const std::string& command,
                                         const std::string& argument) {
    if (impl->bridge && bridge.RouteGeneration() != impl->generation) Close();
    if (command == "zaurus-pc-link-open") {
        if (impl->bridge) return {true, "opened", {}};
        if (bridge.Host()) return {false, "busy", "The virtual USB cable is already in use."};
        auto endpoint = std::make_shared<Endpoint>();
        try {
            PrepareEndpoint(*endpoint, impl->controlPath, impl->directory);
            std::string error;
            if (!bridge.StartZaurusNetworkProbe(&soc, error, 0, endpoint)) {
                Close();
                return {false, "failed", error};
            }
        } catch (...) {
            Close();
            throw;
        }
        impl->bridge = &bridge;
        impl->generation = bridge.RouteGeneration();
        impl->endpoint = std::move(endpoint);
        return {true, "opened", {}};
    }
    if (!impl->bridge) return {false, "failed", "PC Link is closed."};
    if (command == "zaurus-pc-link-result") {
        if (!impl->job.Busy()) return {false, "failed", "No PC Link transfer is pending."};
        if (!impl->job.Finished()) return {true, "busy", {}};
        impl->job.Wait();
        if (!impl->error.empty()) return {false, "failed", std::move(impl->error)};
        return {true, "done", std::move(impl->result)};
    }
    if (impl->job.Busy()) return {false, "busy", "A PC Link job is already running."};
    std::string remote = argument, local;
    if (command == "zaurus-pc-link-send" || command == "zaurus-pc-link-receive") {
        ArduinoJson::DynamicJsonDocument input(argument.size() + JSON_OBJECT_SIZE(2));
        if (ArduinoJson::deserializeJson(input, argument) ||
            !input["remote"].is<std::string>() || !input["local"].is<std::string>())
            return {false, "argument", "Choose a file to transfer."};
        remote = input["remote"].as<std::string>();
        local = input["local"].as<std::string>();
        if (local.find('\0') != std::string::npos || !frondPosix::IsAbsolutePath(local))
            return {false, "argument", "An absolute file path is required."};
    } else if (command != "zaurus-pc-link-list") {
        return {false, "argument", "Unknown PC Link command."};
    }
    impl->cancelled = false;
    impl->abort = false;
    impl->result.clear();
    impl->error.clear();
    impl->job.Start([state = impl.get(), command, remote = std::move(remote), local = std::move(local)] {
        try {
            state->Run(command, remote, local);
        } catch (const std::exception& error) {
            state->error = error.what();
        }
    });
    return {true, "started", {}};
}

#else

struct ZaurusPcLink::Impl {};

ZaurusPcLink::ZaurusPcLink(std::string) {}
ZaurusPcLink::~ZaurusPcLink() = default;

ZaurusPcLink::Reply ZaurusPcLink::Command(UsbHostBridge&, SocPXA&, const std::string&, const std::string&) {
    return {false, "unsupported", "PC Link files are unavailable on Windows."};
}
void ZaurusPcLink::BeginClose() {}
bool ZaurusPcLink::FinishClose() { return true; }
void ZaurusPcLink::Close() {}

#endif
