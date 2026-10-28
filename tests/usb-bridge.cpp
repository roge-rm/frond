// Firmware-free tests for the emulator's USB host (UsbBusHost) against a scripted guest
// laid out like the TX ROM (EP1 IN/EP2 OUT, HotSync port on EP6 IN/EP7 OUT): enumeration,
// frame rules, the Palm connection-information requests, transparent byte bridging with
// flow control, reconnect on UDE, a port that appears later, and HostByteLink on a PTY.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include "usb-host-fixture.h"
#include "UsbBusHost.h"
#include "HostSerial.h"

static ArmMemAccessF mmio;
static bool irq;
extern "C" void uarmAbort() { std::abort(); }
bool memRegionAdd(ArmMem*, uint32_t, uint32_t, ArmMemAccessF f, void*) { mmio = f; return true; }
void pxaIcInt(PxaIc*, uint_fast8_t n, bool level) { assert(n == PXA_I_USB); irq = level; }

static Pxa270Udc* udc;
static void wr(unsigned offset, uint32_t value) { assert(mmio(udc, 0x40600000 + offset, 4, true, &value)); }
static uint32_t rd(unsigned offset) { uint32_t v = 0; assert(mmio(udc, 0x40600000 + offset, 4, false, &v)); return v; }
static void wr8(unsigned offset, uint8_t value) { assert(mmio(udc, 0x40600000 + offset, 1, true, &value)); }

static uint32_t ccr(bool in, unsigned number) {
    return 1u | 64u << 2 | (in ? 1u << 12 : 0u) | 2u << 13 | number << 15 | 1u << 25;  // bulk 64, config 1
}

// Scripted guest in the style of the TX ROM's XLLP driver (EP0 W1C after the handler).
struct Guest {
    static constexpr uint64_t kLatencyNs = 4000, kHandlerNs = 30000;
    uint64_t now = 0, w1cAt = 0, irqSince = UINT64_MAX, workAt = 0;
    bool w1cPending = false, working = false, stallVendor4 = false;
    unsigned state0 = 0, left = 0, ports = 1;
    char function[5] = "cnys";
    const uint8_t* ptr = nullptr;
    std::vector<uint8_t> device, config, reply, received;
    std::vector<std::vector<uint8_t>> requests;
    std::deque<uint8_t> toSend;
    uint32_t isr0 = 0;
    bool outToB = false, silent = false, dropSetup = false;
    uint64_t vendorDelay = 0, vendorAt = 0;  // answer the port list this late
    unsigned vendorLength = 0;
    Guest() {
        device = {18, 1, 0x00, 0x02, 0, 0, 0, 16, 0x30, 0x08, 0x61, 0x00, 0x00, 0x01, 1, 2, 0, 1};
        config = {9, 2, 46, 0, 1, 1, 0, 0xc0, 0, 9, 4, 0, 0, 4, 0xff, 0, 0, 0};
        for (uint8_t e : {0x81, 0x02, 0x86, 0x07}) config.insert(config.end(), {7, 5, e, 2, 64, 0, 0});
        assert(config.size() == 46);
    }
    void enable() {
        wr(0x404, ccr(true, 1)); wr(0x408, ccr(false, 2)); wr(0x418, ccr(true, 6)); wr(0x41c, ccr(false, 7));
        wr(0, 1); wr(4, 3); wr(8, (1u << 31) | (1u << 27));
    }
    void send0() {
        const unsigned n = std::min(16u, left);
        unsigned i = 0;
        for (; i + 4 <= n; i += 4) { uint32_t w; std::memcpy(&w, ptr + i, 4); wr(0x300, w); }
        for (; i < n; ++i) wr8(0x300, ptr[i]);
        ptr += n; left -= n;
        if (n < 16) { wr(0x100, 2); state0 = 3; } else state0 = 2;
    }
    void portList() {
        std::vector<uint8_t> info(20, 0);
        info[0] = uint8_t(ports); info[1] = 1;
        std::memcpy(&info[4], function, 4); info[9] = 0x67;
        answer(info, vendorLength);
    }
    void answer(const std::vector<uint8_t>& data, unsigned length) {
        reply = data;
        ptr = reply.data(); left = std::min<unsigned>(length, unsigned(reply.size()));
        send0();
    }
    void ep0() {
        const uint32_t csr = rd(0x100);
        if (csr & 1) {
            const unsigned count = rd(0x200) & 0x3ff;
            uint8_t req[16] = {};
            for (unsigned i = 0; i < count; i += 4) { uint32_t w = rd(0x300); std::memcpy(req + i, &w, 4); }
            wr(0x100, 0x81);
            state0 = 0;
            if (count < 8) return;  // status OUT
            requests.push_back(std::vector<uint8_t>(req, req + 8));
            const unsigned length = req[6] | req[7] << 8;
            if (req[0] == 0x80 && req[1] == 6 && req[3] == 1) return answer(device, length);
            if (req[0] == 0x80 && req[1] == 6 && req[3] == 2) return answer(config, length);
            if (req[0] == 0xc2 && req[1] == 4 && !stallVendor4 && vendorDelay && !vendorAt) {
                vendorAt = now + vendorDelay;
                vendorLength = length;
                return;
            }
            if (req[0] == 0xc2 && req[1] == 4 && !stallVendor4) {
                std::vector<uint8_t> info(20, 0);
                info[0] = uint8_t(ports); info[1] = 1;
                std::memcpy(&info[4], function, 4); info[9] = 0x67;
                return answer(info, length);
            }
            if (req[0] == 0xc2 && req[1] == 3)  // Visor: GENERIC on 1, HOTSYNC on 6
                return answer({2, 0, 0, 1, 2, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, length);
            wr(0x100, 0x20);  // FST
            return;
        }
        if (state0 == 2) send0();
    }
    void fillIn() {
        while ((rd(0x118) & 1) && !toSend.empty()) {
            unsigned n = 0;
            while (n < 64 && !toSend.empty()) { wr8(0x318, toSend.front()); toSend.pop_front(); ++n; }
            if (n < 64) wr(0x118, 0x80);
        }
    }
    void epOut() {
        if (rd(0x108) & 2) outToB = true;
        for (uint32_t csr; (csr = rd(0x11c)) & 3;) {
            const unsigned count = rd(0x21c);
            for (unsigned i = 0; i < count; i += 4) {
                const uint32_t w = rd(0x31c);
                for (unsigned b = 0; b < 4 && i + b < count; ++b) received.push_back(uint8_t(w >> (8 * b)));
            }
            wr(0x11c, 2);
        }
    }
    void tick() {
        if (silent) return;
        if (w1cPending && now >= w1cAt) { wr(0x0c, 1); wr(0x100, 4); w1cPending = false; }
        if (vendorAt && now >= vendorAt) { vendorAt = 0; vendorDelay = 0; portList(); }
        if (working && now >= workAt) {
            working = false;
            if (isr0 & (3u << 12)) fillIn();
            if (isr0 & (3u << 14)) epOut();
            if (isr0 & 1) { ep0(); w1cAt = now + 1000; w1cPending = true; }
        }
        if (!toSend.empty() && (rd(0x118) & 1)) fillIn();
        if (working || w1cPending || !irq) { irqSince = UINT64_MAX; return; }
        if (irqSince == UINT64_MAX) irqSince = now;
        if (now < irqSince + kLatencyNs) return;
        irqSince = UINT64_MAX;
        isr0 = rd(0x0c);
        const uint32_t isr1 = rd(0x10);
        wr(0x10, isr1 & 0xf8000000u);
        if (isr1 & (1u << 31)) { wr(0, rd(0) | 0x10); wr(4, 0xf03f); }  // IRCC: SMAC, enable A, B, F, G
        if (isr0 & ~1u) wr(0x0c, isr0 & ~1u);
        if (dropSetup && (isr0 & 1) && (rd(0x100) & 1)) {
            // Starting up: clears EP0 and its interrupt without looking at the SETUP.
            dropSetup = false;
            wr(0x100, 0x81); wr(0x0c, 1);
            return;
        }
        if (isr0 & (3u << 12)) wr(0x118, 4);
        working = true; workAt = now + kHandlerNs;
    }
};

struct Bench {
    Guest guest;
    std::deque<uint8_t> fromHost, toHost;  // the host side of the link
    size_t space = 4096;
    std::vector<std::pair<uint64_t, uint64_t>> log;
    UsbBusHost host;
    uint64_t now = 0, nextHostTick = 0;
    Bench() : host([] { return udc; },
                   UsbBusHost::Link{[this] { return space - std::min(space, toHost.size()); },
                                    [this](const uint8_t* d, size_t n) { toHost.insert(toHost.end(), d, d + n); return n; },
                                    [this](uint8_t* d, size_t n) {
                                        size_t i = 0;
                                        for (; i < n && !fromHost.empty(); ++i) { d[i] = fromHost.front(); fromHost.pop_front(); }
                                        return i;
                                    },
                                    [this] { return fromHost.size(); }}) {
        if (udc) free(udc);
        udc = pxa270UdcInit(nullptr, nullptr, nullptr);
        pxa270UdcHostSetNotify(udc, [](void* host) { static_cast<UsbBusHost*>(host)->Poke(); }, &host);
        irq = false;
        host.log = &log;
    }
    // Guest in 1 us steps. The host runs as in SocPXA: on the 9766 ns peripheral batch
    // grid, only once NextWake has come (a guest UDC write pokes it), and the emulator
    // loop checks the link every 1 ms slice.
    void run(uint64_t ns) {
        const uint64_t end = now + ns;
        for (; now < end; now += 1000) {
            guest.now = now;
            guest.tick();
            if (now % 1000000 == 0 && host.LinkWaiting()) host.Poke();
            if (now >= nextHostTick) {
                if (host.NextWake() <= now) host.Tick(now);
                nextHostTick += 9766;
            }
        }
    }
    bool runUntil(UsbBusHost::Phase phase, uint64_t limitNs, const std::string& port = "") {
        for (uint64_t spent = 0; spent < limitNs; spent += 1000000) {
            if (host.GetPhase() == phase && (port.empty() || host.PortFunction() == port)) return true;
            run(1000000);
        }
        return host.GetPhase() == phase && (port.empty() || host.PortFunction() == port);
    }
};

static void dump(Bench& b) {
    for (auto& m : b.host.TakeMessages()) std::printf("  %s\n", m.c_str());
}

static void testEnumerationAndFrames() {
    Bench b;
    b.guest.enable();
    b.host.Attach();
    b.run(1000);
    assert(b.host.GetPhase() == UsbBusHost::Phase::connect && b.host.NextWake() == UsbBusHost::connectNs);
    if (!b.runUntil(UsbBusHost::Phase::running, 2000000000ull, "cnys")) { dump(b); assert(false); }
    assert(b.host.Vendor() == 0x0830 && b.host.Product() == 0x0061);
    assert(b.host.InEndpoint() == 6 && b.host.OutEndpoint() == 7 && pxa270UdcHostAddress(udc) == UsbBusHost::deviceAddress);
    // The guest saw only forwarded requests: device(8), device(18), config(9), config(46), vendor 4.
    const auto& r = b.guest.requests;
    assert(r.size() == 5 && r[0][1] == 6 && r[0][6] == 8 && r[1][6] == 18 && r[2][3] == 2 && r[2][6] == 9 &&
           r[3][6] == 46 && r[4][0] == 0xc2 && r[4][1] == 4 && r[4][6] == 20);
    assert(((rd(0) >> 11) & 3) == 1 && udc->memSwitched);
    // Every transaction stays inside one frame and starts after that frame's SOF.
    for (auto& t : b.log) {
        assert(t.first / UsbBusHost::frameNs == (t.second - 1) / UsbBusHost::frameNs);
        assert(t.first % UsbBusHost::frameNs >= 2800);
    }
    assert(udc->udcfnr == (b.host.GetCounters().frames & 0x7ff));
    std::printf("PASS: enumeration 0830:0061 at address %u, port 'cnys' IN 6 OUT 7; %zu transactions inside frames\n",
                UsbBusHost::deviceAddress, b.log.size());

    // Transparent bytes both ways: a NetSync packet split over two OUT packets, 300 bytes IN.
    std::vector<uint8_t> net = {0x01, 0x0d, 0x00, 0x00, 0x00, 0x40};
    for (unsigned i = 0; i < 64; ++i) net.push_back(uint8_t(0x90 + i));
    b.fromHost.assign(net.begin(), net.end());
    std::vector<uint8_t> payload(300);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 13 + 1);
    b.guest.toSend.assign(payload.begin(), payload.end());
    b.run(20000000);
    assert(b.guest.received == net);
    assert(std::vector<uint8_t>(b.toHost.begin(), b.toHost.end()) == payload);
    assert(!b.guest.outToB && !udc->fifo[2].count);
    puts("PASS: bridge: 70-byte NET packet to OUT 7 and 300 bytes from IN 6 arrive unchanged; nothing on EP2");

    // Flow control: no room on the host side, no IN tokens; the packet waits in the FIFO.
    b.toHost.clear();
    b.space = 0;
    b.guest.toSend.assign(payload.begin(), payload.begin() + 40);
    b.run(10000000);
    assert(b.toHost.empty() && udc->fifo[6].count == 1);
    b.space = 4096;
    b.run(5000000);
    assert(b.toHost.size() == 40 && !udc->fifo[6].count);
    puts("PASS: bridge: IN tokens stop while the host side is full and resume after");

    // UDE off and on between two host ticks: the device left the bus, enumerate again.
    const auto before = b.host.GetCounters().enumerations;
    wr(0, 0); b.guest.enable();
    b.guest.requests.clear();
    b.run(20000);
    assert(b.host.GetPhase() == UsbBusHost::Phase::connect);
    assert(b.runUntil(UsbBusHost::Phase::running, 2000000000ull, "cnys"));
    assert(b.host.GetCounters().enumerations == before + 1 && b.guest.requests.size() == 5);
    puts("PASS: a UDE 0->1 the host did not see as 0 still re-enumerates");

    // Idle on the HotSync port, no SOF interrupt: the host wakes about once a frame.
    const auto idleTicks = b.host.GetCounters().ticks, idleNaks = b.host.GetCounters().naks;
    b.run(1000000000ull);
    const auto ticks = b.host.GetCounters().ticks - idleTicks;
    assert(ticks >= 900 && ticks <= 2100 && b.host.GetCounters().naks > idleNaks);
    assert(udc->udcfnr == (b.host.GetCounters().frames & 0x7ff));
    std::printf("PASS: idle bridge sleeps between frames: %llu host ticks in 1 s, UDCFNR kept\n", (unsigned long long)ticks);

    b.host.Detach();
    assert(b.host.NextWake() == UINT64_MAX);
    const auto transactions = b.host.GetCounters().transactions;
    b.run(5000000);
    assert(b.host.GetCounters().transactions == transactions);
    puts("PASS: detach stops all bus traffic");
}

static void testPortLater() {
    Bench b;
    b.guest.ports = 0;  // outside HotSync the TX may list no port
    b.guest.enable();
    b.host.Attach();
    assert(b.runUntil(UsbBusHost::Phase::running, 2000000000ull));
    assert(!b.host.InEndpoint() && b.host.PortFunction().empty());
    b.fromHost = {1, 2, 3};
    const auto requests = b.guest.requests.size();
    b.run(2500000000ull);
    // Asked again each second, never sent bulk data anywhere.
    assert(b.guest.requests.size() >= requests + 2 && b.guest.received.empty() && !b.guest.outToB);
    assert(b.fromHost.size() == 3);
    b.guest.ports = 1;
    std::memcpy(b.guest.function, "cnys", 4);
    assert(b.runUntil(UsbBusHost::Phase::running, 1500000000ull, "cnys"));
    b.run(5000000);
    assert(b.guest.received == (std::vector<uint8_t>{1, 2, 3}));
    puts("PASS: no port listed: no bulk tokens, asked again each second, bridged once 'cnys' appears");

    // GET_EXT_CONNECTION_INFORMATION stalls: the Visor request names the HotSync port.
    Bench v;
    v.guest.stallVendor4 = true;
    v.guest.enable();
    v.host.Attach();
    assert(v.runUntil(UsbBusHost::Phase::running, 2000000000ull, "hotsync"));
    assert(v.host.InEndpoint() == 6 && v.host.OutEndpoint() == 6);
    puts("PASS: vendor request 0x04 STALLs: 0x03 picks the HotSync port");
}

static void testFailures() {
    // UDE never set: the host waits and sends nothing.
    Bench b;
    b.guest.silent = true;
    b.host.Attach();
    b.run(300000000);
    assert(b.host.GetPhase() == UsbBusHost::Phase::waitEnable && b.log.empty());
    // A device that never answers: a few resets, then the host gives up.
    wr(0x404, ccr(true, 1)); wr(0, 1);
    b.run(300000000);
    assert(b.host.GetPhase() == UsbBusHost::Phase::waitEnable && b.log.empty());  // EP0/reset IRQs off
    wr(4, 1); wr(8, 1u << 27);
    b.run(UsbBusHost::maxAttempts * (UsbBusHost::connectNs + UsbBusHost::resetNs + UsbBusHost::controlTimeoutNs +
                                    UsbBusHost::retryNs + 20000000ull));
    assert(b.host.GetPhase() == UsbBusHost::Phase::failed && b.host.NextWake() == UINT64_MAX);
    bool gaveUp = false;
    for (auto& m : b.host.TakeMessages()) gaveUp = gaveUp || m.find("gave up") != std::string::npos;
    assert(gaveUp);
    puts("PASS: no traffic until UDE and the EP0/reset interrupts are on; a silent device gets a few resets, then the host gives up");
}

static void testReenableAndLostSetup() {
    Bench b;
    b.guest.enable();
    b.host.Attach();
    assert(b.runUntil(UsbBusHost::Phase::running, 2000000000ull, "cnys"));
    // UDE off and on with UDCICR left as it was: not listening until the ICR is written.
    wr(0, 0); wr(0, 1);
    b.run(300000000);
    assert(b.host.GetPhase() == UsbBusHost::Phase::waitEnable && !pxa270UdcHostListening(udc));
    const auto waitingTicks = b.host.GetCounters().ticks;
    b.run(1000000000ull);
    assert(b.host.GetCounters().ticks == waitingTicks);  // asleep until the guest writes
    // The guest loses the first SETUP while it starts up; the host sends it again.
    b.guest.dropSetup = true;
    b.guest.requests.clear();
    b.host.TakeMessages();
    wr(4, 3); wr(8, (1u << 31) | (1u << 27));
    assert(pxa270UdcHostListening(udc));
    assert(b.runUntil(UsbBusHost::Phase::running, 2000000000ull, "cnys"));
    bool resent = false, restarted = false;
    for (auto& m : b.host.TakeMessages()) {
        resent = resent || m.find("sending it again") != std::string::npos;
        restarted = restarted || m.find("timed out") != std::string::npos;
    }
    assert(resent && !restarted && b.host.GetCounters().setupRetries == 1 && b.guest.requests.size() == 5);
    puts("PASS: UDE 1->0->1 waits for a new UDCICR write; a lost SETUP is sent again after 100 ms, no restart");

    // A port list that takes 300 ms is waited for, not asked again.
    wr(0, 0); b.guest.enable();
    b.guest.vendorDelay = 300000000;
    b.guest.requests.clear();
    b.run(20000);
    assert(b.host.GetPhase() == UsbBusHost::Phase::connect);
    assert(b.runUntil(UsbBusHost::Phase::running, 2000000000ull, "cnys"));
    if (b.host.GetCounters().setupRetries != 1 || b.guest.requests.size() != 5) {
        std::printf("retries %llu requests %zu\n", (unsigned long long)b.host.GetCounters().setupRetries, b.guest.requests.size());
        for (auto& r : b.guest.requests) std::printf("  %02x %02x %02x len %u\n", r[0], r[1], r[3], r[6]);
        dump(b);
        assert(false);
    }
    puts("PASS: a slow vendor request is waited for without a second SETUP");
}

static bool waitReadable(int fd) {
    pollfd p{fd, POLLIN, 0};
    return poll(&p, 1, 2000) == 1;
}

static void testByteLink() {
    HostByteLink link;
    std::string error;
    assert(link.Open({HostSerial::Kind::pty, 0}, error) && link.Connected());
    const std::string path = link.Endpoint();
    const int slave = open(path.c_str(), O_RDWR | O_NOCTTY);
    assert(slave >= 0);
    termios raw{}; tcgetattr(slave, &raw); cfmakeraw(&raw); tcsetattr(slave, TCSANOW, &raw);
    assert(write(slave, "\x01\x00\x00\x00\x00\x06hello!", 12) == 12);
    uint8_t got[64]; size_t n = 0;
    for (int i = 0; i < 200 && n < 12; ++i) { n += link.Receive(got + n, sizeof(got) - n); if (n < 12) usleep(5000); }
    assert(n == 12 && !std::memcmp(got, "\x01\x00\x00\x00\x00\x06hello!", 12));
    assert(link.SendSpace() == HostSerial::queueBytes && link.Send(reinterpret_cast<const uint8_t*>("palm"), 4) == 4);
    assert(waitReadable(slave));
    char back[8] = {}; assert(read(slave, back, sizeof(back)) == 4 && !std::memcmp(back, "palm", 4));
    close(slave);
    link.Close();
    assert(!link.IsOpen() && !link.SendSpace() && !link.Send(got, 1));
    puts("PASS: HostByteLink on a PTY moves bytes both ways and closes");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    testEnumerationAndFrames();
    testPortLater();
    testReenableAndLostSetup();
    testFailures();
    testByteLink();
    free(udc);
}
