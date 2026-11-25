// The cores' end of the desktop control socket: requests from the launcher come from the input
// over a Unix socket in the scratch folder, with a reply to each and now and then a cancel check
// as a long job makes.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <thread>

#include "DesktopChannel.h"
#include "Scratch.h"

namespace {
std::string path;
int listener = -1;
}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    unsigned runs = 0;
    path = FuzzScratch("desktop-channel", runs) + "/control";
    unlink(path.c_str());
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) std::abort();
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0 || bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener, 4))
        std::abort();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    if (size < 1 || size > 65536) return 0;
    const unsigned choices = bytes[0];
    DesktopChannel channel(path);
    const int launcher = accept(listener, nullptr, nullptr);
    if (launcher < 0) std::abort();
    if (send(launcher, bytes + 1, size - 1, MSG_NOSIGNAL) != ssize_t(size - 1)) std::abort();
    shutdown(launcher, SHUT_WR);
    std::thread reader([launcher] {
        char sink[4096];
        while (recv(launcher, sink, sizeof(sink), 0) > 0) {}
    });
    try {
        DesktopChannel::Request request;
        for (unsigned i = 0; i < 100000; ++i) {
            const auto poll = channel.Receive(request);
            if (poll == DesktopChannel::Poll::closed) break;
            if (poll == DesktopChannel::Poll::request) {
                channel.Reply(request.id, choices & 1, "fuzz", request.argument);
                if (choices & 2) channel.Event("card", request.command);
                if ((choices & 4) && channel.CheckCancel(request.id) == DesktopChannel::Cancel::gone) break;
            }
            channel.QuitPending();
        }
    } catch (const std::runtime_error&) {
    }
    shutdown(launcher, SHUT_RDWR);
    reader.join();
    close(launcher);
    return 0;
}
