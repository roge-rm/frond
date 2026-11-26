// The Android link library's JNI entry points (LinkJni.cpp) on a fake JNIEnv: two installs dial,
// answer and pair through them, with the bytes between them cut, dropped or changed and the
// channel calls, numbers and lengths Kotlin passes taken from the input.
#include <fuzzer/FuzzedDataProvider.h>

#include "FakeJni.h"
#include "../../android/app/src/main/cpp/LinkJni.cpp"

namespace {
#define J(name) Java_com_rm_frond_LinkNative_##name

struct Side {
    jbyteArray secret, id, name;
    jlong link{0}, pair{0};
};

// Moves one side's output to the other, perhaps cut short or with a byte changed. Every choice is
// read before looking at the output, so an input means the same whatever the handshake made.
void Carry(fakeJni::Env& env, FuzzedDataProvider& data, jlong from, jlong to, bool pairing, jlong now) {
    JNIEnv* e = env.Jni();
    const jint max = data.ConsumeIntegralInRange(0, 4096);
    const int change = data.ConsumeIntegralInRange(0, 7);
    const size_t where = data.ConsumeIntegralInRange<size_t>(0, 4095);
    const uint8_t mask = data.ConsumeIntegralInRange(1, 255);
    // Kotlin passes the array's own length; a shorter or longer one is a caller mistake the library must survive.
    const bool wrongLength = !data.ConsumeIntegralInRange(0, 15);
    const jint length = data.ConsumeIntegralInRange<jint>(-1, 4160);
    const jbyteArray out = pairing ? J(pairTakeOutput)(e, nullptr, from) : J(linkTakeOutput)(e, nullptr, from, max);
    auto bytes = fakeJni::Env::Read(out);
    if (bytes.empty() || change == 2) return;
    if (change == 0) bytes.resize(where % (bytes.size() + 1));
    if (change == 1) bytes[where % bytes.size()] ^= mask;
    if (bytes.empty()) return;
    const jbyteArray in = env.Bytes(bytes);
    const jint n = wrongLength ? length : jint(bytes.size());
    if (pairing) J(pairReceive)(e, nullptr, to, in, n, now);
    else J(linkReceive)(e, nullptr, to, in, n, now);
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* bytes, size_t size) {
    if (size > 65536) return 0;
    FuzzedDataProvider data(bytes, size);
    fakeJni::Env env;
    JNIEnv* e = env.Jni();
    const std::array<uint8_t, 32> secretA{1, 2, 3}, secretB{4, 5, 6};
    const std::array<uint8_t, 16> idA{0xa}, idB{0xb};
    Side a{env.Bytes(secretA), env.Bytes(idA), env.Bytes(std::string("Alpha"))};
    Side b{env.Bytes(secretB), env.Bytes(idB), env.Bytes(data.ConsumeRandomLengthString(80))};
    const auto keyA = fakeJni::Env::Read(J(publicKey)(e, nullptr, a.secret));
    const auto keyB = fakeJni::Env::Read(J(publicKey)(e, nullptr, b.secret));
    jlong now = 1000;
    const bool pairing = data.ConsumeBool();
    jlong code = 0;
    if (pairing) {
        code = J(codeNew)(e, nullptr, now);
        auto text = fakeJni::Env::Read(J(codeTextUtf8)(e, nullptr, code));
        if (data.ConsumeBool()) text = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange(0, 12));
        a.pair = J(pairDialUtf8)(e, nullptr, a.secret, a.id, a.name, env.Bytes(text), now);
        b.pair = J(pairAnswerUtf8)(e, nullptr, b.secret, b.id, b.name, code, now);
    } else {
        std::vector<uint8_t> table(idA.begin(), idA.end());
        table.insert(table.end(), keyA.begin(), keyA.end());
        if (data.ConsumeBool()) table.resize(data.ConsumeIntegralInRange<size_t>(0, table.size() + 8));
        a.link = J(linkDialUtf8)(e, nullptr, a.secret, a.id, a.name, env.Bytes(idB), env.Bytes(keyB), now);
        b.link = J(linkAnswerUtf8)(e, nullptr, b.secret, b.id, b.name, env.Bytes(table), now);
    }
    std::vector<jint> channels{1, 2};
    for (unsigned step = 0; step < 200 && data.remaining_bytes() && (pairing ? a.pair && b.pair : a.link && b.link); ++step) {
        Side& s = data.ConsumeBool() ? a : b;
        Side& t = &s == &a ? b : a;
        now += data.ConsumeIntegralInRange(0, 5000);
        if (pairing) {
            switch (data.ConsumeIntegralInRange(0, 3)) {
                case 0: Carry(env, data, s.pair, t.pair, true, now); break;
                case 1: J(pairTick)(e, nullptr, s.pair, now); break;
                case 2: J(pairResult)(e, nullptr, s.pair), J(pairPeer)(e, nullptr, s.pair), J(codeTriesLeft)(e, nullptr, code); break;
                case 3: if (J(pairResult)(e, nullptr, s.pair) == jint(PairResult::Paired)) J(codeSucceeded)(e, nullptr, code); break;
            }
            continue;
        }
        const jint ch = channels[data.ConsumeIntegralInRange<size_t>(0, 15) % channels.size()];
        switch (data.ConsumeIntegralInRange(0, 13)) {
            case 0: case 1: case 2: Carry(env, data, s.link, t.link, false, now); break;
            case 3: J(linkTick)(e, nullptr, s.link, now); break;
            case 4: {
                const auto machine = env.Bytes(data.ConsumeRandomLengthString(40)), device = env.Bytes(data.ConsumeRandomLengthString(40));
                const jint opened = J(muxOpenUtf8)(e, nullptr, s.link, data.ConsumeIntegralInRange(0, 4), machine, device);
                if (opened > 0) channels.push_back(opened);
                break;
            }
            case 5: {
                const auto machine = env.Bytes(data.ConsumeRandomLengthString(40)), device = env.Bytes(data.ConsumeRandomLengthString(40));
                if (data.ConsumeBool()) J(muxAcceptUtf8)(e, nullptr, s.link, ch, machine, device);
                else J(muxBusyUtf8)(e, nullptr, s.link, ch, machine, device);
                break;
            }
            case 6: {
                // Kotlin sends a part of its buffer: offset and length inside it, or (rarely here) not.
                const bool wrong = !data.ConsumeIntegralInRange(0, 15);
                const auto skip = data.ConsumeIntegralInRange<jint>(0, 3000), wrongOffset = data.ConsumeIntegralInRange<jint>(-4, 3004),
                           wrongLength = data.ConsumeIntegralInRange<jint>(-4, 3064);
                const auto payload = data.ConsumeBytes<uint8_t>(data.ConsumeIntegralInRange(1, 3000));
                if (payload.empty()) break;
                const jint offset = wrong ? wrongOffset : skip % jint(payload.size());
                const jint n = wrong ? wrongLength : jint(payload.size()) - offset;
                J(muxSend)(e, nullptr, s.link, ch, env.Bytes(payload), offset, n);
                break;
            }
            case 7: J(muxConsumed)(e, nullptr, s.link, ch, data.ConsumeIntegralInRange(-8, 70000)); break;
            case 8:
                for (jbyteArray ev; (ev = J(muxNextEvent)(e, nullptr, s.link));) fakeJni::Env::Read(ev);
                while (J(linkNextRadio)(e, nullptr, s.link)) {}
                break;
            case 9: J(muxSendState)(e, nullptr, s.link, ch, data.ConsumeIntegralInRange(0, 5)); break;
            case 10: J(muxClose)(e, nullptr, s.link, ch, data.ConsumeIntegralInRange(0, 8)); break;
            case 11: J(linkSendRadio)(e, nullptr, s.link, data.ConsumeBool(), data.ConsumeIntegral<jlong>()); break;
            case 12:
                J(linkState)(e, nullptr, s.link), J(linkError)(e, nullptr, s.link), J(linkRtt)(e, nullptr, s.link);
                J(linkPeerId)(e, nullptr, s.link), J(linkPeerNameUtf8)(e, nullptr, s.link), J(muxWaiting)(e, nullptr, s.link, ch);
                break;
            case 13: if (!data.ConsumeIntegralInRange(0, 7)) J(linkClose)(e, nullptr, s.link, data.ConsumeIntegralInRange(0, 8)); break;
        }
    }
    J(peekFirstFrame)(e, nullptr, env.Bytes(data.ConsumeRemainingBytes<uint8_t>()));
    const jint first = size ? bytes[0] : 0;
    J(refuseFrame)(e, nullptr, first & 7);
    J(randomBytes)(e, nullptr, first - 128);
    if (pairing) {
        J(pairFree)(e, nullptr, a.pair), J(pairFree)(e, nullptr, b.pair);
        if (code) J(codeFree)(e, nullptr, code);
    } else {
        J(linkFree)(e, nullptr, a.link), J(linkFree)(e, nullptr, b.link);
    }
    return 0;
}
