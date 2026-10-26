// Optional bounded investigation against a user-supplied TX session. No firmware
// data or instruction addresses are embedded here; no session is written back.
// Exit 2 = reproduced enumeration blocker, 1 = failed/unexpected prerequisites.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>
#include <unistd.h>
#include "CPU.h"
#include "session/session_file5.h"
#include "soc_PXA.h"
#include "usb-host-fixture.h"

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
        require(session.GetDeviceType() == deviceTypeTX, "TX session required");
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
    void run(unsigned cycles) { require(soc->Run(cycles, 50000000) >= cycles, "CPU stopped"); }
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
static void roundTrip(UsbHostFixture& host) {
    Savestate<ChunkType> saved; require(saved.Save(host), "UDC snapshot failed");
    const auto count = host.udc->tx0Count, pos = host.udc->rx0Pos;
    const auto csr = host.udc->ep[0].csr;
    uint8_t bytes[16]; std::memcpy(bytes, host.udc->tx0, 16);
    pxa270UdcPrvFlushTx0(host.udc);
    SavestateLoader<ChunkType> loader;
    require(loader.Load(saved.GetBuffer(), saved.GetSize(), host), "UDC restore failed");
    require(host.udc->tx0Count == count && host.udc->rx0Pos == pos && host.udc->ep[0].csr == csr &&
            !std::memcmp(bytes, host.udc->tx0, 16), "queued response changed across UDC restore");
}
static unsigned trial(const std::vector<uint8_t>& input, bool immediate) {
    Machine m(input); m.prepare(); UsbHostFixture host{m.soc->GetUsbController()};
    require(host.descriptorSetup(18), "setup prerequisite failed");
    for (unsigned i = 0; i < 2000000 && !(host.udc->ep[0].csr & 0x22); ++i) m.run(1);
    require((host.udc->ep[0].csr & 2) && host.udc->tx0Count == 16 &&
            host.udc->tx0[0] == 18 && host.udc->tx0[1] == 1, "firmware did not stage the device descriptor prefix");
    roundTrip(host);
    UsbHostFixture::Packet packet;
    unsigned received = 0;
    if (immediate) {
        require(host.in(packet) == UsbHostFixture::Result::Data && packet.size == 16, "first IN failed");
        received = packet.size;
        // This is deliberately an atomic IN/ACK candidate, not a wire-timed bus.
        for (unsigned i = 0; i < 2000000 && !(host.udc->ep[0].csr & 0x22); ++i) m.run(1);
        if (host.in(packet) == UsbHostFixture::Result::Data) received += packet.size;
    } else {
        // With no IN token at all, only guest activity can discard the response.
        for (unsigned i = 0; i < 2000000 && (host.udc->ep[0].csr & 2); ++i) m.run(1);
        require(!(host.udc->ep[0].csr & 2) && !host.udc->tx0Count, "guest did not discard the pending response");
        require(host.in(packet) == UsbHostFixture::Result::Nak, "flushed response was incorrectly retained");
    }
    require(!(host.udc->udcisr[0] & 1), "setup/completion IRQ was not acknowledged");
    std::printf("USB_PROBE host=%s staged=16 received=%u requested=18 snapshot=ok reset_irq=acknowledged\n",
                immediate ? "instruction-boundary-atomic-ack" : "wait-for-handler", received);
    require(host.resetStimulus() && !host.udc->tx0Count && !host.udc->ep[0].bcr, "reset stimulus did not clear FIFO");
    return received;
}
int main(int argc, char** argv) {
    alarm(120); // bounds even a broken core or unexpected guest loop
    try {
        require(argc == 2, "usage: test-tx-usb-rom PRIVATE_TX_SESSION");
        auto input = readFile(argv[1]);
        const auto waited = trial(input, false), immediate = trial(input, true);
        require(waited == 0 && immediate == 16, "firmware ordering differs; inspect before drawing conclusions");
        std::puts("BLOCKED: queued EP0 response is flushed by the guest; an immediate atomic ACK overlaps the setup IRQ acknowledgement.");
        std::puts("No enumeration: full descriptor, address assignment, configuration selection and wire/ACK timing remain unverified.");
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1;
    }
}
