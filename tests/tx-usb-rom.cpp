// Optional EP0 check against a user-supplied TX session: the ROM's USB driver answers
// GET_DESCRIPTOR through the UDC model. No firmware data or instruction addresses are
// embedded here; no session is written back. Exit 0 = pass, 1 = failure.
// Address, configuration, bulk endpoints and HotSync are not covered.
#include <cstdio>
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>
#include <unistd.h>
#include "CPU.h"
#include "session/session_file5.h"
#include "soc_PXA.h"
#include "usb-host-fixture.h"
#include "usb-frame-host.h"

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static std::vector<uint8_t> readFile(const char* path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    require(bool(in) && in.tellg() > 0 && in.tellg() <= 512 * 1024 * 1024, "invalid session file");
    std::vector<uint8_t> bytes(size_t(in.tellg())); in.seekg(0);
    require(bool(in.read(reinterpret_cast<char*>(bytes.data()), bytes.size())), "short session read");
    return bytes;
}
struct Machine {
    std::vector<uint8_t> nor, nand;
    std::unique_ptr<SocPXA> soc;
    explicit Machine(const std::vector<uint8_t>& bytes) {
        SessionFile5 session;
        require(session.Deserialize(bytes.size(), bytes.data()), "invalid session");
        require(session.GetDeviceName() == "palm-tx", "TX session required");
        auto copy = [](const void* data, size_t size) {
            auto* begin = static_cast<const uint8_t*>(data);
            return std::vector<uint8_t>(begin, begin + size);
        };
        nor = copy(session.GetNor(), session.GetNorSize());
        nand = copy(session.GetNand(), session.GetNandSize());
        soc = std::make_unique<SocPXA>(deviceTypeTX, session.GetRamSize(), nor.data(), nor.size(),
                                      nand.data(), nand.size(), -1, 2);
        require(soc->GetMemoryData().size == session.GetMemorySize(), "incomplete session RAM");
        std::memcpy(soc->GetMemoryData().data, session.GetMemory(), session.GetMemorySize());
        auto state = copy(session.GetSavestate(), session.GetSavestateSize());
        require(soc->Load(state.size(), state.data()), "session restore failed");
    }
    static constexpr uint64_t hz = 50000000; // guest cycles per emulated second
    void run(unsigned cycles) { require(soc->Run(cycles, hz) >= cycles, "CPU stopped"); }
    // Emulated time for a bus event, rounded up to whole cycles.
    void runNs(uint64_t ns) { run(unsigned((ns * hz + 999999999) / 1000000000)); }
    void prepare() {
        run(50000000);
        auto* u = soc->GetUsbController(); require(u, "TX UDC missing");
        require(soc->SetUsbTestCable(false), "cable detector unavailable");
        for (unsigned i = 0; i < 10000 && (u->udcicr[1] & (1u << 27)); ++i) run(250000);
        require(!(u->udcicr[1] & (1u << 27)), "firmware did not detect cable removal");
        require(soc->SetUsbTestCable(true), "cable detector unavailable");
        for (unsigned i = 0; i < 10000 && !(u->udcicr[1] & (1u << 27)); ++i) run(250000);
        require((u->udccr & 1) && (u->udcicr[1] & (1u << 27)), "firmware did not initialize USB after attachment");
        run(500000);
        unsigned enabled = 0, groups[256] = {};
        for (unsigned i = 1; i < 24; ++i) {
            const uint32_t c = u->ep[i].ccra;
            if (!(c & 1)) continue;
            ++enabled;
            const unsigned size = (c >> 2) & 1023, type = (c >> 13) & 3;
            require((type == 1 && size) || (type == 2 && (size == 8 || size == 16 || size == 32 || size == 64)) ||
                    (type == 3 && size && size <= 64), "firmware endpoint packet size is invalid");
            const unsigned group = (c >> 19) & 255;
            groups[group] += ((size + 3) & ~3u) * ((c & 2) ? 2 : 1);
            require(groups[group] <= 4064, "firmware endpoint memory exceeds documented capacity");
        }
        require(enabled, "firmware has not configured data endpoints");
        std::printf("USB_PROBE cable_detect=ok enabled_endpoints=%u memory_sizes=valid\n", enabled);
        UsbHostFixture host{u}; require(host.resetStimulus(), "EP0 PIO/reset prerequisite failed");
        for (unsigned i = 0; i < 1000 && (u->udcisr[1] & (1u << 27)); ++i) run(1000);
        require(!(u->udcisr[1] & (1u << 27)), "firmware did not acknowledge reset IRQ");
        run(500000);
    }
};
static std::string hex(const std::vector<uint8_t>& bytes) {
    std::string out; char b[4];
    for (auto v : bytes) { std::snprintf(b, sizeof(b), "%02x", v); out += b; }
    return out;
}
// One control read as a full-speed host would run it. Each IN is issued only after the
// guest set IPR and the packet's bus time has passed, never at the instruction where
// IPR appears. Returns the data stage; `afterFirst` runs between packets 1 and 2.
struct Transfer { std::vector<uint8_t> data; unsigned packets = 0, inInsideHandler = 0; bool zlp = false, zlpFlushed = false; };
template<class F>
static Transfer controlRead(Machine& m, UsbHostFixture& host, const uint8_t setup[8], F afterFirst) {
    Transfer t;
    const unsigned length = setup[6] | setup[7] << 8;
    require(host.setup(setup), "SETUP refused");
    for (;;) {
        unsigned i = 0;
        for (; i < 2000000 && !(host.udc->ep[0].csr & 2); ++i) m.run(1);
        require(host.udc->ep[0].csr & 2, "firmware did not stage an IN packet");
        m.runNs(UsbHostFixture::inTransactionNs(host.udc->tx0Count));
        if (host.udc->udcisr[0] & 1) ++t.inInsideHandler; // the guest has not acknowledged yet
        UsbHostFixture::Packet packet;
        require(host.in(packet) == UsbHostFixture::Result::Data, "IN not answered");
        t.data.insert(t.data.end(), packet.bytes.begin(), packet.bytes.begin() + packet.size);
        if (++t.packets == 1) afterFirst();
        if (!packet.size) t.zlp = true;
        if (packet.size < 16 || t.data.size() >= length) break;
    }
    require(t.data.size() <= length, "device sent more than wLength");
    // Status stage: zero-length OUT. The ROM reads 0 bytes and releases OPC. A ZLP the
    // ROM armed after an exact multiple of 16 bytes is dropped by this OUT.
    m.run(20000);
    t.zlpFlushed = (host.udc->ep[0].csr & 2) && !host.udc->tx0Count;
    require(host.statusOut(), "status OUT refused");
    for (unsigned i = 0; i < 2000000 && (host.udc->ep[0].csr & 1); ++i) m.run(1);
    m.run(20000); // the ROM acknowledges UDCISR0 after its EP0 handler returns
    if ((host.udc->ep[0].csr & 1) || (host.udc->udcisr[0] & 1))
        std::fprintf(stderr, "status stage: csr0=%#x isr0=%#x icr0=%#x data=%s\n", host.udc->ep[0].csr,
                     host.udc->udcisr[0], host.udc->udcicr[0], hex(t.data).c_str());
    require(!(host.udc->ep[0].csr & 1) && !(host.udc->udcisr[0] & 1), "firmware did not finish the status stage");
    m.runNs(1000000); // next transfer in a later 1 ms frame
    return t;
}
static Transfer controlRead(Machine& m, UsbHostFixture& host, const uint8_t setup[8]) {
    return controlRead(m, host, setup, [] {});
}
static std::array<uint8_t, 8> getDescriptor(uint8_t type, uint8_t index, uint16_t length, uint16_t language = 0) {
    return {0x80, 6, index, type, uint8_t(language), uint8_t(language >> 8), uint8_t(length), uint8_t(length >> 8)};
}
static const char* statusName(UsbFrameHost::Status s) {
    switch (s) {
        case UsbFrameHost::Status::Ok: return "ok";
        case UsbFrameHost::Status::Stall: return "stall";
        case UsbFrameHost::Status::Timeout: return "timeout";
        default: return "error";
    }
}
static std::string csrs(Pxa270Udc* u) {
    std::string out; char b[64];
    for (unsigned n = 1; n < 24; ++n) {
        if (!(u->ep[n].ccra & 1)) continue;
        std::snprintf(b, sizeof(b), " %c:csr=%03x,fifo=%u", 'A' + n - 1, pxa270UdcPrvReadCsrX(u, n), u->fifo[n].count);
        out += b;
    }
    return out;
}
static void enumerate(Machine& m, const std::vector<uint8_t>& configDescriptor, const std::vector<uint8_t>& deviceDescriptor) {
    uint64_t cycles = 0;
    UsbFrameHost bus([&] { return m.soc->GetUsbController(); }, [&](uint64_t ns) {
        const uint64_t target = ns * Machine::hz / 1000000000ull;
        while (cycles < target) { const unsigned step = unsigned(std::min<uint64_t>(target - cycles, 50000000)); m.run(step); cycles += step; }
    });
    auto u = [&] { return m.soc->GetUsbController(); };
    using S = UsbFrameHost::Status;
    // Linux order: reset, 64-byte device descriptor at address 0, reset, SET_ADDRESS.
    bus.reset();
    require(!(u()->udcisr[1] & (1u << 27)), "firmware did not acknowledge bus reset");
    auto first = bus.getDescriptor(1, 0, 64);
    require(first.status == S::Ok && first.data == deviceDescriptor, "device descriptor at address 0 failed");
    bus.reset();
    auto addr = bus.setAddress(9);
    require(addr.status == S::Ok && pxa270UdcHostAddress(u()) == 9 && !u()->ep[0].csr && !(u()->udccr & 2),
            "SET_ADDRESS failed or reached the guest");
    bus.address = 0;
    require(bus.getDescriptor(1, 0, 18).status == S::Timeout, "device still answers address 0");
    bus.address = 9;
    auto device = bus.getDescriptor(1, 0, 18);
    auto header = bus.getDescriptor(2, 0, 9);
    auto config = bus.getDescriptor(2, 0, uint16_t(configDescriptor.size()));
    auto lang = bus.getDescriptor(3, 0, 255);
    require(device.status == S::Ok && device.data == deviceDescriptor && header.status == S::Ok &&
            config.status == S::Ok && config.data == configDescriptor && lang.status == S::Ok,
            "descriptors at the new address differ");
    std::printf("USB_ENUM address=9 device_packets=%u device_naks=%u config_packets=%u frames=%u\n",
                device.packets, device.naks, config.packets, bus.frames);
    auto status = bus.request(0x80, 0, 0, 0, 2);
    auto getConfig0 = bus.request(0x80, 8, 0, 0, 1);
    require(status.status == S::Ok && status.data.size() == 2 && getConfig0.status == S::Ok &&
            getConfig0.data == std::vector<uint8_t>{0}, "GET_STATUS/GET_CONFIGURATION unconfigured");
    require(bus.request(0x81, 10, 0, 0, 1).status == S::Stall, "GET_INTERFACE before configuration not stalled");
    std::printf("USB_ENUM get_status=%s get_configuration=0\n", hex(status.data).c_str());

    // Endpoints from the configuration descriptor.
    struct EpDesc { uint8_t address; unsigned mps, type; };
    std::vector<EpDesc> eps;
    unsigned configValue = configDescriptor[5];
    for (size_t i = 0; i + 1 < configDescriptor.size() && configDescriptor[i];) {
        if (configDescriptor[i + 1] == 5 && i + 6 < configDescriptor.size())
            eps.push_back({configDescriptor[i + 2], unsigned(configDescriptor[i + 4] | configDescriptor[i + 5] << 8),
                           unsigned(configDescriptor[i + 3] & 3)});
        i += configDescriptor[i];
    }
    require(!eps.empty() && configValue >= 1 && configValue <= 3, "configuration descriptor has no endpoints");
    for (unsigned n = 1; n < 24; ++n)
        if (u()->ep[n].ccra & 1) std::printf("USB_ENUM UDCCR%c=%08x\n", 'A' + n - 1, u()->ep[n].ccra);

    // Bulk tokens before SET_CONFIGURATION get no handshake.
    {
        UsbFrameHost::Result r; std::vector<uint8_t> packet;
        require(bus.in(eps[0].address & 15, packet, bus.now, r) == S::Timeout, "bulk IN answered while unconfigured");
    }
    const uint32_t icrBefore = u()->udcicr[0];
    auto set = bus.request(0x00, 9, uint16_t(configValue), 0, 0);
    require(set.status == S::Ok, "SET_CONFIGURATION failed");
    bus.advance(20 * UsbFrameHost::kFrameNs);
    require(((u()->udccr >> 11) & 3) == configValue && u()->memSwitched && !(u()->udcisr[1] & 0x80000000u),
            "firmware did not take the configuration change (IRCC/SMAC)");
    std::printf("USB_ENUM set_configuration=%u acn=%u smac=done icr0 %08x->%08x udcicr1=%08x naks=%u\n", configValue,
                (u()->udccr >> 11) & 3, icrBefore, u()->udcicr[0], u()->udcicr[1], set.naks);
    auto getConfig = bus.request(0x80, 8, 0, 0, 1);
    auto getInterface = bus.request(0x81, 10, 0, 0, 1);
    auto epStatus = bus.request(0x82, 0, 0, eps[0].address, 2);
    require(getConfig.data == std::vector<uint8_t>{uint8_t(configValue)} && getInterface.data == std::vector<uint8_t>{0} &&
            epStatus.data == std::vector<uint8_t>{0, 0}, "configured GET_CONFIGURATION/GET_INTERFACE/GET_STATUS wrong");
    std::printf("USB_ENUM configured csrs:%s\n", csrs(u()).c_str());
    for (unsigned n = 1; n < 24; ++n)
        require(!((u()->ep[n].ccra & 1) && (u()->ep[n].csr & 8)), "firmware enabled endpoint DMA (not modelled)");

    // Palm vendor requests (visor): GET_CONNECTION_INFORMATION 0x03, GET_EXT_CONNECTION_INFORMATION 0x04.
    for (auto req : {std::pair<uint8_t, uint16_t>{3, 18}, {4, 20}}) {
        auto r = bus.request(0xc2, req.first, 0, 0, req.second);
        std::printf("USB_VENDOR request=%u status=%s data=%s\n", req.first, statusName(r.status), hex(r.data).c_str());
    }
    // Bulk IN: the ROM has nothing queued while no application has the port open.
    for (auto& ep : eps) {
        if (!(ep.address & 0x80)) continue;
        auto r = bus.bulkIn(ep.address & 15, ep.mps * 4, ep.mps, 100 * UsbFrameHost::kFrameNs);
        std::printf("USB_BULK in=%02x status=%s bytes=%zu naks=%u\n", ep.address, statusName(r.status), r.data.size(), r.naks);
        require(r.status == S::Timeout && r.data.empty() && r.naks, "bulk IN did not NAK");
    }
    // Whole-machine snapshot while configured; each OUT probe below starts from it.
    require(m.soc->Save(), "snapshot failed");
    auto s = m.soc->GetSavestate(); auto ram = m.soc->GetMemoryData();
    std::vector<uint8_t> state(static_cast<uint8_t*>(s.data), static_cast<uint8_t*>(s.data) + s.size);
    std::vector<uint8_t> memory(static_cast<uint8_t*>(ram.data), static_cast<uint8_t*>(ram.data) + ram.size);
    const uint64_t savedNow = bus.now, savedCycles = cycles, savedNext = bus.nextControlNs;
    auto restore = [&] {
        std::memcpy(m.soc->GetMemoryData().data, memory.data(), memory.size());
        require(m.soc->Load(state.size(), state.data()), "restore failed");
        bus.now = savedNow; cycles = savedCycles; bus.nextControlNs = savedNext;
        require(pxa270UdcHostAddress(u()) == 9 && ((u()->udccr >> 11) & 3) == configValue && u()->memSwitched,
                "restore lost address or configuration");
    };
    // Bulk OUT: one full and one short packet to each OUT endpoint, then check the guest still answers.
    for (auto& ep : eps) {
        if (ep.address & 0x80) continue;
        restore();
        std::vector<uint8_t> payload(ep.mps + 7);
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = uint8_t(i * 7 + ep.address);
        auto r = bus.bulkOut(ep.address, payload, ep.mps, false, 100 * UsbFrameHost::kFrameNs);
        bus.advance(50 * UsbFrameHost::kFrameNs);
        const uint8_t probe[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};
        auto alive = bus.control(probe, {}, 200 * UsbFrameHost::kFrameNs);
        std::printf("USB_BULK out=%02x status=%s packets=%u naks=%u csrs:%s guest_answers_ep0=%s\n", ep.address,
                    statusName(r.status), r.packets, r.naks, csrs(u()).c_str(), statusName(alive.status));
        require(r.packets >= 1, "bulk OUT packet not accepted");
        for (auto& in : eps) {
            if (!(in.address & 0x80) || alive.status != S::Ok) continue;
            auto reply = bus.bulkIn(in.address & 15, in.mps * 4, in.mps, 20 * UsbFrameHost::kFrameNs);
            std::printf("USB_BULK after_out=%02x in=%02x status=%s bytes=%zu\n", ep.address, in.address,
                        statusName(reply.status), reply.data.size());
        }
    }
    restore();
    // Halt and clear on the first endpoint, through the UDC.
    require(bus.request(0x02, 3, 0, eps[0].address, 0).status == S::Ok, "SET_FEATURE(ENDPOINT_HALT) failed");
    auto halted = bus.request(0x82, 0, 0, eps[0].address, 2);
    require(halted.data == std::vector<uint8_t>{1, 0}, "endpoint not halted");
    {
        UsbFrameHost::Result r; std::vector<uint8_t> packet;
        require(bus.in(eps[0].address & 15, packet, bus.now, r) == S::Stall, "halted endpoint did not STALL");
    }
    require(bus.request(0x02, 1, 0, eps[0].address, 0).status == S::Ok, "CLEAR_FEATURE(ENDPOINT_HALT) failed");
    require(bus.request(0x82, 0, 0, eps[0].address, 2).data == std::vector<uint8_t>{0, 0}, "endpoint still halted");
    std::puts("USB_ENUM halt_clear=ok");
    auto again = bus.getDescriptor(1, 0, 18);
    require(again.status == S::Ok && again.data == deviceDescriptor, "control transfer after restore failed");
    std::puts("USB_ENUM snapshot_configured=ok");
}
int main(int argc, char** argv) {
    alarm(300); // bounds even a broken core or unexpected guest loop
    try {
        require(argc == 2, "usage: test-tx-usb-rom PRIVATE_TX_SESSION");
        auto input = readFile(argv[1]);
        Machine m(input); m.prepare(); UsbHostFixture host{m.soc->GetUsbController()};

        // 1: device descriptor with wLength 18 and 64.
        auto device = controlRead(m, host, getDescriptor(1, 0, 18).data());
        require(device.data.size() == 18 && device.data[0] == 18 && device.data[1] == 1, "device descriptor (18) wrong");
        auto device64 = controlRead(m, host, getDescriptor(1, 0, 64).data());
        require(device64.data == device.data, "device descriptor (64) differs");
        std::printf("USB_ROM device=%s packets=%u in_inside_handler=%u\n", hex(device.data).c_str(),
                    device.packets, device.inInsideHandler + device64.inInsideHandler);
        require(device.inInsideHandler == 0 && device64.inInsideHandler == 0,
                "an IN completed before the guest acknowledged its last EP0 event");

        // 2: with no IN for 50 ms, the staged packet stays queued through the ROM's FTF.
        require(host.descriptorSetup(18), "SETUP refused");
        for (unsigned i = 0; i < 2000000 && !(host.udc->ep[0].csr & 2); ++i) m.run(1);
        m.runNs(50000000);
        require((host.udc->ep[0].csr & 2) && host.udc->tx0Count == 16 && host.udc->tx0[0] == 18 &&
                !(host.udc->udcisr[0] & 1), "staged packet did not survive 50 ms without IN");
        UsbHostFixture::Packet packet;
        require(host.in(packet) == UsbHostFixture::Result::Data && packet.size == 16, "late IN failed");
        for (unsigned i = 0; i < 2000000 && !(host.udc->ep[0].csr & 2); ++i) m.run(1);
        m.runNs(UsbHostFixture::inTransactionNs(2));
        require(host.in(packet) == UsbHostFixture::Result::Data && packet.size == 2, "late tail failed");
        m.run(20000); require(host.statusOut(), "status OUT refused");
        for (unsigned i = 0; i < 2000000 && (host.udc->ep[0].csr & 1); ++i) m.run(1);
        require(!(host.udc->ep[0].csr & 1), "status stage after a late IN failed");
        m.runNs(1000000);
        std::puts("USB_ROM queued_50ms=ok");

        // 3: configuration descriptor header, then all of it, then with a large wLength.
        auto header = controlRead(m, host, getDescriptor(2, 0, 9).data());
        require(header.data.size() == 9 && header.data[0] == 9 && header.data[1] == 2, "config header wrong");
        const unsigned total = header.data[2] | header.data[3] << 8;
        require(total >= 9 && total <= 1024, "config wTotalLength implausible");
        auto config = controlRead(m, host, getDescriptor(2, 0, total).data());
        require(config.data.size() == total && std::equal(header.data.begin(), header.data.end(), config.data.begin()),
                "config descriptor wrong");
        auto configLong = controlRead(m, host, getDescriptor(2, 0, 255).data());
        require(configLong.data == config.data, "config descriptor with wLength 255 differs");
        require((total % 16 == 0) == configLong.zlp, "short packet or ZLP ending wrong");
        // wLength 32 ends the data stage on a full packet; the status OUT drops the ZLP the ROM arms.
        auto config32 = controlRead(m, host, getDescriptor(2, 0, 32).data());
        require(config32.data.size() == 32 && std::equal(config32.data.begin(), config32.data.end(), config.data.begin()),
                "config descriptor (32) wrong");
        require(host.in(packet) == UsbHostFixture::Result::Nak, "data left after status stage");
        std::printf("USB_ROM config32=ok zlp_dropped_by_status_out=%d\n", int(config32.zlpFlushed));
        std::printf("USB_ROM config_total=%u packets=%u zlp_at_255=%d config=%s\n", total, config.packets,
                    int(configLong.zlp), hex(config.data).c_str());

        // 4: string descriptor 0 (language IDs).
        auto langs = controlRead(m, host, getDescriptor(3, 0, 255).data());
        require(langs.data.size() >= 4 && langs.data[0] == langs.data.size() && langs.data[1] == 3 &&
                langs.data.size() % 2 == 0, "string descriptor 0 wrong");
        std::printf("USB_ROM string0=%s\n", hex(langs.data).c_str());
        const unsigned language = langs.data[2] | langs.data[3] << 8;
        for (unsigned index : {unsigned(device.data[14]), unsigned(device.data[15]), unsigned(device.data[16])}) {
            if (!index) continue;
            auto str = controlRead(m, host, getDescriptor(3, index, 255, language).data());
            require(str.data.size() >= 2 && str.data[0] == str.data.size() && str.data[1] == 3, "string descriptor wrong");
            std::string ascii;
            for (size_t i = 2; i + 1 < str.data.size(); i += 2) ascii += char(str.data[i + 1] ? '?' : str.data[i]);
            std::printf("USB_ROM string%u=\"%s\"\n", index, ascii.c_str());
        }

        // 5: whole-machine snapshot between packets 1 and 2, then finish from the restore.
        std::vector<uint8_t> state, ram;
        auto first = controlRead(m, host, getDescriptor(1, 0, 18).data(), [&] {
            require(m.soc->Save(), "snapshot failed");
            auto s = m.soc->GetSavestate(); auto r = m.soc->GetMemoryData();
            state.assign(static_cast<uint8_t*>(s.data), static_cast<uint8_t*>(s.data) + s.size);
            ram.assign(static_cast<uint8_t*>(r.data), static_cast<uint8_t*>(r.data) + r.size);
        });
        require(first.data == device.data && !state.empty(), "transfer around snapshot failed");
        std::memcpy(m.soc->GetMemoryData().data, ram.data(), ram.size());
        require(m.soc->Load(state.size(), state.data()), "restore failed");
        host.udc = m.soc->GetUsbController();
        for (unsigned i = 0; i < 2000000 && !(host.udc->ep[0].csr & 2); ++i) m.run(1);
        m.runNs(UsbHostFixture::inTransactionNs(host.udc->tx0Count));
        require(host.in(packet) == UsbHostFixture::Result::Data && packet.size == 2 &&
                packet.bytes[0] == device.data[16] && packet.bytes[1] == device.data[17], "tail after restore wrong");
        m.run(20000); require(host.statusOut(), "status OUT after restore refused");
        for (unsigned i = 0; i < 2000000 && (host.udc->ep[0].csr & 1); ++i) m.run(1);
        require(!(host.udc->ep[0].csr & 1), "status stage after restore failed");
        m.runNs(1000000);
        auto again = controlRead(m, host, getDescriptor(1, 0, 18).data());
        require(again.data == device.data, "descriptor after restore differs");
        std::puts("USB_ROM snapshot_between_packets=ok");

        require(host.resetStimulus() && !host.udc->tx0Count && !host.udc->ep[0].bcr, "reset stimulus did not clear FIFO");
        m.run(500000);

        // 6: enumeration to a configured device and bulk data on the frame-scheduled host.
        enumerate(m, config.data, device.data);
        std::puts("PASS: TX ROM answers GET_DESCRIPTOR (device, configuration, strings) over EP0 with bus-timed INs");
        std::puts("PASS: enumeration to configured on the frame-scheduled host; bulk endpoints probed");
        std::puts("Not covered: Palm USB serial protocol, HotSync.");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1;
    }
}
