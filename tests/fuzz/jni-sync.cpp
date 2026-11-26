// The Android sync manager's JNI entry point (SyncJni.cpp SyncNative.exchange) on a fake JNIEnv:
// JSON requests against a scratch sync folder. The input's first byte splits it into the request
// and a file's bytes; any "path" in the request is pointed at that file in the scratch folder, so
// installs read it and exports write there and nowhere else.
#include <fstream>

#include "FakeJni.h"
#include "Scratch.h"
#include "../../android/app/src/main/cpp/SyncJni.cpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    static unsigned runs = 0;
    if (size < 1 || size > 65536) return 0;
    const auto root = FuzzScratch("jni-sync", runs);
    const size_t split = 1 + bytes[0] * (size - 1) / 256;
    std::string request(reinterpret_cast<const char*>(bytes + 1), split - 1);
    const auto file = root + "/file";
    std::ofstream(file, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(bytes + split), std::streamsize(size - split));
    {
        DynamicJsonDocument doc(16384 + 4 * request.size());
        const auto error = deserializeJson(doc, request);
        if (error == DeserializationError::NoMemory) return 0;
        if (!error && doc.is<JsonObject>() && doc.containsKey("path")) {
            doc["path"] = file;
            if (doc.overflowed()) return 0;
            request.clear();
            serializeJson(doc, request);
        }
    }
    fakeJni::Env env;
    JNIEnv* e = env.Jni();
    const auto reply = Java_com_rm_frond_SyncNative_exchange(e, nullptr, env.Bytes(root), env.Bytes(request));
    if (!reply || env.exception) __builtin_trap();
    return 0;
}
