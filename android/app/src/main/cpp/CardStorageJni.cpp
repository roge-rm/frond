// The storage library (src/common/storage) for the app: card files edited in place, raw images
// and the FAT on either, for CardFile.kt and FatImage.kt. Every call runs on the caller's thread;
// one handle is used by one thread at a time. Paths and names are Java strings, UTF-16 here and
// UTF-8 in the library.
//
// Errors come back as FatImage.FatException with the library's message, a cancel as
// FatImage.Cancelled. An exception thrown by a Kotlin callback (a stream, a hook, a replace's work)
// stops the change and comes back as it is.
#include <jni.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "storage/CardFile.h"
#include "storage/FatImage.h"

namespace {

using frondStorage::Blocks;
using frondStorage::FatImage;

JavaVM* vm = nullptr;

// A Java exception is pending; the library unwinds and the call returns with it.
struct JavaThrown {};

JNIEnv* envHere() {
    JNIEnv* env = nullptr;
    if (!vm || vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return nullptr;
    return env;
}

void check(JNIEnv* env) {
    if (env->ExceptionCheck()) throw JavaThrown{};
}

std::string utf8(JNIEnv* env, jstring s) {
    if (!s) return {};
    const jsize n = env->GetStringLength(s);
    std::u16string text(size_t(n), u'\0');
    env->GetStringRegion(s, 0, n, reinterpret_cast<jchar*>(text.data()));
    check(env);
    return frondStorage::fat::utf8(text);
}

jstring java(JNIEnv* env, const std::string& s) {
    const auto text = frondStorage::fat::utf16(s);
    jstring out = env->NewString(reinterpret_cast<const jchar*>(text.data()), jsize(text.size()));
    check(env);
    return out;
}

void throwNamed(JNIEnv* env, const char* cls, const std::string* message) {
    if (env->ExceptionCheck()) return;
    jclass c = env->FindClass(cls);
    if (!c) return;
    jmethodID init = env->GetMethodID(c, "<init>", message ? "(Ljava/lang/String;)V" : "()V");
    if (!init) return;
    jobject e = message ? env->NewObject(c, init, java(env, *message)) : env->NewObject(c, init);
    if (e) env->Throw(static_cast<jthrowable>(e));
}

// Runs `f`, turning what the library throws into Java exceptions. `fallback` is returned then.
template <class R, class F>
R guarded(JNIEnv* env, R fallback, F&& f) {
    try {
        return f();
    } catch (const JavaThrown&) {
    } catch (const frondStorage::Cancelled&) {
        throwNamed(env, "com/rm/frond/FatImage$Cancelled", nullptr);
    } catch (const frondStorage::Error& e) {
        throwNamed(env, "com/rm/frond/FatImage$FatException", &e.message);
    } catch (const std::bad_alloc&) {
        if (!env->ExceptionCheck()) env->ThrowNew(env->FindClass("java/lang/OutOfMemoryError"), "card storage");
    } catch (const std::exception& e) {
        const std::string message = e.what();
        throwNamed(env, "com/rm/frond/FatImage$FatException", &message);
    }
    return fallback;
}

template <class F>
void guardedVoid(JNIEnv* env, F&& f) {
    guarded(env, 0, [&] {
        f();
        return 0;
    });
}

// An open card file or raw image. `owner` is the Kotlin CardFile, for its hooks.
struct Card {
    std::unique_ptr<frondStorage::CardFile> card;
    std::unique_ptr<frondStorage::RawBlocks> raw;
    jobject owner{nullptr};
    Blocks& blocks() { return card ? static_cast<Blocks&>(*card) : static_cast<Blocks&>(*raw); }
};

Card* cardOf(jlong h) { return reinterpret_cast<Card*>(h); }
FatImage* fatOf(jlong h) { return reinterpret_cast<FatImage*>(h); }

// Calls owner.name(args) for a hook; a Kotlin exception stops the change.
void callOwner(jobject owner, const char* name, const char* step) {
    JNIEnv* env = envHere();
    if (!env || !owner) return;
    check(env);
    jclass c = env->GetObjectClass(owner);
    if (step) {
        jmethodID m = env->GetMethodID(c, name, "(Ljava/lang/String;)V");
        check(env);
        env->CallVoidMethod(owner, m, java(env, step));
    } else {
        jmethodID m = env->GetMethodID(c, name, "()V");
        check(env);
        env->CallVoidMethod(owner, m);
    }
    env->DeleteLocalRef(c);
    check(env);
}

frondStorage::Entry entryOf(JNIEnv* env, jstring name, jboolean directory, jlong size, jint firstCluster) {
    frondStorage::Entry e;
    e.name = utf8(env, name);
    e.directory = directory;
    e.size = size;
    e.firstCluster = uint32_t(firstCluster);
    return e;
}

// Entries as FatImage.kt decodes them: names, short names, then the numbers (directory, size,
// first cluster, DOS date, DOS time) for each.
jobjectArray entries(JNIEnv* env, const std::vector<frondStorage::Entry>& list) {
    jclass object = env->FindClass("java/lang/Object");
    jobjectArray out = env->NewObjectArray(jsize(2 * list.size() + 1), object, nullptr);
    check(env);
    std::vector<jlong> numbers;
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& e = list[i];
        jstring name = java(env, e.name), shortName = java(env, e.shortName);
        env->SetObjectArrayElement(out, jsize(1 + 2 * i), name);
        env->SetObjectArrayElement(out, jsize(2 + 2 * i), shortName);
        env->DeleteLocalRef(name);
        env->DeleteLocalRef(shortName);
        numbers.insert(numbers.end(), {jlong(e.directory), jlong(e.size), jlong(e.firstCluster), jlong(e.modified.date),
                                       jlong(e.modified.time)});
    }
    jlongArray n = env->NewLongArray(jsize(numbers.size()));
    check(env);
    env->SetLongArrayRegion(n, 0, jsize(numbers.size()), numbers.data());
    env->SetObjectArrayElement(out, 0, n);
    return out;
}

jlongArray longs(JNIEnv* env, const std::vector<jlong>& v) {
    jlongArray out = env->NewLongArray(jsize(v.size()));
    check(env);
    env->SetLongArrayRegion(out, 0, jsize(v.size()), v.data());
    return out;
}

jstring reply(JNIEnv* env, const frondStorage::Result& r) {
    return java(env, r.ok ? std::string("ok") : "error " + r.code + " " + r.message);
}

constexpr jint chunkBytes = 1 << 20;

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* javaVm, void*) {
    vm = javaVm;
    return JNI_VERSION_1_6;
}

#define STORAGE(name) Java_com_rm_frond_CardStorage_##name

// ---- Card files ----

// openCard(path, writable, owner): a card file opened for a Files… visit, settled first.
extern "C" JNIEXPORT jlong JNICALL STORAGE(openCard)(JNIEnv* env, jclass, jstring path, jboolean writable, jobject owner) {
    return guarded(env, jlong(0), [&] {
        auto c = std::make_unique<Card>();
        c->card = frondStorage::CardFile::open(utf8(env, path), writable);
        c->owner = env->NewGlobalRef(owner);
        return reinterpret_cast<jlong>(c.release());
    });
}

// openRaw(path, writable): a raw image, written straight in.
extern "C" JNIEXPORT jlong JNICALL STORAGE(openRaw)(JNIEnv* env, jclass, jstring path, jboolean writable) {
    return guarded(env, jlong(0), [&] {
        auto c = std::make_unique<Card>();
        c->raw = std::make_unique<frondStorage::RawBlocks>(utf8(env, path), writable);
        return reinterpret_cast<jlong>(c.release());
    });
}

// close(card): fixes the CRCs of data a failed change left, then closes and unlocks the file.
extern "C" JNIEXPORT void JNICALL STORAGE(close)(JNIEnv* env, jclass, jlong h) {
    std::unique_ptr<Card> c(cardOf(h));
    if (!c) return;
    if (c->card && !c->card->dead) guardedVoid(env, [&] { c->card->settleData(); });
    // A settle that fails leaves the pending list, which the next settle finishes.
    if (c->card) c->card->dead = true;
    if (c->owner) env->DeleteGlobalRef(c->owner);
}

// hooks(card, firstWrite, crash): whether to call the owner's firstWrite() and crashAt(step).
extern "C" JNIEXPORT void JNICALL STORAGE(hooks)(JNIEnv*, jclass, jlong h, jboolean firstWrite, jboolean crash) {
    auto* c = cardOf(h);
    if (!c->card) return;
    jobject owner = c->owner;
    c->card->beforeFirstWrite = nullptr;
    c->card->crashHook = nullptr;
    if (firstWrite) c->card->beforeFirstWrite = [owner] { callOwner(owner, "firstWrite", nullptr); };
    if (crash) c->card->crashHook = [owner](const char* step) { callOwner(owner, "crashAt", step); };
}

// Tests: after a simulated crash nothing more reaches the disk.
extern "C" JNIEXPORT void JNICALL STORAGE(kill)(JNIEnv*, jclass, jlong h) {
    if (auto* c = cardOf(h); c->card) c->card->dead = true;
}

// numbers(card): length, generation, card bytes written, journal bytes written.
extern "C" JNIEXPORT jlongArray JNICALL STORAGE(numbers)(JNIEnv* env, jclass, jlong h) {
    return guarded(env, jlongArray(nullptr), [&] {
        auto* c = cardOf(h);
        if (!c->card) return longs(env, {jlong(c->raw->length()), 0, 0, 0});
        return longs(env, {jlong(c->card->payloadBytes()), jlong(c->card->generation()), jlong(c->card->cardBytesWritten),
                           jlong(c->card->journalBytesWritten)});
    });
}

// writes(card): each write into the card file as file offset, length.
extern "C" JNIEXPORT jlongArray JNICALL STORAGE(writes)(JNIEnv* env, jclass, jlong h) {
    return guarded(env, jlongArray(nullptr), [&] {
        std::vector<jlong> out;
        if (auto* c = cardOf(h); c->card)
            for (const auto& [at, n] : c->card->writes) out.insert(out.end(), {jlong(at), jlong(n)});
        return longs(env, out);
    });
}

extern "C" JNIEXPORT jboolean JNICALL STORAGE(inUse)(JNIEnv* env, jclass, jstring path) {
    return guarded(env, jboolean(false), [&] { return jboolean(frondStorage::inUse(utf8(env, path))); });
}

extern "C" JNIEXPORT jboolean JNICALL STORAGE(unsettled)(JNIEnv* env, jclass, jstring path) {
    return guarded(env, jboolean(false), [&] { return jboolean(frondStorage::unsettled(utf8(env, path))); });
}

// settle(path): what Files… or a crash left beside the card finished. "ok" or "error CODE MESSAGE".
extern "C" JNIEXPORT jstring JNICALL STORAGE(settle)(JNIEnv* env, jclass, jstring path) {
    return guarded(env, jstring(nullptr), [&] { return reply(env, frondStorage::settleChanges(utf8(env, path))); });
}

// readiness(path): as settle, and "error busy" when the card is open in Files.
extern "C" JNIEXPORT jstring JNICALL STORAGE(readiness)(JNIEnv* env, jclass, jstring path) {
    return guarded(env, jstring(nullptr), [&] { return reply(env, frondStorage::readiness(utf8(env, path))); });
}

// verify(path): the generation (0 for v1) when the header and every CRC check out, else -1.
extern "C" JNIEXPORT jlong JNICALL STORAGE(verify)(JNIEnv* env, jclass, jstring path) {
    return guarded(env, jlong(-1), [&] {
        const auto g = frondStorage::verifiedGeneration(utf8(env, path));
        return g ? jlong(*g) : jlong(-1);
    });
}

// header(path): the first 64 bytes of a card file, or null.
extern "C" JNIEXPORT jbyteArray JNICALL STORAGE(header)(JNIEnv* env, jclass, jstring path) {
    return guarded(env, jbyteArray(nullptr), [&]() -> jbyteArray {
        const auto p = std::filesystem::path(utf8(env, path));
        const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return nullptr;
        frondStorage::Fd closer(fd);
        uint8_t head[frondSdFile::headerBytes];
        if (!frondStorage::readAllAt(fd, head, sizeof(head), 0)) return nullptr;
        jbyteArray out = env->NewByteArray(jsize(sizeof(head)));
        check(env);
        env->SetByteArrayRegion(out, 0, jsize(sizeof(head)), reinterpret_cast<const jbyte*>(head));
        return out;
    });
}

// encodeV2(payload, generation): a v2 card file of the payload, as the card tool makes one.
extern "C" JNIEXPORT jbyteArray JNICALL STORAGE(encodeV2)(JNIEnv* env, jclass, jbyteArray payload, jlong generation) {
    return guarded(env, jbyteArray(nullptr), [&]() -> jbyteArray {
        std::vector<uint8_t> in(size_t(env->GetArrayLength(payload))), file;
        env->GetByteArrayRegion(payload, 0, jsize(in.size()), reinterpret_cast<jbyte*>(in.data()));
        if (!frondSdFile::encodeV2(in.data(), in.size(), uint64_t(generation), file)) return nullptr;
        jbyteArray out = env->NewByteArray(jsize(file.size()));
        check(env);
        env->SetByteArrayRegion(out, 0, jsize(file.size()), reinterpret_cast<const jbyte*>(file.data()));
        return out;
    });
}

// format(card, label): a fresh FAT12 or FAT16 file system over the whole card.
extern "C" JNIEXPORT void JNICALL STORAGE(format)(JNIEnv* env, jclass, jlong h, jstring label) {
    guardedVoid(env, [&] { FatImage::format(cardOf(h)->blocks(), label ? utf8(env, label) : "FROND"); });
}

// ---- The FAT on an open card ----

extern "C" JNIEXPORT jlong JNICALL STORAGE(fatOpen)(JNIEnv* env, jclass, jlong h) {
    return guarded(env, jlong(0), [&] { return reinterpret_cast<jlong>(new FatImage(cardOf(h)->blocks())); });
}

extern "C" JNIEXPORT void JNICALL STORAGE(fatClose)(JNIEnv*, jclass, jlong f) { delete fatOf(f); }

// fatNumbers(fat): cluster bytes, free bytes, total bytes, bytes written, committed, FAT bits.
extern "C" JNIEXPORT jlongArray JNICALL STORAGE(fatNumbers)(JNIEnv* env, jclass, jlong f) {
    return guarded(env, jlongArray(nullptr), [&] {
        auto* fat = fatOf(f);
        return longs(env, {jlong(fat->clusterBytes()), jlong(fat->freeBytes()), jlong(fat->totalBytes()),
                           jlong(fat->bytesWritten), jlong(fat->committed), jlong(fat->fatBits())});
    });
}

extern "C" JNIEXPORT jobjectArray JNICALL STORAGE(fatList)(JNIEnv* env, jclass, jlong f, jstring path) {
    return guarded(env, jobjectArray(nullptr), [&] { return entries(env, fatOf(f)->list(utf8(env, path))); });
}

extern "C" JNIEXPORT jobjectArray JNICALL STORAGE(fatFind)(JNIEnv* env, jclass, jlong f, jstring path) {
    return guarded(env, jobjectArray(nullptr), [&] {
        std::vector<frondStorage::Entry> found;
        if (auto e = fatOf(f)->find(utf8(env, path))) found.push_back(*e);
        return entries(env, found);
    });
}

// fatRead(fat, name, directory, size, firstCluster, out): the file's bytes into an OutputStream.
extern "C" JNIEXPORT void JNICALL STORAGE(fatRead)(JNIEnv* env, jclass, jlong f, jstring name, jboolean directory,
                                                    jlong size, jint firstCluster, jobject out) {
    guardedVoid(env, [&] {
        const auto entry = entryOf(env, name, directory, size, firstCluster);
        jmethodID write = env->GetMethodID(env->GetObjectClass(out), "write", "([BII)V");
        check(env);
        jbyteArray buffer = env->NewByteArray(chunkBytes);
        check(env);
        fatOf(f)->read(entry, [&](const char* b, int64_t n) {
            for (int64_t done = 0; done < n;) {
                const jint part = jint(std::min<int64_t>(chunkBytes, n - done));
                env->SetByteArrayRegion(buffer, 0, part, reinterpret_cast<const jbyte*>(b + done));
                env->CallVoidMethod(out, write, buffer, 0, part);
                check(env);
                done += part;
            }
        });
        env->DeleteLocalRef(buffer);
    });
}

// fatAdd(fat, path, name, input, size): a new file of `size` bytes from an InputStream.
extern "C" JNIEXPORT void JNICALL STORAGE(fatAdd)(JNIEnv* env, jclass, jlong f, jstring path, jstring name, jobject input,
                                                   jlong size) {
    guardedVoid(env, [&] {
        jmethodID read = env->GetMethodID(env->GetObjectClass(input), "read", "([BII)I");
        check(env);
        jbyteArray buffer = env->NewByteArray(chunkBytes);
        check(env);
        fatOf(f)->add(utf8(env, path), utf8(env, name), [&](char* b, int64_t n) -> int64_t {
            const jint got = env->CallIntMethod(input, read, buffer, 0, jint(std::min<int64_t>(chunkBytes, n)));
            check(env);
            if (got <= 0) return 0;
            env->GetByteArrayRegion(buffer, 0, got, reinterpret_cast<jbyte*>(b));
            return got;
        }, size);
        env->DeleteLocalRef(buffer);
    });
}

extern "C" JNIEXPORT void JNICALL STORAGE(fatMakeFolder)(JNIEnv* env, jclass, jlong f, jstring path, jstring name) {
    guardedVoid(env, [&] { fatOf(f)->makeFolder(utf8(env, path), utf8(env, name)); });
}

extern "C" JNIEXPORT void JNICALL STORAGE(fatRemove)(JNIEnv* env, jclass, jlong f, jstring path) {
    guardedVoid(env, [&] { fatOf(f)->remove(utf8(env, path)); });
}

extern "C" JNIEXPORT void JNICALL STORAGE(fatMove)(JNIEnv* env, jclass, jlong f, jstring from, jstring to, jstring newName) {
    guardedVoid(env, [&] { fatOf(f)->move(utf8(env, from), utf8(env, to), utf8(env, newName)); });
}

// fatCopy(fat, from, to, newName, steps): steps.cancelled() is asked and steps.copied(bytes) told
// as the copy goes.
extern "C" JNIEXPORT void JNICALL STORAGE(fatCopy)(JNIEnv* env, jclass, jlong f, jstring from, jstring to, jstring newName,
                                                    jobject steps) {
    guardedVoid(env, [&] {
        jclass c = env->GetObjectClass(steps);
        jmethodID cancelled = env->GetMethodID(c, "cancelled", "()Z");
        check(env);
        jmethodID copied = env->GetMethodID(c, "copied", "(J)V");
        check(env);
        fatOf(f)->copy(
            utf8(env, from), utf8(env, to), utf8(env, newName),
            [&] {
                const bool stop = env->CallBooleanMethod(steps, cancelled);
                check(env);
                return stop;
            },
            [&](int64_t done) {
                env->CallVoidMethod(steps, copied, jlong(done));
                check(env);
            });
    });
}

// fatReplace(fat, victim, work): work.run() makes the new entry; the victim goes in the same commit.
extern "C" JNIEXPORT void JNICALL STORAGE(fatReplace)(JNIEnv* env, jclass, jlong f, jstring victim, jobject work) {
    guardedVoid(env, [&] {
        jmethodID run = env->GetMethodID(env->GetObjectClass(work), "run", "()V");
        check(env);
        fatOf(f)->replace(utf8(env, victim), [&] {
            env->CallVoidMethod(work, run);
            check(env);
        });
    });
}

extern "C" JNIEXPORT jlong JNICALL STORAGE(fatSizeOf)(JNIEnv* env, jclass, jlong f, jstring path) {
    return guarded(env, jlong(0), [&] { return jlong(fatOf(f)->sizeOf(utf8(env, path))); });
}

extern "C" JNIEXPORT jboolean JNICALL STORAGE(fatCopiesMatch)(JNIEnv* env, jclass, jlong f) {
    return guarded(env, jboolean(false), [&] { return jboolean(fatOf(f)->fatCopiesMatch()); });
}

// fatCheck(fat): the problems an fsck would find, as text.
extern "C" JNIEXPORT jobjectArray JNICALL STORAGE(fatCheck)(JNIEnv* env, jclass, jlong f) {
    return guarded(env, jobjectArray(nullptr), [&] {
        const auto problems = fatOf(f)->check();
        jobjectArray out = env->NewObjectArray(jsize(problems.size()), env->FindClass("java/lang/String"), nullptr);
        check(env);
        for (size_t i = 0; i < problems.size(); ++i) {
            jstring s = java(env, problems[i]);
            env->SetObjectArrayElement(out, jsize(i), s);
            env->DeleteLocalRef(s);
        }
        return out;
    });
}

extern "C" JNIEXPORT void JNICALL STORAGE(sync)(JNIEnv* env, jclass, jlong h) {
    guardedVoid(env, [&] { cardOf(h)->blocks().sync(); });
}

// checkName(name): throws FatException when FAT can't hold the name.
extern "C" JNIEXPORT void JNICALL STORAGE(checkName)(JNIEnv* env, jclass, jstring name) {
    guardedVoid(env, [&] { frondStorage::fat::checkName(utf8(env, name)); });
}
