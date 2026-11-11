// The JIT compile thread's bookkeeping, with asserts on. A block whose first instruction is
// left to the interpreter ends where it starts (last_pc is first_pc - 1); taking it off the
// done list while an invalidation is logged must not look the empty range up. A block kept
// ready twice is kept once. A sync compile here runs on the calling thread and is left done.
#undef NDEBUG

#include <cstdio>
#include <memory>
#include <thread>

#include "dynarmic/common/frond_compile_thread.h"

using namespace Dynarmic;

namespace {

struct Snapshot final : A32::CodeSnapshot {
    std::optional<std::uint32_t> MemoryReadCode(A32::VAddr) override { return std::nullopt; }
    bool PreCodeReadHook(bool, A32::VAddr, A32::IREmitter&) override { return false; }
};

struct Range {
    std::uint32_t first, last;
};

using Thread = Common::FrondCompileThread<int>;

int failures = 0;

void Expect(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

}  // namespace

int main() {
    // Each job's range by its pc: an empty one, one inside the invalidated range, one outside.
    const std::uint32_t empty_pc = 0x1000, hit_pc = 0x2000, miss_pc = 0x3000;
    std::thread::id compiled_on;
    Thread thread{[&](Thread::Job& job) {
        compiled_on = std::this_thread::get_id();
        const std::uint32_t pc = static_cast<std::uint32_t>(job.location.Value());
        job.first_pc = pc;
        job.last_pc = pc == empty_pc ? pc - 1 : pc + 0x0f;
        job.status = Thread::Status::Done;
    }};

    for (const std::uint32_t pc : {empty_pc, hit_pc, miss_pc}) {
        thread.Push(IR::LocationDescriptor{pc}, std::make_unique<Snapshot>());
    }
    // Covers the empty block's pc and the hit block; logged, as the jobs are pending.
    boost::icl::interval_set<std::uint32_t> ranges;
    ranges.add(boost::icl::discrete_interval<std::uint32_t>::closed(0x0f00, 0x2007));
    thread.Invalidated(ranges);
    thread.WaitIdle();

    int seen = 0;
    for (const auto& job : thread.TakeDone()) {
        const std::uint32_t pc = static_cast<std::uint32_t>(job->location.Value());
        Expect(job->status == Thread::Status::Done, "job done");
        if (pc == empty_pc) {
            Expect(thread.StillValid(*job), "empty block still valid");
        } else if (pc == hit_pc) {
            Expect(!thread.StillValid(*job), "invalidated block dropped");
        } else {
            Expect(thread.StillValid(*job), "block outside the range kept");
        }
        seen++;
    }
    Expect(seen == 3, "three jobs done");

    // The same ahead block kept ready twice (one TakeDone batch queued it again): the second
    // copy is dropped, so an invalidation over it finds every indexed block.
    const std::uint32_t twice_pc = 0x4000;
    for (int i = 0; i < 2; i++) {
        auto job = std::make_unique<Thread::Job>();
        job->location = IR::LocationDescriptor{twice_pc};
        job->ahead = true;
        job->first_pc = twice_pc;
        job->last_pc = twice_pc + 0x0f;
        job->status = Thread::Status::Done;
        thread.KeepReady(std::move(job));
    }
    Expect(thread.TakeReady(IR::LocationDescriptor{twice_pc}) != nullptr, "kept block taken");
    Expect(thread.TakeReady(IR::LocationDescriptor{twice_pc}) == nullptr, "kept only once");
    boost::icl::interval_set<std::uint32_t> twice;
    twice.add(boost::icl::discrete_interval<std::uint32_t>::closed(twice_pc, twice_pc + 3));
    thread.KeepReady([&] {
        auto job = std::make_unique<Thread::Job>();
        job->location = IR::LocationDescriptor{miss_pc};
        job->ahead = true;
        job->first_pc = miss_pc;
        job->last_pc = miss_pc + 0x0f;
        job->status = Thread::Status::Done;
        return job;
    }());
    thread.Invalidated(twice);
    Expect(thread.TakeReady(IR::LocationDescriptor{miss_pc}) != nullptr, "block outside the invalidation still ready");

    // Compiled here: on this thread, done at once and off the pending list, counted as queued.
    const std::uint32_t here_pc = 0x5000;
    const auto queued = thread.GetStats().queued;
    thread.CompileHere(IR::LocationDescriptor{here_pc}, std::make_unique<Snapshot>());
    Expect(compiled_on == std::this_thread::get_id(), "compiled on the calling thread");
    Expect(thread.IsPending(IR::LocationDescriptor{here_pc}), "pending until taken");
    Expect(thread.HasDone(), "left on the done list");
    auto here = thread.TakeDone();
    Expect(here.size() == 1 && here[0]->status == Thread::Status::Done && thread.StillValid(*here[0]), "done and valid");
    Expect(!thread.IsPending(IR::LocationDescriptor{here_pc}), "taken off the pending list");
    Expect(thread.GetStats().queued == queued + 1, "counted as queued");

    if (failures) {
        return 1;
    }
    std::printf("jit-compile-thread: ok\n");
    return 0;
}
