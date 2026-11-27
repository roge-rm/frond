// Shared pieces for the libFuzzer targets: a SyncStream fed from the fuzz input.
#pragma once

#include <fuzzer/FuzzedDataProvider.h>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "SyncStream.h"

// Reads come in chunks whose sizes the input picks; once the input runs out Read reports a
// closed stream. Writes are dropped.
class FuzzStream : public frondSync::SyncStream {
   public:
    explicit FuzzStream(FuzzedDataProvider& data) : data(data) {}
    long Read(uint8_t* out, size_t max, int) override {
        if (closed || data.remaining_bytes() == 0) return -1;
        const size_t want = std::min<size_t>(max, data.ConsumeIntegralInRange<size_t>(1, 512));
        const auto bytes = data.ConsumeBytes<uint8_t>(want);
        if (bytes.empty()) return -1;
        std::copy(bytes.begin(), bytes.end(), out);
        return long(bytes.size());
    }
    bool Write(const uint8_t*, size_t size) override { return !closed && (written += size, true); }
    void Abort() override { closed = true; }
    size_t written{0};

   private:
    FuzzedDataProvider& data;
    bool closed{false};
};
