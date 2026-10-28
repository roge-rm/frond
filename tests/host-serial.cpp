// Firmware-free checks for the host serial worker, its queues, flow control,
// peer disconnects, detach paths and both endpoints (PTY and 127.0.0.1 TCP).
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "HostSerial.h"
#include "pxa_UART.h"

extern void (*hostSerialAfterWriteHook)();

static ArmMemAccessF mmio;
static void* context;
extern "C" void uarmAbort() { std::abort(); }
bool memRegionAdd(ArmMem*, uint32_t, uint32_t, ArmMemAccessF f, void* p) {
    mmio = f;
    context = p;
    return true;
}
void pxaIcInt(PxaIc*, uint_fast8_t number, bool) { assert(number == PXA_I_FFUART); }

enum { RBR = 0, IER = 1, FCR = 2, LCR = 3, MCR = 4, LSR = 5, MSR = 6, FOR = 9 };
enum { DR = 1, OE = 2, TEMT = 0x40, DDCD = 8, CTS = 0x10, DSR = 0x20, DCD = 0x80 };

struct Guest {
    PxaUart* uart;
    explicit Guest(unsigned divisor) {
        uart = pxaUartInit(nullptr, {[](void*, uint32_t) {}, nullptr}, nullptr, PXA_FFUART_BASE, PXA_I_FFUART, true);
        write(LCR, 0x83);
        write(0, divisor);
        write(1, 0);
        write(LCR, 3);    // 8N1
        write(MCR, 0x2b); // AFE, OUT2, RTS, DTR
        write(IER, 0x40); // UUE, no interrupts
        write(FCR, 0x07 | 0x80);  // FIFO on and reset, RX trigger 8
    }
    ~Guest() {
        pxaUartDetachExternal(uart);
        free(uart);
    }
    void write(unsigned reg, uint32_t value) { assert(mmio(context, PXA_FFUART_BASE + 4 * reg, 4, true, &value)); }
    uint32_t read(unsigned reg) {
        uint32_t value = 0;
        assert(mmio(context, PXA_FFUART_BASE + 4 * reg, 4, false, &value));
        return value;
    }
    void step(unsigned n) {
        for (unsigned i = 0; i < n; ++i) pxaUartProcess(uart);
    }
    std::vector<uint8_t> drain() {
        std::vector<uint8_t> out;
        while (read(LSR) & DR) out.push_back(uint8_t(read(RBR)));
        return out;
    }
};

static bool waitFor(HostSerial& serial, const std::function<bool()>& done, int ms = 3000) {
    for (int i = 0; i < ms; ++i) {
        serial.Service();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

static int connectTo(uint16_t port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    assert(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    return fd;
}

static uint16_t portOf(const HostSerial& serial) {
    const auto endpoint = serial.Endpoint();
    assert(endpoint.rfind("127.0.0.1:", 0) == 0);
    return uint16_t(std::stoi(endpoint.substr(10)));
}

static std::vector<uint8_t> readAll(int fd, size_t count, int ms = 3000) {
    std::vector<uint8_t> out;
    while (out.size() < count) {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, ms) <= 0) break;
        uint8_t buffer[4096];
        const auto got = read(fd, buffer, sizeof(buffer));
        if (got <= 0) break;
        out.insert(out.end(), buffer, buffer + got);
    }
    return out;
}

static bool peerClosed(int fd) {
    pollfd p{fd, POLLIN, 0};
    if (poll(&p, 1, 3000) <= 0) return false;
    char c;
    return read(fd, &c, 1) == 0;
}

int main() {
    {
        HostSerial::Config c;
        assert(HostSerial::Parse("off", c) && c.kind == HostSerial::Kind::off);
        assert(HostSerial::Parse("pty", c) && c.kind == HostSerial::Kind::pty);
        assert(HostSerial::Parse("tcp:5023", c) && c.kind == HostSerial::Kind::tcp && c.port == 5023);
        assert(HostSerial::Parse("tcp:0", c) && c.port == 0);
        for (auto bad : {"", "tcp:", "tcp:65536", "tcp:-1", "tcp:12a", "udp:1", "PTY", "tcp:123456"})
            assert(!HostSerial::Parse(bad, c));
        assert(HostSerial::Format({HostSerial::Kind::tcp, 7}) == "tcp:7");
        HostSerial serial;
        std::string error;
        assert(serial.Start(nullptr, {}, error) && serial.Describe() == "off");
        assert(!serial.Start(nullptr, {HostSerial::Kind::pty, 0}, error) && !error.empty());
        serial.Service();
    }
    puts("PASS: endpoint parsing; off opens nothing; no UART refuses an endpoint");

    {
        Guest guest(8);  // 115200 baud
        HostSerial serial;
        std::string error;
        assert(serial.Start(guest.uart, {HostSerial::Kind::tcp, 0}, error));
        const auto port = portOf(serial);
        // Bound to loopback only: the port refuses on this host's other addresses.
        assert(serial.Endpoint() == "127.0.0.1:" + std::to_string(port));
        unsigned others = 0;
        ifaddrs* list = nullptr;
        assert(getifaddrs(&list) == 0);
        for (auto* entry = list; entry; entry = entry->ifa_next) {
            if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET || (entry->ifa_flags & IFF_LOOPBACK)) continue;
            sockaddr_in address = *reinterpret_cast<sockaddr_in*>(entry->ifa_addr);
            if ((ntohl(address.sin_addr.s_addr) >> 24) == 127) continue;
            address.sin_port = htons(port);
            const int probe = socket(AF_INET, SOCK_STREAM, 0);
            assert(connect(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 && errno == ECONNREFUSED);
            close(probe);
            ++others;
        }
        freeifaddrs(list);
        if (!others) puts("note: no non-loopback IPv4 address to check the bind against");
        serial.Service();
        assert(serial.Attached() && !serial.Connected());
        assert(!(guest.read(MSR) & (CTS | DSR | DCD)));
        int client = connectTo(port);
        assert(waitFor(serial, [&] { return serial.Connected(); }));
        serial.Service();
        assert((guest.read(MSR) & (CTS | DSR | DCD)) == (CTS | DSR | DCD));
        assert(serial.Describe() == "tcp " + serial.Endpoint() + " connected");

        // Host to guest, paced one frame per byte on emulated time.
        assert(::write(client, "hello", 5) == 5);
        assert(waitFor(serial, [&] { return serial.PendingReceive() == 5; }));
        std::vector<unsigned> arrivals;
        for (unsigned i = 0; i < 100 && arrivals.size() < 5; ++i) {
            const auto before = guest.read(FOR);
            guest.step(1);
            if (guest.read(FOR) != before) arrivals.push_back(i);
        }
        assert(arrivals.size() == 5);
        // 115200 baud 8N1 is 160 half-bit units, 18 per batch: 8 or 9 batches apart.
        for (size_t i = 1; i < arrivals.size(); ++i) {
            const auto gap = arrivals[i] - arrivals[i - 1];
            assert(gap == 8 || gap == 9);
        }
        assert(guest.drain() == std::vector<uint8_t>({'h', 'e', 'l', 'l', 'o'}));

        // Guest to host.
        for (uint8_t c : {0x00, 0x7f, 0x80, 0xff}) guest.write(RBR, c);
        for (int i = 0; i < 100; ++i) guest.step(1);
        assert(readAll(client, 4) == std::vector<uint8_t>({0x00, 0x7f, 0x80, 0xff}));

        // A second client is closed at once; the first keeps the line.
        int second = connectTo(port);
        assert(peerClosed(second));
        close(second);
        assert(serial.Connected());

        // RX backpressure: the guest does not read, auto RTS drops, the host queue
        // stops at its bound and nothing overruns. Then everything arrives in order.
        std::vector<uint8_t> burst(20000);
        for (size_t i = 0; i < burst.size(); ++i) burst[i] = uint8_t(i * 7 + (i >> 8));
        std::thread sender([&] {
            size_t done = 0;
            while (done < burst.size()) {
                const auto n = ::write(client, burst.data() + done, burst.size() - done);
                assert(n > 0);
                done += size_t(n);
            }
        });
        assert(waitFor(serial, [&] { return serial.PendingReceive() == HostSerial::queueBytes; }));
        guest.step(2000);
        assert(!(pxaUartGetModemOutputs(guest.uart) & PXA_UART_RTS));
        assert(serial.PendingReceive() <= HostSerial::queueBytes);
        assert(!(guest.read(LSR) & OE));
        std::vector<uint8_t> got;
        for (int i = 0; i < 2000000 && got.size() < burst.size(); ++i) {
            guest.step(1);
            if (!(i % 64)) serial.Service();
            if (guest.read(LSR) & OE) assert(!"overrun with flow control");
            auto bytes = guest.drain();
            got.insert(got.end(), bytes.begin(), bytes.end());
        }
        sender.join();
        assert(got == burst);
        assert(serial.GetCounters().received == 5 + burst.size());

        // Peer EOF drops DCD, DSR and CTS; a new client raises them again.
        guest.read(MSR);
        close(client);
        assert(waitFor(serial, [&] { return !serial.Connected(); }));
        serial.Service();
        auto msr = guest.read(MSR);
        assert(!(msr & (CTS | DSR | DCD)) && (msr & DDCD));
        guest.write(MCR, 0x0b);  // no auto flow, so the byte goes out to no one
        guest.write(RBR, 'x');
        guest.step(20);
        assert(serial.PendingSend() == 0 && serial.GetCounters().txDropped == 1);
        guest.write(MCR, 0x2b);
        client = connectTo(port);
        assert(waitFor(serial, [&] { return serial.Connected(); }));
        serial.Service();
        assert((guest.read(MSR) & (CTS | DSR | DCD)) == (CTS | DSR | DCD));

        // A closed port has no receiver: host bytes are dropped, not saved for later.
        guest.write(IER, 0);
        assert(::write(client, "stale", 5) == 5);
        assert(waitFor(serial, [&] { return serial.GetCounters().rxDiscarded >= 5; }));
        assert(serial.PendingReceive() == 0);
        guest.write(IER, 0x40);
        guest.step(200);
        assert(!(guest.read(LSR) & DR));
        // So does a close and reopen between two Service calls.
        assert(::write(client, "old", 3) == 3);
        assert(waitFor(serial, [&] { return serial.PendingReceive() == 3; }));
        guest.write(IER, 0);
        guest.write(IER, 0x40);
        serial.Service();
        assert(serial.PendingReceive() == 0);
        guest.step(200);
        assert(!(guest.read(LSR) & DR));

        // Reset or load detaches; the peer stays, queues are dropped and the next
        // Service attaches again with fresh modem inputs.
        assert(::write(client, "q", 1) == 1);
        assert(waitFor(serial, [&] { return serial.PendingReceive() == 1; }));
        pxaUartDetachExternal(guest.uart);
        assert(!serial.Attached() && serial.PendingReceive() == 0 && serial.Connected());
        assert(!(guest.read(MSR) & DCD));
        // Frozen (checkpoint) or busy UARTs are not attached.
        pxaUartBeginCheckpoint(guest.uart);
        serial.Service();
        assert(!serial.Attached());
        pxaUartEndCheckpoint(guest.uart);
        guest.write(MCR, 0x0b);
        guest.write(RBR, 'b');  // TX busy
        serial.Service();
        assert(!serial.Attached());
        guest.step(20);
        guest.write(MCR, 0x2b);
        serial.Service();
        assert(serial.Attached());
        assert((guest.read(MSR) & (CTS | DSR | DCD)) == (CTS | DSR | DCD));

        // Mode change: the TCP peer sees EOF and the new endpoint is a PTY.
        assert(serial.Start(guest.uart, {HostSerial::Kind::pty, 0}, error));
        assert(peerClosed(client));
        close(client);
        assert(serial.Endpoint().rfind("/dev/pts/", 0) == 0 && !serial.Attached());

        // Stop detaches and closes everything; the UART then writes nowhere.
        serial.Service();
        assert(serial.Attached());
        serial.Stop();
        assert(!serial.Attached() && serial.Describe() == "off");
        guest.write(MCR, 0x0b);
        guest.write(RBR, 'z');
        guest.step(20);
        assert(guest.read(LSR) & TEMT);
        assert(!(guest.read(MSR) & DCD));
    }
    puts("PASS: TCP on 127.0.0.1 both ways, frame pacing, one client, RTS backpressure without overrun, EOF drops DCD/DSR/CTS, closed port drops RX, detach/frozen/busy/mode change/stop");

    {
        Guest guest(1);  // 921600 baud
        HostSerial serial;
        std::string error;
        assert(serial.Start(guest.uart, {HostSerial::Kind::pty, 0}, error));
        const auto path = serial.Endpoint();
        int slave = open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        assert(slave >= 0);
        assert(waitFor(serial, [&] { return serial.Attached() && serial.Connected(); }));
        assert((guest.read(MSR) & (CTS | DSR | DCD)) == (CTS | DSR | DCD));
        assert(::write(slave, "\r\n\x03", 3) == 3);  // raw: no CR/LF translation or signals
        assert(waitFor(serial, [&] { return serial.PendingReceive() == 3; }));
        guest.step(100);
        assert(guest.drain() == std::vector<uint8_t>({'\r', '\n', 3}));

        // TX backpressure: the host does not read, the PTY buffer and then the send
        // queue fill, CTS drops and the guest stops without losing a byte.
        std::vector<uint8_t> sent;
        bool ctsDropped = false;
        for (int i = 0; i < 3000000 && !ctsDropped; ++i) {
            if ((guest.read(LSR) & 0x20) && sent.size() < 400000) {  // TDRQ
                for (int k = 0; k < 32; ++k) {
                    const uint8_t c = uint8_t(sent.size() * 13 + 1);
                    guest.write(RBR, c);
                    sent.push_back(c);
                }
            }
            guest.step(1);
            if (!(i % 16)) {
                serial.Service();
                ctsDropped = !(guest.read(MSR) & CTS);
            }
        }
        assert(ctsDropped);
        assert(serial.PendingSend() >= HostSerial::queueBytes / 2 && serial.GetCounters().txDropped == 0);
        // Nothing moves while CTS is low except the frame in flight.
        guest.step(5000);
        std::vector<uint8_t> received;
        for (int i = 0; i < 10000 && received.size() < sent.size(); ++i) {
            auto chunk = readAll(slave, 1, 10);
            received.insert(received.end(), chunk.begin(), chunk.end());
            guest.step(50);
            serial.Service();
        }
        assert(received == sent);
        assert(guest.read(MSR) & CTS);
        assert(serial.GetCounters().txDropped == 0);

        // Closing the slave hangs up: DCD, DSR and CTS drop. Reopening raises them.
        close(slave);
        assert(waitFor(serial, [&] { return !serial.Connected(); }));
        serial.Service();
        assert(!(guest.read(MSR) & (CTS | DSR | DCD)));
        slave = open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        assert(waitFor(serial, [&] { return serial.Connected(); }));
        serial.Service();
        assert((guest.read(MSR) & (CTS | DSR | DCD)) == (CTS | DSR | DCD));
        guest.write(RBR, 'k');
        guest.step(5);
        assert(readAll(slave, 1) == std::vector<uint8_t>({'k'}));
        serial.Stop();
        close(slave);
        assert(::access(path.c_str(), F_OK) != 0);  // the PTY is gone
    }
    puts("PASS: PTY raw both ways, TX backpressure through CTS without drops, hangup and reopen");
    {
        // A hangup is seen while the receive queue is full and the guest holds RTS
        // low; the bytes queued before it stay for the guest.
        Guest guest(8);
        HostSerial serial;
        std::string error;
        assert(serial.Start(guest.uart, {HostSerial::Kind::tcp, 0}, error));
        int client = connectTo(portOf(serial));
        assert(waitFor(serial, [&] { return serial.Attached() && serial.Connected(); }));
        guest.write(MCR, 0x29);  // RTS low
        std::vector<uint8_t> lots(HostSerial::queueBytes + 2000, 'a');
        assert(::write(client, lots.data(), lots.size()) == ssize_t(lots.size()));
        assert(waitFor(serial, [&] { return serial.PendingReceive() == HostSerial::queueBytes; }));
        close(client);
        assert(waitFor(serial, [&] { return !serial.Connected(); }, 500));
        serial.Service();
        assert(!(guest.read(MSR) & (CTS | DSR | DCD)));
        assert(serial.PendingReceive() == HostSerial::queueBytes);

        assert(serial.Start(guest.uart, {HostSerial::Kind::pty, 0}, error));
        const auto path = serial.Endpoint();
        int slave = open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        assert(waitFor(serial, [&] { return serial.Attached() && serial.Connected(); }));
        size_t written = 0;
        while (written < lots.size()) {
            const auto n = ::write(slave, lots.data() + written, lots.size() - written);
            if (n > 0) written += size_t(n);
            serial.Service();
        }
        assert(waitFor(serial, [&] { return serial.PendingReceive() == HostSerial::queueBytes; }));
        close(slave);
        assert(waitFor(serial, [&] { return !serial.Connected(); }, 500));
        serial.Service();
        assert(!(guest.read(MSR) & (CTS | DSR | DCD)));
        // What the old peer left in the PTY is not handed to the next one.
        guest.write(MCR, 0x2b);
        slave = open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        assert(waitFor(serial, [&] { return serial.Connected(); }));
        uint64_t before = serial.GetCounters().received;
        for (int i = 0; i < 400000 && serial.PendingReceive(); ++i) {
            guest.step(1);
            guest.drain();
            if (!(i % 64)) serial.Service();
        }
        assert(serial.PendingReceive() == 0 && waitFor(serial, [&] { return false; }, 50) == false);
        assert(serial.GetCounters().received == before);

        // A reset while the worker is writing never erases bytes queued after it:
        // the worker stops after its write, the guest resets and queues new bytes.
        readAll(slave, 1 << 20, 50);
        static std::atomic<int> stage{0};
        hostSerialAfterWriteHook = [] {
            int expected = 0;
            if (!stage.compare_exchange_strong(expected, 1)) return;
            while (stage != 2) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        };
        for (uint8_t c : {'x', 'x', 'x', 'x'}) guest.write(RBR, c);
        for (int i = 0; i < 2000 && stage != 1; ++i) {
            guest.step(1);
            serial.Service();
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        assert(stage == 1);
        guest.step(200);  // the rest of the x bytes join the old queue
        pxaUartDetachExternal(guest.uart);
        serial.Service();
        assert(serial.Attached());
        for (int k = 0; k < 16; ++k) guest.write(RBR, 'A' + k);
        guest.step(400);
        assert(serial.PendingSend() == 16);
        stage = 2;
        std::vector<uint8_t> drained;
        for (int i = 0; i < 100 && drained.size() < 4 + 16; ++i) {
            serial.Service();
            auto chunk = readAll(slave, 1, 10);
            drained.insert(drained.end(), chunk.begin(), chunk.end());
        }
        hostSerialAfterWriteHook = nullptr;
        assert(drained.size() >= 16);
        for (int k = 0; k < 16; ++k) assert(drained[drained.size() - 16 + k] == 'A' + k);
        for (size_t k = 0; k + 16 < drained.size(); ++k) assert(drained[k] == 'x');
        serial.Stop();
        close(slave);
    }
    puts("PASS: hangup seen with a full receive queue (TCP and PTY), PTY leftovers flushed, reset during a write keeps later bytes");

    return 0;
}
