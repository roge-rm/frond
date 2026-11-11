// The card tool (CardTool.h) for the app: one make, import or export on the calling thread, with a
// progress listener and a cancel flag. The app calls it from a worker thread.
#include <jni.h>

#include <string>
#include <vector>

#include "CardTool.h"

namespace {
// The listener's progress(JJ)V and the cancel flag's get()Z. A lookup that throws is cleared and
// left out, so no JNI call runs with an exception pending.
void lookUp(JNIEnv* env, jobject listener, jobject cancel, jmethodID& progress, jmethodID& cancelled) {
    if (listener) progress = env->GetMethodID(env->GetObjectClass(listener), "progress", "(JJ)V");
    if (env->ExceptionCheck()) { env->ExceptionClear(); progress = nullptr; }
    if (cancel) cancelled = env->GetMethodID(env->GetObjectClass(cancel), "get", "()Z");
    if (env->ExceptionCheck()) { env->ExceptionClear(); cancelled = nullptr; }
}
// Reports progress and reads the cancel flag. A callback that throws counts as a cancel: the
// exception is cleared at once so the rest of the run and its reply make no JNI call with one pending.
bool report(JNIEnv* env, jobject listener, jobject cancel, jmethodID progress, jmethodID cancelled, uint64_t done,
            uint64_t total) {
    if (progress) {
        env->CallVoidMethod(listener, progress, jlong(done), jlong(total));
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
    }
    if (cancelled) {
        const bool stop = env->CallBooleanMethod(cancel, cancelled);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        if (stop) return false;
    }
    return true;
}
}  // namespace

// CardTool.run(args, listener, cancel): args as for `frond-core --card` without the flag, e.g.
// ["make", "sd", "1886", path]. Returns "ok KIND BYTES" or "error CODE MESSAGE". `listener` gets
// progress(done, total) in bytes after each chunk; `cancel` is read after each chunk.
extern "C" JNIEXPORT jstring JNICALL Java_com_rm_frond_CardTool_run(JNIEnv* env, jclass, jobjectArray args,
                                                                  jobject listener, jobject cancel) {
    std::vector<std::string> words;
    for (jsize i = 0, n = env->GetArrayLength(args); i < n; ++i) {
        auto* word = static_cast<jstring>(env->GetObjectArrayElement(args, i));
        const char* chars = env->GetStringUTFChars(word, nullptr);
        words.emplace_back(chars ? chars : "");
        if (chars) env->ReleaseStringUTFChars(word, chars);
        env->DeleteLocalRef(word);
    }
    jmethodID progress = nullptr, cancelled = nullptr;
    lookUp(env, listener, cancel, progress, cancelled);
    const auto result = frondCardTool::run(words, [&](uint64_t done, uint64_t total) {
        return report(env, listener, cancel, progress, cancelled, done, total);
    });
    const std::string reply = result.ok ? "ok " + result.message : "error " + result.code + " " + result.message;
    return env->NewStringUTF(reply.c_str());
}

// CardTool.exportFd(card, fd, listener, cancel): the raw image of a settled SD or Memory Stick Pro
// card file written straight into an open regular file (a picked document), with no temp copy.
// Same checks, progress and reply words as the tool's export; a classic stick is "error kind".
extern "C" JNIEXPORT jstring JNICALL Java_com_rm_frond_CardTool_exportFd(JNIEnv* env, jclass, jstring cardPath, jint out,
                                                                       jobject listener, jobject cancel) {
    jmethodID progress = nullptr, cancelled = nullptr;
    lookUp(env, listener, cancel, progress, cancelled);
    const frondCardTool::Progress report = [&](uint64_t done, uint64_t total) {
        return ::report(env, listener, cancel, progress, cancelled, done, total);
    };
    const char* chars = env->GetStringUTFChars(cardPath, nullptr);
    const std::filesystem::path card(chars ? chars : "");
    if (chars) env->ReleaseStringUTFChars(cardPath, chars);
    auto reply = [&](const frondCardTool::Result& r) {
        const std::string text = r.ok ? "ok " + r.message : "error " + r.code + " " + r.message;
        return env->NewStringUTF(text.c_str());
    };
    using frondCardTool::fail;
    struct stat st{}, target{};
    if (::stat(card.c_str(), &st) < 0 || !S_ISREG(st.st_mode)) return reply(fail("read", "Could not read the card file."));
    if (::fstat(out, &target) < 0 || !S_ISREG(target.st_mode)) return reply(fail("write", "Could not write the output file."));
    if (st.st_dev == target.st_dev && st.st_ino == target.st_ino)
        return reply(fail("same", "The image and the card file are the same file."));
    const int in = ::open(card.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return reply(fail("read", "Could not read the card file."));
    struct Closer { int fd; ~Closer() { ::close(fd); } } closer{in};
    uint8_t header[frondSdFile::headerBytes]{};
    if (!frondSession::readAllAt(in, header, sizeof(header), 0)) return reply(fail("image", "That is not a Frond card file."));
    if (!std::memcmp(header, "FRONDMS", 7)) return reply(fail("kind", "A classic Memory Stick has no raw image."));
    std::error_code ec;
    const std::string journal = card.filename().string() + ".journal-";
    for (std::filesystem::directory_iterator it(frondCardTool::folderOf(card), ec), end; !ec && it != end; it.increment(ec))
        if (it->path().filename().string().rfind(journal, 0) == 0)
            return reply(fail("unsettled", "The card has an unfinished save. Open its machine once to finish it."));
    frondSdFile::Info info;
    if (!frondSdFile::parseHeader(header, info)) return reply(fail("image", "That is not a Frond card file."));
    frondCardTool::Run run{report};
    if (auto verified = frondCardTool::verifySdFile(in, info, uint64_t(st.st_size), header, run); !verified.ok)
        return reply(verified);
    frondCardTool::FileSnapshot snapshot(in, info.payloadBytes);
    std::vector<uint8_t> chunk(frondSdCandidate::chunkBytes);
    for (uint64_t off = 0; off < info.payloadBytes; off += chunk.size()) {
        const auto n = size_t(std::min<uint64_t>(chunk.size(), info.payloadBytes - off));
        if (!snapshot.Read(off, n, chunk.data())) return reply(fail("read", "Could not read the card."));
        if (!frondSdCandidate::writeAll(out, chunk.data(), n, off)) return reply(fail("write", "Could not write the output file."));
        if (!run.report(info.payloadBytes + off + n, 2 * info.payloadBytes)) return reply(fail("cancelled", "Cancelled."));
    }
    if (::ftruncate(out, off_t(info.payloadBytes)) < 0 || ::fsync(out) < 0)
        return reply(fail("write", "Could not write the output file."));
    return reply({true, "exported", "image " + std::to_string(info.payloadBytes)});
}

// CardTool.importFd(fd, kind, dest, listener, cancel): a raw SD or Memory Stick Pro image read straight
// from an open regular file (a picked document) into a new card file, with no temp copy. Reopening the
// document as /proc/self/fd/N goes back through its path, which shared storage refuses to the app.
// Same checks, progress and replies as the tool's import; a classic stick ("ms") is "error kind".
extern "C" JNIEXPORT jstring JNICALL Java_com_rm_frond_CardTool_importFd(JNIEnv* env, jclass, jint in, jstring kindWord,
                                                                       jstring destPath, jobject listener, jobject cancel) {
    jmethodID progress = nullptr, cancelled = nullptr;
    lookUp(env, listener, cancel, progress, cancelled);
    const frondCardTool::Progress report = [&](uint64_t done, uint64_t total) {
        return ::report(env, listener, cancel, progress, cancelled, done, total);
    };
    auto text = [&](jstring s) {
        const char* chars = env->GetStringUTFChars(s, nullptr);
        std::string out(chars ? chars : "");
        if (chars) env->ReleaseStringUTFChars(s, chars);
        return out;
    };
    const std::string word = text(kindWord);
    const std::filesystem::path dest(text(destPath));
    auto reply = [&](const frondCardTool::Result& r) {
        const std::string out = r.ok ? "ok " + r.message : "error " + r.code + " " + r.message;
        return env->NewStringUTF(out.c_str());
    };
    using namespace frondCardTool;
    Kind kind;
    if (!parseKind(word, kind)) return reply(fail("usage", "The card kind is sd, ms or mspro."));
    if (kind == Kind::ms) return reply(fail("kind", "A classic Memory Stick is imported through a copy."));
    struct stat st{};
    if (::fstat(in, &st) < 0 || !S_ISREG(st.st_mode)) return reply(fail("read", "Could not read the card image."));
    const uint64_t bytes = uint64_t(st.st_size);
    const bool taken = kind == Kind::sd ? sdBytesTaken(bytes) : msProMibTaken(frondMsPro::mibOfBytes(bytes));
    if (!taken)
        return reply(fail("size", kind == Kind::sd ? "Use a raw SD image of " + sizeList(kind) + ", aligned to 512 bytes."
                                                   : "Use a raw Memory Stick Pro image of " + sizeList(kind) + "."));
    uint8_t first[8]{};
    if (!frondSession::readAllAt(in, first, sizeof(first), 0) || frondMagic(first))
        return reply(fail("image", "That is a Frond card file, not a raw card image."));
    if (auto space = checkSpace(dest, sdFileBytes(bytes)); !space.ok) return reply(space);
    Run run{report};
    auto result = writeSd(dest, bytes, run, [&](uint64_t off, uint64_t n, uint8_t* out) {
        return frondSession::readAllAt(in, out, size_t(n), off);
    });
    if (result.ok) result.message = std::string(kindName(kind)) + " " + std::to_string(sizeOf(dest));
    return reply(result);
}
