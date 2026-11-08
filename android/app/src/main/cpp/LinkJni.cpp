// libfrondlink.so: the link/ library (pairing, Noise KK, records, mux) for LinkNative.kt. No sockets;
// Kotlin owns them and feeds bytes and the clock. Handles are pointers as jlong. A handle is used
// from one thread at a time (the link engine's), which Kotlin keeps to.
#include <jni.h>

#include <memory>
#include <string>

#include "link.h"

using namespace frond::link;

namespace {

Bytes bytesOf(JNIEnv* env, jbyteArray a) {
    if (!a) return {};
    const jsize n = env->GetArrayLength(a);
    Bytes b(static_cast<size_t>(n));
    if (n) env->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte*>(b.data()));
    return b;
}

jbyteArray arrayOf(JNIEnv* env, const uint8_t* p, size_t n) {
    jbyteArray a = env->NewByteArray(jsize(n));
    if (a && n) env->SetByteArrayRegion(a, 0, jsize(n), reinterpret_cast<const jbyte*>(p));
    return a;
}
jbyteArray arrayOf(JNIEnv* env, const Bytes& b) { return arrayOf(env, b.data(), b.size()); }

template <size_t N>
bool fixed(JNIEnv* env, jbyteArray a, std::array<uint8_t, N>& out) {
    if (!a || env->GetArrayLength(a) != jsize(N)) return false;
    env->GetByteArrayRegion(a, 0, jsize(N), reinterpret_cast<jbyte*>(out.data()));
    return true;
}

std::string stringOf(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r(c ? c : "");
    if (c) env->ReleaseStringUTFChars(s, c);
    return r;
}

// Kotlin passes the identity secret, install id and name each time a link or pairing starts.
bool localInfo(JNIEnv* env, jbyteArray secret, jbyteArray id, jstring name, LocalInfo& out) {
    Key32 s{};
    if (!fixed(env, secret, s) || !fixed(env, id, out.id)) return false;
    const bool ok = identityFromSecret(s, out.identity);
    wipe(s.data(), s.size());
    out.name = stringOf(env, name);
    return ok;
}

struct PairWrap {
    std::shared_ptr<PairCode> code;  // B: the dialog's code, kept while this pairing lives
    std::unique_ptr<Pairing> pairing;
};

Link* L(jlong h) { return reinterpret_cast<Link*>(h); }
PairWrap* P(jlong h) { return reinterpret_cast<PairWrap*>(h); }
std::shared_ptr<PairCode>* C(jlong h) { return reinterpret_cast<std::shared_ptr<PairCode>*>(h); }

}  // namespace

#define FN(name) JNIEXPORT JNICALL Java_com_rm_frond_LinkNative_##name

extern "C" {

// ---- Identity ----

jbyteArray FN(newSecret)(JNIEnv* env, jclass) {
    Identity id = makeIdentity();
    jbyteArray r = arrayOf(env, id.secret.data(), id.secret.size());
    wipe(&id, sizeof id);
    return r;
}

jbyteArray FN(publicKey)(JNIEnv* env, jclass, jbyteArray secret) {
    Key32 s{};
    Identity id;
    if (!fixed(env, secret, s) || !identityFromSecret(s, id)) return nullptr;
    wipe(s.data(), s.size());
    jbyteArray r = arrayOf(env, id.pub.data(), id.pub.size());
    wipe(&id, sizeof id);
    return r;
}

jbyteArray FN(randomBytes)(JNIEnv* env, jclass, jint n) {
    Bytes b(size_t(n > 0 ? n : 0));
    if (!b.empty()) frond::link::randomBytes(b.data(), b.size());
    return arrayOf(env, b);
}

// ---- Wire ----

jint FN(peekFirstFrame)(JNIEnv* env, jclass, jbyteArray data) {
    const auto b = bytesOf(env, data);
    return jint(frond::link::peekFirstFrame(b.data(), b.size()));
}

jbyteArray FN(refuseFrame)(JNIEnv* env, jclass, jint reason) {
    return arrayOf(env, frond::link::refuseFrame(RefuseReason(reason)));
}

// ---- Links ----

// We dial a paired peer.
jlong FN(linkDial)(JNIEnv* env, jclass, jbyteArray secret, jbyteArray id, jstring name, jbyteArray peerId,
                   jbyteArray peerKey, jlong now) {
    LinkConfig cfg;
    InstallId pid{};
    Key32 pkey{};
    if (!localInfo(env, secret, id, name, cfg.local) || !fixed(env, peerId, pid) || !fixed(env, peerKey, pkey)) return 0;
    auto* link = new Link(cfg, pid, pkey, uint64_t(now));
    wipe(&cfg.local.identity, sizeof cfg.local.identity);
    link->start();
    return reinterpret_cast<jlong>(link);
}

// A peer dialled us. paired is the pinned table: 48 bytes per peer, id then key.
jlong FN(linkAnswer)(JNIEnv* env, jclass, jbyteArray secret, jbyteArray id, jstring name, jbyteArray paired, jlong now) {
    LinkConfig cfg;
    if (!localInfo(env, secret, id, name, cfg.local)) return 0;
    const auto table = std::make_shared<Bytes>(bytesOf(env, paired));
    auto lookup = [table](const InstallId& who, Key32& key) {
        for (size_t at = 0; at + 48 <= table->size(); at += 48) {
            if (std::equal(who.begin(), who.end(), table->begin() + long(at))) {
                std::copy(table->begin() + long(at + 16), table->begin() + long(at + 48), key.begin());
                return true;
            }
        }
        return false;
    };
    auto* link = new Link(cfg, lookup, uint64_t(now));
    wipe(&cfg.local.identity, sizeof cfg.local.identity);
    return reinterpret_cast<jlong>(link);
}

void FN(linkFree)(JNIEnv*, jclass, jlong h) { delete L(h); }

void FN(linkReceive)(JNIEnv* env, jclass, jlong h, jbyteArray data, jint n, jlong now) {
    if (n <= 0) return;
    jbyte* p = env->GetByteArrayElements(data, nullptr);
    L(h)->receive(reinterpret_cast<const uint8_t*>(p), size_t(n), uint64_t(now));
    env->ReleaseByteArrayElements(data, p, JNI_ABORT);
}

void FN(linkTick)(JNIEnv*, jclass, jlong h, jlong now) { L(h)->tick(uint64_t(now)); }

jbyteArray FN(linkTakeOutput)(JNIEnv* env, jclass, jlong h, jint max) {
    if (!L(h)->hasOutput()) return nullptr;
    return arrayOf(env, L(h)->takeOutput(max > 0 ? size_t(max) : SIZE_MAX));
}

jboolean FN(linkHasOutput)(JNIEnv*, jclass, jlong h) { return L(h)->hasOutput(); }
void FN(linkClose)(JNIEnv*, jclass, jlong h, jint reason) { L(h)->close(CloseReason(reason)); }
jint FN(linkState)(JNIEnv*, jclass, jlong h) { return jint(L(h)->state()); }
jint FN(linkError)(JNIEnv*, jclass, jlong h) { return jint(L(h)->error()); }
jbyteArray FN(linkPeerId)(JNIEnv* env, jclass, jlong h) {
    const auto& id = L(h)->peerId();
    return arrayOf(env, id.data(), id.size());
}
jstring FN(linkPeerName)(JNIEnv* env, jclass, jlong h) { return env->NewStringUTF(L(h)->peerName().c_str()); }
jlong FN(linkRtt)(JNIEnv*, jclass, jlong h) { return jlong(L(h)->rttMs()); }

// The Bluetooth radio choice (MsgType::Radio): ours to the peer, and the peer's, oldest first, as
// {senderHolds 0 or 1, at} or null.
void FN(linkSendRadio)(JNIEnv*, jclass, jlong h, jboolean senderHolds, jlong at) {
    L(h)->sendRadio({senderHolds == JNI_TRUE, uint64_t(at)});
}
jlongArray FN(linkNextRadio)(JNIEnv* env, jclass, jlong h) {
    auto& queue = L(h)->radio;
    if (queue.empty()) return nullptr;
    const RadioBody r = queue.front();
    queue.pop_front();
    const jlong v[2] = {r.senderHolds ? 1 : 0, jlong(r.at)};
    jlongArray a = env->NewLongArray(2);
    if (a) env->SetLongArrayRegion(a, 0, 2, v);
    return a;
}

// ---- Channels ----

jint FN(muxOpen)(JNIEnv* env, jclass, jlong h, jint kind, jstring machine, jstring device) {
    return L(h)->mux().open(ChannelKind(kind), stringOf(env, machine), stringOf(env, device));
}
jboolean FN(muxAccept)(JNIEnv* env, jclass, jlong h, jint ch, jstring machine, jstring device) {
    return L(h)->mux().accept(uint16_t(ch), stringOf(env, machine), stringOf(env, device));
}
jboolean FN(muxWaiting)(JNIEnv*, jclass, jlong h, jint ch) { return L(h)->mux().waiting(uint16_t(ch)); }
jboolean FN(muxBusy)(JNIEnv* env, jclass, jlong h, jint ch, jstring machine, jstring device) {
    return L(h)->mux().busy(uint16_t(ch), stringOf(env, machine), stringOf(env, device));
}
void FN(muxClose)(JNIEnv*, jclass, jlong h, jint ch, jint reason) { L(h)->mux().close(uint16_t(ch), CloseReason(reason)); }
void FN(muxSendState)(JNIEnv*, jclass, jlong h, jint ch, jint state) { L(h)->mux().sendState(uint16_t(ch), PeerState(state)); }
jint FN(muxSend)(JNIEnv* env, jclass, jlong h, jint ch, jbyteArray data, jint offset, jint n) {
    if (n <= 0) return 0;
    jbyte* p = env->GetByteArrayElements(data, nullptr);
    const size_t taken = L(h)->mux().send(uint16_t(ch), reinterpret_cast<const uint8_t*>(p) + offset, size_t(n));
    env->ReleaseByteArrayElements(data, p, JNI_ABORT);
    return jint(taken);
}
void FN(muxConsumed)(JNIEnv*, jclass, jlong h, jint ch, jint n) { L(h)->mux().consumed(uint16_t(ch), size_t(n)); }

// The next channel event as bytes, or null: kind u8, channel u16, channel kind u8, code u8,
// machine str8, device str8, then the data. LinkEvents.kt reads it.
jbyteArray FN(muxNextEvent)(JNIEnv* env, jclass, jlong h) {
    auto& events = L(h)->mux().events;
    if (events.empty()) return nullptr;
    ChannelEvent e = std::move(events.front());
    events.pop_front();
    Bytes b;
    putU8(b, uint8_t(e.kind));
    putU16(b, e.channel);
    putU8(b, uint8_t(e.channelKind));
    putU8(b, e.code);
    putStr8(b, e.machine);
    putStr8(b, e.device);
    putBytes(b, e.data.data(), e.data.size());
    return arrayOf(env, b);
}

// ---- Pairing ----

jlong FN(codeNew)(JNIEnv*, jclass, jlong now) {
    return reinterpret_cast<jlong>(new std::shared_ptr<PairCode>(std::make_shared<PairCode>(uint64_t(now))));
}
void FN(codeFree)(JNIEnv*, jclass, jlong h) { delete C(h); }
jstring FN(codeText)(JNIEnv* env, jclass, jlong h) { return env->NewStringUTF((*C(h))->code().c_str()); }
jint FN(codeTriesLeft)(JNIEnv*, jclass, jlong h) { return (*C(h))->triesLeft(); }
void FN(codeSucceeded)(JNIEnv*, jclass, jlong h) { (*C(h))->succeeded(); }

// A: the code as typed; queues P1.
jlong FN(pairDial)(JNIEnv* env, jclass, jbyteArray secret, jbyteArray id, jstring name, jstring code, jlong now) {
    LocalInfo local;
    if (!localInfo(env, secret, id, name, local)) return 0;
    auto* w = new PairWrap;
    w->pairing = std::make_unique<Pairing>(local, stringOf(env, code), uint64_t(now));
    wipe(&local.identity, sizeof local.identity);
    w->pairing->start();
    return reinterpret_cast<jlong>(w);
}

// B: one per incoming pairing connection, sharing the dialog's code.
jlong FN(pairAnswer)(JNIEnv* env, jclass, jbyteArray secret, jbyteArray id, jstring name, jlong code, jlong now) {
    LocalInfo local;
    if (!localInfo(env, secret, id, name, local)) return 0;
    auto* w = new PairWrap;
    w->code = *C(code);
    w->pairing = std::make_unique<Pairing>(local, *w->code, uint64_t(now));
    wipe(&local.identity, sizeof local.identity);
    return reinterpret_cast<jlong>(w);
}

void FN(pairFree)(JNIEnv*, jclass, jlong h) { delete P(h); }
void FN(pairReceive)(JNIEnv* env, jclass, jlong h, jbyteArray data, jint n, jlong now) {
    if (n <= 0) return;
    jbyte* p = env->GetByteArrayElements(data, nullptr);
    P(h)->pairing->receive(reinterpret_cast<const uint8_t*>(p), size_t(n), uint64_t(now));
    env->ReleaseByteArrayElements(data, p, JNI_ABORT);
}
void FN(pairTick)(JNIEnv*, jclass, jlong h, jlong now) { P(h)->pairing->tick(uint64_t(now)); }
jbyteArray FN(pairTakeOutput)(JNIEnv* env, jclass, jlong h) {
    const auto out = P(h)->pairing->takeOutput();
    return out.empty() ? nullptr : arrayOf(env, out);
}
jint FN(pairResult)(JNIEnv*, jclass, jlong h) { return jint(P(h)->pairing->result()); }
// After Paired: id[16], key[32], then the name.
jbyteArray FN(pairPeer)(JNIEnv* env, jclass, jlong h) {
    const auto& peer = P(h)->pairing->peer();
    Bytes b;
    putBytes(b, peer.id);
    putBytes(b, peer.key);
    b.insert(b.end(), peer.name.begin(), peer.name.end());
    return arrayOf(env, b);
}

}  // extern "C"
