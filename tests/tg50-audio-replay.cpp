// Deterministic TG50 sound replay on the ring-buffer queue, in Exact or Fast (first argument).
// A guest loop feeds the I2S FIFO and moves the PWM duty with its own progress, so the PCM
// depends on how far the CPU gets in each batch. The consumer pops what guest time says is
// due and drains the queue every 40 steps; the queue is big enough that nothing is dropped.
#include "tg50-audio-fixture.h"
#include "audio_queue.h"
#include "CPU.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
constexpr unsigned steps = 400, mid = 160;
constexpr uint64_t stepCycles = 200000, frequency = 100000000;  // 2 ms of guest time asked
constexpr size_t queueFrames = 8192;

struct Consumer { uint64_t start{}, consumed{}; };
// The savestate leaves RAM out (sessions store it apart), so a snapshot holds both.
struct Snapshot {
    std::vector<uint8_t> state, ram;
    bool operator==(const Snapshot& o) const { return state == o.state && ram == o.ram; }
};
struct Replay {
    std::vector<uint32_t> pcm;
    Snapshot end, midState;
    Consumer midConsumer;
    size_t midFrames{};
};

void program(AudioMachine& m) {
    std::vector<uint32_t> code{
        0xe3a05101, 0xe3855840,  // r5 = 0x40400000 (I2S)
        0xe3a0a101, 0xe38aa8b0,  // r10 = 0x40b00000 (PWM0)
        0xe3a06000, 0xe3a07801, 0xe2877003,  // r6 = 0, r7 = 0x00010003
        0xe3a08000,              // r8 = 0
        0xe595100c, 0xe3110001,  // loop: ldr r1, [r5, #0xc]; tst r1, #1 (TNF)
        0x15856080, 0x10866007,  // strne r6, [r5, #0x80]; addne r6, r6, r7
    };
    // A straight run longer than the Fast engine's unchecked limit.
    for (unsigned i = 0; i < 100; ++i) code.push_back(0xe2888001);  // add r8, r8, #1
    code.push_back(0xe1a09228);  // mov r9, r8, lsr #4
    code.push_back(0xe20990ff);  // and r9, r9, #0xff
    code.push_back(0xe58a9004);  // str r9, [r10, #4] (duty)
    const int32_t offset = 8 - int32_t(code.size() + 2);
    code.push_back(0xea000000 | (uint32_t(offset) & 0xffffff));
    for (unsigned i = 0; i < code.size(); ++i) m.wr(0xa0003000 + 4 * i, code[i]);
    cpuInvalidateCodeCache(m.soc.GetCpu());
    cpuSetReg(m.soc.GetCpu(), ARM_REG_NUM_CPSR, ARM_SR_MODE_SVC | ARM_SR_I | ARM_SR_F);
    cpuSetReg(m.soc.GetCpu(), 15, 0xa0003000);
}

Snapshot snapshot(AudioMachine& m) {
    auto memory = m.soc.GetMemoryData();
    const auto* bytes = static_cast<const uint8_t*>(memory.data);
    return {m.save(), {bytes, bytes + memory.size}};
}

bool restore(AudioMachine& m, Snapshot snap) {
    auto memory = m.soc.GetMemoryData();
    assert(memory.size == snap.ram.size());
    memcpy(memory.data, snap.ram.data(), snap.ram.size());
    cpuInvalidateCodeCache(m.soc.GetCpu());
    return m.soc.Load(snap.state.size(), snap.state.data());
}

void attach(AudioMachine& m, AudioQueue* queue) {
    m.soc.SetAudioQueue(queue);
    m.soc.SetPcmOutputEnabled(true);
}

Replay replay(AudioMachine& m, AudioQueue* queue, unsigned first, Consumer consumer, bool reloadAtMid = false) {
    Replay r;
    std::vector<uint32_t> chunk;
    for (unsigned step = first; step < steps; ++step) {
        if (step == mid) {
            // The queue was drained by the step before, so the state carries no PCM.
            assert(!audioQueuePendingSamples(queue));
            r.midState = snapshot(m);
            r.midConsumer = consumer;
            r.midFrames = r.pcm.size();
            if (reloadAtMid) assert(restore(m, r.midState));
        }
        // No admission: every step runs its whole budget.
        assert(m.soc.Run(stepCycles, frequency) >= stepCycles);
        const size_t pending = audioQueuePendingSamples(queue);
        const uint64_t due = (m.soc.GetTime() - consumer.start) * 441 / 10000000;
        size_t take = step % 40 == 39 ? pending
                                       : size_t(std::min<uint64_t>(pending, due > consumer.consumed ? due - consumer.consumed : 0));
        chunk.resize(take);
        const size_t popped = audioQueuePopChunk(queue, chunk.data(), take);
        assert(popped == take);
        consumer.consumed += take;
        r.pcm.insert(r.pcm.end(), chunk.begin(), chunk.end());
    }
    r.end = snapshot(m);
    return r;
}

void same(const char* what, const Replay& a, const Replay& b) {
    if (a.pcm == b.pcm && a.end == b.end) return;
    if (a.end.ram != b.end.ram) fprintf(stderr, "%s: RAM differs\n", what);
    size_t i = 0;
    while (i < a.pcm.size() && i < b.pcm.size() && a.pcm[i] == b.pcm[i]) ++i;
    size_t j = 0;
    while (j < a.end.state.size() && j < b.end.state.size() && a.end.state[j] == b.end.state[j]) ++j;
    {  // name the savestate chunk where the end states first differ
        const auto* w = reinterpret_cast<const uint32_t*>(a.end.state.data());
        size_t off = 4 * (1 + 2 * w[0]);
        for (unsigned k = 0; k < w[0]; ++k) {
            const size_t size = 4 * size_t(w[2 + 2 * k]);
            if (j >= off && j < off + size)
                fprintf(stderr, "chunk %#x size %zu at +%zu: %08x vs %08x\n", w[1 + 2 * k], size, j - off,
                        *reinterpret_cast<const uint32_t*>(a.end.state.data() + (j & ~size_t(3))),
                        *reinterpret_cast<const uint32_t*>(b.end.state.data() + (j & ~size_t(3))));
            off += size;
        }
    }
    fprintf(stderr, "%s differs: frames %zu/%zu first at %zu, state %zu/%zu first at %zu\n", what,
            a.pcm.size(), b.pcm.size(), i, a.end.state.size(), b.end.state.size(), j);
    std::abort();
}

uint64_t hash(const std::vector<uint32_t>& pcm) {
    uint64_t h = 1469598103934665603ULL;
    for (auto frame : pcm)
        for (unsigned i = 0; i < 4; ++i) { h ^= (frame >> (8 * i)) & 0xff; h *= 1099511628211ULL; }
    return h;
}

void setup(AudioMachine& m) {
    m.configure(); m.reg(0, 0x80); m.reg(1, 0x61); m.reg(4, 0x6a); m.reg(6, 0x10);
    m.wr(0x40400004, 8);
    m.wr(0x40e0000c, 1u << 16); m.wr(0x40e00058, 2); m.wr(0x40b00000, 0x7f);
    m.wr(0x40b00004, 144); m.wr(0x40b00008, 288); m.wr(0x41300004, 0x101);
    program(m);
}
}  // namespace

int main(int argc, char** argv) {
    cpuSetJitSync(true);  // Fast compiles where asked for, so runs repeat
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode != "exact" && mode != "fast") { fprintf(stderr, "usage: %s exact|fast\n", argv[0]); return 2; }
    unsetenv("FROND_JIT");
    if (!cpuSetFastEngine(mode == "fast")) { puts("Fast engine not built in, skipped"); return 0; }

    AudioMachine m; setup(m);
    const uint32_t bound = cpuMaxOvershootCycles(m.soc.GetCpu());
    assert(mode == "fast" ? bound > 19 && bound < UINT32_MAX : bound == 19);
    auto* queue = audioQueueCreate(queueFrames);
    attach(m, queue);
    m.soc.Run(100000, frequency);  // past the I2S start, then begin from an empty queue
    audioQueueClear(queue);
    const auto before = audioQueueAccounting(queue);
    const auto start = snapshot(m);
    const Consumer consumer{m.soc.GetTime(), 0};

    // Every replay starts from a load, which also queues the host serial resume.
    assert(restore(m, start));
    const auto a = replay(m, queue, 0, consumer);
    assert(a.pcm.size() > 10000);
    unsigned distinct{};
    for (size_t i = 1; i < a.pcm.size(); ++i) distinct += a.pcm[i] != a.pcm[i - 1];
    assert(distinct > a.pcm.size() / 4);

    // Repeat in the same machine.
    assert(restore(m, start));
    const auto b = replay(m, queue, 0, consumer);
    same("repeat", a, b);
    assert(b.midState == a.midState);

    // A fresh machine loaded from the start, then one loaded from the middle.
    AudioMachine fresh; auto* freshQueue = audioQueueCreate(queueFrames);
    attach(fresh, freshQueue);  // before the load, as in the first machine
    assert(restore(fresh, start));
    const auto c = replay(fresh, freshQueue, 0, consumer);
    same("fresh", a, c);
    AudioMachine resumed; auto* resumedQueue = audioQueueCreate(queueFrames);
    attach(resumed, resumedQueue);
    assert(restore(resumed, a.midState));
    const auto d = replay(resumed, resumedQueue, mid, a.midConsumer);
    assert(std::vector<uint32_t>(a.pcm.begin() + a.midFrames, a.pcm.end()) == d.pcm);
    // A load also resumes the host serial ports, so the end state is compared with a run
    // that reloads in place at the same step.
    assert(restore(m, start));
    const auto e = replay(m, queue, 0, consumer, true);
    same("reload in place", a, Replay{e.pcm, a.end});
    same("resumed end", e, Replay{e.pcm, d.end});

    // Nothing dropped: every committed frame was consumed, and loads only cleared empty queues.
    for (auto* q : {queue, freshQueue, resumedQueue}) {
        const auto acc = audioQueueAccounting(q);
        assert(acc.committed == acc.consumed + acc.discarded + audioQueuePendingSamples(q));
    }
    assert(audioQueueAccounting(queue).discarded == before.discarded);
    assert(!audioQueueAccounting(freshQueue).discarded && !audioQueueAccounting(resumedQueue).discarded);
    printf("TG50 audio replay %s (bound %u): %zu frames, pcm %016llx, 0 dropped PASS\n",
           mode.c_str(), bound, a.pcm.size(), (unsigned long long)hash(a.pcm));
    m.soc.SetAudioQueue(nullptr); fresh.soc.SetAudioQueue(nullptr); resumed.soc.SetAudioQueue(nullptr);
    return 0;
}
