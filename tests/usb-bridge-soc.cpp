// UsbHostBridge on a synthetic TX SoC (no firmware): the cable is plugged on emulated
// time inside Run, the host sleeps while the UDC is off, a read-only checkpoint restore
// leaves the bridge alone, a load replugs, and off puts back a cable a save left out.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include "CPU.h"
#include "ReadOnlyGuest.h"
#include "UsbHostBridge.h"
#include "soc_PXA.h"

static std::vector<uint8_t> saved(SoC& soc) {
    assert(soc.Save());
    const auto state = soc.GetSavestate();
    const auto* data = static_cast<const uint8_t*>(state.data);
    return std::vector<uint8_t>(data, data + state.size);
}

static void exercise(DeviceType5 type) {
    const bool tx=type==deviceTypeTX;
    std::vector<uint8_t> rom((tx?16:8) * 1024 * 1024), nand(tx?132 * 1024 * 1024:34603008, 0xff);
    SocPXA soc(type, (tx?32:16) * 1024 * 1024, rom.data(), rom.size(), nand.data(), nand.size(), -1, tx?2:0);
    auto* cpu = soc.GetCpu();
    const uint32_t branch = 0xeafffffe;
    std::memcpy(static_cast<uint8_t*>(soc.GetMemoryData().data) + 0x3000, &branch, 4);
    cpuSetReg(cpu, 15, 0xa0003000);
    auto run = [&](unsigned ms) { for (unsigned i = 0; i < ms; ++i) assert(soc.Run(100000, 100000000) >= 100000); };
    auto udc = [&](unsigned offset, uint32_t value) { assert(cpuMemOpExternal(cpu, &value, 0x40600000 + offset, 4, true)); };
    assert(soc.UsbTestCableAttached()==tx); // TX cable in by default, E2 cable out.
    assert(soc.SetUsbTestCable(true));

    UsbHostBridge bridge;
    std::string error;
    assert(bridge.Start(&soc, {HostSerial::Kind::pty, 0}, error));
    assert(!soc.UsbTestCableAttached());
    // Plugged in 500 ms of emulated time later by the peripheral batch; Service is not called.
    run(499);
    assert(!soc.UsbTestCableAttached());
    run(2);
    assert(soc.UsbTestCableAttached());
    const auto* host = bridge.Host();
    assert(host && host->GetPhase() == UsbBusHost::Phase::waitEnable);
    // UDC off: the host sleeps until the guest writes to it.
    auto ticks = host->GetCounters().ticks;
    run(1000);
    assert(host->GetCounters().ticks - ticks <= 2);
    puts("PASS: the cable goes in on emulated time inside Run; the host sleeps while the UDC is off");

    // EP1 IN, UDE, then the EP0 and reset interrupts: connect wait, reset, first SETUP.
    if(tx)udc(0x404, 1u | 64u << 2 | 1u << 12 | 2u << 13 | 1u << 15 | 1u << 25);
    udc(0, 1);
    run(200);
    assert(host->GetPhase() == UsbBusHost::Phase::waitEnable);  // no UDCICR write yet
    if(tx){udc(4, 1);udc(8, 1u << 27);}
    else udc(0x50,0xfe);
    run(50);
    assert(host->GetPhase() == UsbBusHost::Phase::connect);
    run(80);
    assert(host->GetPhase() == UsbBusHost::Phase::enumerate);
    // Nobody answers EP0: NAKs wait for a guest write or the next frame, not every 10 us.
    ticks = host->GetCounters().ticks;
    run(100);
    const auto busy = host->GetCounters().ticks - ticks;
    assert(busy > 50 && busy < 400);
    std::printf("PASS: listening needs a UDCICR write after UDE; %llu host ticks in 100 ms of NAKs\n",
                (unsigned long long)busy);

    // A read-only checkpoint (database list, export) restores without a replug.
    const auto phase = host->GetPhase();
    { ReadOnlyGuest guest(soc); run(5); }
    bridge.Service();
    run(1);
    assert(soc.UsbTestCableAttached() && host->GetPhase() == phase);
    puts("PASS: a read-only checkpoint restore leaves the cable and the host as they were");

    // A load that fails and is rolled back leaves it too.
    std::vector<uint8_t> junk(256, 0x5a);
    assert(!soc.Load(junk.size(), junk.data()));
    bridge.Service();
    run(1);
    assert(soc.UsbTestCableAttached() && host->GetPhase() == phase);
    puts("PASS: a failed, rolled-back load leaves the cable and the host as they were");

    // A load replugs: out on the next tick, in again 500 ms later.
    auto state = saved(soc);
    assert(soc.Load(state.size(), state.data()));
    bridge.Service();
    run(1);
    assert(!soc.UsbTestCableAttached() && host->GetPhase() == UsbBusHost::Phase::detached);
    const auto unplugged = saved(soc);  // saved in the replug window: GPIO13 high
    run(497);
    assert(!soc.UsbTestCableAttached());
    run(4);
    assert(soc.UsbTestCableAttached());
    puts("PASS: a load unplugs the cable and plugs it in again 500 ms later");

    // That save loaded with USB off gets the cable back, as the TX has it without a bridge.
    bridge.Stop();
    assert(soc.UsbTestCableAttached()==tx);
    UsbHostBridge off;
    assert(off.Start(&soc, {}, error) && off.Describe() == "off");
    assert(soc.Load(unplugged.size(), const_cast<uint8_t*>(unplugged.data())));
    assert(!soc.UsbTestCableAttached());
    off.Service();
    assert(soc.UsbTestCableAttached()==tx);
    off.Stop();
    assert(soc.Load(unplugged.size(), const_cast<uint8_t*>(unplugged.data())));
    assert(!soc.UsbTestCableAttached());
    UsbHostBridge later;
    assert(later.Start(&soc, {}, error) && soc.UsbTestCableAttached()==tx);
    later.Stop();
    run(10);
    puts("PASS: USB off puts back a cable a save left out, at start and after a load");

    // An endpoint that can't open leaves USB off, with the same cable handling.
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 1) == 0);
    socklen_t length = sizeof(address);
    assert(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    assert(soc.Load(unplugged.size(), const_cast<uint8_t*>(unplugged.data())));
    assert(!soc.UsbTestCableAttached());
    UsbHostBridge failed;
    error.clear();
    assert(!failed.Start(&soc, {HostSerial::Kind::tcp, ntohs(address.sin_port)}, error) && !error.empty());
    assert(soc.UsbTestCableAttached()==tx && failed.Describe() == "off");
    assert(soc.Load(unplugged.size(), const_cast<uint8_t*>(unplugged.data())));
    assert(!soc.UsbTestCableAttached());
    failed.Service();
    assert(soc.UsbTestCableAttached()==tx);
    failed.Stop();
    close(listener);
    puts("PASS: a port in use leaves USB off and the cable in, also after a load");
}

int main(){exercise(deviceTypeTX);exercise(deviceTypeE2);}
