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
void pxaDmaExternalReq(PxaDma*,uint_fast8_t,bool){}
void pxaDmaSetPeripheral(PxaDma*,uint_fast8_t,const PxaDmaPeripheral*){}
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
    unsigned inMps = 64, outMps = 64;
    std::vector<unsigned> outPackets;
    Guest() {
        device = {18, 1, 0x00, 0x02, 0, 0, 0, 16, 0x30, 0x08, 0x61, 0x00, 0x00, 0x01, 1, 2, 0, 1};
        config = {9, 2, 46, 0, 1, 1, 0, 0xc0, 0, 9, 4, 0, 0, 4, 0xff, 0, 0, 0};
        for (uint8_t e : {0x81, 0x02, 0x86, 0x07}) config.insert(config.end(), {7, 5, e, 2, 64, 0, 0});
        assert(config.size() == 46);
    }
    void enable() {
        wr(0x404, ccr(true, 1)); wr(0x408, ccr(false, 2)); wr(0x418, (ccr(true, 6) & ~(0x3ffu << 2)) | inMps << 2); wr(0x41c, (ccr(false, 7) & ~(0x3ffu << 2)) | outMps << 2);
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
            if (req[0] == 0x80 && req[1] == 6 && req[3] == 3) {
                if (req[2] == 0) return answer({4, 3, 9, 4}, length);
                return answer({6, 3, 'U', 0, uint8_t('0' + req[2]), 0}, length);
            }
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
            while (n < inMps && !toSend.empty()) { wr8(0x318, toSend.front()); toSend.pop_front(); ++n; }
            if (n < inMps) wr(0x118, 0x80);
        }
    }
    void epOut() {
        if (rd(0x11c) & 0x10) wr(0x11c, 0x10);
        if (rd(0x108) & 2) outToB = true;
        for (uint32_t csr; (csr = rd(0x11c)) & 3;) {
            const unsigned count = rd(0x21c);
            outPackets.push_back(count);
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
    struct InEvent { uint64_t ns; UsbPacketResult result; unsigned size; };
    std::vector<InEvent> inEvents;
    unsigned timeoutBudget = 0, timedOut = 0, configSetups = 0;
    bool configStatus = false, outCredit = true;
    unsigned outCalls = 0, creditQueries = 0;
    UsbDevicePort port() {
        using R = UsbPacketResult;
        static const UsbDevicePort::Operations ops{
            [](void*) { return UsbDevicePort(udc).Reset(); },
            [](void*) { return UsbDevicePort(udc).Frame(); },
            [](void*) { return UsbDevicePort(udc).Enabled(); },
            [](void*) { return UsbDevicePort(udc).Listening(); },
            [](void*) { return UsbDevicePort(udc).SofWanted(); },
            [](void*) { return UsbDevicePort(udc).EnableCount(); },
            [](void*) { return UsbDevicePort(udc).Address(); },
            [](void*, void (*notify)(void*), void* ctx) { UsbDevicePort(udc).SetNotify(notify,ctx); },
            [](void* ctx, const uint8_t* d) {
                auto& b=*static_cast<Bench*>(ctx);
                b.configStatus = d[1]==9;
                if(b.configStatus) ++b.configSetups;
                return UsbDevicePort(udc).Setup(d);
            },
            [](void*, const uint8_t* d, unsigned n) { return UsbDevicePort(udc).Out(d,n); },
            [](void* ctx, uint8_t* d, unsigned* n) {
                auto& b=*static_cast<Bench*>(ctx);
                if(b.configStatus && b.timedOut < b.timeoutBudget) { ++b.timedOut; return R::Timeout; }
                return UsbDevicePort(udc).In(d,n);
            },
            [](void* ctx, uint8_t e, const uint8_t* d, unsigned n) { ++static_cast<Bench*>(ctx)->outCalls; return UsbDevicePort(udc).EpOut(e,d,n); },
            [](void* ctx, uint8_t e, uint8_t* d, unsigned* n) {
                auto& b=*static_cast<Bench*>(ctx); auto result=UsbDevicePort(udc).EpIn(e,d,n);
                b.inEvents.push_back({b.now,result,result==R::Ack?*n:0}); return result;
            },
            [](void* ctx, uint8_t e, unsigned* n) {
                auto& b=*static_cast<Bench*>(ctx);
                if(!e && b.configStatus && b.timedOut < b.timeoutBudget) return R::Timeout;
                return UsbDevicePort(udc).PeekIn(e,n);
            },
            [](void* ctx, uint8_t, unsigned) { auto& b=*static_cast<Bench*>(ctx); ++b.creditQueries; return b.outCredit; }
        };
        return {this,ops};
    }
    std::deque<uint8_t> fromHost, toHost;  // the host side of the link
    size_t space = 4096;
    std::vector<std::pair<uint64_t, uint64_t>> log;
    UsbBusHost host;
    uint64_t now = 0, nextHostTick = 0;
    Bench() : host([this] { return port(); },
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

static void testControlNoResponseRetry() {
    {
        Bench b;
        b.timeoutBudget=2;
        b.guest.enable(); b.host.Attach();
        assert(b.runUntil(UsbBusHost::Phase::running, 2000000000ull, "cnys"));
        assert(b.timedOut==2 && b.configSetups==1);
        assert(b.host.GetCounters().enumerations==1);
    }
    {
        Bench b;
        b.timeoutBudget=1000;
        b.guest.enable(); b.host.Attach();
        assert(b.runUntil(UsbBusHost::Phase::failed, 8000000000ull));
        assert(b.timedOut==3*UsbBusHost::maxAttempts);
        assert(b.configSetups==UsbBusHost::maxAttempts);
        assert(b.host.GetCounters().bytesIn==0 && b.host.GetCounters().bytesOut==0);
    }
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
    // The Visor reply selects both directions of endpoint 6.
    v.guest.config[2] += 7; v.guest.config[13] += 1;
    v.guest.config.insert(v.guest.config.end(), {7, 5, 6, 2, 64, 0, 0});
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

static void testStringsAndControlOwnership() {
    Bench b;
    b.guest.ports = 0;
    unsigned strings = 0;
    b.host.controlTrace = [&](const uint8_t* setup, const std::vector<uint8_t>& data, bool ok) {
        if (setup[1] == 6 && setup[3] == 3) {
            assert(ok && data.size() >= 4 && data[1] == 3);
            ++strings;
        }
    };
    b.guest.enable(); b.host.Attach();
    assert(b.runUntil(UsbBusHost::Phase::running, 2000000000ull));
    assert(strings == 0 && !b.host.InEndpoint() && !b.host.OutEndpoint());
    assert(b.host.ReadStringDescriptor(1, 0x0409));
    assert(!b.host.ReadStringDescriptor(2, 0x0409));
    b.run(20000000);
    assert(strings == 1 && !b.host.InEndpoint() && !b.host.OutEndpoint());
    // A diagnostic completion may submit another diagnostic without replacing enumeration.
    b.host.controlTrace = [&](const uint8_t* setup, const std::vector<uint8_t>&, bool ok) {
        if (setup[1] == 6 && setup[3] == 3 && setup[2] == 1) {
            assert(ok && b.host.ReadStringDescriptor(2, 0x0409));
        }
    };
    assert(b.host.ReadStringDescriptor(1, 0x0409));
    b.run(20000000);
    assert(!b.host.InEndpoint() && !b.host.OutEndpoint());
    b.guest.ports = 1;
    assert(b.runUntil(UsbBusHost::Phase::running, 1500000000ull, "cnys"));
    assert(b.host.InEndpoint() == 6 && b.host.OutEndpoint() == 7);
    puts("PASS: separate diagnostic completion, idle wake, reentrant callback and later real port discovery");
}

static void testDirectionalMps() {
    for (unsigned outMps : {8u, 16u, 32u, 64u}) {
        Bench b;
        b.guest.outMps = outMps; b.guest.inMps = outMps == 64 ? 16 : 64;
        b.guest.config[36] = uint8_t(b.guest.inMps);
        b.guest.config[43] = uint8_t(outMps);
        b.guest.enable(); b.host.Attach();
        assert(b.runUntil(UsbBusHost::Phase::running, 2000000000ull, "cnys"));
        std::vector<uint8_t> expected;
        for (unsigned round = 0; round < 3; ++round) {
            std::vector<uint8_t> bytes(4097 + round);
            for (unsigned i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(i * 73 + round);
            expected.insert(expected.end(), bytes.begin(), bytes.end());
            b.fromHost.insert(b.fromHost.end(), bytes.begin(), bytes.end());
            b.guest.toSend.insert(b.guest.toSend.end(), bytes.begin(), bytes.end());
            // Hold guest service until both OUT buffers fill and the host retries NAKs.
            b.log.clear();
            b.guest.silent = true; b.run(5000000);
            const uint64_t outNs = ((90 + 8 * outMps) * 1000 + 11) / 12;
            bool reservedPacket = false;
            for (const auto& interval : b.log) {
                const auto ns = interval.second - interval.first;
                assert(ns == outNs || ns == 4500);
                reservedPacket |= ns == outNs;
            }
            assert(reservedPacket);
            const auto naks = b.host.GetCounters().naks;
            b.run(5000000); assert(b.host.GetCounters().naks > naks);
            b.guest.silent = false;
            b.space = expected.size() + 64;
            // At MPS 8, 4097 bytes need over 512 one-packet requests.
            b.run(1000000000);
            if (b.guest.received != expected || !b.fromHost.empty()) std::fprintf(stderr, "MPS %u round %u expected %zu received %zu queued %zu\n", outMps, round, expected.size(), b.guest.received.size(), b.fromHost.size());
            assert(b.guest.received == expected && b.fromHost.empty());
            assert(std::vector<uint8_t>(b.toHost.begin(), b.toHost.end()) == expected);
            assert(b.host.GetCounters().bytesOut == expected.size());
            assert(b.host.GetCounters().bytesIn == expected.size());
        }
        // The controller stalls OUT, then the host clears the halt and retries the same bytes.
        wr(0x11c, 0x20);
        b.fromHost = {9,8,7,6,5};
        expected.insert(expected.end(), {9,8,7,6,5});
        b.run(30000000);
        assert(b.guest.received == expected && b.fromHost.empty());
        assert(b.host.GetCounters().bytesOut == expected.size());
        size_t total = 0;
        for (unsigned n : b.guest.outPackets) { assert(n && n <= outMps); total += n; }
        assert(total == expected.size());
    }
    puts("PASS: directional bulk MPS 8/16/32/64, asymmetric IN, repeated streams, NAK retention and exact accounting");
}

static void testInLimits() {
    for (bool oversized : {false, true}) {
        Bench b;
        b.guest.inMps = oversized ? 64 : 16;
        b.guest.config[36] = 16;
        b.guest.enable(); b.host.Attach();
        assert(b.runUntil(UsbBusHost::Phase::running, 2000000000ull, "cnys"));
        b.space = 16;
        b.guest.toSend.assign(oversized ? 32 : 16, 0x5a);
        b.run(1000000);
        if (oversized) {
            assert(b.host.GetPhase() == UsbBusHost::Phase::connect);
            assert(b.toHost.empty() && b.host.GetCounters().bytesIn == 0 && udc->fifo[6].count == 1);
        } else {
            assert(b.toHost.size() == 16 && b.host.GetCounters().bytesIn == 16);
        }
    }
    puts("PASS: IN uses its own MPS for queue space and rejects oversized packets before ACK");
}

static void testPendingReset() {
    Bench b;
    b.guest.enable(); b.host.Attach();
    assert(b.runUntil(UsbBusHost::Phase::running, 2000000000ull, "cnys"));
    b.guest.silent = true;
    std::vector<uint8_t> bytes(257);
    for (unsigned i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(i);
    b.fromHost.assign(bytes.begin(), bytes.end());
    b.run(5000000);
    const size_t acked = b.host.GetCounters().bytesOut;
    assert(acked && acked < bytes.size() && b.fromHost.size() < bytes.size() - acked);
    // Already ACKed FIFO bytes are intentionally lost at reset. Only unsent bytes continue.
    wr(0, 0);
    b.guest.outMps = 16; b.guest.config[43] = 16;
    b.guest.silent = false; b.guest.enable();
    b.run(300000000);
    assert(b.host.GetPhase() == UsbBusHost::Phase::running);
    assert(b.guest.received == std::vector<uint8_t>(bytes.begin() + acked, bytes.end()));
    assert(b.host.GetCounters().bytesOut == bytes.size() && b.fromHost.empty());
    for (unsigned n : b.guest.outPackets) assert(n && n <= 16);
    puts("PASS: reset retains unACKed bytes, resegments 64 to 16 and never replays ACKed bytes");
}

static void testInvalidDescriptors() {
    for (unsigned variant = 0; variant < 20; ++variant) {
        Bench b;
        auto& c = b.guest.config;
        switch (variant) {
            case 0: c[18] = 0; break;
            case 1: c[18] = 1; break;
            case 2: c[39] = 8; break; // truncated final descriptor
            case 3: c[2] = 47; break;
            case 4: c[2] = 45; break;
            case 5: c[5] = 0; break;
            case 6: c[4] = 2; break;
            case 7: c[13] = 3; break;
            case 8: c[43] = 17; break;
            case 9: c[44] = 1; break;
            case 10: c[41] = 0x87; break; // OUT exists only in wrong direction
            case 11: c[41] = 8; break; // vendor endpoint absent
            case 12: c[42] = 3; break; // selected OUT is interrupt
            case 13: c[12] = 1; break; // no alternate zero
            case 14: c[41] = 0; break;
            case 15: c[41] = 0x77; break;
            case 16: c[41] = 0x86; break; // duplicate active address
            case 17: c[11] = 1; break; // interface number outside configuration
            case 18: c[43] = 0; break;
            case 19: c[42] = 0; break; // selected OUT is control
        }
        b.fromHost = {1,2,3,4,5};
        b.guest.enable(); b.host.Attach();
        assert(b.runUntil(UsbBusHost::Phase::failed, 7000000000ull));
        assert(b.fromHost.size() == 5 && b.guest.received.empty());
        assert(b.host.GetCounters().bytesOut == 0);
    }
    puts("PASS: malformed descriptors and invalid directional/vendor endpoints leave queued bytes untouched");
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

static void testRequestCadence() {
    for(unsigned length : {0u, 7u, 64u}) {
        Bench b; b.guest.enable(); b.host.Attach();
        assert(b.runUntil(UsbBusHost::Phase::running, 2000000000ull, "cnys"));
        b.guest.silent=true; b.inEvents.clear();
        for(unsigned i=0;i<length;++i) wr8(0x318,uint8_t(i));
        if(length<64) wr(0x118,0x80);
        for(unsigned i=0;i<2000 && (b.inEvents.empty() || b.inEvents.back().result!=UsbPacketResult::Ack);++i) b.run(1000);
        assert(!b.inEvents.empty() && b.inEvents.back().result==UsbPacketResult::Ack);
        const auto ack=b.inEvents.back(); assert(ack.size==length);
        const auto count=b.inEvents.size();
        b.fromHost={1,2,3,4};
        // Pokes and masked guest service cannot change the request submission deadline.
        for(unsigned i=0;i<500;++i) { b.host.Poke(); b.run(1000); }
        assert(b.inEvents.size()==count);
        assert(b.host.GetCounters().bytesOut==4); // OUT remains eligible during IN cooldown.
        const auto naks=b.host.GetCounters().naks;
        b.run(3000000);
        assert(b.inEvents.size()>count && b.inEvents[count].result==UsbPacketResult::Nak);
        assert(b.inEvents[count].ns>=ack.ns+UsbBusHost::frameNs);
        assert(b.host.GetCounters().naks>naks); // empty controller is really polled with guest still masked.
        b.guest.epOut(); assert(b.guest.received==std::vector<uint8_t>({1,2,3,4}));
        b.guest.silent=false; b.space=0;
        b.guest.toSend.assign(4096,0x5a); b.run(5000000);
        assert(b.host.GetCounters().bytesIn==length);
        b.space=8192; b.run(120000000);
        assert(b.host.GetCounters().bytesIn==length+4096);
        uint64_t lastAck=0;
        for(const auto& e:b.inEvents) if(e.result==UsbPacketResult::Ack) {
            if(lastAck) assert(e.ns>=lastAck+UsbBusHost::frameNs);
            lastAck=e.ns;
        }
    }
    puts("PASS: bounded MPS request cadence, short/full/ZLP, masked guest real NAK, poke independence, OUT progress and backpressure/throughput");
}

static void testCooperativeOutCredit() {
    Bench b;
    b.outCredit=false;
    b.guest.enable();b.host.Attach();
    assert(b.runUntil(UsbBusHost::Phase::running,1000000000));
    assert(UsbDevicePort(udc).OutReady(7,64)); // Ports without a hook keep normal bus polling.
    std::vector<uint8_t> expected(130);
    for(unsigned i=0;i<expected.size();++i) expected[i]=uint8_t(i*37);
    b.fromHost.assign(expected.begin(),expected.end());
    b.guest.toSend.assign(97,0x5a);
    b.run(20000000);
    assert(b.creditQueries && !b.outCalls && !b.host.GetCounters().bytesOut);
    assert(b.fromHost.size()==66 && b.guest.received.empty()); // One retained 64-byte request.
    assert(b.toHost==std::deque<uint8_t>(97,0x5a)); // IN continues while OUT has no credit.
    assert(b.host.ReadStringDescriptor(1,0x409));b.run(10000000);
    assert(!b.outCalls && !b.host.GetCounters().bytesOut);
    b.outCredit=true;b.host.Poke();b.run(30000000);
    assert(b.guest.received==expected && b.fromHost.empty());
    assert(b.outCalls==3 && b.host.GetCounters().bytesOut==expected.size());
    b.run(3000000);assert(b.outCalls==3);
    b.outCredit=false;b.fromHost.assign(35,0xa5);b.run(3000000);
    assert(b.fromHost.empty() && b.outCalls==3);
    b.host.Detach();b.host.Attach();
    assert(b.runUntil(UsbBusHost::Phase::running,1000000000));
    b.outCredit=true;b.run(5000000);
    assert(b.outCalls==3 && b.guest.received==expected); // Detach cancelled only the unacknowledged request.
    puts("PASS: cooperative OUT credit retains bytes without tokens or ACK accounting, independent IN/control, exact resume and detach cancellation");
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    testCooperativeOutCredit();
    testRequestCadence();
    testControlNoResponseRetry();
    testEnumerationAndFrames();
    testPortLater();
    testReenableAndLostSetup();
    testFailures();
    testStringsAndControlOwnership();
    testDirectionalMps();
    testInLimits();
    testPendingReset();
    testInvalidDescriptors();
    testByteLink();
    free(udc);
}
