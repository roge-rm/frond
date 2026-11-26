// A JNIEnv for the Android JNI entry points built on the host: byte and long arrays only, each
// array its own heap block of exactly its length so ASan sees reads past the end. Region calls
// out of range set a pending exception and copy nothing, as the JVM does.
#pragma once

#include <jni.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace fakeJni {

struct Array {
    explicit Array(size_t length, size_t width) : length(length), bytes(new uint8_t[std::max<size_t>(length * width, 1)]) {}
    size_t length;
    std::unique_ptr<uint8_t[]> bytes;
};

struct Env {
    JNIEnv env{};
    JNINativeInterface_ table{};
    std::vector<std::unique_ptr<Array>> arrays;
    bool exception{false};

    Env() {
        table.GetArrayLength = [](JNIEnv*, jarray a) { return jsize(Of(a)->length); };
        table.NewByteArray = [](JNIEnv* e, jsize n) { return reinterpret_cast<jbyteArray>(Self(e)->Make(size_t(n), 1)); };
        table.NewLongArray = [](JNIEnv* e, jsize n) { return reinterpret_cast<jlongArray>(Self(e)->Make(size_t(n), 8)); };
        table.GetByteArrayElements = [](JNIEnv*, jbyteArray a, jboolean* copy) {
            if (copy) *copy = JNI_FALSE;
            return reinterpret_cast<jbyte*>(Of(a)->bytes.get());
        };
        table.ReleaseByteArrayElements = [](JNIEnv*, jbyteArray, jbyte*, jint) {};
        table.GetByteArrayRegion = [](JNIEnv* e, jbyteArray a, jsize at, jsize n, jbyte* out) {
            if (Self(e)->Check(a, at, n)) std::memcpy(out, Of(a)->bytes.get() + at, size_t(n));
        };
        table.SetByteArrayRegion = [](JNIEnv* e, jbyteArray a, jsize at, jsize n, const jbyte* in) {
            if (Self(e)->Check(a, at, n)) std::memcpy(Of(a)->bytes.get() + at, in, size_t(n));
        };
        table.SetLongArrayRegion = [](JNIEnv* e, jlongArray a, jsize at, jsize n, const jlong* in) {
            if (Self(e)->Check(a, at, n)) std::memcpy(Of(a)->bytes.get() + size_t(at) * 8, in, size_t(n) * 8);
        };
        table.ExceptionCheck = [](JNIEnv* e) { return jboolean(Self(e)->exception); };
        env.functions = &table;
    }
    JNIEnv* Jni() { return &env; }

    Array* Make(size_t length, size_t width) {
        if (length > (1u << 28)) return exception = true, nullptr;
        arrays.push_back(std::make_unique<Array>(length, width));
        return arrays.back().get();
    }
    jbyteArray Bytes(const void* data, size_t length) {
        auto* a = Make(length, 1);
        if (length) std::memcpy(a->bytes.get(), data, length);
        return reinterpret_cast<jbyteArray>(a);
    }
    template <typename T>
    jbyteArray Bytes(const T& container) { return Bytes(container.data(), container.size()); }
    static std::vector<uint8_t> Read(jbyteArray a) {
        if (!a) return {};
        const auto* p = Of(a)->bytes.get();
        return {p, p + Of(a)->length};
    }

   private:
    static Env* Self(JNIEnv* e) { return reinterpret_cast<Env*>(e); }
    static Array* Of(jobject a) { return reinterpret_cast<Array*>(a); }
    bool Check(jarray a, jsize at, jsize n) {
        if (!a || at < 0 || n < 0 || size_t(at) + size_t(n) > Of(a)->length) return exception = true, false;
        return true;
    }
};

}  // namespace fakeJni
