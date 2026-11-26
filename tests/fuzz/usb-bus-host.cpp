// The USB host's enumeration and bulk/interrupt handling (Palm HotSync port list or RNDIS) against a
// device whose descriptors and packets come from the input. The input starts with the device
// descriptor (18 bytes), a 16-bit little-endian length and that much configuration descriptor; the
// rest drives the vendor replies, handshakes and endpoint data.
#include <fuzzer/FuzzedDataProvider.h>

#include <algorithm>
#include <cstring>
#include <map>

#include "RndisHost.h"
#include "UsbBusHost.h"

namespace {
using R = UsbPacketResult;
using Bytes = std::vector<uint8_t>;

struct Device {
    explicit Device(FuzzedDataProvider& data) : data(data) {}
    FuzzedDataProvider& data;
    Bytes deviceDescriptor, configuration;
    uint8_t address{0}, pendingAddress{0};
    bool addressPending{false};
    Bytes ep0;
    size_t ep0Sent{0};
    bool ep0Stall{false};
    std::map<uint8_t, Bytes> staged;
    void (*notify)(void*){nullptr};

    unsigned MaxPacket0() const {
        const unsigned m = deviceDescriptor.size() > 7 ? deviceDescriptor[7] : 8;
        return m == 8 || m == 16 || m == 32 || m == 64 ? m : 8;
    }
    R Handshake() {
        if (!data.remaining_bytes()) return R::Nak;
        switch (data.ConsumeIntegralInRange(0, 15)) {
            case 0: return R::Nak;
            case 1: return R::Stall;
            case 2: return R::Timeout;
            default: return R::Ack;
        }
    }
    R Setup(const uint8_t* s) {
        const uint8_t type = s[0], request = s[1];
        const uint16_t value = uint16_t(s[2] | s[3] << 8), length = uint16_t(s[6] | s[7] << 8);
        ep0.clear();
        ep0Sent = 0;
        ep0Stall = false;
        if ((type & 0x60) == 0 && request == 5) {
            pendingAddress = uint8_t(value & 0x7f);
            addressPending = true;
        } else if ((type & 0x60) == 0 && request == 6) {
            const uint8_t kind = uint8_t(value >> 8);
            if (kind == 1) ep0 = deviceDescriptor;
            else if (kind == 2) ep0 = configuration;
            else ep0 = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(0, 255));
        } else if (type & 0x80) {
            ep0 = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(0, 1100));
        }
        if (ep0.size() > length) ep0.resize(length);
        if (data.remaining_bytes() && data.ConsumeIntegralInRange(0, 31) == 0) ep0Stall = true;
        return R::Ack;
    }
    R PeekIn(uint8_t ep, unsigned* size) {
        if (ep == 0) {
            if (ep0Stall) return R::Stall;
            *size = unsigned(std::min<size_t>(MaxPacket0(), ep0.size() - ep0Sent));
            return R::Ack;
        }
        auto it = staged.find(ep);
        if (it == staged.end()) {
            const R r = Handshake();
            if (r != R::Ack) return r;
            it = staged.emplace(ep, data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(0, 64))).first;
        }
        *size = unsigned(it->second.size());
        return R::Ack;
    }
    R In(uint8_t* out, unsigned* size) {
        if (ep0Stall) return R::Stall;
        const size_t n = std::min<size_t>(MaxPacket0(), ep0.size() - ep0Sent);
        if (n) std::memcpy(out, ep0.data() + ep0Sent, n);
        ep0Sent += n;
        *size = unsigned(n);
        if (!n && addressPending) {
            address = pendingAddress;
            addressPending = false;
        }
        return R::Ack;
    }
    R Out(const uint8_t*, unsigned size) {
        if (!size && addressPending) {
            address = pendingAddress;
            addressPending = false;
        }
        return ep0Stall ? R::Stall : R::Ack;
    }
    R EpIn(uint8_t ep, uint8_t* out, unsigned* size) {
        unsigned peeked = 0;
        const R r = PeekIn(ep, &peeked);
        if (r != R::Ack) return r;
        auto it = staged.find(ep);
        if (!it->second.empty()) std::memcpy(out, it->second.data(), it->second.size());
        *size = unsigned(it->second.size());
        staged.erase(it);
        return R::Ack;
    }
    R EpOut(uint8_t, const uint8_t*, unsigned) { return Handshake(); }
};

Device* current;
const UsbDevicePort::Operations operations{
    [](void*) { return true; },
    [](void*) { return true; },
    [](void*) { return true; },
    [](void*) { return true; },
    [](void*) { return current->data.remaining_bytes() ? current->data.ConsumeBool() : false; },
    [](void*) -> uint32_t { return 1; },
    [](void*) -> uint8_t { return current->address; },
    [](void*, void (*notify)(void*), void*) { current->notify = notify; },
    [](void*, const uint8_t* s) { return current->Setup(s); },
    [](void*, const uint8_t* d, unsigned n) { return current->Out(d, n); },
    [](void*, uint8_t* d, unsigned* n) { return current->In(d, n); },
    [](void*, uint8_t ep, const uint8_t* d, unsigned n) { return current->EpOut(ep, d, n); },
    [](void*, uint8_t ep, uint8_t* d, unsigned* n) { return current->EpIn(ep, d, n); },
    [](void*, uint8_t ep, unsigned* n) { return current->PeekIn(ep, n); },
};
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    if (size < 21) return 0;
    const size_t configLength = std::min<size_t>(bytes[18] | bytes[19] << 8, size - 21);
    FuzzedDataProvider data(bytes + 20 + configLength, size - 20 - configLength);
    Device device(data);
    device.deviceDescriptor.assign(bytes, bytes + 18);
    device.configuration.assign(bytes + 20, bytes + 20 + configLength);
    current = &device;
    UsbBusHost::Link link;
    size_t toHost = 0;
    link.sendSpace = [] { return size_t(4096); };
    link.send = [&](const uint8_t*, size_t n) { return toHost += n, n; };
    link.receive = [&](uint8_t* out, size_t max) {
        const auto chunk = data.ConsumeBytes<uint8_t>(std::min<size_t>(max, data.ConsumeIntegralInRange<size_t>(0, 64)));
        std::copy(chunk.begin(), chunk.end(), out);
        return chunk.size();
    };
    link.pending = [&] { return data.remaining_bytes() ? data.ConsumeIntegralInRange<size_t>(0, 64) : size_t(0); };
    UsbBusHost host([] { return UsbDevicePort(current, operations); }, link);
    RndisHost rndis;
    rndis.frameFromDevice = [](const uint8_t*, size_t) {};
    if (data.ConsumeBool()) host.SetRndis(&rndis);
    host.Attach();
    uint64_t now = 0;
    // Bounded so an input that never runs out still ends.
    for (size_t i = 0; i < 20000 + 64 * size; ++i) {
        const uint64_t wake = host.NextWake();
        if (wake == UINT64_MAX && !data.remaining_bytes()) break;
        now = wake == UINT64_MAX ? now + 1000000 : std::max(now, wake);
        host.Tick(now);
        if (rndis.Up() && data.remaining_bytes() && data.ConsumeIntegralInRange(0, 15) == 0) {
            const auto frame = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange<size_t>(14, 1514));
            rndis.FrameToDevice(frame.data(), frame.size());
        }
        if (!data.remaining_bytes() && i > 20000) break;
        if (data.remaining_bytes() && data.ConsumeIntegralInRange(0, 1023) == 0) host.Poke();
    }
    host.TakeMessages();
    host.Detach();
    current = nullptr;
    return 0;
}
