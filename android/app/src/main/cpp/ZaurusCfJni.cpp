// The Zaurus internal CF builder (common/storage/ZaurusCf.h) for ZaurusCf.kt. Runs on the caller's
// thread, which must be a background one; the file descriptors stay owned by the caller.
#include <jni.h>

#include <string>

#include "storage/Errors.h"
#include "storage/FatNames.h"
#include "storage/ZaurusCf.h"

namespace {

// A Java exception from a callback is pending; the builder unwinds and the call returns.
struct JavaThrown {};

}  // namespace

// ZaurusCf.build(hdd1Fd, hdd1Size, fontFd, requireFonts, sizeGb, outFd, listener): "ok", "cancelled"
// or "error MESSAGE". outFd must be an empty file; it's left sparse at the full size. The listener's
// progress(done, total) gets archive bytes read, cancelled() is polled as it runs.
extern "C" JNIEXPORT jstring JNICALL Java_com_rm_frond_ZaurusCf_build(JNIEnv* env, jclass, jint hdd1Fd, jlong hdd1Size,
                                                                     jint fontFd, jboolean requireFonts, jint sizeGb,
                                                                     jint outFd, jobject listener) {
    jclass type = env->GetObjectClass(listener);
    jmethodID progress = env->GetMethodID(type, "progress", "(JJ)V");
    jmethodID cancelled = env->GetMethodID(type, "cancelled", "()Z");
    if (!progress || !cancelled) return nullptr;

    frondZaurusCf::Request request;
    request.hdd1Fd = hdd1Fd;
    request.hdd1Size = hdd1Size;
    request.fontRomFd = fontFd;
    request.requireFonts = requireFonts;
    request.sizeGb = sizeGb;
    request.progress = [&](uint64_t done, uint64_t total) {
        env->CallVoidMethod(listener, progress, jlong(done), jlong(total));
        if (env->ExceptionCheck()) throw JavaThrown{};
    };
    request.cancelled = [&] {
        const bool stop = env->CallBooleanMethod(listener, cancelled);
        if (env->ExceptionCheck()) throw JavaThrown{};
        return stop;
    };

    std::string reply;
    try {
        frondZaurusCf::buildToFd(request, outFd);
        reply = "ok";
    } catch (const JavaThrown&) {
        return nullptr;
    } catch (const frondStorage::Cancelled&) {
        reply = "cancelled";
    } catch (const frondStorage::Error& error) {
        reply = "error " + error.message;
    } catch (const std::bad_alloc&) {
        reply = "error Not enough memory to create the CF image.";
    } catch (const std::exception& error) {
        reply = std::string("error ") + error.what();
    }
    // Through UTF-16: archive names in a message may not be valid modified UTF-8.
    const auto text = frondStorage::fat::utf16(reply);
    return env->NewString(reinterpret_cast<const jchar*>(text.data()), jsize(text.size()));
}
