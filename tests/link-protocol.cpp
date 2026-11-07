// Link protocol checks: known vectors, pairing, the KK link, records and the channel mux.
// FROND_LINK_PRINT=1 prints the pinned regression values instead of only checking them.
#include "../link/link.h"

extern "C" {
#include "../link/monocypher/monocypher.h"
}

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace frond::link;

static int failures = 0;
static bool printPins = false;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static Bytes hex(const char* s) {
    Bytes b;
    std::string clean;
    for (const char* p = s; *p; p++)
        if (!std::isspace(uint8_t(*p))) clean.push_back(*p);
    for (size_t i = 0; i + 1 < clean.size(); i += 2) b.push_back(uint8_t(std::stoi(clean.substr(i, 2), nullptr, 16)));
    return b;
}

static std::string toHex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}
template <class T>
static std::string toHex(const T& v) { return toHex(v.data(), v.size()); }

template <size_t N>
static std::array<uint8_t, N> arr(const char* s) {
    Bytes b = hex(s);
    std::array<uint8_t, N> a{};
    if (b.size() == N) std::copy(b.begin(), b.end(), a.begin());
    return a;
}

template <class T>
static bool same(const T& v, const char* want) { return toHex(v) == toHex(hex(want)); }

static void pin(const char* name, const std::string& got, const char* want) {
    if (printPins) std::printf("pin %s = %s\n", name, got.c_str());
    if (got != want) {
        std::fprintf(stderr, "pin %s changed: got %s want %s\n", name, got.c_str(), want);
        failures++;
    }
}

// ---- Known vectors ----

static void testX25519() {
    Key32 a = arr<32>("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    Key32 b = arr<32>("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    Identity ia, ib;
    identityFromSecret(a, ia);
    identityFromSecret(b, ib);
    CHECK(same(ia.pub, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));
    CHECK(same(ib.pub, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"));
    Key32 k1, k2;
    CHECK(x25519(a, ib.pub, k1));
    CHECK(x25519(b, ia.pub, k2));
    CHECK(same(k1, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
    CHECK(k1 == k2);
    // A low-order peer key gives all zeros, which is refused.
    Key32 zero{}, out;
    CHECK(!x25519(a, zero, out));
}

static void testAead() {
    // RFC 8439 2.8.2
    const char* text =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
        "sunscreen would be it.";
    Bytes pt(text, text + std::strlen(text));
    Bytes key = hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
    Bytes nonce = hex("070000004041424344454647");
    Bytes ad = hex("50515253c0c1c2c3c4c5c6c7");
    Bytes ct(pt.size());
    uint8_t mac[16];
    crypto_aead_ctx ctx;
    crypto_aead_init_ietf(&ctx, key.data(), nonce.data());
    crypto_aead_write(&ctx, ct.data(), mac, ad.data(), ad.size(), pt.data(), pt.size());
    CHECK(same(ct,
               "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b"
               "1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
               "3ff4def08e4b7a9de576d26586cec64b6116"));
    CHECK(toHex(mac, 16) == "1ae10b594f09e26a7e902ecbd0600691");
}

static void testBlake2b() {
    // RFC 7693 appendix A
    const uint8_t abc[] = {'a', 'b', 'c'};
    Hash h = blake2b(abc, 3);
    CHECK(same(h,
               "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc95"
               "18d38aa8dbf1925ab92386edd4009923"));
}

// Noise_KK_25519_ChaChaPoly_BLAKE2b from the cacophony vectors (the snow test suite's copy).
static void testNoiseKK() {
    Bytes prologue = hex("4a6f686e2047616c74");
    Identity is, rs;
    identityFromSecret(arr<32>("e61ef9919cde45dd5f82166404bd08e38bceb5dfdfded0a34c8df7ed542214d1"), is);
    identityFromSecret(arr<32>("4a3acbfdb163dec651dfa3194dece676d437029c62a408b4c5ea9114246e4893"), rs);
    CHECK(same(rs.pub, "31e0303fd6418d2f8c0e78b91f22e8caed0fbe48656dcf4767e4834f701b8f62"));
    CHECK(same(is.pub, "6bc3822a2aa7f4e6981d6538692b3cdf3e6df9eea6ed269eb41d93c22757b75a"));
    KKHandshake ini(true, is, rs.pub, prologue.data(), prologue.size());
    KKHandshake res(false, rs, is.pub, prologue.data(), prologue.size());
    ini.setEphemeral(arr<32>("893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a"));
    res.setEphemeral(arr<32>("bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b"));
    struct { const char* payload; const char* ct; } msgs[] = {
        {"4c756477696720766f6e204d69736573",
         "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c7944f79a1d4b21fc3ea4a0c87213b8b4f0599d758682c26a3ae5e09195a3e742bc74"},
        {"4d757272617920526f746862617264",
         "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f144808843e20b1bf85731f75d7e21b5d54baaa66341de4292c3d42571c1bd7e7f1abe38"},
        {"462e20412e20486179656b", "25bfaa58833b07cdd6af7c07f2c51daac681a8ac0a02dd373259bd"},
        {"4361726c204d656e676572", "ef586cff556dec8ef0053871ff0d4bf3f2c72e842487ec6d1da69f"},
        {"4a65616e2d426170746973746520536179", "3265d50513550a354425d0218ba1e5f25d4994ce8990e6964398dba5982dbcbd85"},
        {"457567656e2042f6686d20766f6e2042617765726b", "f11c02d5c3223d7b9281b52e1b134962b91bc3bfbd1646354dab9fc19b66bf6c1e0a6f431e"},
    };
    Bytes m1, p1, m2, p2;
    Bytes pay = hex(msgs[0].payload);
    CHECK(ini.writeMessage(pay.data(), pay.size(), m1));
    CHECK(same(m1, msgs[0].ct));
    CHECK(res.readMessage(m1.data(), m1.size(), p1) && p1 == pay);
    pay = hex(msgs[1].payload);
    CHECK(res.writeMessage(pay.data(), pay.size(), m2));
    CHECK(same(m2, msgs[1].ct));
    CHECK(ini.readMessage(m2.data(), m2.size(), p2) && p2 == pay);
    CHECK(ini.done() && res.done());
    const char* hh =
        "76dbc866183c8ee7363dbf0ebab8d6355010245f9817aa78359818a03a052586d7e8b4bb2ae5622a1a61212df90af04bb2b2cc189ce0e819ba0c4970c9f71805";
    CHECK(same(ini.handshakeHash(), hh) && same(res.handshakeHash(), hh));
    CipherState is_, ir_, rs_, rr_;
    ini.split(is_, ir_);
    res.split(rs_, rr_);
    for (int i = 2; i < 6; i++) {
        bool fromIni = i % 2 == 0;
        Bytes p = hex(msgs[i].payload), c, back;
        CipherState& tx = fromIni ? is_ : rs_;
        CipherState& rx = fromIni ? rr_ : ir_;
        CHECK(tx.encrypt(nullptr, 0, p.data(), p.size(), c));
        CHECK(same(c, msgs[i].ct));
        CHECK(rx.decrypt(nullptr, 0, c.data(), c.size(), back) && back == p);
    }
}

// draft-irtf-cfrg-cpace-13 B.1, X25519. The draft hashes with SHA-512; Frond uses BLAKE2b, so
// the pieces that don't depend on the hash are checked against the draft and the ISK is pinned.
static void testCpace() {
    Bytes ci = hex("6f630b425f726573706f6e6465720b415f696e69746961746f72");
    Bytes sid = hex("7e4b4791d6a8ef019b936c79fb7f2c57");
    Bytes gs = cpaceGeneratorString("Password", ci, sid.data(), sid.size());
    CHECK(same(gs,
               "0843506163653235350850617373776f72646d000000000000000000"
               "00000000000000000000000000000000000000000000000000000000"
               "00000000000000000000000000000000000000000000000000000000"
               "00000000000000000000000000000000000000000000000000000000"
               "000000000000000000000000000000001a6f630b425f726573706f6e"
               "6465720b415f696e69746961746f72107e4b4791d6a8ef019b936c79"
               "fb7f2c57"));
    // The draft's hash has bit 254 clear, so Monocypher's map gives the same point.
    Bytes h = hex("92806dc608984dbf4e4aae478c6ec453ae979cc01ecc1a2a7cf49f5cee56551b");
    Key32 g = cpaceMapToCurve(h.data());
    CHECK(same(g, "64e8099e3ea682cfdc5cb665c057ebb514d06bf23ebc9f743b51b82242327074"));
    Key32 ya = arr<32>("21b4f4bd9e64ed355c3eb676a28ebedaf6d8f17bdc365995b319097153044080");
    Key32 yb = arr<32>("848b0779ff415f0af4ea14df9dd1d3c29ac41d836c7808896c4eba19c51ac40a");
    Key32 Ya, Yb, ka, kb;
    CHECK(cpaceScalarMult(ya, g, Ya));
    CHECK(cpaceScalarMult(yb, g, Yb));
    CHECK(same(Ya, "1b02dad6dbd29a07b6d28c9e04cb2f184f0734350e32bb7e62ff9dbcfdb63d15"));
    CHECK(same(Yb, "20cda5955f82c4931545bcbf40758ce1010d7db4db2a907013d79c7a8fcf957f"));
    CHECK(cpaceScalarMult(ya, Yb, ka) && cpaceScalarMult(yb, Ya, kb));
    CHECK(same(ka, "f97fdfcfff1c983ed6283856a401de3191ca919902b323c5f950c9703df7297a") && ka == kb);
    Bytes adA = {'A', 'D', 'a'}, adB = {'A', 'D', 'b'};
    Bytes in = cpaceIskInput(sid.data(), sid.size(), ka, Ya, adA, Yb, adB);
    CHECK(same(in,
               "0c43506163653235355f49534b107e4b4791d6a8ef019b936c79fb7f"
               "2c5720f97fdfcfff1c983ed6283856a401de3191ca919902b323c5f9"
               "50c9703df7297a201b02dad6dbd29a07b6d28c9e04cb2f184f073435"
               "0e32bb7e62ff9dbcfdb63d15034144612020cda5955f82c4931545bc"
               "bf40758ce1010d7db4db2a907013d79c7a8fcf957f03414462"));
    Hash isk = cpaceIsk(sid.data(), sid.size(), ka, Ya, adA, Yb, adB);
    pin("cpace-isk-blake2b", toHex(isk),
        "7b97a22ad2f19e04451c4f162291958c7ff2c371801e5132ac4baca3f38e7c5d8646a5604abb7d09cd003678613005b8ace285ae93797e01ac4c568868475071");
    // B.1.10: low-order points must give the neutral element and abort.
    Key32 s = arr<32>("af46e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449aff");
    const char* lowOrder[] = {
        "0000000000000000000000000000000000000000000000000000000000000000",
        "0100000000000000000000000000000000000000000000000000000000000000",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
        "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157",
        "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
        "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    };
    for (const char* u : lowOrder) {
        Key32 q;
        CHECK(!cpaceScalarMult(s, arr<32>(u), q));
    }
    struct { const char* u; const char* q; } nonZero[] = {
        {"daffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
         "d8e2c776bbacd510d09fd9278b7edcd25fc5ae9adfba3b6e040e8d3b71b21806"},
        {"dbffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
         "c85c655ebe8be44ba9c0ffde69f2fe10194458d137f09bbff725ce58803cdb38"},
        {"d9ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
         "db64dafa9b8fdd136914e61461935fe92aa372cb056314e1231bc4ec12417456"},
        {"cdeb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b880",
         "e062dcd5376d58297be2618c7498f55baa07d7e03184e8aada20bca28888bf7a"},
        {"4c9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f11d7",
         "993c6ad11c4c29da9a56f7691fd0ff8d732e49de6250b6c2e80003ff4629a175"},
    };
    for (auto& v : nonZero) {
        Key32 q;
        CHECK(cpaceScalarMult(s, arr<32>(v.u), q) && same(q, v.q));
    }
}

// ---- Fixtures ----

static LocalInfo makeLocal(const char* secretHex, uint8_t idByte, const char* name) {
    LocalInfo l;
    identityFromSecret(arr<32>(secretHex), l.identity);
    l.id.fill(idByte);
    l.name = name;
    return l;
}

static LocalInfo localA() {
    return makeLocal("a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf", 0xaa, "dan-laptop");
}
static LocalInfo localB() {
    return makeLocal("c0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedf", 0xbb, "Pixel 8");
}

// Moves bytes both ways until neither side has anything to say.
template <class X, class Y>
static void pump(X& a, Y& b, uint64_t now, Bytes* aToB = nullptr, Bytes* bToA = nullptr) {
    for (int i = 0; i < 1000; i++) {
        Bytes x = a.takeOutput();
        Bytes y = b.takeOutput();
        if (x.empty() && y.empty()) return;
        if (aToB) putBytes(*aToB, x.data(), x.size());
        if (bToA) putBytes(*bToA, y.data(), y.size());
        if (!x.empty()) b.receive(x.data(), x.size(), now);
        if (!y.empty()) a.receive(y.data(), y.size(), now);
    }
}

// ---- Pairing ----

static bool pairOnce(PairCode& code, const std::string& typed, uint64_t now, PairResult& ra, PairResult& rb,
                     PeerInfo* pa = nullptr, PeerInfo* pb = nullptr, Bytes* aToB = nullptr, Bytes* bToA = nullptr,
                     bool fixed = false) {
    Pairing a(localA(), typed, now);
    Pairing b(localB(), code, now);
    if (fixed) {
        a.setTestScalar(arr<32>("1111111111111111111111111111111111111111111111111111111111111111"));
        a.setTestSid(arr<16>("22222222222222222222222222222222"));
        b.setTestScalar(arr<32>("3333333333333333333333333333333333333333333333333333333333333333"));
    }
    a.start();
    pump(a, b, now, aToB, bToA);
    ra = a.result();
    rb = b.result();
    if (pa) *pa = a.peer();
    if (pb) *pb = b.peer();
    return ra == PairResult::Paired && rb == PairResult::Paired;
}

static void testPairing() {
    PairResult ra, rb;
    PeerInfo pa, pb;
    {
        PairCode code("482913", 0);
        Bytes ab, ba;
        CHECK(pairOnce(code, "482 913", 1000, ra, rb, &pa, &pb, &ab, &ba, true));
        CHECK(pa.key == localB().identity.pub && pa.name == "Pixel 8" && pa.id == localB().id);
        CHECK(pb.key == localA().identity.pub && pb.name == "dan-laptop" && pb.id == localA().id);
        Hash t = blake2b(ab.data(), ab.size(), ba.data(), ba.size());
        pin("pairing-transcript", toHex(t),
            "26f4e49e8410e99893565e2de978f2ba9c57cd9bcdecdc33c06abc6a97fd9d79abb2a247ade94a6610c21598fdb4d5a61f9f65cd89b6152d29fc9602c9c69e97");
        // The code is used up.
        CHECK(!pairOnce(code, "482913", 2000, ra, rb));
        CHECK(ra == PairResult::Expired && rb == PairResult::Expired);
    }
    {
        // Random codes are 6 digits.
        PairCode code(0);
        CHECK(code.code().size() == 6);
        CHECK(pairOnce(code, code.code(), 10, ra, rb));
    }
    {
        // Wrong code three times, then the right one is locked out.
        PairCode code("482913", 0);
        for (int i = 0; i < 3; i++) {
            CHECK(!pairOnce(code, "482914", 1000, ra, rb));
            CHECK(ra == PairResult::WrongCode && rb == PairResult::WrongCode);
        }
        CHECK(code.triesLeft() == 0);
        CHECK(!pairOnce(code, "482913", 1000, ra, rb));
        CHECK(ra == PairResult::LockedOut && rb == PairResult::LockedOut);
    }
    {
        // Two wrong, then right, still pairs.
        PairCode code("000001", 0);
        CHECK(!pairOnce(code, "000002", 1, ra, rb));
        CHECK(!pairOnce(code, "000003", 1, ra, rb));
        CHECK(pairOnce(code, "000001", 1, ra, rb));
    }
    {
        // An abandoned attempt still takes a try.
        PairCode code("123456", 0);
        Pairing a(localA(), "123456", 0);
        Pairing b(localB(), code, 0);
        a.start();
        Bytes p1 = a.takeOutput();
        b.receive(p1.data(), p1.size(), 0);
        CHECK(code.triesLeft() == 2);
    }
    {
        // Expiry: fine just before 2 minutes, refused at 2 minutes.
        PairCode code("654321", 0);
        CHECK(pairOnce(code, "654321", PairCode::kLifetimeMs - 1, ra, rb));
        PairCode late("654321", 0);
        CHECK(!pairOnce(late, "654321", PairCode::kLifetimeMs, ra, rb));
        CHECK(ra == PairResult::Expired && rb == PairResult::Expired);
    }
    {
        // Not six digits: refused before anything is sent.
        PairCode code("654321", 0);
        Pairing a(localA(), "65432", 0);
        a.start();
        CHECK(a.result() == PairResult::WrongCode && a.takeOutput().empty());
    }
    {
        // Tampered P2 fails confirmation on A.
        PairCode code("482913", 0);
        Pairing a(localA(), "482913", 0);
        Pairing b(localB(), code, 0);
        a.start();
        Bytes x = a.takeOutput();
        b.receive(x.data(), x.size(), 0);
        Bytes y = b.takeOutput();
        y[y.size() - 40] ^= 1;  // inside Yb
        a.receive(y.data(), y.size(), 0);
        CHECK(a.result() == PairResult::WrongCode);
    }
    {
        // Timeout.
        PairCode code("482913", 0);
        Pairing b(localB(), code, 0);
        b.tick(Pairing::kTimeoutMs);
        CHECK(b.result() == PairResult::Timeout);
    }
}

// ---- Link ----

struct Pair {
    LinkConfig ca, cb;
    std::unique_ptr<Link> a, b;
    uint64_t now = 0;
    Pair(LocalInfo la = localA(), LocalInfo lb = localB(), Key32 pinnedB = localB().identity.pub,
         Key32 pinnedA = localA().identity.pub, uint16_t bMin = 1, uint16_t bMax = 1) {
        ca.local = la;
        cb.local = lb;
        cb.minVersion = bMin;
        cb.maxVersion = bMax;
        a = std::make_unique<Link>(ca, lb.id, pinnedB, now);
        InstallId aid = localA().id;  // B has paired with A only
        b = std::make_unique<Link>(cb, [aid, pinnedA](const InstallId& id, Key32& key) {
            if (id != aid) return false;
            key = pinnedA;
            return true;
        }, now);
    }
    void up() {
        a->start();
        pump(*a, *b, now);
    }
};

static std::vector<Bytes> splitFrames(const Bytes& wire) {
    FrameReader r;
    r.feed(wire.data(), wire.size());
    std::vector<Bytes> out;
    Bytes body;
    while (r.next(body)) {
        Bytes f;
        writeFrame(f, body);
        out.push_back(f);
    }
    return out;
}

static size_t dataBytes(Mux& m, uint16_t ch) {
    size_t n = 0;
    for (auto& e : m.events)
        if (e.kind == ChannelEvent::Kind::Data && e.channel == ch) n += e.data.size();
    return n;
}

static void testLinkBasics() {
    Pair p;
    p.a->setTestEphemeral(arr<32>("4444444444444444444444444444444444444444444444444444444444444444"));
    p.b->setTestEphemeral(arr<32>("5555555555555555555555555555555555555555555555555555555555555555"));
    p.up();
    CHECK(p.a->state() == LinkState::Up && p.b->state() == LinkState::Up);
    CHECK(p.a->peerName() == "Pixel 8" && p.b->peerName() == "dan-laptop");
    CHECK(p.b->peerId() == localA().id && p.a->version() == 1);
    CHECK(p.a->handshakeHash() == p.b->handshakeHash());
    pin("link-handshake-hash", toHex(p.a->handshakeHash()),
        "54e3124c4cd73af1620357174381067cb4646937b0440de826bef39875a8e1eb3e896e0ecb3723097279ded3ead806ab65c0459fb78d7f6252206d9f35cdf939");

    // Open IR from A; B waits, then joins.
    uint16_t ch = p.a->mux().open(ChannelKind::Infrared, "Palm TX", "T|X");
    CHECK(ch == 1);
    pump(*p.a, *p.b, p.now);
    CHECK(p.b->mux().events.size() == 1 && p.b->mux().events[0].kind == ChannelEvent::Kind::OpenRequest);
    CHECK(p.b->mux().events[0].machine == "Palm TX");
    p.b->mux().events.clear();
    CHECK(p.b->mux().waiting(ch));
    pump(*p.a, *p.b, p.now);
    CHECK(p.a->mux().events.back().kind == ChannelEvent::Kind::Waiting);
    CHECK(p.b->mux().accept(ch, "Tungsten E2", "E2"));
    pump(*p.a, *p.b, p.now);
    CHECK(p.a->mux().events.back().kind == ChannelEvent::Kind::Accepted);
    CHECK(p.a->mux().events.back().machine == "Tungsten E2");
    uint8_t beam[100] = {0xc0, 1, 2, 3};
    CHECK(p.a->mux().send(ch, beam, sizeof beam) == sizeof beam);
    pump(*p.a, *p.b, p.now);
    CHECK(dataBytes(p.b->mux(), ch) == sizeof beam);

    // A second IR open from B's side is busy.
    uint16_t ch2 = p.b->mux().open(ChannelKind::Infrared, "Zire 72", "Zire72");
    CHECK(ch2 == 2);
    pump(*p.a, *p.b, p.now);
    CHECK(p.a->mux().events.back().kind == ChannelEvent::Kind::OpenRequest);
    CHECK(p.a->mux().busy(ch2, "Palm TX", "T|X"));
    pump(*p.a, *p.b, p.now);
    CHECK(p.b->mux().events.back().kind == ChannelEvent::Kind::Busy);
    CHECK(p.b->mux().events.back().machine == "Palm TX");
    CHECK(!p.b->mux().isOpen(ch2));

    // State and close.
    p.b->mux().sendState(ch, PeerState::Asleep);
    pump(*p.a, *p.b, p.now);
    CHECK(p.a->mux().events.back().kind == ChannelEvent::Kind::State &&
          p.a->mux().events.back().code == uint8_t(PeerState::Asleep));
    p.a->mux().close(ch);
    pump(*p.a, *p.b, p.now);
    CHECK(p.b->mux().events.back().kind == ChannelEvent::Kind::Closed && !p.b->mux().isOpen(ch));

    // Ping and pong: RTT, and still up.
    p.now = 2000;
    p.a->tick(p.now);
    Bytes ping = p.a->takeOutput();
    p.b->receive(ping.data(), ping.size(), p.now);
    p.now = 2007;
    Bytes pong = p.b->takeOutput();
    p.a->receive(pong.data(), pong.size(), p.now);
    CHECK(p.a->rttMs() == 7 && p.a->state() == LinkState::Up);

    // Link close.
    p.a->close();
    pump(*p.a, *p.b, p.now);
    CHECK(p.b->state() == LinkState::Down && p.b->error() == LinkError::Closed);
}

static void testTamperReplayReorder() {
    for (int mode = 0; mode < 3; mode++) {
        Pair p;
        p.up();
        uint16_t ch = p.a->mux().open(ChannelKind::Serial, "Palm TX", "T|X");
        pump(*p.a, *p.b, p.now);
        p.b->mux().accept(ch, "Tungsten E2", "E2");
        pump(*p.a, *p.b, p.now);
        uint8_t one[10] = {1}, two[10] = {2};
        p.a->mux().send(ch, one, sizeof one);
        Bytes r1 = p.a->takeOutput();
        p.a->mux().send(ch, two, sizeof two);
        Bytes r2 = p.a->takeOutput();
        CHECK(splitFrames(r1).size() == 1 && splitFrames(r2).size() == 1);
        if (mode == 0) {
            r1[5] ^= 0x40;
            p.b->receive(r1.data(), r1.size(), 0);
        } else if (mode == 1) {
            p.b->receive(r1.data(), r1.size(), 0);
            CHECK(p.b->state() == LinkState::Up && dataBytes(p.b->mux(), ch) == 10);
            p.b->receive(r1.data(), r1.size(), 0);
        } else {
            p.b->receive(r2.data(), r2.size(), 0);
        }
        CHECK(p.b->state() == LinkState::Down && p.b->error() == LinkError::Decrypt);
        // Nothing more comes out of a dead link, and nothing in the clear.
        CHECK(p.b->takeOutput().empty());
    }
    {
        // A cut length field is a decrypt failure too.
        Pair p;
        p.up();
        p.a->mux().queueControl({MsgType::Ping, 0, Bytes(8, 0)});
        Bytes r = p.a->takeOutput();
        Bytes cut(r.begin(), r.end() - 1);
        cut[1] = uint8_t(cut[1] - 1);
        p.b->receive(cut.data(), cut.size(), 0);
        CHECK(p.b->error() == LinkError::Decrypt);
    }
}

static void testKeyChange() {
    {
        // B made a new identity; A still has the old key pinned.
        LocalInfo nb = makeLocal("e0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", 0xbb, "Pixel 8");
        Pair p(localA(), nb, localB().identity.pub, localA().identity.pub);
        p.up();
        CHECK(p.a->state() == LinkState::Down && p.a->error() == LinkError::KeyChanged);
        CHECK(p.b->state() == LinkState::Down && p.b->error() == LinkError::KeyChanged);
    }
    {
        // A made a new identity; B has the old one pinned.
        LocalInfo na = makeLocal("e0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", 0xaa, "dan-laptop");
        Pair p(na, localB(), localB().identity.pub, localA().identity.pub);
        p.up();
        CHECK(p.a->error() == LinkError::KeyChanged && p.b->error() == LinkError::KeyChanged);
    }
    {
        // B doesn't know A.
        LocalInfo stranger = makeLocal("0101010101010101010101010101010101010101010101010101010101010101", 0xcc, "x");
        Pair p(stranger, localB(), localB().identity.pub, localA().identity.pub);
        p.up();
        CHECK(p.a->error() == LinkError::UnknownPeer && p.b->error() == LinkError::UnknownPeer);
    }
    {
        // A pairing request at a link responder is refused.
        Pair p;
        Pairing a(localA(), "482913", 0);
        a.start();
        pump(a, *p.b, 0);
        CHECK(p.b->state() == LinkState::Down && a.result() == PairResult::Protocol);
    }
}

static void testVersion() {
    {
        Pair p(localA(), localB(), localB().identity.pub, localA().identity.pub, 2, 3);
        p.up();
        CHECK(p.a->error() == LinkError::Version && p.b->error() == LinkError::Version);
    }
    {
        // Overlap: the higher common version wins.
        Pair p(localA(), localB(), localB().identity.pub, localA().identity.pub, 1, 3);
        p.up();
        CHECK(p.a->state() == LinkState::Up && p.b->version() == 1);
    }
    {
        // An unknown wire version in the first frame.
        Pair p;
        p.a->start();
        Bytes first = p.a->takeOutput();
        first[2 + 3] = 2;
        p.b->receive(first.data(), first.size(), 0);
        Bytes refuse = p.b->takeOutput();
        p.a->receive(refuse.data(), refuse.size(), 0);
        CHECK(p.b->error() == LinkError::Version && p.a->error() == LinkError::Version);
        CHECK(peekFirstFrame(first.data(), first.size()) == FirstFrame::BadVersion);
    }
    {
        Bytes x = {0x00, 0x10, 'G', 'E', 'T'};
        CHECK(peekFirstFrame(x.data(), x.size()) == FirstFrame::Garbage);
    }
}

static void testTimeouts() {
    {
        Pair p;
        p.up();
        // Pongs never come back: down when the 4th ping is due.
        for (uint64_t t = 2000; t <= 6000; t += 2000) {
            p.a->tick(t);
            p.a->takeOutput();
            CHECK(p.a->state() == LinkState::Up);
        }
        p.a->tick(8000);
        CHECK(p.a->state() == LinkState::Down && p.a->error() == LinkError::Timeout);
    }
    {
        Pair p;
        p.a->start();
        p.a->tick(Link::kHandshakeMs);
        CHECK(p.a->error() == LinkError::Timeout);
    }
}

// ---- Mux ----

static void openChannel(Pair& p, uint16_t& ch, ChannelKind kind) {
    ch = p.a->mux().open(kind, "Palm TX", "T|X");
    pump(*p.a, *p.b, p.now);
    p.b->mux().accept(ch, "Tungsten E2", "E2");
    pump(*p.a, *p.b, p.now);
}

static void testInfraredDrop() {
    Pair p;
    p.up();
    uint16_t ir;
    openChannel(p, ir, ChannelKind::Infrared);
    Bytes big(5000, 0x55);
    CHECK(p.a->mux().send(ir, big.data(), big.size()) == 0);
    CHECK(p.a->mux().infraredDropped() == 5000);
    CHECK(p.a->mux().send(ir, big.data(), 4096) == 4096);
    CHECK(p.a->mux().send(ir, big.data(), 1) == 0);
    CHECK(p.a->mux().infraredDropped() == 5001);
    pump(*p.a, *p.b, p.now);
    CHECK(dataBytes(p.b->mux(), ir) == 4096);
    // Room again once sent.
    CHECK(p.a->mux().send(ir, big.data(), 10) == 10);
    // IR before an open channel is dropped too.
    uint16_t ir2 = p.a->mux().open(ChannelKind::Infrared, "Palm TX", "T|X");
    CHECK(p.a->mux().send(ir2, big.data(), 5) == 0);
}

static void testCreditBackPressure() {
    Pair p;
    p.up();
    uint16_t ser, bt, ir;
    openChannel(p, ser, ChannelKind::Serial);
    openChannel(p, bt, ChannelKind::Bluetooth);
    openChannel(p, ir, ChannelKind::Infrared);
    Mux& ma = p.a->mux();
    Mux& mb = p.b->mux();
    mb.events.clear();

    // Serial: 4 KB goes out, then nothing until B's side takes bytes.
    Bytes lots(20000, 0x11);
    CHECK(ma.send(ser, lots.data(), lots.size()) == Mux::kSerialWindow);
    CHECK(ma.writable(ser) == 0);
    pump(*p.a, *p.b, p.now);
    CHECK(dataBytes(mb, ser) == Mux::kSerialWindow);
    CHECK(ma.send(ser, lots.data(), lots.size()) == Mux::kSerialWindow);  // queued, no credit
    pump(*p.a, *p.b, p.now);
    CHECK(dataBytes(mb, ser) == Mux::kSerialWindow);
    CHECK(ma.queued(ser) == Mux::kSerialWindow && ma.writable(ser) == 0);

    // While serial is stalled, Bluetooth and IR still flow.
    Bytes btData(150000, 0x22);
    size_t btSent = 0, btGot = 0;
    for (int round = 0; round < 20 && btGot < btData.size(); round++) {
        btSent += ma.send(bt, btData.data() + btSent, btData.size() - btSent);
        uint8_t beam[64] = {0xc0};
        CHECK(ma.send(ir, beam, sizeof beam) == sizeof beam);
        pump(*p.a, *p.b, p.now);
        size_t now = dataBytes(mb, bt);
        CHECK(now > btGot || btGot == btData.size());
        mb.consumed(bt, now - btGot);
        btGot = now;
        CHECK(dataBytes(mb, ir) == size_t(64 * (round + 1)));
    }
    CHECK(btGot == btData.size());
    CHECK(dataBytes(mb, ser) == Mux::kSerialWindow);

    // B takes a little: below a quarter window no credit goes yet.
    mb.consumed(ser, 100);
    pump(*p.a, *p.b, p.now);
    CHECK(dataBytes(mb, ser) == Mux::kSerialWindow);
    // Taking the rest returns all the credit and the queue drains.
    mb.consumed(ser, Mux::kSerialWindow - 100);
    pump(*p.a, *p.b, p.now);
    CHECK(dataBytes(mb, ser) == 2 * Mux::kSerialWindow);
    CHECK(ma.queued(ser) == 0 && ma.writable(ser) == Mux::kSerialWindow);

    // IR jumps ahead of a serial backlog in one output batch.
    mb.consumed(ser, Mux::kSerialWindow);
    pump(*p.a, *p.b, p.now);
    ma.send(ser, lots.data(), Mux::kSerialWindow);
    uint8_t beam[8] = {0xc0};
    ma.send(ir, beam, sizeof beam);
    Message first;
    CHECK(ma.next(first) && first.channel == ir);
}

static void testCreditViolation() {
    // A peer sending past our window drops the link.
    Mux m(false);
    Message open{MsgType::Open, 1, encodeOpen({ChannelKind::Serial, "x", "y"})};
    CHECK(m.receive(open));
    CHECK(m.accept(1, "a", "b"));
    Message d{MsgType::Data, 1, Bytes(Mux::kSerialWindow, 0)};
    CHECK(m.receive(d));
    Message more{MsgType::Data, 1, Bytes(1, 0)};
    CHECK(!m.receive(more));
    // Credit past the window is refused too.
    Mux s(true);
    uint16_t ch = s.open(ChannelKind::Serial, "x", "y");
    CHECK(s.receive({MsgType::Accept, ch, encodeMachine({"a", "b"})}));
    Bytes c;
    putU32(c, 1);
    CHECK(!s.receive({MsgType::Credit, ch, c}));
    // An open with our own parity is refused.
    Mux r(true);
    CHECK(!r.receive({MsgType::Open, 1, encodeOpen({ChannelKind::Infrared, "x", "y"})}));
}

int main() {
    printPins = std::getenv("FROND_LINK_PRINT") != nullptr;
    struct { const char* name; void (*fn)(); } cases[] = {
        {"vectors x25519", testX25519},
        {"vectors chacha20-poly1305", testAead},
        {"vectors blake2b", testBlake2b},
        {"vectors noise kk", testNoiseKK},
        {"vectors cpace", testCpace},
        {"pairing: code, wrong code, lockout, expiry", testPairing},
        {"link: channels, ping, close", testLinkBasics},
        {"link: tamper, replay, reorder", testTamperReplayReorder},
        {"link: key change refused", testKeyChange},
        {"link: version refuse", testVersion},
        {"link: timeouts", testTimeouts},
        {"mux: infrared drop above 4 KB", testInfraredDrop},
        {"mux: credit back-pressure without head-of-line blocking", testCreditBackPressure},
        {"mux: window violations", testCreditViolation},
    };
    for (auto& c : cases) {
        int before = failures;
        c.fn();
        std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", c.name);
    }
    if (failures) {
        std::printf("test-link-protocol: %d failures\n", failures);
        return 1;
    }
    std::printf("test-link-protocol: all passed\n");
    return 0;
}
