#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace frondConduit {

using Bytes = std::vector<uint8_t>;
constexpr uint32_t version = 3;
constexpr uint32_t maxFrame = 131072;
constexpr uint32_t maxRecord = 65535;

enum class Operation : uint8_t {
    complete, check, registerConduit, unregisterConduit, user, open, close, removeDb, create,
    count, readIndex, readId, readModified, write, remove, purgeDeleted, purgeAll,
    resetFlags, readAppBlock, writeAppBlock, findName, findTypeCreator, readPreference,
    writePreference, readFeature, dateTime, cardInfo, log, deviceLog, writeResource,
};

enum Status : uint32_t {
    success = 0,
    unknown = 0x4001,
    notFound = 0x4003,
    recordBusy = 0x4006,
    recordDeleted = 0x4007,
    readOnly = 0x4009,
    disconnected = 0x400a,
    exists = 0x400b,
    alreadyOpen = 0x400c,
    notOpen = 0x400d,
    badOperation = 0x400e,
    badRemoteArgument = 0x400f,
    smallBuffer = 0x4012,
    remoteMemory = 0x4013,
    remoteSpace = 0x4014,
    remoteSystem = 0x4015,
    badArgument = 0x4017,
    limit = 0x4018,
    unsupported = 0x4019,
    tooManyOpen = 0x10004403,
    cancelled = 0x10004405,
};

struct Writer {
    Bytes data;

    void Put(uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) data.push_back(uint8_t(value >> (8 * i)));
    }

    void Blob(const void* bytes, size_t size) {
        Put(uint32_t(size));
        if (size) {
            const auto* first = static_cast<const uint8_t*>(bytes);
            data.insert(data.end(), first, first + size);
        }
    }

    void Text(std::string_view value) { Blob(value.data(), value.size()); }
};

struct Reader {
    const Bytes& data;
    size_t position{};
    bool ok{true};

    uint32_t Get() {
        if (!Need(4)) return 0;
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= uint32_t(data[position++]) << (8 * i);
        return value;
    }

    Bytes Blob(size_t limit) {
        const auto size = Get();
        if (size > limit) ok = false;
        if (!Need(size) || !size) return {};
        auto first = data.begin() + position;
        position += size;
        return {first, first + size};
    }

    std::string Text(size_t limit) {
        auto bytes = Blob(limit);
        if (std::find(bytes.begin(), bytes.end(), uint8_t(0)) != bytes.end()) ok = false;
        return {bytes.begin(), bytes.end()};
    }

    bool End() const { return ok && position == data.size(); }

   private:
    bool Need(size_t amount) {
        if (amount > data.size() - position) ok = false;
        return ok;
    }
};

template <typename Read>
bool ReadFrame(Read&& read, Bytes& body) {
    Bytes length(4);
    if (!read(length.data(), length.size())) return false;
    const auto size = Reader{length}.Get();
    if (!size || size > maxFrame) return false;
    body.resize(size);
    return read(body.data(), body.size());
}

template <typename Write>
bool WriteFrame(Write&& write, const Bytes& body) {
    if (body.empty() || body.size() > maxFrame) return false;
    Writer length;
    length.Put(uint32_t(body.size()));
    return write(length.data.data(), length.data.size()) && write(body.data(), body.size());
}

}  // namespace frondConduit
