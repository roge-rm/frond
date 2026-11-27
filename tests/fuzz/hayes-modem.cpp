// Hayes AT commands and online data from the guest's serial port.
#include <fuzzer/FuzzedDataProvider.h>

#include "modem/HayesModem.h"

namespace {
class Line : public frondModem::ModemLine {
   public:
    void Answer(uint64_t) override { up = true; }
    void HangUp() override { up = false; }
    void FromModem(const uint8_t*, size_t, uint64_t) override {}
    size_t ToModem(uint8_t* out, size_t max, uint64_t) override {
        const size_t n = std::min<size_t>(max, 16);
        std::fill(out, out + n, 'x');
        return up ? n : 0;
    }
    bool Carrier(uint64_t) const override { return up; }
    bool up{false};
};
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    FuzzedDataProvider data(bytes, size);
    Line line;
    frondModem::HayesModem::Options options;
    options.handshakeNs = 1000000;
    options.dialPlan["5551234"] = frondModem::HayesModem::Result::Busy;
    frondModem::HayesModem modem(line, options);
    uint64_t now = 0;
    modem.SetPower(true, now);
    uint8_t out[256];
    while (data.remaining_bytes()) {
        now += data.ConsumeIntegralInRange<uint64_t>(0, 2000000000ull);
        switch (data.ConsumeIntegralInRange(0, 7)) {
            case 0: modem.SetDTR(data.ConsumeBool(), now); break;
            case 1: modem.Drop(now); break;
            case 2: modem.LineDropped(modem.SavedCall(), now); break;
            default: {
                const auto chunk = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange(1, 64));
                modem.Write(chunk.data(), chunk.size(), now);
            }
        }
        modem.Service(now);
        while (modem.Read(out, sizeof(out))) {}
    }
    return 0;
}
