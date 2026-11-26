// The H4 Bluetooth controller link (BluetoothBackend.cpp): the controller's bytes (events, ACL and
// SCO data) come from the input over a Unix socket in pieces, interleaved with guest commands and
// the guest reading what arrived byte by byte, as the UART does.
#include <fuzzer/FuzzedDataProvider.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <string>

#include "BluetoothBackend.h"
#include "Scratch.h"

namespace {
int listener = -1;
std::string path;
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    static unsigned runs = 0;
    if (size > 262144) return 0;
    if (listener < 0) {
        unsigned once = 1;
        path = FuzzScratch("bluetooth-hci", once) + "/h4";
        unlink(path.c_str());
        listener = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path.size() >= sizeof(address.sun_path)) std::abort();
        path.copy(address.sun_path, path.size());
        if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || listen(listener, 4)) std::abort();
    }
    ++runs;
    FuzzedDataProvider data(bytes, size);
    BluetoothBackend backend;
    if (data.ConsumeBool()) backend.SetAddress({1, 2, 3, 4, 5, 6});
    std::string error;
    if (!backend.Open(path, error)) std::abort();
    const int controller = accept(listener, nullptr, nullptr);
    if (controller < 0) std::abort();
    unsigned reconnects = 0;
    while (data.remaining_bytes() && backend.Connected()) {
        switch (data.ConsumeIntegralInRange(0, 4)) {
            case 0: {
                const auto piece = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(1, 2048));
                if (send(controller, piece.data(), piece.size(), MSG_NOSIGNAL | MSG_DONTWAIT) < 0) break;
                backend.Poll();
                break;
            }
            case 1: {
                const auto command = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(0, 300));
                backend.Write(command.data(), command.size());
                backend.Poll(data.ConsumeBool());
                break;
            }
            case 2:
                for (unsigned n = data.ConsumeIntegralInRange(1, 4096); n--;) {
                    if (backend.Read() == 0x100) break;
                    backend.FrameInProgress();
                }
                break;
            case 3: {
                char sink[4096];
                while (recv(controller, sink, sizeof(sink), MSG_DONTWAIT) > 0) {}
                std::array<uint8_t, 6> address{};
                backend.Address(address);
                break;
            }
            case 4:
                // Open again on the same backend, as choosing the controller again does.
                if (reconnects++ < 2 && backend.Open(path, error)) {
                    const int next = accept(listener, nullptr, nullptr);
                    if (next >= 0) close(next);
                }
                break;
        }
    }
    while (backend.Read() != 0x100) {}
    backend.Close();
    close(controller);
    return 0;
}
