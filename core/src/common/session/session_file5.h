#ifndef _SESSION_FILE5_H_
#define _SESSION_FILE5_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

struct mz_stream_s;

class SessionFile5 {
   public:
    explicit SessionFile5() = default;

    static constexpr uint32_t kMagic = 0x19800819;
    static constexpr uint32_t kVersionV6 = 6;
    static constexpr uint32_t kVersionV7 = 7;
    static constexpr uint32_t kVersionV8 = 8;
    static constexpr uint32_t kVersionV9 = 9;
    static constexpr uint32_t kCurrentVersion = kVersionV9;
    static constexpr size_t kDeviceNameBytes = 32;
    static constexpr size_t kHeaderBytes = 8 + kDeviceNameBytes;
    static constexpr size_t kMediaDescriptorBytesV7 = 92;
    static constexpr size_t kMediaDescriptorBytes = 100;
    static size_t MediaDescriptorBytes(uint32_t version) {
        return version == kVersionV7 ? kMediaDescriptorBytesV7 : kMediaDescriptorBytes;
    }
    static constexpr size_t kMaxInflatedBytes = 512 * 1024 * 1024;
    static constexpr size_t kMaxSdBytes = 128u << 20;

    static bool IsSessionFile(size_t size, const void* data);

    // The device's fixed name from device_profile5.h, like "palm-tx".
    const std::string& GetDeviceName() const;
    int32_t GetDisplayMode() const;
    SessionFile5& SetDeviceName(const std::string& name);
    SessionFile5& SetDisplayMode(int32_t displayMode);

    const void* GetMetadata() const;
    size_t GetMetadataSize() const;
    SessionFile5& SetMetadata(size_t size, const void* data);

    const void* GetNor() const;
    size_t GetNorSize() const;
    SessionFile5& SetNor(size_t size, const void* data);

    const void* GetNand() const;
    size_t GetNandSize() const;
    SessionFile5& SetNand(size_t size, const void* data);

    const void* GetMemory() const;
    size_t GetMemorySize() const;
    SessionFile5& SetMemory(size_t size, const void* data);

    const void* GetSavestate() const;
    size_t GetSavestateSize() const;
    SessionFile5& SetSavestate(size_t size, const void* data);

    const void* GetSd() const;
    size_t GetSdSize() const;
    SessionFile5& SetSd(size_t size, const void* data);

    const void* GetMedia() const;
    size_t GetMediaSize() const;
    uint32_t GetMediaCount() const;
    SessionFile5& SetMedia(size_t size, const void* data);

    size_t GetRamSize() const;
    SessionFile5& SetRamSize(uint32_t size);

    bool Serialize();
    bool SerializeAs(uint32_t version);
    const void* GetSerializedSession() const;
    size_t GetSerializedSessionSize() const;

    uint32_t GetVersion() const;

    bool Deserialize(size_t size, const void* data);

   private:
    bool Write32(uint32_t data);
    bool Write64(uint64_t data);
    uint32_t Read32(bool& success);
    uint64_t Read64(bool& success);

    bool AppendToCompressionStream(size_t size, const uint8_t* data, mz_stream_s& stream);
    bool GrowBuffer(mz_stream_s& stream);
    bool Flush(mz_stream_s& stream);

    bool Deserialize_v6();
    bool Deserialize_v7();

   private:
    std::string deviceName;
    int32_t displayMode{0};

    size_t metadataSize{0};
    const uint8_t* metadata{nullptr};

    size_t norSize{0};
    const uint8_t* nor{nullptr};

    size_t nandSize{0};
    const uint8_t* nand{nullptr};

    size_t memorySize{0};
    const uint8_t* memory{nullptr};

    size_t savestateSize{0};
    const uint8_t* savestate{nullptr};

    size_t sdSize{0};
    const uint8_t* sd{nullptr};

    size_t mediaSize{0};
    const uint8_t* media{nullptr};

    size_t serializedSessionSize{0};
    const uint8_t* serializedSession{nullptr};

    size_t bufferSize{0};
    std::unique_ptr<uint8_t[]> buffer;

    uint32_t ramSize{0};

    uint8_t* cursor;
    const uint8_t* ccursor;

    uint32_t version{0};

   private:
    SessionFile5(const SessionFile5&) = delete;
    SessionFile5(SessionFile5&&) = delete;
    SessionFile5& operator=(const SessionFile5&) = delete;
    SessionFile5& operator=(SessionFile5&&) = delete;
};

#endif  // _SESSION_FILE5_H_
