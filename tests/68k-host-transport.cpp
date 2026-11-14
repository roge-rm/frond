// HostTransport68k pacing: bytes from a host link reach the guest at the line's rate from when
// they're first seen, after an idle spell, a drain and refill, and an IrDA frame gap.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#include "EmHAL.h"
#include "EmRegsEZ.h"
#include "EmSession.h"
#include "HostTransport.h"

static uint64 gNow = 0;

class Board : public EmRegsEZ {
   public:
    Board() { SetSubBankHandlers(); }
    Bool GetLCDScreenOn() override { return true; }
    Bool GetLCDBacklightOn() override { return false; }
    void GetKeyInfo(int* r, int* c, uint16*, Bool*) override { *r = *c = 0; }
};

struct Rig {
    HostLink68k link;
    HostTransport68k t{link};
    int peer{-1};
    uint64 byteTime{0};

    Rig(const std::string& path, long baud, bool irda) {
        std::string error;
        if (!link.Open(HostLink68k::Kind::unixSocket, 0, error, path)) {
            fprintf(stderr, "%s\n", error.c_str());
            assert(false);
        }
        peer = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
        assert(connect(peer, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        link.Poll();
        EmTransportSerial::Config config;
        config.fBaud = baud;
        config.fIrda = irda;
        t.SetConfig(config);
        t.SetFrameGap(irda);
        t.Service(true);
        byteTime = uint64(gSession->GetClocksPerSecond()) * 10 / baud;
    }
    ~Rig() {
        close(peer);
        link.Close();
    }
    void Send(const std::vector<uint8_t>& bytes) {
        assert(write(peer, bytes.data(), bytes.size()) == ssize_t(bytes.size()));
        link.Poll();
        t.Service(true);
    }
    // Steps the clock an eighth of a byte at a time, the guest reading up to a FIFO's worth each
    // step, until count bytes are in. Returns the clock at each byte.
    std::vector<uint64> Receive(size_t count) {
        std::vector<uint64> at;
        for (int steps = 0; at.size() < count; ++steps) {
            assert(steps < 100000);
            gNow += byteTime / 8;
            t.Service(true);
            long n = std::min<long>(12, long(t.BytesPending()));
            uint8_t buffer[12];
            if (n > 0 && t.CanRead()) {
                t.Read(n, buffer);
                for (long i = 0; i < n; ++i) at.push_back(gNow);
            }
        }
        return at;
    }
};

static std::vector<uint8_t> Frame(size_t size) {
    std::vector<uint8_t> bytes(size, 0x55);
    bytes.back() = 0xc1;
    return bytes;
}

// In byte times, how long count bytes took from the first.
static double Span(const Rig& rig, const std::vector<uint64>& at, size_t from, size_t count) {
    return double(at[from + count - 1] - at[from]) / double(rig.byteTime);
}

int main(int, char** argv) {
    Board board;  // registers itself as the HAL root
    auto clock = EmHAL::onSystemClockChange.AddHandler([] { gSession->SetClockFactor(1); });
    gSession->SetClockFactor(1);
    board.Initialize();
    HostTransport68k::testClock = [] { return gNow; };
    // The socket goes in a private folder beside the test binary, in build/.
    std::string dir = std::string(argv[0]) + "-XXXXXX";
    assert(mkdtemp(dir.data()));
    const std::string path = dir + "/s";

    {
        // A 45-byte frame after a second of quiet comes in over about 45 byte times.
        Rig rig(path, 9600, false);
        gNow += uint64(gSession->GetClocksPerSecond());
        rig.Send(Frame(45));
        const auto at = rig.Receive(45);
        const double span = Span(rig, at, 0, 45);
        printf("idle then 45 bytes at 9600: %.1f byte times\n", span);
        assert(span >= 43 && span <= 46);

        // Drained, then quiet with no service, then 20 more: still at the line's rate.
        gNow += 100 * rig.byteTime;
        rig.Send(Frame(20));
        const auto again = rig.Receive(20);
        const double span2 = Span(rig, again, 0, 20);
        printf("drain, quiet, 20 bytes: %.1f byte times\n", span2);
        assert(span2 >= 18 && span2 <= 21);
    }
    {
        // Two IrDA frames back to back at 115200: the second starts after the 5 ms gap and comes
        // in at the line's rate, not as a burst of the credit built during the gap.
        Rig rig(path, 115200, true);
        rig.Send(Frame(30));
        std::vector<uint8_t> second = Frame(30);
        rig.Send(second);
        const auto at = rig.Receive(60);
        const double gap = double(at[30] - at[29]) / double(rig.byteTime);
        const double span = Span(rig, at, 30, 30);
        printf("second frame at 115200: gap %.1f byte times, frame %.1f byte times\n", gap, span);
        assert(gap >= 0.005 * 115200 / 10 - 1);
        assert(span >= 28 && span <= 31);
    }
    unlink(path.c_str());
    rmdir(dir.c_str());
    board.Dispose();
    EmHAL::onSystemClockChange.RemoveHandler(clock);
    printf("host transport pacing: ok\n");
}
