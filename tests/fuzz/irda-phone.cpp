// The IrDA stack (SIR framing, IrLAP, IrLMP, IAS, TinyTP, IrCOMM) behind the infrared phone, fed
// what the guest's IR port sends.
#include <fuzzer/FuzzedDataProvider.h>

#include "ir/IrPhone.h"

namespace {
class Line : public frondModem::ModemLine {
   public:
    void Answer(uint64_t) override { up = true; }
    void HangUp() override { up = false; }
    void FromModem(const uint8_t*, size_t, uint64_t) override {}
    size_t ToModem(uint8_t*, size_t, uint64_t) override { return 0; }
    bool Carrier(uint64_t) const override { return up; }
    bool up{false};
};
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    FuzzedDataProvider data(bytes, size);
    Line line;
    frondIr::IrPhone phone(line);
    uint64_t now = 0;
    uint8_t out[512];
    while (data.remaining_bytes()) {
        now += data.ConsumeIntegralInRange<uint64_t>(0, 500000000ull);
        const auto chunk = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange(1, 300));
        phone.Write(chunk.data(), chunk.size(), now);
        phone.Service(now);
        while (phone.Read(out, sizeof(out), now)) {}
    }
    phone.Drop(now);
    return 0;
}
