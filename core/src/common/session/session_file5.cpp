#include "session_file5.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "Defer.h"
#include "zip/miniz.h"

using namespace std;

// Versions
//
// * V0: first RLE compressed version
// * V1: Replace RLE with Gzip compression
// * V2: Add RAM size to header
// * V3: RAM page size change 512b -> 1k: migrate memory image
// * V4: Add display mode
// * V5: Add an embedded SD image, committed atomically with the peripheral state
// * V6: The device is a fixed name in a 32-byte NUL-padded field, and the SD size is always present.
//       Earlier versions name the device by number and are refused.
// * V7: 64-bit section lengths, 92-byte media descriptors, no embedded card payloads.
// * V8: as V7 with 100-byte media descriptors that add the SD content generation.
// * V9: as V8 with internal NAND in nand.frondnand (media kind 4); the NAND section is always empty.

namespace {
    constexpr uint32_t MAGIC = SessionFile5::kMagic;
    constexpr uint32_t CURRENT_VERSION = SessionFile5::kCurrentVersion;
    constexpr uint32_t FIRST_NAMED_VERSION = SessionFile5::kVersionV6;
    constexpr size_t SIZE_DEVICE_NAME = SessionFile5::kDeviceNameBytes;

    constexpr size_t SIZE_HEADER = SessionFile5::kHeaderBytes;
    constexpr size_t SIZE_TOC_V6 = 8 * 4;

    constexpr size_t BUFFER_MAX_SIZE = SessionFile5::kMaxInflatedBytes;
    constexpr size_t BUFFER_MIN_SIZE = 1024;

    // Lower-case letters, digits and dashes, 1 to 31 of them.
    bool validDeviceName(const std::string& name) {
        if (name.empty() || name.size() >= SIZE_DEVICE_NAME) return false;
        for (char c : name)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
        return true;
    }
}  // namespace

bool SessionFile5::IsSessionFile(size_t size, const void* data) {
    const uint8_t* data8 = reinterpret_cast<const uint8_t*>(data);

    if (size < 8) return false;

    const uint32_t magic = data8[0] | (data8[1] << 8) | (data8[2] << 16) | (data8[3] << 24);
    if (magic != MAGIC) return false;

    const uint32_t version = data8[4] | (data8[5] << 8) | (data8[6] << 16) | (data8[7] << 24);
    if (version > CURRENT_VERSION) return false;

    return true;
}

const std::string& SessionFile5::GetDeviceName() const { return deviceName; }

int32_t SessionFile5::GetDisplayMode() const { return displayMode; }

SessionFile5& SessionFile5::SetDeviceName(const std::string& name) {
    deviceName = name;

    return *this;
}

SessionFile5& SessionFile5::SetDisplayMode(int32_t displayMode) {
    this->displayMode = displayMode;

    return *this;
}

const void* SessionFile5::GetMetadata() const { return metadata; }

size_t SessionFile5::GetMetadataSize() const { return metadataSize; }

SessionFile5& SessionFile5::SetMetadata(size_t size, const void* data) {
    metadataSize = size;
    metadata = reinterpret_cast<const uint8_t*>(data);

    return *this;
}

const void* SessionFile5::GetNor() const { return nor; }

size_t SessionFile5::GetNorSize() const { return norSize; }

SessionFile5& SessionFile5::SetNor(size_t size, const void* data) {
    norSize = size;
    nor = reinterpret_cast<const uint8_t*>(data);

    return *this;
}

const void* SessionFile5::GetNand() const { return nand; }

size_t SessionFile5::GetNandSize() const { return nandSize; }

SessionFile5& SessionFile5::SetNand(size_t size, const void* data) {
    nandSize = size;
    nand = reinterpret_cast<const uint8_t*>(data);

    return *this;
}

const void* SessionFile5::GetMemory() const { return memory; }

size_t SessionFile5::GetMemorySize() const { return memorySize; }

SessionFile5& SessionFile5::SetMemory(size_t size, const void* data) {
    memorySize = size;
    memory = reinterpret_cast<const uint8_t*>(data);

    return *this;
}

const void* SessionFile5::GetSavestate() const { return savestate; }

size_t SessionFile5::GetSavestateSize() const { return savestateSize; }

SessionFile5& SessionFile5::SetSavestate(size_t size, const void* data) {
    savestateSize = size;
    savestate = reinterpret_cast<const uint8_t*>(data);

    return *this;
}

const void* SessionFile5::GetSd() const { return sd; }
size_t SessionFile5::GetSdSize() const { return sdSize; }
SessionFile5& SessionFile5::SetSd(size_t size, const void* data) {
    sdSize = size; sd = reinterpret_cast<const uint8_t*>(data); return *this;
}

const void* SessionFile5::GetMedia() const { return media; }
size_t SessionFile5::GetMediaSize() const { return mediaSize; }
uint32_t SessionFile5::GetMediaCount() const {
    return static_cast<uint32_t>(mediaSize / MediaDescriptorBytes(version));
}
SessionFile5& SessionFile5::SetMedia(size_t size, const void* data) {
    mediaSize = size;
    media = reinterpret_cast<const uint8_t*>(data);
    return *this;
}

size_t SessionFile5::GetRamSize() const { return ramSize; }

SessionFile5& SessionFile5::SetRamSize(uint32_t size) {
    ramSize = size;

    return *this;
}

bool SessionFile5::Serialize() { return SerializeAs(CURRENT_VERSION); }

bool SessionFile5::SerializeAs(uint32_t writeVersion) {
    serializedSessionSize = 0;
    serializedSession = nullptr;

    if (writeVersion != kVersionV6 && writeVersion != kVersionV7 && writeVersion != kVersionV8 &&
        writeVersion != kVersionV9)
        return false;
    if (writeVersion == kVersionV9 && nandSize) {
        cerr << "v9 sessions store NAND as a sibling" << endl;
        return false;
    }
    const bool folderLayout = writeVersion != kVersionV6;
    if (folderLayout && sdSize) {
        cerr << "v7 to v9 sessions store cards as siblings" << endl;
        return false;
    }
    const size_t descriptorBytes = MediaDescriptorBytes(writeVersion);
    if (mediaSize % descriptorBytes) return false;
    if (writeVersion == kVersionV6 && mediaSize) return false;

    const uint64_t sizeUncompressed =
        uint64_t(metadataSize) + norSize + nandSize + memorySize + savestateSize +
        (writeVersion == kVersionV6 ? sdSize : 0);
    if (sizeUncompressed > BUFFER_MAX_SIZE || sdSize > kMaxSdBytes || sdSize % 512 || (sdSize && !sd))
        return false;
    if (!validDeviceName(deviceName)) {
        cerr << "session device name is missing or invalid" << endl;
        return false;
    }

    const size_t tocExtra =
        folderLayout ? (52 + mediaSize) : SIZE_TOC_V6;
    bufferSize =
        max(static_cast<size_t>(BUFFER_MIN_SIZE), SIZE_HEADER + tocExtra + size_t(sizeUncompressed) / 2);
    buffer = make_unique<uint8_t[]>(bufferSize);
    cursor = buffer.get();

    bool success = true;

    success &= Write32(MAGIC);
    success &= Write32(writeVersion);
    if (success) {
        memset(cursor, 0, SIZE_DEVICE_NAME);
        memcpy(cursor, deviceName.data(), deviceName.size());
        cursor += SIZE_DEVICE_NAME;
    }
    success &= Write32(displayMode);
    success &= Write32(ramSize);

    if (folderLayout) {
        success &= Write64(metadataSize);
        success &= Write64(norSize);
        success &= Write64(nandSize);
        success &= Write64(memorySize);
        success &= Write64(savestateSize);
        success &= Write32(static_cast<uint32_t>(mediaSize / descriptorBytes));
        if (success && mediaSize) {
            if (cursor - buffer.get() + static_cast<ssize_t>(mediaSize) >
                static_cast<ssize_t>(bufferSize))
                return false;
            memcpy(cursor, media, mediaSize);
            cursor += mediaSize;
        }
    } else {
        success &= Write32(static_cast<uint32_t>(metadataSize));
        success &= Write32(static_cast<uint32_t>(norSize));
        success &= Write32(static_cast<uint32_t>(nandSize));
        success &= Write32(static_cast<uint32_t>(memorySize));
        success &= Write32(static_cast<uint32_t>(savestateSize));
        success &= Write32(static_cast<uint32_t>(sdSize));
    }

    if (!success) {
        cerr << "failed to write header" << endl;
        return false;
    }

    z_stream stream;
    memset(&stream, 0, sizeof(stream));

    if (deflateInit(&stream, Z_DEFAULT_COMPRESSION) != Z_OK) {
        return false;
    }

    Defer cleanupDeflate([&]() { deflateEnd(&stream); });

    stream.next_out = cursor;
    stream.avail_out = bufferSize - (cursor - buffer.get());

    if (!AppendToCompressionStream(metadataSize, metadata, stream)) {
        cerr << "failed to write metadata" << endl;
        return false;
    }

    if (!AppendToCompressionStream(norSize, nor, stream)) {
        cerr << "failed to write NOR" << endl;
        return false;
    }

    if (!AppendToCompressionStream(nandSize, nand, stream)) {
        cerr << "failed to write NAND" << endl;
        return false;
    }

    if (!AppendToCompressionStream(memorySize, memory, stream)) {
        cerr << "failed to write memory" << endl;
        return false;
    }

    if (!AppendToCompressionStream(savestateSize, savestate, stream)) {
        cerr << "failed to write savestate" << endl;
        return false;
    }

    if (writeVersion == kVersionV6 && !AppendToCompressionStream(sdSize, sd, stream)) return false;

    if (!Flush(stream)) {
        cerr << "failed to flush" << endl;
        return false;
    }

    serializedSession = buffer.get();
    serializedSessionSize = cursor - buffer.get();
    version = writeVersion;

    return true;
}

const void* SessionFile5::GetSerializedSession() const { return serializedSession; }

size_t SessionFile5::GetSerializedSessionSize() const { return serializedSessionSize; }

uint32_t SessionFile5::GetVersion() const { return version; }

bool SessionFile5::Deserialize(size_t size, const void* data) {
    metadataSize = norSize = nandSize = memorySize = savestateSize = 0;
    metadata = nor = nand = memory = savestate = nullptr;

    sdSize = 0; sd = nullptr;
    mediaSize = 0; media = nullptr;
    serializedSession = reinterpret_cast<const uint8_t*>(data);
    serializedSessionSize = size;

    ccursor = serializedSession;

    bool success = true;

    const uint32_t magic = Read32(success);
    version = Read32(success);

    if (!success) {
        cerr << "unable to read header" << endl;
        return false;
    }

    if (magic != MAGIC) {
        cerr << "bad magic" << endl;
        return false;
    }

    if (version < FIRST_NAMED_VERSION) {
        cerr << "this session was saved by an older Frond and can't be loaded" << endl;
        return false;
    }
    if (version > CURRENT_VERSION) {
        cerr << "unsupported session version " << version << endl;
        return false;
    }
    if (version == kVersionV7 || version == kVersionV8 || version == kVersionV9) return Deserialize_v7();
    return Deserialize_v6();
}

bool SessionFile5::Write32(uint32_t data) {
    if (!buffer || cursor - buffer.get() + 4 > static_cast<ssize_t>(bufferSize)) return false;

    *(cursor++) = data;
    *(cursor++) = (data >> 8);
    *(cursor++) = (data >> 16);
    *(cursor++) = (data >> 24);

    return true;
}

bool SessionFile5::Write64(uint64_t data) {
    return Write32(static_cast<uint32_t>(data)) && Write32(static_cast<uint32_t>(data >> 32));
}

uint32_t SessionFile5::Read32(bool& success) {
    if (!serializedSession ||
        ccursor - serializedSession + 4 > static_cast<ssize_t>(serializedSessionSize)) {
        success = false;
        return 0;
    }

    uint32_t result = *(ccursor++);
    result |= (*(ccursor++) << 8);
    result |= (*(ccursor++) << 16);
    result |= (*(ccursor++) << 24);

    return result;
}

uint64_t SessionFile5::Read64(bool& success) {
    const uint32_t lo = Read32(success);
    const uint32_t hi = Read32(success);
    return uint64_t(lo) | (uint64_t(hi) << 32);
}

bool SessionFile5::AppendToCompressionStream(size_t size, const uint8_t* data, mz_stream& stream) {
    if (size == 0) return true;

    stream.next_in = reinterpret_cast<const unsigned char*>(data);
    stream.avail_in = size;

    int deflateResult;
    do {
        const auto outBytesBefore = stream.total_out;
        deflateResult = deflate(&stream, Z_NO_FLUSH);
        cursor += stream.total_out - outBytesBefore;

        if (deflateResult == Z_BUF_ERROR && GrowBuffer(stream)) deflateResult = Z_OK;
    } while (stream.avail_in != 0 && deflateResult == Z_OK);

    return deflateResult == Z_OK;
}

bool SessionFile5::GrowBuffer(mz_stream& stream) {
    if (bufferSize == BUFFER_MAX_SIZE) return false;

    size_t newBufferSize = min((bufferSize * 3) / 2, BUFFER_MAX_SIZE);
    unique_ptr<uint8_t[]> newBuffer = make_unique<uint8_t[]>(newBufferSize);

    memcpy(newBuffer.get(), buffer.get(), cursor - buffer.get());
    cursor = newBuffer.get() + (cursor - buffer.get());
    bufferSize = newBufferSize;

    buffer.swap(newBuffer);

    stream.next_out = cursor;
    stream.avail_out = bufferSize - (cursor - buffer.get());

    return true;
}

bool SessionFile5::Flush(mz_stream& stream) {
    int deflateResult;

    do {
        const auto outBytesBefore = stream.total_out;
        deflateResult = deflate(&stream, Z_FINISH);
        cursor += stream.total_out - outBytesBefore;
    } while ((deflateResult == Z_BUF_ERROR || deflateResult == Z_OK) && GrowBuffer(stream));

    return deflateResult == Z_STREAM_END;
}

bool SessionFile5::Deserialize_v6() {
    if (serializedSessionSize - (ccursor - serializedSession) < SIZE_DEVICE_NAME) {
        cerr << "unable to read session device name" << endl;
        return false;
    }
    const char* field = reinterpret_cast<const char*>(ccursor);
    deviceName.assign(field, strnlen(field, SIZE_DEVICE_NAME));
    ccursor += SIZE_DEVICE_NAME;
    for (size_t i = deviceName.size(); i < SIZE_DEVICE_NAME; ++i)
        if (field[i]) deviceName.clear();
    if (!validDeviceName(deviceName)) {
        cerr << "session device name is invalid" << endl;
        return false;
    }

    bool success = true;

    displayMode = Read32(success);
    ramSize = Read32(success);

    metadataSize = Read32(success);
    norSize = Read32(success);
    nandSize = Read32(success);
    memorySize = Read32(success);
    savestateSize = Read32(success);
    sdSize = Read32(success);

    if (!success) {
        cerr << "failed to read session toc" << endl;
        return false;
    }

    const uint64_t total = uint64_t(metadataSize) + norSize + nandSize + memorySize + savestateSize + sdSize;
    if (total > BUFFER_MAX_SIZE || sdSize > kMaxSdBytes || sdSize % 512) return false;
    bufferSize = size_t(total);
    if (bufferSize > BUFFER_MAX_SIZE) {
        cerr << "session image: bad image size" << endl;
        return false;
    }

    buffer = make_unique<uint8_t[]>(bufferSize);

    mz_ulong destLen = bufferSize;

    const auto uncompressResult = uncompress(buffer.get(), &destLen, ccursor,
                                             serializedSessionSize - (ccursor - serializedSession));

    if (uncompressResult != Z_OK) {
        cerr << "failed to uncompress session image" << endl;
        return false;
    }

    if (destLen != bufferSize) {
        cerr << "session image: bad size" << endl;
        return false;
    }

    cursor = buffer.get();

    metadata = cursor;
    cursor += metadataSize;

    nor = cursor;
    cursor += norSize;

    nand = cursor;
    cursor += nandSize;

    memory = cursor;
    cursor += memorySize;

    savestate = cursor;
    sd = sdSize ? cursor + savestateSize : nullptr;

    return true;
}

bool SessionFile5::Deserialize_v7() {
    if (serializedSessionSize - (ccursor - serializedSession) < SIZE_DEVICE_NAME) {
        cerr << "unable to read session device name" << endl;
        return false;
    }
    const char* field = reinterpret_cast<const char*>(ccursor);
    deviceName.assign(field, strnlen(field, SIZE_DEVICE_NAME));
    ccursor += SIZE_DEVICE_NAME;
    for (size_t i = deviceName.size(); i < SIZE_DEVICE_NAME; ++i)
        if (field[i]) deviceName.clear();
    if (!validDeviceName(deviceName)) {
        cerr << "session device name is invalid" << endl;
        return false;
    }

    bool success = true;
    displayMode = Read32(success);
    ramSize = Read32(success);

    const uint64_t metadataLen = Read64(success);
    const uint64_t norLen = Read64(success);
    const uint64_t nandLen = Read64(success);
    const uint64_t memoryLen = Read64(success);
    const uint64_t savestateLen = Read64(success);
    const uint32_t mediaCount = Read32(success);

    if (!success) {
        cerr << "failed to read session toc" << endl;
        return false;
    }
    if (version == kVersionV9 && nandLen) {
        cerr << "v9 session embeds NAND" << endl;
        return false;
    }

    const uint64_t descriptorBytes = MediaDescriptorBytes(version);
    const uint64_t mediaBytes = uint64_t(mediaCount) * descriptorBytes;
    if (mediaCount && mediaBytes / descriptorBytes != mediaCount) return false;
    if (serializedSessionSize - (ccursor - serializedSession) < mediaBytes) {
        cerr << "truncated v7 media descriptors" << endl;
        return false;
    }
    media = mediaCount ? ccursor : nullptr;
    mediaSize = size_t(mediaBytes);
    ccursor += mediaBytes;

    const uint64_t total = metadataLen + norLen + nandLen + memoryLen + savestateLen;
    if (total > BUFFER_MAX_SIZE) return false;
    if (metadataLen > BUFFER_MAX_SIZE || norLen > BUFFER_MAX_SIZE || nandLen > BUFFER_MAX_SIZE ||
        memoryLen > BUFFER_MAX_SIZE || savestateLen > BUFFER_MAX_SIZE)
        return false;

    metadataSize = size_t(metadataLen);
    norSize = size_t(norLen);
    nandSize = size_t(nandLen);
    memorySize = size_t(memoryLen);
    savestateSize = size_t(savestateLen);

    bufferSize = size_t(total);
    buffer = make_unique<uint8_t[]>(bufferSize ? bufferSize : 1);

    mz_ulong destLen = bufferSize;
    const auto remaining = serializedSessionSize - (ccursor - serializedSession);
    if (bufferSize == 0) {
        if (remaining) {
            cerr << "session image: bad size" << endl;
            return false;
        }
        metadata = nor = nand = memory = savestate = nullptr;
        return true;
    }

    const auto uncompressResult = uncompress(buffer.get(), &destLen, ccursor, remaining);
    if (uncompressResult != Z_OK) {
        cerr << "failed to uncompress session image" << endl;
        return false;
    }
    if (destLen != bufferSize) {
        cerr << "session image: bad size" << endl;
        return false;
    }

    cursor = buffer.get();
    metadata = cursor;
    cursor += metadataSize;
    nor = cursor;
    cursor += norSize;
    nand = cursor;
    cursor += nandSize;
    memory = cursor;
    cursor += memorySize;
    savestate = cursor;
    return true;
}
