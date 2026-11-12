// The Zodiac ROM from Tapwave's update (zodiac-rom/ZodiacRom.h) for the app.
#include <jni.h>

#include <cstdio>
#include <string>

#include "ZodiacRom.h"

// ZodiacRom.convert(input, output): "ok" with the ROM written to output, "none" when input is not a
// zip or PRC (the caller carries on with the file as it is), or "error MESSAGE".
extern "C" JNIEXPORT jstring JNICALL Java_com_rm_frond_ZodiacRom_convert(JNIEnv* env, jclass, jstring input, jstring output) {
    const char* in = env->GetStringUTFChars(input, nullptr);
    const char* out = env->GetStringUTFChars(output, nullptr);
    std::string reply;
    if (!in || !out) {
        reply = "error Couldn't read the file.";
    } else {
        const auto result = frondZodiacRom::extractFile(in);
        using Status = frondZodiacRom::Result::Status;
        if (result.status == Status::error) {
            reply = "error " + result.error;
        } else if (result.status == Status::notUpdate) {
            reply = "none";
        } else {
            std::FILE* file = std::fopen(out, "wb");
            const bool written = file && std::fwrite(result.rom.data(), 1, result.rom.size(), file) == result.rom.size();
            if (file && std::fclose(file) != 0) reply = "error Couldn't save the firmware.";
            else reply = written ? "ok" : "error Couldn't save the firmware.";
        }
    }
    if (in) env->ReleaseStringUTFChars(input, in);
    if (out) env->ReleaseStringUTFChars(output, out);
    return env->NewStringUTF(reply.c_str());
}
