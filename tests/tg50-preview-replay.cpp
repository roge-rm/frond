// Original TG50 Sony preview replay in Exact or Fast: tg50-preview-replay exact|fast session.
// The session is the Voice Recorder ready to play a 44.1 kHz stereo WAVE; a tap plays it.
// The consumer pops what guest time says is due, nothing in 100 ms stall windows, and drains
// the queue every 200 steps, so admission blocks under backpressure. Checks the same PCM,
// preview counts and trace hash on a repeat, from a mid-run reload in place and from a fresh
// machine loaded at the middle. The guest RTC follows guest time. Prints the counts for comparing Exact with Fast.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "audio_queue.h"
#include "CPU.h"
#include "device_profile5.h"
#include "session/session_file5.h"
#include "soc_PXA.h"
#include "timeutil.h"

namespace {
SocPXA* clockSoc = nullptr;
constexpr unsigned steps = 2400, mid = 1200, tapStep = 100, releaseStep = 130;
constexpr uint64_t frequency = 200000000, stepCycles = frequency / 400;  // 2.5 ms of guest time
constexpr size_t queueFrames = 2048;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Session {
    std::vector<uint8_t> nor, nand, memory, state;
    uint32_t ramSize{};
};

Session readSession(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(file && file.tellg() > 0 && file.tellg() < 512 * 1024 * 1024, "cannot read session");
    std::vector<uint8_t> data(size_t(file.tellg()));
    file.seekg(0);
    require(bool(file.read(reinterpret_cast<char*>(data.data()), data.size())), "short session read");
    SessionFile5 session;
    require(session.Deserialize(data.size(), data.data()), "invalid session");
    require(session.GetDeviceName() == "sony-clie-tg50", "a TG50 session is required");
    require(!session.GetSdSize(), "embedded card is not supported by this check");
    Session s;
    auto copy = [](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        return std::vector<uint8_t>(b, b + n);
    };
    s.nor = copy(session.GetNor(), session.GetNorSize());
    s.nand = copy(session.GetNand(), session.GetNandSize());
    s.memory = copy(session.GetMemory(), session.GetMemorySize());
    s.state = copy(session.GetSavestate(), session.GetSavestateSize());
    s.ramSize = session.GetRamSize();
    return s;
}

struct Snapshot {
    std::vector<uint8_t> state, ram;
    bool operator==(const Snapshot& o) const { return state == o.state && ram == o.ram; }
};

struct Machine {
    std::unique_ptr<SocPXA> soc;
    AudioQueue* queue{};
    explicit Machine(Session& s, bool preview = true) {
        soc = std::make_unique<SocPXA>(deviceTypeTG50, s.ramSize, s.nor.data(), s.nor.size(),
                                       s.nand.empty() ? nullptr : s.nand.data(), s.nand.size(), -1, 0);
        restore({s.state, s.memory});
        queue = audioQueueCreate(queueFrames);
        soc->SetAudioQueue(queue);
        soc->SetPcmOutputEnabled(true);
        audioQueueSetPreviewAllowed(queue, true);
        require(!preview || soc->EnableSonyPreview(), "Sony preview needs the original TG50 NOR");
    }
    ~Machine() { soc->SetAudioQueue(nullptr); }
    Snapshot snapshot() {
        require(soc->Save(), "save failed");
        auto st = soc->GetSavestate();
        auto mem = soc->GetMemoryData();
        const auto* sb = static_cast<const uint8_t*>(st.data);
        const auto* mb = static_cast<const uint8_t*>(mem.data);
        return {{sb, sb + st.size}, {mb, mb + mem.size}};
    }
    void restore(Snapshot snap) {
        auto mem = soc->GetMemoryData();
        require(mem.size == snap.ram.size(), "RAM size mismatch");
        memcpy(mem.data, snap.ram.data(), snap.ram.size());
        cpuInvalidateCodeCache(soc->GetCpu());
        clockSoc = soc.get();
        require(soc->Load(snap.state.size(), snap.state.data()), "load failed");
    }
};

struct Consumer { uint64_t start{}, consumed{}; };
struct Replay {
    std::vector<uint32_t> pcm;
    Snapshot end, midState;
    Consumer midConsumer;
    size_t midFrames{};
    SocPXA::SonyPreviewCounts midCounts, endCounts;
    unsigned blocked{};
};

Replay replay(Machine& m, unsigned first, Consumer consumer, bool reloadAtMid = false) {
    Replay r;
    std::vector<uint32_t> chunk;
    m.soc->ResetSonyPreviewTotals();
    for (unsigned step = first; step < steps; ++step) {
        if (step == mid) {
            // The queue was drained the step before, so the state carries no PCM.
            require(!audioQueuePendingSamples(m.queue), "queue not drained at the middle");
            r.midState = m.snapshot();
            r.midConsumer = consumer;
            r.midFrames = r.pcm.size();
            r.midCounts = m.soc->SonyPreviewTotals();
            if (reloadAtMid) m.restore(r.midState);
            // Counts and trace from here, comparable with a run started at the middle.
            if (step != first) m.soc->ResetSonyPreviewTotals();
        }
        if (step == tapStep) m.soc->PenDown(32, 250);
        if (step == releaseStep) m.soc->PenUp();
        clockSoc = m.soc.get();
        m.soc->Run(stepCycles, frequency);
        r.blocked += m.soc->AudioAdmissionBlocked();
        const bool stalled = step % 200 >= 120 && step % 200 < 160;  // 100 ms, more than the queue
        if (stalled) continue;
        const size_t pending = audioQueuePendingSamples(m.queue);
        const uint64_t due = (m.soc->GetTime() - consumer.start) * 441 / 10000000;
        const size_t take = step % 200 == 199
            ? pending
            : size_t(std::min<uint64_t>(pending, due > consumer.consumed ? due - consumer.consumed : 0));
        chunk.resize(take);
        require(audioQueuePopChunk(m.queue, chunk.data(), take) == take, "short pop");
        consumer.consumed += take;
        r.pcm.insert(r.pcm.end(), chunk.begin(), chunk.end());
    }
    r.end = m.snapshot();
    r.endCounts = m.soc->SonyPreviewTotals();
    return r;
}

// Names the savestate chunk where two states first differ.
void stateDiff(const char* what, const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    size_t j = 0;
    while (j < a.size() && j < b.size() && a[j] == b[j]) ++j;
    if (j == a.size() && j == b.size()) return;
    const auto* w = reinterpret_cast<const uint32_t*>(a.data());
    size_t off = 4 * (1 + 2 * size_t(w[0]));
    for (unsigned k = 0; k < w[0]; ++k) {
        const size_t size = 4 * size_t(w[2 + 2 * k]);
        if (j >= off && j < off + size)
            fprintf(stderr, "%s: chunk %#x size %zu differs at +%zu\n", what, w[1 + 2 * k], size, j - off);
        off += size;
    }
}

uint64_t hash(const uint32_t* pcm, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t k = 0; k < n; ++k)
        for (unsigned i = 0; i < 4; ++i) { h ^= (pcm[k] >> (8 * i)) & 0xff; h *= 1099511628211ULL; }
    return h;
}

bool sameCounts(const SocPXA::SonyPreviewCounts& a, const SocPXA::SonyPreviewCounts& b) {
    return a.submitCalls == b.submitCalls && a.submits == b.submits && a.completeCalls == b.completeCalls &&
           a.completes == b.completes && a.pauses == b.pauses && a.cancels == b.cancels;
}
void print(const char* what, const SocPXA::SonyPreviewCounts& c) {
    printf("%s: submit calls %llu accepted %llu, complete calls %llu matched %llu, pauses %llu, cancels %llu\n",
           what, (unsigned long long)c.submitCalls, (unsigned long long)c.submits,
           (unsigned long long)c.completeCalls, (unsigned long long)c.completes,
           (unsigned long long)c.pauses, (unsigned long long)c.cancels);
}
}  // namespace

// Replaces timeutil.o: the guest RTC follows the running machine's guest time.
uint64_t palmEpochSeconds() {
    return 3875817600ull + (clockSoc ? clockSoc->GetTime() / 1000000000ull : 0);  // 2026-10-26
}

int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        if (argc != 3 || (mode != "exact" && mode != "fast")) {
            fprintf(stderr, "usage: %s exact|fast session\n", argv[0]);
            return 2;
        }
        unsetenv("FROND_JIT");
        if (!cpuSetFastEngine(mode == "fast")) { puts("Fast engine not built in, skipped"); return 0; }
        auto session = readSession(argv[2]);

        Machine m(session);
        const Snapshot start = m.snapshot();
        const Consumer consumer{m.soc->GetTime(), 0};

        // Counts are reset at the start and the middle, so a replay's end counts cover the
        // second half and its mid counts the first.
        m.restore(start);
        const auto a = replay(m, 0, consumer);
        require(a.midCounts.submits + a.endCounts.submits > 0 && a.midCounts.completes + a.endCounts.completes > 0,
                "the preview saw no submissions");
        size_t nonzero = 0;
        for (auto f : a.pcm) nonzero += f != 0;
        require(nonzero > a.pcm.size() / 8, "the recording did not play");
        require(a.blocked > 0, "admission never blocked");

        m.restore(start);
        const auto b = replay(m, 0, consumer);
        require(a.pcm == b.pcm, "repeat PCM differs");
        stateDiff("repeat middle", a.midState.state, b.midState.state);
        stateDiff("repeat end", a.end.state, b.end.state);
        require(a.end.ram == b.end.ram && a.midState.ram == b.midState.ram, "repeat RAM differs");
        require(a.end == b.end && a.midState == b.midState, "repeat state differs");
        require(sameCounts(a.midCounts, b.midCounts) && sameCounts(a.endCounts, b.endCounts) &&
                a.midCounts.trace == b.midCounts.trace && a.endCounts.trace == b.endCounts.trace,
                "repeat preview counts or trace differ");

        // A reload in place at the middle, then a fresh machine loaded from the middle.
        m.restore(start);
        const auto e = replay(m, 0, consumer, true);
        require(std::equal(a.pcm.begin(), a.pcm.begin() + a.midFrames, e.pcm.begin()),
                "reload run PCM before the middle differs");
        Machine resumed(session);
        resumed.restore(a.midState);
        const auto d = replay(resumed, mid, a.midConsumer);
        require(std::vector<uint32_t>(e.pcm.begin() + a.midFrames, e.pcm.end()) == d.pcm,
                "PCM after a reload differs between in place and fresh");
        require(sameCounts(e.endCounts, d.endCounts) && e.endCounts.trace == d.endCounts.trace,
                "preview counts or trace after a reload differ");
        require(e.end.ram == d.end.ram, "RAM after a reload differs");

        // The same input without the preview, to show whether the preview reached the PCM.
        // Today it fences after a few buffers in both engines, so the PCM is the same.
        Machine plain(session, false);
        plain.restore(start);
        const auto p = replay(plain, 0, consumer);

        uint64_t checked{}, overruns{};
        SocPXA::AudioReservationTotals(checked, overruns);
        require(checked > 0 && !overruns, "audio reservation overrun");
        for (auto* q : {m.queue, resumed.queue, plain.queue}) {
            const auto acc = audioQueueAccounting(q);
            require(acc.committed == acc.consumed + acc.discarded + audioQueuePendingSamples(q),
                    "queue accounting");
            require(!acc.discarded, "frames discarded");
        }
        print("first half", a.midCounts);
        print("second half", a.endCounts);
        print("second half after reload", d.endCounts);
        printf("TG50 Sony preview replay %s: %zu frames (%zu nonzero), pcm %016llx, after reload %016llx, "
               "trace %016llx %016llx, no preview %016llx (%s), %u/%u steps blocked, %llu batches checked, 0 overruns PASS\n",
               mode.c_str(), a.pcm.size(), nonzero, (unsigned long long)hash(a.pcm.data(), a.pcm.size()),
               (unsigned long long)hash(d.pcm.data(), d.pcm.size()), (unsigned long long)a.midCounts.trace,
               (unsigned long long)a.endCounts.trace, (unsigned long long)hash(p.pcm.data(), p.pcm.size()), p.pcm == a.pcm ? "same" : "differs", a.blocked, steps, (unsigned long long)checked);
        return 0;
    } catch (const std::exception& error) {
        fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
