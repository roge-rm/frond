// The Zodiac's MIDI out on Android: the core's MIDI out thread hands each message to
// FrondActivity.onZodiacMidi, which sends it to the MidiManager port the app opened by device id.
#include <SDL.h>
#include <jni.h>

#include <memory>

#include "ZodiacMidiOut.h"

namespace {
class JniPort : public ZodiacMidiOut::Port {
   public:
    JniPort(jobject activity, jmethodID send) : activity_(activity), send_(send) {}
    ~JniPort() override {
        if (auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv())) env->DeleteGlobalRef(activity_);
    }
    // On the MIDI out thread, which SDL attaches to the VM on its first call. hostNs is on CLOCK_MONOTONIC,
    // the clock MidiInputPort.send's timestamps use (System.nanoTime).
    void send(uint64_t hostNs, const uint8_t* bytes, size_t size) override {
        auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        if (!env) return;
        jbyteArray message = env->NewByteArray(jsize(size));
        if (!message) {
            env->ExceptionClear();
            return;
        }
        env->SetByteArrayRegion(message, 0, jsize(size), reinterpret_cast<const jbyte*>(bytes));
        env->CallVoidMethod(activity_, send_, message, jlong(hostNs));
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(message);
    }

   private:
    const jobject activity_;
    const jmethodID send_;
};
}  // namespace

// On SDL's thread at machine start. Null when the activity has no onZodiacMidi.
std::unique_ptr<ZodiacMidiOut::Port> zodiacAndroidMidiPort() {
    auto* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) return nullptr;
    jclass type = env->GetObjectClass(activity);
    jmethodID send = env->GetMethodID(type, "onZodiacMidi", "([BJ)V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(type);
    std::unique_ptr<ZodiacMidiOut::Port> port;
    if (send) port = std::make_unique<JniPort>(env->NewGlobalRef(activity), send);
    env->DeleteLocalRef(activity);
    return port;
}
